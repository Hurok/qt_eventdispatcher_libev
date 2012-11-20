#include <QtCore/QCoreApplication>
#include "eventdispatcher_libev.h"
#if QT_VERSION >= 0x050000
#	include <event2/thread.h>
#endif
#include "utils_p.h"
#include "eventdispatcher_libev_p.h"

EventDispatcherLibEvPrivate::EventDispatcherLibEvPrivate(EventDispatcherLibEv* const q)
	: q_ptr(q), m_interrupt(false), m_pipe_read(), m_pipe_write(), m_base(0), m_wakeup(0),
	  m_notifiers(), m_timers(), m_timers_to_reactivate(), m_seen_event(false)
{
	static bool init = false;
	if (!init) {
		init = true;
		event_set_log_callback(event_log_callback);
#if QT_VERSION >= 0x050000
		evthread_use_pthreads();
#endif
	}

	this->m_base = event_base_new();

	if (-1 == make_tco(&this->m_pipe_read, &this->m_pipe_write)) {
		qFatal("%s: Fatal: Unable to create thread communication object", Q_FUNC_INFO);
	}
	else {
		this->m_wakeup = event_new(this->m_base, this->m_pipe_read, EV_READ | EV_PERSIST, EventDispatcherLibEvPrivate::wake_up_handler, 0);
		event_add(this->m_wakeup, 0);
	}
}

EventDispatcherLibEvPrivate::~EventDispatcherLibEvPrivate(void)
{
#ifdef HAVE_SYS_EVENTFD_H
	Q_ASSERT(this->m_pipe_read == this->m_pipe_write);
#else
	if (-1 != this->m_pipe_write) {
		evutil_closesocket(this->m_pipe_write);
	}
#endif

	if (-1 != this->m_pipe_read) {
		evutil_closesocket(this->m_pipe_read);
	}

	if (this->m_wakeup) {
		event_del(this->m_wakeup);
		event_free(this->m_wakeup);
		this->m_wakeup = 0;
	}

	if (this->m_base) {
		event_base_free(this->m_base);
		this->m_base = 0;
	}
}

bool EventDispatcherLibEvPrivate::processEvents(QEventLoop::ProcessEventsFlags flags)
{
	Q_Q(EventDispatcherLibEv);

	Q_ASSERT(this->m_timers_to_reactivate.isEmpty());

	bool exclude_notifiers = (flags & QEventLoop::ExcludeSocketNotifiers);
	bool exclude_timers    = (flags & QEventLoop::X11ExcludeTimers);

	if (exclude_notifiers) {
		this->disableSocketNotifiers(true);
	}

	if (exclude_timers) {
		this->disableTimers(true);
	}

	bool result = false;
	this->m_seen_event = false;

	if (q->hasPendingEvents()) {
		QCoreApplication::sendPostedEvents();
		result = true;
		flags &= ~QEventLoop::WaitForMoreEvents;
	}

	QSet<int> timers;

	if ((flags & QEventLoop::WaitForMoreEvents)) {
		if (!this->m_interrupt) {
			this->m_seen_event = false;
			do {
				Q_EMIT q->aboutToBlock();
				event_base_loop(this->m_base, EVLOOP_ONCE);

				timers.unite(this->m_timers_to_reactivate);
				this->m_timers_to_reactivate.clear();

				if (!this->m_seen_event) {
					// When this->m_seen_event == false, this means we have been woken up by wake().
					// Send all potsed events here or tst_QEventLoop::execAfterExit() freezes
					QCoreApplication::sendPostedEvents(); // an event handler invoked by sendPostedEvents() may invoke processEvents() again
				}

				Q_EMIT q->awake();
			} while (!this->m_interrupt && !this->m_seen_event);
		}
	}
	else {
		event_base_loop(this->m_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
		Q_EMIT q->awake(); // If removed, tst_QEventLoop::processEvents() fails
		result |= this->m_seen_event;
	}

	timers.unite(this->m_timers_to_reactivate);
	this->m_timers_to_reactivate.clear();

	result |= this->m_seen_event;
	QCoreApplication::sendPostedEvents(); // an event handler invoked by sendPostedEvents() may invoke processEvents() again

	// Now that all event handlers have finished (and we returned from the recusrion), reactivate all pending timers
	if (!timers.isEmpty()) {
		struct timeval now;
		struct timeval delta;
		evutil_gettimeofday(&now, 0);
		QSet<int>::ConstIterator it = timers.constBegin();
		while (it != timers.constEnd()) {
			TimerHash::Iterator tit = this->m_timers.find(*it);
			if (tit != this->m_timers.end()) {
				EventDispatcherLibEvPrivate::TimerInfo* info = tit.value();

				if (!event_pending(info->ev, EV_TIMEOUT, 0)) { // false in tst_QTimer::restartedTimerFiresTooSoon()
					EventDispatcherLibEvPrivate::calculateNextTimeout(info, now, delta);
					event_add(info->ev, &delta);
				}
			}

			++it;
		}
	}

	if (exclude_notifiers) {
		this->disableSocketNotifiers(false);
	}

	if (exclude_timers) {
		this->disableTimers(false);
	}

	this->m_interrupt = false;
	return result;
}

void EventDispatcherLibEvPrivate::wake_up_handler(int fd, short int events, void* arg)
{
	Q_UNUSED(events)
	Q_UNUSED(arg)

#ifdef HAVE_SYS_EVENTFD_H
	quint64 t;
	if (safe_read(fd, &t, sizeof(t)) != sizeof(t)) {
		qErrnoWarning("%s: read failed", Q_FUNC_INFO);
	}
#else
	char buf[256];
	while (safe_read(fd, buf, sizeof(buf)) > 0) {
		// Do nothing
	}
#endif
}
