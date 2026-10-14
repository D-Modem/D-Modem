
/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *    modem_half.cpp - a single modem core whose line audio is read from
 *    fd 3 and written to fd 4, so that two processes (for example one
 *    linked with the original dsplibs.o and one with the open data
 *    pumps) can be connected back to back by interop_call.
 *
 *    Derived from modem_test.cpp.
 */


/*
 *
 *    modem_test.cpp  --  modem simulator.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <modem.h>
#include <sys/time.h>
#include <modem_debug.h>
#include "dsplibs.h"

#define INFO(fmt,args...) fprintf(stderr, fmt , ##args );
#define ERR(fmt,args...) fprintf(stderr, "error: " fmt , ##args );

#define DBG(fmt,args...) if(modem_debug_level) \
                             fprintf(stderr, "main: " fmt , ##args );


//#define SIMULATE_RING 1


/* modem init externals : FIXME remove it */
extern int  dp_dummy_init(void);
extern void dp_dummy_exit(void);
extern int  dp_sinus_init(void);
extern void dp_sinus_exit(void);
extern int datafile_load_info(char *name,struct dsp_info *info);
extern int datafile_save_info(char *name,struct dsp_info *info);
/* the core's timers (dmodem_timer.cpp): its time, set here, and the ones
   due, run here */
extern void dmodem_set_time(unsigned long ticks);
extern void dmodem_run_timers(void *owner);


/* global config data */
extern unsigned modem_debug_logging;


struct modem_test {
	struct modem *modem;
	struct modem_test *link;
	int in,out;
	unsigned int delay;
	unsigned int started;
	unsigned pty_closed;
	unsigned close_count;
};

static void pace_clock(struct modem_test *t, int samples);



#define CLOSE_COUNT_MAX 100

/* static data */
static char inbuf[4096];
#define HALF_FRAME 48           /* samples, 5 ms at 9600 Hz */
static char pend[8192];
static int npend;
static char outbuf[4096];


/* 'driver' simulation */

static int modem_test_start (struct modem *m)
{
	struct modem_test *t = static_cast<modem_test*>(m->dev_data);
	DBG("modem_test_start...\n");
	t->delay = 256;
	t->started = 1;
	memset(outbuf,0,t->delay);
	write(t->out,outbuf,t->delay);
	return 0;
}

static int modem_test_stop (struct modem *m)
{
	struct modem_test *t = static_cast<modem_test*>(m->dev_data);
	DBG("modem_test_stop...\n");
	t->started = 0;
	t->delay = 0;
	return 0;
}

static int modem_test_ioctl(struct modem *m, unsigned int cmd, unsigned long arg)
{
	struct modem_test *t = static_cast<modem_test*>(m->dev_data);
	DBG("modem_test_ioctl: cmd %x, arg %lx...\n",cmd,arg);
        switch (cmd) {
        case MDMCTL_CAPABILITIES:
                return -1;
        case MDMCTL_HOOKSTATE:
		return 0;
        case MDMCTL_SPEED:
                return 0;
        case MDMCTL_GETFMTS:
        case MDMCTL_SETFMT:
                return 0;
        case MDMCTL_SETFRAGMENT:
                return 0;
        case MDMCTL_SPEAKERVOL:
		return 0;
        case MDMCTL_CODECTYPE:
                return CODEC_UNKNOWN;
        case MDMCTL_IODELAY:
		return t->delay/2;
        default:
                break;
        }
	return -2;
}



struct modem_driver modem_test_driver = {
        .name = "modem_test driver",
        .start = modem_test_start,
        .stop = modem_test_stop,
        .ioctl = modem_test_ioctl,
};


/*
 *
 */

static volatile sig_atomic_t keep_running = 1;

void mark_termination(int signum)
{
	DBG("signal %d: mark termination.\n",signum);
	keep_running = 0;
}


