###########################################################################
#
#
#       Makefile  --  modem application Makefile.
#
#       Copyright(c) 2003, Smart Link Ltd. (www.smlink.com)
#	All rights reserved.
#
#       Author: Sasha K (sashak@smlink.com)
#
#
###########################################################################
#
###########################################################################

# DSPLIBS=blob (default) links the original binary-only dsplibs.o (i386 only),
# DSPLIBS=open builds the C re-implementation in dsp/ (any architecture,
# low speed modulations only, see dsp/README.md)
DSPLIBS ?= blob

ARCH64:=$(shell uname -m | sed -e '/amd64/s//x86_64/' | grep -c x86_64)

ifeq ($(DSPLIBS), open)
CC?= gcc
else ifeq ($(ARCH64), 0)
CC:= gcc
else
# SUPPORT_ALSA:=1
CC:= gcc -m32
endif

RM:= rm -f

CFLAGS+= -Wall -g -O -I. -DCONFIG_DEBUG_MODEM

modem-objs:= \
	modem.o modem_datafile.o modem_at.o modem_timer.o \
	modem_pack.o modem_ec.o modem_comp.o \
	modem_param.o modem_debug.o homolog_data.o
dp-objs:= dp_sinus.o dp_dummy.o
sysdep-objs:= sysdep_common.o
dsp-objs-open:= dsp/dsp_glue.o dsp/dsp_util.o dsp/dp_callprog.o dsp/dp_fsk.o dsp/dp_v22.o \
	dsp/dp_v32.o dsp/qam.o dsp/qam_tables.o dsp/fax_modems.o dsp/fax_class1.o \
	dsp/dp_v34.o dsp/v34_tables.o dsp/v34_modem.o dsp/v34_data.o \
	dsp/v90.o
ifeq ($(DSPLIBS), open)
dsp-objs:= dsp/dsp_glue.o dsp/dsp_util.o dsp/dp_callprog.o dsp/dp_fsk.o dsp/dp_v22.o \
	dsp/dp_v32.o dsp/qam.o dsp/qam_tables.o dsp/fax_modems.o dsp/fax_class1.o \
	dsp/dp_v34.o dsp/v34_tables.o dsp/v34_modem.o dsp/v34_data.o \
	dsp/v90.o
LFLAGS+= -lm -lpthread
else
dsp-objs:= dsplibs.o
endif
all-objs:= modem_cmdline.o $(modem-objs) $(dp-objs) $(dsp-objs) $(sysdep-objs) 

all: slmodemd modem_test

slmodemd: modem_main.o $(all-objs)
modem_test: modem_test.o $(all-objs)

ifdef SUPPORT_ALSA
CFLAGS+= -DSUPPORT_ALSA=1
ifeq ($(ARCH64), 0)
LFLAGS+= /usr/lib/libasound.so
else
# LFLAGS+= /usr/lib32/libasound.so , replaced by -l commands search for asound
LFLAGS+= -lasound
endif
endif

slmodemd:
	$(CC) -o slmodemd modem_main.o $(all-objs) $(LFLAGS)

modem_test:
	$(CC) -o modem_test modem_test.o $(all-objs) $(LFLAGS)

# libdmodem.a: the modem core and the open data pumps at 8000 Hz as a
# library (dmodem.h), for programs that bring their own audio
LIB_SRCS:= modem.c modem_datafile.c modem_at.c modem_pack.c modem_ec.c \
	modem_comp.c modem_param.c modem_debug.c homolog_data.c \
	dp_sinus.c dp_dummy.c sysdep_common.c dmodem.c dmodem_timer.c \
	dmodem_fax.c dsp/t30.c dsp/t4.c dsp/tiff.c \
	$(patsubst %.o,%.c,$(filter dsp/%,$(subst dsplibs.o,,$(dsp-objs-open))))
LIB_OBJS:= $(patsubst %.c,lib/%.o,$(LIB_SRCS))
# the library has no binary-only parts: native, whatever CC the blob needs
LIB_CC ?= gcc
LIB_CFLAGS:= -Wall -g -O2 -fPIC -I. -DCONFIG_DEBUG_MODEM -DMODEM_RATE=8000

# -MMD: the objects follow the headers (struct layouts shared with
# app_dmodem must not go stale)
lib/%.o: %.c
	@mkdir -p $(dir $@)
	$(LIB_CC) $(LIB_CFLAGS) -MMD -MP -c $< -o $@

-include $(LIB_OBJS:.o=.d)

libdmodem.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

clean:
	$(RM) -r lib libdmodem.a
	$(RM) slmodemd modem_test modem_main.o modem_cmdline.o modem_test.o $(modem-objs) $(dp-objs) $(sysdep-objs)
	$(RM) dsp/*.o
	$(RM) *~ *.orig *.rej

.PHONY: all dep generic-dep clean clean-build-profile


#
# rules
#

%.o: %.c .build_profile
	$(CC) $(CFLAGS) $(CPPFLAGS) $(EXTRA_CFLAGS) $(CFLAGS_$@) -o $@ -c $<

dep: generic-dep
generic-dep:
	$(RM) .depend
	$(foreach src,$(wildcard *.c),$(CC) -M $(CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS_$(src:.c=.o)) -c $(src) >> .depend ;)

-include .depend

# build_profile
-include .build_profile

.build_profile::
ifneq ($(CFLAGS),$(PROFILE_CFLAGS))
	@if [ "$(CFLAGS) $(EXTRA_CFLAGS)" != "$(PROFILE_CFLAGS)" ] ; then \
	  echo "rebuild profile..." ; \
	  echo "PROFILE_CFLAGS=$(CFLAGS) $(EXTRA_CFLAGS)" > $(@) ; fi
endif

clean: clean-build-profile
clean-build-profile:
	@$(RM) -f .build_profile

