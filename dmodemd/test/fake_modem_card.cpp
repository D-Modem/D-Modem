/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    fake_modem_card.cpp - an ALSA modem card for the tests: alsa-lib's
 *                          external PCM (ioplug) and control (ctl_ext)
 *                          plugins "dmline", a softmodem codec whose
 *                          telephone line is line_exchange's
 *
 *    In an ALSA configuration (ALSA_CONFIG_PATH):
 *
 *      pcm_type.dmline { lib "<dir>/libasound_module_dmline.so" }
 *      ctl_type.dmline { lib "<dir>/libasound_module_dmline.so" }
 *      pcm.linea { type dmline socket "<exchange's socket of the line>" }
 *      ctl.linea { type dmline socket "<the same>" }
 *
 *    The PCM is S16 mono at 8000, 9600 or 16000 Hz, read and write
 *    access.  Each way (and the control) is a connection to the
 *    exchange's socket, its first octet saying which: 'C' capture (the
 *    exchange sends the line's samples, 20 ms at a time, in its time),
 *    'P' playback (the modem's samples, sent as they are written; the
 *    playback's position runs on the clock from its start, as a DMA
 *    would), 'K' the control.  The control has the two switches of the
 *    kernel's modem codecs, "Off-hook Switch" and "Caller ID Switch";
 *    a change is two octets to the exchange: the switch (0 the hook,
 *    1 caller ID) and its value.
 *
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <deque>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <alsa/pcm_external.h>

namespace {

/* a connection to the exchange at path, its first octet kind; -1 */
int connect_exchange(const char *path, char kind)
{
	struct sockaddr_un a = {};
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
	if (connect(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0 ||
	    write(fd, &kind, 1) != 1) {
		close(fd);
		return -1;
	}
	return fd;
}

/* the configuration's socket (its other fields: type, comment) */
int socket_of(snd_config_t *conf, const char **path)
{
	snd_config_iterator_t i, next;

	*path = nullptr;
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") ||
		    !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "socket")) {
			if (snd_config_get_string(n, path) < 0)
				return -EINVAL;
			continue;
		}
		SNDERR("dmline: unknown field %s", id);
		return -EINVAL;
	}
	if (!*path) {
		SNDERR("dmline: no socket");
		return -EINVAL;
	}
	return 0;
}

/*
 *    the PCM
 */

struct Pcm {
	snd_pcm_ioplug_t io = {};
	int fd = -1;
	/* capture: the samples come in, kept until read; how many since
	   prepare; an octet of a sample split between reads */
	std::deque<int16_t> fifo;
	unsigned long long received = 0;
	int half = -1;
	/* playback: started then, the samples played before */
	struct timespec started = {};
	bool running = false;
	unsigned long long base = 0;
};

/* the PCM of io */
Pcm *pcm_of(snd_pcm_ioplug_t *io)
{
	return static_cast<Pcm *>(io->private_data);
}

/* the samples the exchange has sent (none kept beyond the buffer):
   false if the buffer is full with more waiting (an overrun) */
bool take(Pcm *p)
{
	unsigned char b[4096];

	while (p->fifo.size() < p->io.buffer_size) {
		size_t want = (p->io.buffer_size - p->fifo.size()) * 2;
		ssize_t n = recv(p->fd, b, want < sizeof(b) ? want : sizeof(b),
				 MSG_DONTWAIT);
		if (n <= 0)
			return true;
		for (ssize_t i = 0; i < n; i++) {
			if (p->half < 0) {
				p->half = b[i];
				continue;
			}
			p->fifo.push_back((int16_t)(p->half | b[i] << 8));
			p->received++;
			p->half = -1;
		}
	}
	unsigned char c;
	return recv(p->fd, &c, 1, MSG_DONTWAIT | MSG_PEEK) <= 0;
}

/* what is waiting from before discarded */
void discard(Pcm *p)
{
	unsigned char b[4096];

	while (recv(p->fd, b, sizeof(b), MSG_DONTWAIT) > 0)
		;
	p->fifo.clear();
	p->half = -1;
}

/* the playback's position: played since prepare */
unsigned long long played(Pcm *p)
{
	struct timespec now;

	if (!p->running)
		return p->base;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double s = (double)(now.tv_sec - p->started.tv_sec) +
		(now.tv_nsec - p->started.tv_nsec) / 1e9;
	return p->base + (unsigned long long)(s * p->io.rate);
}

/* ioplug's start: the clock running from now (capture:
   what waited discarded) */
int pcm_start(snd_pcm_ioplug_t *io)
{
	Pcm *p = pcm_of(io);

	if (io->stream == SND_PCM_STREAM_CAPTURE)
		discard(p);
	clock_gettime(CLOCK_MONOTONIC, &p->started);
	p->running = true;
	return 0;
}

/* ioplug's stop: the playback's position kept, the clock
   stopped */