/* the DTE's input to the modem: pty readable */
static int pty_input(struct modem_test *t)
{
	struct termios termios;
	int pty = t->modem->pty;
	int count;

	//DBG("pty read...\n");
	/* check termios */
	tcgetattr(pty,&termios);
	if(memcmp(&termios,&t->modem->termios,
		  sizeof(termios))) {
		DBG("termios changed.\n");
		modem_update_termios(t->modem, &termios);
	}
	/* read data */
	count = t->modem->xmit.size - t->modem->xmit.count;
	if(count == 0)
		return 0;
	if (count > sizeof(inbuf))
		count = sizeof(inbuf);
	count = read(pty,inbuf,count);
	if(count < 0) {
		if(errno == EAGAIN) {
			DBG("pty read, errno = EAGAIN\n");
			return 0;
		}
		if(errno == EIO) {
			if(!t->pty_closed) {
				DBG("pty closed.\n");
				t->pty_closed = 1;
				if(termios.c_cflag&HUPCL)
					modem_hangup(t->modem);
			}
			t->close_count = 1;
			//DBG("pty read, errno=EIO\n");
			return 0;
		}
		else
			ERR("pty read: %s\n",
			    strerror(errno));
		return -1;
	}
	else if (count == 0) {
		DBG("pty read = 0\n");
	}
	t->pty_closed = 0;
	//DBG("pty read %d\n",count);
	count = modem_write(t->modem,inbuf,count);
	if(count < 0) {
		DBG("modem_write failed.\n");
		return -1;
	}
	return 0;
}

/* PIPE_SYNC=<samples>: the DTE's input only every that many samples of the
   line, so that a test's commands meet the line at the same samples every
   time */
static long sync_n, sync_phase, line_samples, next_sync;
static double line_at;          /* when the line last had samples */

/* no line samples for 0.5 s (no call yet, nothing flows): the DTE's
   input as it comes */
static int line_idle(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec * 1e-6 - line_at > 0.5;
}

static long core_samples;        /* of the line, through the core so far */

/* the core's time in MODEM_HZ ticks: with PIPE_SYNC the line's while it
   runs (its timers then meet the line at the same samples whatever the
   machine, and keep up with it unpaced), else and while the line stands
   the wall clock's */
static unsigned long now_ticks(struct modem_test *t)
{
	static double start;
	static long off;
	static unsigned long last;
	struct timeval tv;
	double wall;
	long tk;
	gettimeofday(&tv, NULL);
	wall = tv.tv_sec + tv.tv_usec * 1e-6;
	if (start == 0)
		start = wall;
	tk = (long)((wall - start) * MODEM_HZ);
	if (sync_n) {
		long line = (long)(core_samples * MODEM_HZ / t->modem->srate);
		if (line_idle())
			off = tk - line;
		tk = line + off;
	}
	if ((long)(tk - last) < 0)
		tk = last;
	return last = tk;
}

/* the core's time set, and its timers that are due run */
static void run_timers(struct modem_test *t)
{
	dmodem_set_time(now_ticks(t));
	dmodem_run_timers(t->modem);
}

/* at the sample: whatever the DTE has sent */
static int sync_input(struct modem_test *t)
{
	struct pollfd p = { t->modem->pty, POLLIN, 0 };
	if (t->modem->xmit.size - t->modem->xmit.count <= 0 ||
	    poll(&p, 1, 0) <= 0)
		return 0;
	DBG("DTE input at line sample %ld\n", line_samples);
	run_timers(t);
	return pty_input(t);
}

