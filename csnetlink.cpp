// ClearSync: system synchronization daemon.
// Copyright (C) 2011-2012 ClearFoundation <http://www.clearfoundation.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/socket.h>
#include <sys/time.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <vector>
#include <map>
#include <string>
#include <stdexcept>

#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>

#include <clearsync/csexception.h>
#include <clearsync/cslog.h>
#include <clearsync/csutil.h>
#include <clearsync/csevent.h>
#include <clearsync/csthread.h>
#include <clearsync/csnetlink.h>

#define _EVENT_TIMEOUT_MS       (500)

csEventNetlink::csEventNetlink(enum Type type, uint16_t query)
    : csEvent(csEVENT_NETLINK), type(type), query(query), query_seq(0)
{
    reply_mutex = new pthread_mutex_t;
    pthread_mutex_init(reply_mutex, NULL);
}

csEventNetlink::~csEventNetlink()
{
    pthread_mutex_destroy(reply_mutex);
}

void csEventNetlink::AddReply(struct nlmsghdr *nh)
{
    struct nlmsghdr *_nh;
    size_t length = NLMSG_LENGTH(nh->nlmsg_len);

    _nh = (struct nlmsghdr *)new uint8_t[length];
    memcpy(_nh, nh, length);

    pthread_mutex_lock(reply_mutex);
    reply.push_back(_nh);
    pthread_mutex_unlock(reply_mutex);
}

struct nlmsghdr *csEventNetlink::GetReply(void)
{
    struct nlmsghdr *nh = NULL;
    vector<struct nlmsghdr *>::iterator i;

    pthread_mutex_lock(reply_mutex);
    if (reply.size() > 0) {
        i = reply.begin();
        nh = (*i);
        reply.erase(i);
    }
    pthread_mutex_unlock(reply_mutex);

    return nh;
}

csEvent *csEventNetlink::Clone(void)
{
    // Error!  Can't broadcast or clone this event type!
    throw csException(EINVAL, "Broadcast/clone");
}

csThreadNetlink *csThreadNetlink::instance = NULL;

csThreadNetlink::csThreadNetlink(csEventClient *parent)
    : csThread(),
    name("csThreadNetlink"), parent(parent), fd_netlink(-1),
    nl_buffer(NULL), nl_buffer_size(0), nl_seq(0)
{
    if (instance != NULL)
        throw csException(EEXIST, name.c_str());

    instance = this;

    memset(&sa_local, 0, sizeof(sa_local));
    sa_local.nl_family = AF_NETLINK;
    sa_local.nl_pid = getpid();
    sa_local.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;

    fd_netlink = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd_netlink == -1) {
        csLog::Log(csLog::Error, "%s: socket: %s",
            name.c_str(), strerror(errno));
        return;
    }

    if (bind(fd_netlink,
        (struct sockaddr *)&sa_local, sizeof(sa_local)) == -1) {
        csLog::Log(csLog::Error, "%s: bind: %s",
            name.c_str(), strerror(errno));
        return;
    }

    nl_buffer_size = ::csGetPageSize();
    nl_buffer = new uint8_t[nl_buffer_size];

    csLog::Log(csLog::Debug, "%s: Initialized.", name.c_str());
}

csThreadNetlink::~csThreadNetlink()
{
    Join();

    if (instance != this) return;
    if (fd_netlink != -1) close(fd_netlink);
    if (nl_buffer != NULL) delete [] nl_buffer;
}

void *csThreadNetlink::Entry(void)
{
    ssize_t length;

    struct iovec iov = { nl_buffer, nl_buffer_size };
    struct msghdr msg = { (void *)&sa_local,
        sizeof(struct sockaddr_nl), &iov, 1, NULL, 0, 0 };

    csLog::Log(csLog::Debug, "Netlink thread started.");

    for ( ;; ) {
        if ((length = recvmsg(fd_netlink, &msg, MSG_DONTWAIT)) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                csLog::Log(csLog::Error, "%s: recvmsg: %s",
                    name.c_str(), strerror(errno));
                return NULL;
            }

            csEvent *event = EventPopWait(_EVENT_TIMEOUT_MS);
            if (event == NULL) continue;

            switch (event->GetId()) {
            case csEVENT_QUIT:
                csLog::Log(csLog::Debug,
                    "Netlink thread terminated.");
                EventDestroy(event);
                return NULL;

            case csEVENT_NETLINK:
                ProcessEvent(static_cast<csEventNetlink *>(event));
                break;

            default:
                csLog::Log(csLog::Debug,
                    "csThreadNetlink: unhandled event: %u",
                    event->GetId());
                EventDestroy(event);
            }

            continue;
        }

        ProcessNetlinkMessage(length);
    }

    return NULL;
}

void csThreadNetlink::ProcessEvent(csEventNetlink *event)
{
#ifdef _CS_DEBUG
    csLog::Log(csLog::Debug, "%s: csEVENT_NETLINK: 0x%08x",
        name.c_str(), event->GetSource());
#endif

    switch (event->GetType()) {
    case csEventNetlink::NL_Query:
        SendNetlinkQuery(event);
        break;
    case csEventNetlink::NL_RouteWatch:
        break;
    }

    csEventClient *src = event->GetTarget();
    csEventClient *dst = event->GetSource();
    event->SetSource(src);
    event->SetTarget(dst);

    event_client.push_back(event);
}

