# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Neo-Desktop
# The tests known to fail now and then (each an open intermittent, most
# of them under a loaded machine): ctest runs them last, after every
# other test it runs, and one at a time, with nothing else beside them.
# They keep their own labels and have the label flaky as well
# (ctest -L flaky runs only them, -LE flaky all but them).
#
# How (test_logs.cmake, gtest.cmake): the other tests require the
# fixture dmodem_steady, whose cleanup test dmodem_flaky_gate runs once
# they have all ended; these depend on that test instead (DEPENDS) and
# run serially (RUN_SERIAL).  Run on their own (ctest -R ...), there is
# no gate to wait for.  A gtest program's tests get this from an include
# file after its discovery (dmodem_gtest), the others when added
# (dmodem_add_test).
#
# Each by its ctest name: <suite>.<test> for a gtest program's.

set(DMODEM_FLAKY_TESTS
	# #188: the Agere modem's V.90 renegotiation
	agere.agere_v90_reneg_open
	# #197: A's AT+FRH=3 gets an empty frame
	broadcom.broadcom_fax_97_broadcom_sends
	# #237
	open.open_v90a_alaw
	dmodemd_alsa
	# #241: the +++ escape sent as data
	smartlink.smartlink_v92_moh_request
	# #257: FB1u with tens of zeros
	open.open_v92a_fpe_precoder
	# #259: ESS's V.29 receiver
	ess.ess_fax_96_open_sends
	# #264: retrain, no tone B
	open.open_v90a_retrain
	# #296: V.34 stalls in phase 3, or the woken call does not connect
	dmodemd_usb
	# #297
	open.open_vcvm_amrwb2385
	# #298: the +++ escape, as #241
	ess.ess_v92_moh_request
)