static int modem_test_run(struct modem_test *modems)
{
	struct timeval tmo;
	fd_set rset,eset;
	struct termios termios;
	struct modem_test *t;
	void *in;
	int max_fd = 0;
	int count, ret;

#ifdef SIMULATE_RING
#define RING_HZ 20
#define RING_ON  1*RING_HZ
#define RING_OFF 4*RING_HZ
	unsigned rcount = RING_ON;
#endif


	if (getenv("PIPE_SYNC")) {
		sync_n = (atol(getenv("PIPE_SYNC")) + HALF_FRAME - 1) /
			HALF_FRAME * HALF_FRAME;
		/* SMARTLINK_SYNC_PHASE: that much later in each period (the
		   other half reads PIPE_SYNC as well) */
		sync_phase = getenv("SMARTLINK_SYNC_PHASE") ?
			atol(getenv("SMARTLINK_SYNC_PHASE")) / HALF_FRAME *
			HALF_FRAME % sync_n : 0;
		next_sync = sync_n + sync_phase;
	}

	while(keep_running) {

		for( t = modems ; t->modem ; t++ ) {
			run_timers(t);
			if(t->modem->event)
				modem_event(t->modem);
		}
#ifdef SIMULATE_RING
		tmo.tv_sec = 0;
		tmo.tv_usec= 1000000/RING_HZ;
#else
		/* (a tick: the timers are run here) */
                tmo.tv_sec = 0;
                tmo.tv_usec= 1000000/MODEM_HZ;
#endif
                FD_ZERO(&rset);
		FD_ZERO(&eset);
		max_fd = 0;

		for( t = modems ; t->modem ; t++ ) {
			FD_SET(t->in,&rset);
			FD_SET(t->in,&eset);
			if(t->in > max_fd)
				max_fd = t->in;
			if( t->pty_closed && t->close_count ) {
				if ( !t->started ||
				     t->close_count++ > CLOSE_COUNT_MAX )
					t->close_count = 0;
			}
			/* with PIPE_SYNC, commands only at the line's
			   periods; data (online, state 5) at once */
			else if ((!sync_n || line_idle() ||
				  t->modem->state == 0x5) &&
				 t->modem->xmit.size - t->modem->xmit.count > 0) {
				FD_SET(t->modem->pty,&rset);
				if(t->modem->pty > max_fd)
					max_fd = t->modem->pty;
			}
		}

                ret = select(max_fd + 1,&rset,NULL,&eset,&tmo);

                if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
                        ERR("select: %s\n",strerror(errno));
                        return ret;
                }

		//DBG("select = %d\n",ret);
		if (ret == 0) {
#ifdef SIMULATE_RING 
			if(!modems->modem->started) {
				rcount++;
				if (rcount <= RING_ON)
					modem_ring(modems->modem);
				else if (rcount > RING_OFF)
					rcount = 0;
			}
#endif
			continue;
		}

		for( t = modems ; t->modem ; t++ ) {
			if(FD_ISSET(t->in,&eset)) {
				DBG("dev exception...\n");
			}
		}

		for( t = modems ; t->modem ; t++ ) {
			if(FD_ISSET(t->in,&rset)) {
				//DBG("dev read...\n");
				/* the modem core wants whole periods: pass the
				   line samples on in frames of HALF_FRAME */
				int left = 0;
			sync_again:
				if (sync_n && line_samples >= next_sync) {
					next_sync += sync_n;
					if (sync_input(t) < 0)
						return -1;
				}
				if (left) {
					count = 0;
					goto sync_left;
				}
				count = read(t->in,pend + npend,sizeof(pend) - npend);
				if(count < 0) {
					ERR("dev read: %s\n",strerror(errno));
					return -1;
				}
				else if (count == 0) {
					DBG("dev read = 0\n");
					continue;
				}
				if (sync_n) {
					struct timeval tv;
					/* the line runs again (a call): its
					   periods from here */
					if (line_idle())
						next_sync = line_samples +
							sync_n + sync_phase;
					gettimeofday(&tv, NULL);
					line_at = tv.tv_sec + tv.tv_usec * 1e-6;
				}
				npend += count;
			sync_left:
				count = npend / (HALF_FRAME * 2) * (HALF_FRAME * 2);
				if (count > sizeof(inbuf) / (HALF_FRAME * 2) * (HALF_FRAME * 2))
					count = sizeof(inbuf) / (HALF_FRAME * 2) * (HALF_FRAME * 2);
				/* up to the next sample the DTE's input is
				   taken at */
				if (sync_n && count > (next_sync - line_samples) * 2)
					count = (int)(next_sync - line_samples) * 2;
				if (count == 0)
					continue;
				memcpy(inbuf, pend, count);
				memmove(pend, pend + count, npend - count);
				npend -= count;
				line_samples += count / 2;
				in = inbuf;
				if(t->modem->update_delay < 0) {
					if ( -t->modem->update_delay >= count/2) {
						DBG("change delay -%d...\n", count/2);
						t->delay -= count;
						t->modem->update_delay += count/2;
						core_samples += count/2;
						continue;
					}
					DBG("change delay %d...\n", t->modem->update_delay);
					in -= t->modem->update_delay*2;
					count += t->modem->update_delay*2;
					t->delay += t->modem->update_delay*2;
					t->modem->update_delay = 0;
				}
				/* no faster than the clock between calls as
				   well: the line's silence goes round */
				pace_clock(t, count>>1);
				if(t->started) {
					static FILE *rec_in, *rec_out;
					if (sync_n) {
						/* a half frame at a time,
						   with the modem's events in
						   between: they meet the
						   line at the same samples
						   whatever the reads bring */
						int k;
						for (k = 0; k < count / 2;
						     k += HALF_FRAME) {
							int len = count / 2 - k < HALF_FRAME ?
								count / 2 - k : HALF_FRAME;
							dmodem_set_time(now_ticks(t));
							if (t->started)
								modem_process(t->modem,
									in + 2 * k,
									outbuf + 2 * k, len);
							else
								memset(outbuf + 2 * k, 0,
								       2 * len);
							core_samples += len;
							run_timers(t);
							if (t->modem->event)
								modem_event(t->modem);
						}
					} else {
					dmodem_set_time(now_ticks(t));
					modem_process(t->modem,in,outbuf,count>>1);
					core_samples += count >> 1;
					}
					/* MODEM_REC=<prefix>: record <prefix>.in/.out */
					if (getenv("MODEM_REC") && !rec_in) {
						char name[256];
						snprintf(name, sizeof(name), "%s.in", getenv("MODEM_REC"));
						rec_in = fopen(name, "wb");
						snprintf(name, sizeof(name), "%s.out", getenv("MODEM_REC"));
						rec_out = fopen(name, "wb");
					}
					if (rec_in) {
						fwrite(in, 1, count, rec_in);
						fwrite(outbuf, 1, count, rec_out);
					}
				}
				else {
					memset(outbuf,0,count);
					core_samples += count >> 1;
					/* ring here */
				}
				count = write(t->out,outbuf,count);
				if(count < 0) {
					ERR("dev write: %s\n",strerror(errno));
					return -1;
				}
				else if (count == 0) {
					DBG("dev write = 0\n");
				}
				if(t->modem->update_delay > 0) {
					DBG("change delay %d...\n", t->modem->update_delay);
					memset(outbuf, 0, t->modem->update_delay*2);
					count = write(t->out,outbuf,t->modem->update_delay*2);
					if(count < 0) {
						ERR("dev write: %s\n",strerror(errno));
						return -1;
					}
					t->delay += t->modem->update_delay*2;
					t->modem->update_delay = 0;
				}
				/* PIPE_SYNC stops at its samples: on with
				   what is left, else it may hold the line's
				   last samples with nothing more to read */
				if (sync_n && npend >= HALF_FRAME * 2) {
					left = 1;
					goto sync_again;
				}
			}
		}

		for( t = modems ; t->modem ; t++ ) {
			if(!FD_ISSET(t->modem->pty,&rset))
				continue;
			run_timers(t);
			if(pty_input(t) < 0)
				return -1;
		}
	}

	return 0;
}


