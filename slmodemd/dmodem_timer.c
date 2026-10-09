/*
 *
 *    dmodem_timer.c - the timers of the modem core for dmodem.c: instead
 *                     of SIGALRM (no good in a program with threads),
 *                     dmodem_process() runs the timers of its modem
 *
 */

#include <pthread.h>

#include "modem_timer.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct timer *list;

/* the time of the modem being run in this thread: its audio, set by
   dmodem.c on the way in (a modem runs in one thread at a time) */
static __thread unsigned long now_ticks;

void dmodem_set_time(unsigned long ticks)
{
	now_ticks = ticks;
}

unsigned long get_time(void)
{
	return now_ticks;
}

void timer_init(struct timer *t)
{
	t->next = NULL;
	t->added = 0;
	t->expires = 0;
	t->func = NULL;
	t->data = NULL;
}

int timer_add(struct timer *t)
{
	pthread_mutex_lock(&lock);
	if (!t->added) {
		t->next = list;
		list = t;
		t->added = 1;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

int timer_del(struct timer *t)
{
	struct timer **p;
	pthread_mutex_lock(&lock);
	for (p = &list; *p; p = &(*p)->next)
		if (*p == t) {
			*p = t->next;
			break;
		}
	t->next = NULL;
	t->added = 0;
	pthread_mutex_unlock(&lock);
	return 0;
}

int modem_timer_init(void)
{
	return 0;
}

/* the expired timers that belong to owner (their data) */
void dmodem_run_timers(void *owner)
{
	unsigned long now = get_time();
	for (;;) {
		struct timer **p, *t = NULL;
		pthread_mutex_lock(&lock);
		for (p = &list; *p; p = &(*p)->next)
			if ((*p)->data == owner &&
			    (long)((*p)->expires - now) <= 0) {
				t = *p;
				*p = t->next;
				t->next = NULL;
				t->added = 0;
				break;
			}
		pthread_mutex_unlock(&lock);
		if (!t)
			break;
		if (t->func)
			t->func(t->data);
	}
}