int pcm_stop(snd_pcm_ioplug_t *io)
{
	Pcm *p = pcm_of(io);

	p->base = played(p);
	p->running = false;
	return 0;
}

/* ioplug's prepare: stopped, the positions 0 (capture:
   what waited discarded) */
int pcm_prepare(snd_pcm_ioplug_t *io)
{
	Pcm *p = pcm_of(io);

	p->running = false;
	p->base = 0;
	p->received = 0;
	if (io->stream == SND_PCM_STREAM_CAPTURE)
		discard(p);
	return 0;
}

/* ioplug's pointer: the position in the buffer (playback
   the clock's, capture the samples come); -EPIPE on an
   overrun */
snd_pcm_sframes_t pcm_pointer(snd_pcm_ioplug_t *io)
{
	Pcm *p = pcm_of(io);

	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		return (snd_pcm_sframes_t)(played(p) % io->buffer_size);
	if (p->running && !take(p))
		return -EPIPE;
	return (snd_pcm_sframes_t)(p->received % io->buffer_size);
}

/* ioplug's transfer: size frames at offset of areas,
   captured from the FIFO (how many it had) or sent to the
   exchange (all of them; -EIO if it can't) */
snd_pcm_sframes_t pcm_transfer(snd_pcm_ioplug_t *io,
			       const snd_pcm_channel_area_t *areas,
			       snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
	Pcm *p = pcm_of(io);
	int16_t *buf = reinterpret_cast<int16_t *>(
		static_cast<char *>(areas[0].addr) +
		(areas[0].first + areas[0].step * offset) / 8);

	if (io->stream == SND_PCM_STREAM_CAPTURE) {
		snd_pcm_uframes_t i;
		for (i = 0; i < size && !p->fifo.empty(); i++) {
			buf[i] = p->fifo.front();
			p->fifo.pop_front();
		}
		return (snd_pcm_sframes_t)i;
	}
	const char *b = reinterpret_cast<const char *>(buf);
	size_t left = size * 2;
	while (left) {
		ssize_t n = write(p->fd, b, left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -EIO;
		b += n;
		left -= (size_t)n;
	}
	return (snd_pcm_sframes_t)size;
}

/* ioplug's close: the connection closed, the PCM freed */
int pcm_close(snd_pcm_ioplug_t *io)
{
	Pcm *p = pcm_of(io);

	close(p->fd);
	delete p;
	return 0;
}

const snd_pcm_ioplug_callback_t PCM_CALLBACKS = [] {
	snd_pcm_ioplug_callback_t c = {};
	c.start = pcm_start;
	c.stop = pcm_stop;
	c.pointer = pcm_pointer;
	c.transfer = pcm_transfer;
	c.close = pcm_close;
	c.prepare = pcm_prepare;
	return c;
}();

/*
 *    the control
 */

const char *const SWITCHES[2] = { "Off-hook Switch", "Caller ID Switch" };

struct Ctl {
	snd_ctl_ext_t ext = {};
	int fd = -1;
	long value[2] = {};
};

/* the control of ext */
Ctl *ctl_of(snd_ctl_ext_t *ext)
{
	return static_cast<Ctl *>(ext->private_data);
}

/* ctl_ext's elem_count: the two switches */
int ctl_count(snd_ctl_ext_t *)
{
	return 2;
}

/* ctl_ext's elem_list: switch offset's id (a mixer
   element); -EINVAL past the two */
int ctl_list(snd_ctl_ext_t *, unsigned offset, snd_ctl_elem_id_t *id)
{
	if (offset >= 2)
		return -EINVAL;
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, SWITCHES[offset]);
	return 0;
}

/* ctl_ext's find_elem: the switch of id's numid or name */
snd_ctl_ext_key_t ctl_find(snd_ctl_ext_t *, const snd_ctl_elem_id_t *id)
{
	const char *name = snd_ctl_elem_id_get_name(id);
	unsigned numid = snd_ctl_elem_id_get_numid(id);

	if (numid >= 1 && numid <= 2)
		return numid - 1;
	for (snd_ctl_ext_key_t k = 0; k < 2; k++)
		if (!strcmp(name, SWITCHES[k]))
			return k;
	return SND_CTL_EXT_KEY_NOT_FOUND;
}

/* ctl_ext's get_attribute: a boolean, read and write,
   one value */
int ctl_attribute(snd_ctl_ext_t *, snd_ctl_ext_key_t, int *type,
		  unsigned *acc, unsigned *count)
{
	*type = SND_CTL_ELEM_TYPE_BOOLEAN;
	*acc = SND_CTL_EXT_ACCESS_READWRITE;
	*count = 1;
	return 0;
}

/* ctl_ext's get_integer_info: 0 to 1, by 1 */
int ctl_info(snd_ctl_ext_t *, snd_ctl_ext_key_t, long *min, long *max,
	     long *step)
{
	*min = 0;
	*max = 1;
	*step = 1;
	return 0;
}

