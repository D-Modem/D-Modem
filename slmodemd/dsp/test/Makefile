# Back to back tests of the open data pumps:  make -C dsp/test check
#
# dsp_loopback <dp id> [snr dB] [seconds] [srate] [line gain] [clock ppm]

CC ?= gcc
CFLAGS += -Wall -O2 -g -I.. -I../..
SRCS := ../dsp_glue.c ../dsp_util.c ../dp_callprog.c ../dp_fsk.c ../dp_v22.c \
	../dp_v32.c ../qam.c ../qam_tables.c ../fax_modems.c ../fax_class1.c \
	../dp_v34.c ../v34_tables.c ../v34_modem.c ../v34_data.c ../v90.c

dsp_loopback: dsp_loopback.c $(SRCS) ../dsp.h ../qam.h ../v34.h ../v90.h
	$(CC) $(CFLAGS) -o $@ dsp_loopback.c $(SRCS) -lm -lpthread

check: dsp_loopback fax_loopback
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
	./dsp_loopback 8 40 22 9600 0.5 0 -10 -15
	FAR_DELAY=1920 ./dsp_loopback 8 40 22 9600 0.5 0 0 -15
	./dsp_loopback 8 40 22 8000 0.5 0 -10 -15
	./dsp_loopback 22 15 20
	./dsp_loopback 212 15 20
	./dsp_loopback 21 8 20
	./dsp_loopback 23 12 30
	./dsp_loopback 103 8 20 9600 0.5 300
	for m in 24 48 72 73 74 96 97 98 121 122 145 146; do \
		./fax_loopback $$m 30 || exit 1; done
	./fax_loopback 48 20
	./fax_loopback 96 25
	./fax_loopback 98 25 20000 8000

clean:
	rm -f dsp_loopback fax_loopback half_blob half_open modemA.log modemB.log

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
	$(CC) $(HALF_CFLAGS) -o $@ modem_half.c $(CORE) $(SRCS) -lm -lpthread

netsim: netsim.c ../dsp_util.c ../dsp.h
	$(CC) $(CFLAGS) -o $@ netsim.c ../dsp_util.c -lm

# V.90: the original library (analogue modem) calls the open one
# (digital modem, 8000 Hz) through the simulated G.711 network
v90_interop: half_blob half_open8 netsim
	NET=9600:8000:ulaw ./interop.py ./half_blob ./half_open8 "ATX3;AT+MS=90"
	DSP_G711=alaw NET=9600:8000:alaw ./interop.py ./half_blob ./half_open8 "ATX3;AT+MS=90"

# the same at 8000 Hz (the G.711 network's rate)
half_blob8: modem_half.c $(CORE) ../../dsplibs.o
	$(CC) $(HALF_CFLAGS) -DMODEM_RATE=8000 -o $@ modem_half.c $(CORE) ../../dsplibs.o

half_open8: modem_half.c $(CORE) $(SRCS)
	$(CC) $(HALF_CFLAGS) -DMODEM_RATE=8000 -o $@ modem_half.c $(CORE) $(SRCS) -lm -lpthread

interop: half_blob half_open
	for ms in 34 132 122 22 212 103 23; do \
		./interop.py ./half_blob ./half_open "ATX3;AT+MS=$$ms" || exit 1; \
		./interop.py ./half_open ./half_blob "ATX3;AT+MS=$$ms" || exit 1; \
	done

.PHONY: interop

# libdmodem (../../dmodem.h): pairs of its modems back to back in one
# process, several pairs in threads at once
../../libdmodem.a: FORCE
	$(MAKE) -C ../.. libdmodem.a
dmodem_pair: dmodem_pair.c ../../libdmodem.a ../../dmodem.h
	$(CC) $(CFLAGS) -o $@ dmodem_pair.c ../../libdmodem.a -lm -lpthread
libcheck: dmodem_pair
	for ms in 22 122 32 132 34; do ./dmodem_pair +MS=$$ms 3 30 || exit 1; done
	# a V.32bis retrain in the middle of the data (V.42 carries on)
	DSP_V32_RETRAIN=1500 ./dmodem_pair +MS=132 1 30

.PHONY: libcheck FORCE

# fax: the page coding against libtiff (tiffcp), two T.30 machines back
# to back at each rate, and libdmodem's fax with the CNG hand-over
FAX_T30 := ../t30.c ../t4.c ../tiff.c ../fax_modems.c ../dp_fsk.c ../qam.c \
	../qam_tables.c ../dsp_util.c
FAXTMP ?= /tmp/dmodem-faxcheck

t4_test: t4_test.c ../t4.c ../tiff.c ../t4.h
	$(CC) $(CFLAGS) -o $@ t4_test.c ../t4.c ../tiff.c -lpthread
fax_pair: fax_pair.c $(FAX_T30) ../t30.h ../t4.h ../fax.h
	$(CC) $(CFLAGS) -o $@ fax_pair.c $(FAX_T30) -lm -lpthread
dmodem_fax_test: dmodem_fax_test.c ../../libdmodem.a ../../dmodem.h
	$(CC) $(CFLAGS) -o $@ dmodem_fax_test.c ../../libdmodem.a -lm -lpthread

faxcheck: t4_test fax_pair dmodem_fax_test
	mkdir -p $(FAXTMP)
	./t4_test write $(FAXTMP)/doc.tif
	./t4_test light $(FAXTMP)/light.tif
	for c in none g3:1d g3:2d g4; do \
		tiffcp -c $$c $(FAXTMP)/doc.tif $(FAXTMP)/lib.tif && \
		./t4_test compare $(FAXTMP)/doc.tif $(FAXTMP)/lib.tif || exit 1; done
	for r in 14400 12000 9600 7200 4800 2400; do \
		./fax_pair $(FAXTMP)/light.tif $(FAXTMP)/rx.tif $$r && \
		./t4_test compare $(FAXTMP)/light.tif $(FAXTMP)/rx.tif || exit 1; done
	for law in ulaw alaw; do \
		./dmodem_fax_test $(FAXTMP)/light.tif $(FAXTMP)/rx.tif $$law && \
		./t4_test compare $(FAXTMP)/light.tif $(FAXTMP)/rx.tif || exit 1; done

.PHONY: faxcheck

FAX_SRCS := ../fax_modems.c ../qam.c ../qam_tables.c ../dsp_util.c ../dp_fsk.c

fax_loopback: fax_loopback.c $(FAX_SRCS) ../dsp.h ../qam.h ../fax.h
	$(CC) $(CFLAGS) -o $@ fax_loopback.c $(FAX_SRCS) -lm

# class 1 fax calls between the original library and this one, in real
# time (about 25 s each); see fax_interop.py
FAX_MODS ?= 24 48 72 96 73 97
fax_interop: half_blob half_open
	for m in $(FAX_MODS); do \
		./fax_interop.py ./half_blob ./half_open $$m || exit 1; \
		./fax_interop.py ./half_open ./half_blob $$m || exit 1; \
	done

.PHONY: fax_interop
