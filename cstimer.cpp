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

#include <stdexcept>
#include <string>
#include <vector>
#include <map>

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>

#include <clearsync/csexception.h>
#include <clearsync/cslog.h>
#include <clearsync/csutil.h>
#include <clearsync/csevent.h>
#include <clearsync/csthread.h>
#include <clearsync/cstimer.h>

csThreadTimer *csThreadTimer::instance = NULL;
pthread_mutex_t *csThreadTimer::vector_mutex = NULL;
vector<csTimer *> csThreadTimer::timer_vector;

csTimer::csTimer(cstimer_id_t id,
    time_t value, time_t interval, csEventClient *target)
    : running(false), id(id), target(target),
    value(value), interval(interval)
{
    timer_mutex = new pthread_mutex_t;
    pthread_mutex_init(timer_mutex, NULL);

    csThreadTimer::GetInstance()->AddTimer(this);

    csLog::Log(csLog::Debug,
        "Created timer: id: %lu, value: %ld, interval: %ld",
        id, value, interval);
}

csTimer::~csTimer()
{
    Stop();
    csThreadTimer::GetInstance()->RemoveTimer(this);
    pthread_mutex_destroy(timer_mutex);
}

void csTimer::Start(void)
{
    pthread_mutex_lock(timer_mutex);
    running = true;
    pthread_mutex_unlock(timer_mutex);
}

void csTimer::Stop(void)
{
    pthread_mutex_lock(timer_mutex);
    running = false;
    pthread_mutex_unlock(timer_mutex);
}

void csTimer::SetValue(time_t value)
{
    pthread_mutex_lock(timer_mutex);
    this->value = value;
    pthread_mutex_unlock(timer_mutex);
    csLog::Log(csLog::Debug,
        "Set timer value: id: %lu, value: %ld, interval: %ld",
        id, value, interval);
}

void csTimer::SetInterval(time_t interval)
{
    pthread_mutex_lock(timer_mutex);
    this->interval = interval;
    pthread_mutex_unlock(timer_mutex);
    csLog::Log(csLog::Debug,
        "Set timer interval: id: %lu, value: %ld, interval: %ld",
        id, value, interval);
}

void csTimer::Extend(time_t value)
{
    pthread_mutex_lock(timer_mutex);
    this->value += value;
    pthread_mutex_unlock(timer_mutex);
    csLog::Log(csLog::Debug,
        "Extend timer value: id: %lu, value: %ld (+%ld), interval: %ld",
        id, this->value, value, interval);
}

time_t csTimer::GetInterval(void)
{
    return interval;
}

time_t csTimer::GetRemaining(void)
{
    time_t _remaining = 0;
    pthread_mutex_lock(timer_mutex);
    _remaining = value;
    pthread_mutex_unlock(timer_mutex);
    return _remaining;
}

csThreadTimer::csThreadTimer(csEventClient *parent, const sigset_t &signal_set)
    : csThread(), parent(parent), signal_set(signal_set)
{
    if (instance != NULL)
        throw csException(EEXIST, "csThreadTimer");

    if (vector_mutex == NULL) {
        vector_mutex = new pthread_mutex_t;
        pthread_mutex_init(vector_mutex, NULL);
        instance = this;
    }

    memset(&sev, 0, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;

    if (timer_create(CLOCK_REALTIME, &sev, &timer_id) < 0)
        throw csException(errno, "timer_create");

    it_spec.it_value.tv_sec = 1;
    it_spec.it_value.tv_nsec = 0;
    it_spec.it_interval.tv_sec = 1;
    it_spec.it_interval.tv_nsec = 0;
}

csThreadTimer::~csThreadTimer()
{
    Join();

    if (instance != this) return;
    timer_delete(timer_id);
    pthread_mutex_destroy(vector_mutex);
    delete vector_mutex;
    vector_mutex = NULL;
}

void *csThreadTimer::Entry(void)
{
    int sig;
    siginfo_t si;
    struct timespec timeout;

    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;

    if (timer_settime(timer_id, 0, &it_spec, NULL) , 0)
        throw csException(errno, "timer_settime");

    csLog::Log(csLog::Debug, "Timer thread started.");

    for ( ;; ) {
        csEvent *event = EventPop();

        if (event != _CS_EVENT_NONE) {
            switch (event->GetId()) {
            case csEVENT_QUIT:
                csLog::Log(csLog::Debug, "Timer thread terminated.");
                delete event;
                return NULL;

            default:
                csLog::Log(csLog::Debug,
                    "Timer: unhandled event: %u", event->GetId());
                delete event;
            }
        }
                
        sig = sigtimedwait(&signal_set, &si, &timeout);
        if (sig < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;

            csLog::Log(csLog::Error,
                "Timer: sigtimedwait: %s", strerror(errno));
            EventBroadcast(new csEvent(csEVENT_QUIT,
                csEvent::Sticky | csEvent::HighPriority));

            return NULL;
        }

        if (sig == sev.sigev_signo) Tick();
        else csLog::Log(csLog::Warning,
            "Timer: unhandled signal: %s", strsignal(sig));
    }

    return NULL;
}

void csThreadTimer::AddTimer(csTimer *timer)
{
    pthread_mutex_lock(vector_mutex);
    timer_vector.push_back(timer);
    pthread_mutex_unlock(vector_mutex);
}

void csThreadTimer::RemoveTimer(csTimer *timer)
{
    pthread_mutex_lock(vector_mutex);
    for (vector<csTimer *>::iterator i = timer_vector.begin();
        i != timer_vector.end(); i++) {
        if ((*i) != timer) continue;
        timer_vector.erase(i);
        break;
    }
    pthread_mutex_unlock(vector_mutex);
}

void csThreadTimer::Tick(void)
{
    pthread_mutex_lock(vector_mutex);
    for (vector<csTimer *>::iterator i = timer_vector.begin();
        i != timer_vector.end(); i++) {
        pthread_mutex_lock((*i)->timer_mutex);
        if ((*i)->running) {
            if (--(*i)->value <= 0) {
                csEventClient *target = (*i)->target;
                if (target == NULL) target = parent;
                EventDispatch(new csEventTimer((*i)), target);

                (*i)->value = (*i)->interval;
                if ((*i)->value > 0) (*i)->running = true;
            }
        }
        pthread_mutex_unlock((*i)->timer_mutex);
    }
    pthread_mutex_unlock(vector_mutex);
}

// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
