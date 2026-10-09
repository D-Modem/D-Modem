# app_dmodem.so: the DModem() dialplan application, built against the
# headers of an installed Asterisk (make install-headers, or the
# asterisk-dev package of a distribution)
#
#   make                          build (libdmodem.a too)
#   make install                  into the modules directory
#   make ASTINC=/path/include     headers elsewhere than /usr/include

ASTINC ?= /usr/include
MODULES_DIR ?= $(shell pkg-config --variable=modulesdir asterisk 2>/dev/null || echo /usr/lib/asterisk/modules)
CONF_DIR ?= /etc/asterisk

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -fPIC -pthread -D_GNU_SOURCE -I$(ASTINC) -I../slmodemd \
	-DAST_MODULE=\"app_dmodem\" -DAST_MODULE_SELF_SYM=__internal_app_dmodem_self

LIBDMODEM := ../slmodemd/libdmodem.a

all: app_dmodem.so

$(LIBDMODEM): FORCE
	$(MAKE) -C ../slmodemd libdmodem.a LIB_CC="$(CC)"

# the library's symbols stay inside the module
app_dmodem.so: app_dmodem.c $(LIBDMODEM)
	$(CC) $(CFLAGS) -shared -o $@ app_dmodem.c $(LIBDMODEM) \
		-Wl,--exclude-libs,ALL -lm

install: app_dmodem.so
	install -d $(DESTDIR)$(MODULES_DIR)
	install -m 755 app_dmodem.so $(DESTDIR)$(MODULES_DIR)/
	test -e $(DESTDIR)$(CONF_DIR)/dmodem.conf || \
		install -D -m 644 dmodem.conf.sample $(DESTDIR)$(CONF_DIR)/dmodem.conf

clean:
	rm -f app_dmodem.so

.PHONY: all install clean FORCE
