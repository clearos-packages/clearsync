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

#include <vector>
#include <map>
#include <string>
#include <stdexcept>

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>

#include <clearsync/csexception.h>
#include <clearsync/cslog.h>
#include <clearsync/csutil.h>
#include <clearsync/csevent.h>
#include <clearsync/csselect.h>

static void *cs_select_entry(void *param)
{
    csSelect *thread = reinterpret_cast<csSelect *>(param);
    return thread->Entry();
}

csEvent *csEventSelect::Clone()
{
    csEventSelect *event = new csEventSelect(*this);
    return dynamic_cast<csEvent *>(event);
}

csSelect::csSelect(csEventClient *parent)
    : parent(parent), select_thread_exit(0)
{
    pthread_mutex_init(&select_mutex, NULL);

    Reset();

    memset(&select_thread, 0xff, sizeof(pthread_t));

    int rc;
    pthread_attr_t attr;

    if ((rc = pthread_attr_init(&attr)) != 0)
        throw csException(rc, "pthread_attr_init");
    if ((rc = pthread_attr_setstacksize(&attr, _CS_SELECT_STACK_SIZE)) != 0)
        throw csException(rc, "pthread_attr_setstacksize");
    if ((rc = pthread_create(&select_thread, &attr,
        &cs_select_entry, (void *)this)) != 0) {
        memset(&select_thread, 0xff, sizeof(pthread_t));
        throw csException(rc, "pthread_create");
    }
}

csSelect::~csSelect()
{
    int rc;
    pthread_t id_invalid;
    memset(&id_invalid, 0xff, sizeof(pthread_t));

    __sync_fetch_and_add(&select_thread_exit, 1);

    pthread_mutex_lock(&select_mutex);

    if (memcmp(&select_thread, &id_invalid, sizeof(pthread_t)) &&
        (rc = pthread_join(select_thread, NULL)) != 0)
        csLog::Log(csLog::Error, "pthread_join: %s", strerror(rc));

    pthread_mutex_unlock(&select_mutex);
    pthread_mutex_destroy(&select_mutex);
}

void csSelect::Set(int fd, int whence)
{
    pthread_mutex_lock(&select_mutex);

    map<int, int>::iterator i = select_fds.find(fd);
    if (i != select_fds.end())
        select_fds[fd] |= whence;
    else
        select_fds[fd] = whence;

    i = select_events.find(fd);
    if (i != select_events.end()) select_events.erase(i);

    pthread_mutex_unlock(&select_mutex);
}

void csSelect::Clear(int fd, int whence)
{
    pthread_mutex_lock(&select_mutex);

    map<int, int>::iterator i;

    i = select_fds.find(fd);
    if (i != select_fds.end()) select_fds.erase(i);
    i = select_events.find(fd);
    if (i != select_events.end()) select_events.erase(i);

    pthread_mutex_unlock(&select_mutex);
}

void csSelect::Reset(void)
{
    pthread_mutex_lock(&select_mutex);

    select_fds.clear();
    select_events.clear();

    pthread_mutex_unlock(&select_mutex);
}

bool csSelect::IsSet(int fd, int whence)
{
    bool is_set = false;

    pthread_mutex_lock(&select_mutex);

    map<int, int>::iterator i = select_events.find(fd);

    if (i != select_events.end()) {
        switch (whence) {
        case FDS_READ:
            if (i->second & FDS_READ) {
                is_set = true;
                i->second &= ~FDS_READ;
            }
            break;
        case FDS_WRITE:
            if (i->second & FDS_WRITE) {
                is_set= true;
                i->second &= ~FDS_WRITE;
            }
            break;
        case FDS_EXCEPT:
            if (i->second & FDS_EXCEPT) {
                is_set = true;
                i->second &= ~FDS_EXCEPT;
            }
            break;
        }
    }

    pthread_mutex_unlock(&select_mutex);

    return is_set;
}

void *csSelect::Entry(void)
{
    int rc, max_fd;
    struct timeval tv;
    map<int, int>::iterator i, j;
    fd_set fds_read, fds_write, fds_except;

    sigset_t signal_set;
    sigfillset(&signal_set);
    sigdelset(&signal_set, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &signal_set, NULL);

    while (!__sync_fetch_and_add(&select_thread_exit, 0)) {
        FD_ZERO(&fds_read);
        FD_ZERO(&fds_write);
        FD_ZERO(&fds_except);

        pthread_mutex_lock(&select_mutex);

        max_fd = -1;
        for (i = select_fds.begin(); i != select_fds.end(); i++) {
            if (i->second & FDS_READ)
                FD_SET(i->first, &fds_read);
            if (i->second & FDS_WRITE)
                FD_SET(i->first, &fds_write);
            if (i->second & FDS_EXCEPT)
                FD_SET(i->first, &fds_except);

            if (i->first > max_fd) max_fd = i->first;
        }

        pthread_mutex_unlock(&select_mutex);

        if (max_fd == -1) {
            usleep(_CS_SELECT_USLEEP * 2);
            continue;
        }

        tv.tv_sec = 0;
        tv.tv_usec = _CS_SELECT_USLEEP;

        rc = select(max_fd + 1, &fds_read, &fds_write, &fds_except, &tv);

        if (rc == -1) {
            csLog::Log(csLog::Warning, "select: %s", strerror(rc));
            usleep(_CS_SELECT_USLEEP * 2);
            continue;
        }

        // Select time-out...
        if (rc == 0) continue;

        pthread_mutex_lock(&select_mutex);

        for (i = select_fds.begin(); i != select_fds.end(); i++) {

            if (i->second & FDS_READ && FD_ISSET(i->first, &fds_read)) {
                j = select_events.find(i->first);
                if (j == select_events.end())
                    select_events[i->first] = FDS_READ;
                else
                    select_events[i->first] |= FDS_READ;
            }
            
            if (i->second & FDS_WRITE && FD_ISSET(i->first, &fds_write)) {
                j = select_events.find(i->first);
                if (j == select_events.end())
                    select_events[i->first] = FDS_WRITE;
                else
                    select_events[i->first] |= FDS_WRITE;
            }
           
            if (i->second & FDS_EXCEPT && FD_ISSET(i->first, &fds_except)) {
                j = select_events.find(i->first);
                if (j == select_events.end())
                    select_events[i->first] = FDS_EXCEPT;
                else
                    select_events[i->first] |= FDS_EXCEPT;
            }
        }

        pthread_mutex_unlock(&select_mutex);

        parent->EventPush(&select_event, parent);
    }

    return NULL;
}

// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
