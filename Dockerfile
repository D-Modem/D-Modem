# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
#
# D-Modem on Alpine Linux (musl), in two stages:
#
#   build    the whole tree with CMake and Ninja (the packages of CI's
#            musl job); libre, dmodemd's SIP stack, is cloned and built
#            while configuring.  Installed into /out.
#   runtime  dmodemd and hda-modem-setup, dmodem-ari, libdmodem.a and its
#            headers, on a plain Alpine with the libraries they link.
#
#   docker build -t dmodem .
#   docker run --rm -it --network host \
#       -e SIP_LOGIN=user:password@domain dmodem
#
# Build arguments:
#   ALPINE_VERSION  the Alpine release (default 3.22, CI's)
#   TESTS=ON        also build the test programs and run the offline
#                   tests (ctest -L "quick|spandsp|alsa"), as CI does
#   JOBS            parallel build jobs (default: all CPUs)
#
# `docker build --target build .` stops at the build stage: its tree is
# /src/build.  The Asterisk and FreeSWITCH modules are not built (Alpine
# has neither's development files in its main repository).

ARG ALPINE_VERSION=3.22

FROM alpine:${ALPINE_VERSION} AS build

# (git: libre is cloned while configuring; python3, bash, coreutils,
# spandsp and the TIFF tools: the tests)
RUN apk add --no-cache \
	build-base cmake samurai git pkgconf linux-headers \
	openssl-dev alsa-lib-dev libusb-dev \
	gtest-dev spandsp-dev tiff-dev tiff-tools python3 bash coreutils

WORKDIR /src
COPY . .

ARG TESTS=OFF
ARG JOBS=
RUN cmake -B build -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DDMODEM_TESTS=${TESTS} \
	&& cmake --build build ${JOBS:+-j ${JOBS}} \
	&& if [ "${TESTS}" = ON ]; then \
		ctest --test-dir build -L "quick|spandsp|alsa" \
			${JOBS:+-j ${JOBS}} --output-on-failure; \
	fi \
	&& DESTDIR=/out cmake --install build

FROM alpine:${ALPINE_VERSION} AS runtime

# dmodemd's libraries (libre is linked in statically); the dialout
# group, its pty's (Alpine has it)
RUN apk add --no-cache libstdc++ libgcc libssl3 libcrypto3 alsa-lib libusb \
	&& mkdir -p /var/lib/dmodem

COPY --from=build /out/ /

# SIP on 5060 (SIP_PORT); each call's RTP and UDPTL take ports the
# system picks, so run it with --network host for calls' media
EXPOSE 5060/udp

ENTRYPOINT ["/usr/sbin/dmodemd"]
CMD ["--help"]
