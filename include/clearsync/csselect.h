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

#ifndef _CSSELECT_H
#define _CSSELECT_H

using namespace std;

#ifndef _CS_SELECT_STACK_SIZE
#define _CS_SELECT_STACK_SIZE   32768
#endif 

#define _CS_SELECT_USLEEP       500000

class csEventSelect : public csEvent
{
public:
    csEventSelect() : csEvent(csEVENT_SELECT) {
        flags = Exclusive | Persistent;
    };
    virtual csEvent *Clone(void);

protected:
};

class csSelect
{
public:
    csSelect(csEventClient *parent);
    virtual ~csSelect();

    enum {
        FDS_NONE,

        FDS_READ = 0x01,
        FDS_WRITE = 0x02,
        FDS_EXCEPT = 0x04,

        FDS_ALL = (FDS_READ | FDS_WRITE | FDS_EXCEPT)
    } csSelectType;

    void Set(int fd, int whence);
    void Clear(int fd, int whence);
    void Reset(void);
    bool IsSet(int fd, int whence);

    void *Entry(void);

protected:
    csEventClient *parent;
    map<int, int> select_fds;
    map<int, int> select_events;

    csEventSelect select_event;
    pthread_mutex_t select_mutex;
    pthread_t select_thread;

private:
    int select_thread_exit;
};

#endif // _CSSELECT_H
// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
