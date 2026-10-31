# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
# The open data pumps (C++): the modulations, V.8, V.34, V.90, V.91, K56flex's
# data mode, class 1
# fax, and the slmodemd pumps without a modulation (dp_dummy, dp_sinus).
# They plug into a modem core through the interface in this directory,
# the one Smart Link's original library has: modem_dp.h (registration,
# bits), modem_param.h (parameters), dsplibs.h (the rest) and
# modem_defs.h.  They do not include the core's own headers.
#
#   dmodem_dsp        object library: the data pumps and the class 1 fax
#   dmodem_t30        object library: the T.30 fax machine, T.4/T.6, TIFF,
#                     class 2 and 2.0 fax (fax_class2.cpp)
#   DMODEM_DSP_SRCS,  the same as source lists, for the 32 bit interop
#   DMODEM_T30_SRCS   test programs
#   DMODEM_DSP_DIR

set(DMODEM_DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_dsp
	dsp_glue.cpp dsp_util.cpp tones.cpp dp_callprog.cpp dp_fsk.cpp dp_text.cpp dp_contactid.cpp dp_sms.cpp sms_pdu.cpp dp_relay.cpp dp_v22.cpp
	dp_v32.cpp qam.cpp qam_tables.cpp fax_modems.cpp fax_class1.cpp
	dp_v34.cpp dp_vcvm.cpp dp_vwb.cpp dp_4wire.cpp dp_clear.cpp v110.cpp vwb.cpp v34_tables.cpp v34_modem.cpp v34_cc.cpp t30_v34.cpp v34_data.cpp v90.cpp v90a.cpp v91.cpp k56flex.cpp k56flex_tables.cpp k56flex_train.cpp v92.cpp v92a.cpp vcvm.cpp vcvm_msg.cpp vcvm_probe.cpp vcvm_startup.cpp vcvm_data.cpp vcvm_modem.cpp rs.cpp
	dp_dummy.cpp dp_sinus.cpp)
list(TRANSFORM _dsp PREPEND ${DMODEM_DSP_DIR}/)
set(_t30 t30.cpp t38.cpp t4.cpp tiff.cpp fax_class2.cpp)
list(TRANSFORM _t30 PREPEND ${DMODEM_DSP_DIR}/)

foreach(_lib dsp t30)
	add_library(dmodem_${_lib} OBJECT ${_${_lib}})
	target_include_directories(dmodem_${_lib} PUBLIC ${DMODEM_DSP_DIR})
	dmodem_flags(dmodem_${_lib})
	set_target_properties(dmodem_${_lib} PROPERTIES
		POSITION_INDEPENDENT_CODE ON)
endforeach()

set(DMODEM_DSP_DIR ${DMODEM_DSP_DIR} PARENT_SCOPE)
set(DMODEM_DSP_SRCS ${_dsp} PARENT_SCOPE)
set(DMODEM_T30_SRCS ${_t30} PARENT_SCOPE)
