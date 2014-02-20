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

#ifndef _CSNETLINK_H
#define _CSNETLINK_H

using namespace std;

class csEventNetlink : public csEvent
{
public:
    enum Type {
        NL_Query,
        NL_RouteWatch,
    };

    csEventNetlink(enum Type type, uint16_t query = 0);

    virtual ~csEventNetlink();

    enum Type GetType(void) { return type; };
    void SetType(enum Type type) { this->type = type; };

    uint16_t GetQuery(void) { return query; };
    uint32_t GetSequence(void) { return query_seq; };
    void SetSequence(uint32_t seq) { query_seq = seq; };

    void AddReply(struct nlmsghdr *nh);
    struct nlmsghdr *GetReply(void);

    virtual csEvent *Clone(void);

protected:
    Type type;
    uint16_t query;
    uint32_t query_seq;
    pthread_mutex_t *reply_mutex;
    vector<struct nlmsghdr *> reply;
};

class csThreadNetlink : public csThread
{
public:
    csThreadNetlink(csEventClient *parent);
    virtual ~csThreadNetlink();

    virtual void *Entry(void);

    static csThreadNetlink *GetInstance(void) { return instance; };

protected:
    string name;
    csEventClient *parent;
    vector<csEventNetlink *> event_client;

    static csThreadNetlink *instance;

    void ProcessEvent(csEventNetlink *event);
    void SendNetlinkQuery(csEventNetlink *event);
    void SendNetlinkReply(struct nlmsghdr *nh);
    void ProcessNetlinkMessage(ssize_t length);

private:
    struct nl_req_t {
        struct nlmsghdr hdr;
        struct rtgenmsg gen;
    };

    int fd_netlink;
    uint8_t *nl_buffer;
    size_t nl_buffer_size;
    uint32_t nl_seq;
    struct sockaddr_nl sa_local;
};

#endif // _CSNETLINK_H
// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