/* MODEM_REALTIME=1: run no faster than the sample clock, so that a
   DTE program can keep up with the protocol timers; with PIPE_SYNC on
   hook as well (the DTE's commands then meet the samples they would in
   real time, however late the DTE answers), and off hook MODEM_SPEED=<x>
   times the sample clock at most (unset: as fast as it goes), the timers
   on the line's clock (now_ticks()) */
static void pace_clock(struct modem_test *t, int samples)
{
	static int realtime = -1;
	static double max_speed, cur, start, done;
	struct timeval tv;
	double now, ahead, speed;
	if (realtime < 0) {
		realtime = getenv("MODEM_REALTIME") != NULL;
		max_speed = getenv("MODEM_SPEED") ?
			atof(getenv("MODEM_SPEED")) : 0;
	}
	speed = realtime || (sync_n && !t->started) ? 1 : max_speed;
	if (speed != cur) {
		cur = speed;
		done = 0;               /* (it starts anew) */
	}
	if (speed <= 0)
		return;
	gettimeofday(&tv, NULL);
	now = tv.tv_sec + tv.tv_usec * 1e-6;
	if (done == 0 ||
	    now - start - done / t->modem->srate / speed > 0.5) {
		/* (re)start the clock after a pause */
		start = now;
		done = 0;
	}
	done += samples;
	ahead = start + done / t->modem->srate / speed - now;
	if (ahead > 0.002)
		usleep(ahead * 1e6);
}

