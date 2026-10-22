/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Neo-Desktop
 *
 *    wire.h - the frames between D-Modem's virtual modem driver (its COM
 *             port, port.h) and dmodemd (--driver), on the named pipe
 *             the driver makes, \\.\pipe\dmodem-vm-<port>:
 *
 *        type (1 byte), length (2 bytes, little-endian), payload
 *
 *    HELLO   both ways, first: the version (2 bytes, WIRE_VERSION) and,
 *            from the driver, its COM port's name
 *    DATA    both ways: the app's characters to the modem, the modem's
 *            to the app (at most WIRE_MAX_DATA bytes a frame)
 *    LINES   dmodemd to the driver: the modem's control lines, 1 byte
 *            of LINE_* (DCD, RI, DSR, CTS)
 *    STATE   the driver to dmodemd: the app's side, 1 byte of STATE_*
 *            (the port open, DTR, RTS) and the baud rate (4 bytes)
 *
 *    Portable: the driver, dmodemd and the tests use the same code.
 *
 */
#ifndef DMODEM_WINMODEM_WIRE_H
#define DMODEM_WINMODEM_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace vmodem {

enum : uint16_t { WIRE_VERSION = 1 };
enum : size_t { WIRE_HEAD = 3, WIRE_MAX_DATA = 4096 };

enum FrameType : uint8_t {
	FRAME_HELLO = 'H', FRAME_DATA = 'D', FRAME_LINES = 'L',
	FRAME_STATE = 'S',
};

/* the modem's lines, as dmodemd gives them (LINES) */
enum : uint8_t { LINE_DCD = 1, LINE_RI = 2, LINE_DSR = 4, LINE_CTS = 8 };
/* the app's side, as the driver gives it (STATE) */
enum : uint8_t { STATE_OPEN = 1, STATE_DTR = 2, STATE_RTS = 4 };

/* a frame of type t with payload p (n bytes) appended to out */
void wire_put(std::string &out, FrameType t, const void *p, size_t n);
/* HELLO: our version and (the driver's) port name */
void wire_hello(std::string &out, const std::string &port);
/* LINES: lines (LINE_*) */
void wire_lines(std::string &out, uint8_t lines);
/* STATE: state (STATE_*) and the baud rate */
void wire_state(std::string &out, uint8_t state, uint32_t baud);
/* DATA: p (n bytes), in frames of WIRE_MAX_DATA */
void wire_data(std::string &out, const void *p, size_t n);

/* a stream of frames back into frames: feed() what was read, then
   next() each whole one */
class WireReader {
public:
	struct Frame {
		FrameType type;
		const uint8_t *p;       /* the payload, valid until feed() */
		size_t n;
	};
	/* bytes read appended */
	void feed(const void *p, size_t n);
	/* the next whole frame into f: false if none is there yet (or
	   after bad(), never again) */
	bool next(Frame &f);
	/* a frame that can't be (an unknown type, DATA too long): the
	   stream is lost */
	bool bad() const { return bad_; }
	/* bytes waiting */
	size_t pending() const { return buf_.size() - pos_; }

	/* a HELLO's version and port name: false if it is short */
	static bool hello(const Frame &f, uint16_t *version,
			  std::string *port);
	/* a LINES frame's lines, a STATE frame's state and baud rate:
	   false if short */
	static bool lines(const Frame &f, uint8_t *lines);
	static bool state(const Frame &f, uint8_t *state, uint32_t *baud);

private:
	std::string buf_;
	size_t pos_ = 0;
	bool bad_ = false;
};

} // namespace vmodem

#endif