/* ctl_ext's read_integer: the switch's value */
int ctl_read(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	*value = ctl_of(ext)->value[key];
	return 0;
}

/* ctl_ext's write_integer: the switch set, a change told
   to the exchange (1; 0 if none, -EIO if it can't) */
int ctl_write(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
	Ctl *c = ctl_of(ext);
	unsigned char m[2] = { (unsigned char)key, (unsigned char)!!*value };

	if (c->value[key] == !!*value)
		return 0;
	c->value[key] = !!*value;
	if (write(c->fd, m, 2) != 2)
		return -EIO;
	return 1;
}

/* ctl_ext's close: the connection closed, the control
   freed */
void ctl_close(snd_ctl_ext_t *ext)
{
	Ctl *c = ctl_of(ext);

	close(c->fd);
	delete c;
}

const snd_ctl_ext_callback_t CTL_CALLBACKS = [] {
	snd_ctl_ext_callback_t c = {};
	c.close = ctl_close;
	c.elem_count = ctl_count;
	c.elem_list = ctl_list;
	c.find_elem = ctl_find;
	c.get_attribute = ctl_attribute;
	c.get_integer_info = ctl_info;
	c.read_integer = ctl_read;
	c.write_integer = ctl_write;
	return c;
}();

} // namespace

extern "C" {

/* the PCM plugin "dmline": stream's way to the
   exchange at its socket, S16 mono at 8000, 9600 or
   16000 Hz */
SND_PCM_PLUGIN_DEFINE_FUNC(dmline)
{
	static const unsigned rates[] = { 8000, 9600, 16000 };
	const char *path;
	int err;

	(void)root;
	if ((err = socket_of(conf, &path)) < 0)
		return err;
	Pcm *p = new Pcm();
	p->fd = connect_exchange(path, stream == SND_PCM_STREAM_CAPTURE ?
				 'C' : 'P');
	if (p->fd < 0) {
		SNDERR("dmline: no exchange at %s", path);
		delete p;
		return -ENODEV;
	}
	p->io.version = SND_PCM_IOPLUG_VERSION;
	p->io.name = "D-Modem test line";
	p->io.callback = &PCM_CALLBACKS;
	p->io.private_data = p;
	p->io.poll_fd = p->fd;
	p->io.poll_events = stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;
	p->io.mmap_rw = 0;
	err = snd_pcm_ioplug_create(&p->io, name, stream, mode);
	if (err < 0) {
		close(p->fd);
		delete p;
		return err;
	}
	unsigned access = SND_PCM_ACCESS_RW_INTERLEAVED;
	unsigned format = SND_PCM_FORMAT_S16;
	if ((err = snd_pcm_ioplug_set_param_list(&p->io,
			SND_PCM_IOPLUG_HW_ACCESS, 1, &access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&p->io,
			SND_PCM_IOPLUG_HW_FORMAT, 1, &format)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io,
			SND_PCM_IOPLUG_HW_CHANNELS, 1, 1)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&p->io,
			SND_PCM_IOPLUG_HW_RATE, 3, rates)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io,
			SND_PCM_IOPLUG_HW_PERIOD_BYTES, 64, 64 * 1024)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&p->io,
			SND_PCM_IOPLUG_HW_PERIODS, 2, 64)) < 0) {
		snd_pcm_ioplug_delete(&p->io);
		return err;
	}
	*pcmp = p->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(dmline);

/* the control plugin "dmline": the two switches, their
   changes to the exchange at its socket */
SND_CTL_PLUGIN_DEFINE_FUNC(dmline)
{
	const char *path;
	int err;

	(void)root;
	if ((err = socket_of(conf, &path)) < 0)
		return err;
	Ctl *c = new Ctl();
	c->fd = connect_exchange(path, 'K');
	if (c->fd < 0) {
		SNDERR("dmline: no exchange at %s", path);
		delete c;
		return -ENODEV;
	}
	c->ext.version = SND_CTL_EXT_VERSION;
	c->ext.card_idx = 0;
	strcpy(c->ext.id, "DMLine");
	strcpy(c->ext.driver, "dmline");
	strcpy(c->ext.name, "D-Modem test line");
	strcpy(c->ext.longname, "D-Modem test line (line_exchange)");
	strcpy(c->ext.mixername, "dmline");
	c->ext.poll_fd = -1;
	c->ext.callback = &CTL_CALLBACKS;
	c->ext.private_data = c;
	err = snd_ctl_ext_create(&c->ext, name, mode);
	if (err < 0) {
		close(c->fd);
		delete c;
		return err;
	}
	*handlep = c->ext.handle;
	return 0;
}

SND_CTL_PLUGIN_SYMBOL(dmline);

}