static int modem_test_init(struct modem_test *t, const char *name, int in, int out)
{
	struct termios termios;
	char *pty_name;
	int pty;
	int ret;
	
	memset(t,0,sizeof(*t));

	t->in  = in;
	t->out = out;

        pty  = getpt();
        if (pty < 0 || grantpt(pty) < 0 || unlockpt(pty) < 0) {
                ERR("getpt: %s\n",strerror(errno));
                exit(-1);
        }

        ret = tcgetattr(pty, &termios);
        /* non canonical raw tty */
        cfmakeraw(&termios);
        cfsetispeed(&termios, B115200);
        cfsetospeed(&termios, B115200);

        ret = tcsetattr(pty, TCSANOW, &termios);
        if (ret) {
                ERR("tcsetattr: %s\n",strerror(errno));
                exit(-1);
        }

	fcntl(pty,F_SETFL,O_NONBLOCK);

	pty_name = ptsname(pty);

	t->modem = modem_create(&modem_test_driver,name);
	if(!t->modem) {
		return -1;
	}
	t->modem->name = name;
	t->modem->pty = pty;
	//t->modem->dev = dev;
	t->modem->dev_name = name;
	t->modem->pty_name = pty_name;
	// datafile_load_info(basename(dev_name),&t->modem->dsp_info);
	modem_update_termios(t->modem,&termios);
	t->modem->dev_data = t;

	DBG("created %s: %s\n",t->modem->name,t->modem->pty_name);
	return 0;
}

static void modem_test_free(struct modem_test *t)
{
	int pty = t->modem->pty;
	modem_delete(t->modem);
	close(pty);
}



/* one modem whose line audio is read from fd 3 and written to fd 4 */
int main(int argc, char *argv[])
{
	extern void modem_cmdline(int argc, char *argv[]);
	struct modem_test modems[2];
	int ret;

	modem_debug_level = 1;
	modem_cmdline(argc,argv);
	modem_debug_init("half");
	memset(modems,0,sizeof(modems));
	prop_dp_init();
	modem_timer_init();
	modem_test_init(&modems[0], getenv("MODEM_NAME") ? getenv("MODEM_NAME") : "modem", 3, 4);
	printf("created %s: %s\n", modems[0].modem->name, modems[0].modem->pty_name);
	fflush(stdout);
	signal(SIGINT,  mark_termination);
	signal(SIGTERM, mark_termination);
	ret = modem_test_run(modems);
	modem_test_free(&modems[0]);
	prop_dp_exit();
	return ret;
}