void csThreadNetlink::SendNetlinkQuery(csEventNetlink *event)
{
    switch (event->GetQuery()) {
    case RTM_GETLINK:
    case RTM_GETADDR:
    case RTM_GETROUTE:
    case RTM_GETNEIGH:
    case RTM_GETRULE:
    case RTM_GETQDISC:
    case RTM_GETTCLASS:
    case RTM_GETTFILTER:
        break;
    default:
        csLog::Log(csLog::Error, "%s: invalid query type: %d",
            name.c_str(), event->GetQuery());
        // TODO: Should probably reply back with an error...
        return;
    }

    struct sockaddr_nl sa_kernel;
    struct msghdr rtnl_msg;
    struct iovec iov;
    struct nl_req_t request;

    memset(&rtnl_msg, 0, sizeof(rtnl_msg));
    memset(&sa_kernel, 0, sizeof(sa_kernel));
    memset(&request, 0, sizeof(struct nl_req_t));

    sa_kernel.nl_family = AF_NETLINK;

    if (++nl_seq >= time(NULL) - 3600 * 24) nl_seq = 1;
    event->SetSequence(nl_seq);

    request.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
    request.hdr.nlmsg_type = event->GetQuery();
    request.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; 
    request.hdr.nlmsg_seq = nl_seq;
    request.hdr.nlmsg_pid = getpid();
    request.gen.rtgen_family = AF_UNSPEC;
    //request.gen.rtgen_family = AF_INET6;
    iov.iov_base = &request;
    iov.iov_len = request.hdr.nlmsg_len;

    rtnl_msg.msg_iov = &iov;
    rtnl_msg.msg_iovlen = 1;
    rtnl_msg.msg_name = &sa_kernel;
    rtnl_msg.msg_namelen = sizeof(sa_kernel);

    if (sendmsg(fd_netlink, (struct msghdr *)&rtnl_msg, 0) < 0) {
        csLog::Log(csLog::Error, "%s: Unable to send NL message: %s",
            name.c_str(), strerror(errno));
    }
}

void csThreadNetlink::SendNetlinkReply(struct nlmsghdr *nh)
{
    vector<csEventNetlink *>::iterator i;

    switch (nh->nlmsg_type) {
    case RTM_NEWROUTE:
    case RTM_DELROUTE:
        for (i = event_client.begin();
            i != event_client.end(); i++) {
            if ((*i)->GetType() != csEventNetlink::NL_RouteWatch)
                continue;

            (*i)->AddReply(nh);
            EventDispatch((*i), (*i)->GetTarget());
        }
        return;

    case NLMSG_NOOP:
        return;
    }

    for (i = event_client.begin();
        i != event_client.end(); i++) {
        if ((*i)->GetType() != csEventNetlink::NL_Query)
            continue;
        if ((*i)->GetSequence() != nh->nlmsg_seq)
            continue;

        (*i)->AddReply(nh);
        EventDispatch((*i), (*i)->GetTarget());

        switch (nh->nlmsg_type) {
        case NLMSG_DONE:
        case NLMSG_ERROR:
        case NLMSG_OVERRUN:
            event_client.erase(i);
            break;

        default:
            if (!(nh->nlmsg_flags & NLM_F_MULTI))
                event_client.erase(i);
        }

        return;
    }

#ifdef _CS_DEBUG
    csLog::Log(csLog::Debug, "%s: Un-handled netlink message",
        name.c_str());
#endif
}

void csThreadNetlink::ProcessNetlinkMessage(ssize_t length)
{
    struct nlmsghdr *nh;

    for (nh = (struct nlmsghdr *)nl_buffer;
        NLMSG_OK(nh, length); nh = NLMSG_NEXT(nh, length)) {
#ifdef _CS_DEBUG
        csLog::Log(csLog::Debug,
            "%s: NLMSG: %hu, len: %u (%u, %u), flags: 0x%x, seq: %u, pid: %u",
            name.c_str(), nh->nlmsg_type, nh->nlmsg_len,
            NLMSG_HDRLEN, NLMSG_LENGTH(nh->nlmsg_len),
            nh->nlmsg_flags, nh->nlmsg_seq, nh->nlmsg_pid);
#endif

        switch (nh->nlmsg_type) {
#ifdef _CS_DEBUG
        case NLMSG_DONE:
            csLog::Log(csLog::Debug, "%s: End of multi-part message",
                name.c_str());
            break;
#endif
        case NLMSG_ERROR:
            csLog::Log(csLog::Error, "%s: NLMSG_ERROR", name.c_str());
            break;

        case NLMSG_OVERRUN:
            csLog::Log(csLog::Error, "%s: NLMSG_OVERRUN", name.c_str());
            break;
        }

        SendNetlinkReply(nh);
    }
}

// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
