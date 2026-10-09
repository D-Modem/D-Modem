# Back to back tests of the open data pumps:  make -C dsp/test check
#
# dsp_loopback <dp id> [snr dB] [seconds] [srate] [line gain] [clock ppm]

CC ?= gcc
CFLAGS += -Wall -O2 -g -I.. -I../..
SRCS := ../dsp_glue.c ../dsp_util.c ../dp_callprog.c ../dp_fsk.c ../dp_v22.c \
	../dp_v32.c ../qam.c ../qam_tables.c

dsp_loopback: dsp_loopback.c $(SRCS) ../dsp.h ../qam.h
	$(CC) $(CFLAGS) -o $@ dsp_loopback.c $(SRCS) -lm

check: dsp_loopback
	./dsp_loopback 132 40 20
	./dsp_loopback 132 25 20
	./dsp_loopback 132 30 20 8000
	./dsp_loopback 132 30 30 9600 0.5 300
	./dsp_loopback 132 30 20 9600 0.5 0 -6
	./dsp_loopback 132 11 20
	./dsp_loopback 32 30 20
	CALLER_DP=122 ./dsp_loopback 8 30 25
	CALLER_DP=8 ./dsp_loopback 22 30 25
	./dsp_loopback 122 40 20
	./dsp_loopback 122 15 20
	./dsp_loopback 122 30 20 8000
	./dsp_loopback 122 30 30 9600 2 -300
	./dsp_loopback 122 30 30 9600 0.05 300
	./dsp_loopback 8 30 20
	./dsp_loopback 22 15 20
	./dsp_loopback 212 15 20
	./dsp_loopback 21 8 20
	./dsp_loopback 23 12 30
	./dsp_loopback 103 8 20 9600 0.5 300

clean:
	rm -f dsp_loopback half_blob half_open modemA.log modemB.log

.PHONY: check clean

# interoperability with the original binary library (needs gcc -m32):
#   make -C dsp/test interop
CORE := $(addprefix ../../,modem_cmdline.c modem.c modem_datafile.c \
	modem_at.c modem_timer.c modem_pack.c modem_ec.c modem_comp.c \
	modem_param.c modem_debug.c homolog_data.c dp_sinus.c dp_dummy.c \
	sysdep_common.c)
HALF_CFLAGS := -m32 -w -g -O -I../.. -DCONFIG_DEBUG_MODEM

half_blob: modem_half.c $(CORE) ../../dsplibs.o
	$(CC) $(HALF_CFLAGS) -o $@ modem_half.c $(CORE) ../../dsplibs.o

half_open: modem_half.c $(CORE) $(SRCS)
	$(CC) $(HALF_CFLAGS) -o $@ modem_half.c $(CORE) $(SRCS) -lm

interop: half_blob half_open
	for ms in 132 122 22 212 103 23; do \
		./interop.py ./half_blob ./half_open "ATX3;AT+MS=$$ms" || exit 1; \
		./interop.py ./half_open ./half_blob "ATX3;AT+MS=$$ms" || exit 1; \
	done

.PHONY: interop
