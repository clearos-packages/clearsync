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

#ifndef _CSTIMER_H
#define _CSTIMER_H

using namespace std;

typedef unsigned long cstimer_id_t;

class csThreadTimer;
class csTimer
{
public:
    csTimer(cstimer_id_t id,
        time_t value, time_t interval, csEventClient *target = NULL);
    virtual ~csTimer();

    inline cstimer_id_t GetId(void) { return id; };
    void Start(void);
    void Stop(void);
    void SetValue(time_t value);
    void SetInterval(time_t value);
    void Extend(time_t value);
    time_t GetInterval(void);
    time_t GetRemaining(void);
    inline csEventClient *GetTarget(void) { return target; };

protected:
    friend class csThreadTimer;

    bool running;
    cstimer_id_t id;
    csEventClient *target;
    time_t value;
    time_t interval;

    pthread_mutex_t *timer_mutex;
};

class csEventTimer : public csEvent
{
public:
    csEventTimer(csTimer *timer)
        : csEvent(csEVENT_TIMER), timer(timer) { };

    inline csTimer *GetTimer(void) { return timer; };

protected:
    csTimer *timer;
};

class csThreadTimer : public csThread
{
public:
    csThreadTimer(csEventClient *parent, const sigset_t &signal_set);
    virtual ~csThreadTimer();

    virtual void *Entry(void);

    void AddTimer(csTimer *timer);
    void RemoveTimer(csTimer *timer);

    static csThreadTimer *GetInstance(void) { return instance; };

protected:
    csEventClient *parent;
    sigset_t signal_set;
    timer_t timer_id;
    struct itimerspec it_spec;
    struct sigevent sev;

    static csThreadTimer *instance;
    static pthread_mutex_t *vector_mutex;
    static vector<csTimer *> timer_vector;

    void Tick(void);
};

#endif // _CSTIMER_H
// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
