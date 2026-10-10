# The open data pumps (C++): the modulations, V.8, V.34, V.90, V.91, class 1
# fax, and the slmodemd pumps without a modulation (dp_dummy, dp_sinus).
# They plug into the modem core of slmodemd behind the C interface of Smart
# Link's original library (../slmodemd/dsplibs.h) and the core's modem.h,
# so they are compiled with each program that links them (the core's
# MODEM_RATE differs between them): these lists name the sources.
#
#   DMODEM_DSP_SRCS   the data pumps and the class 1 fax
#   DMODEM_T30_SRCS   the T.30 fax machine and the T.4/T.6 and TIFF code

set(DMODEM_DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_dsp
	dsp_glue.cpp dsp_util.cpp dp_callprog.cpp dp_fsk.cpp dp_v22.cpp
	dp_v32.cpp qam.cpp qam_tables.cpp fax_modems.cpp fax_class1.cpp
	dp_v34.cpp v34_tables.cpp v34_modem.cpp v34_data.cpp v90.cpp v91.cpp
	dp_dummy.cpp dp_sinus.cpp)
list(TRANSFORM _dsp PREPEND ${DMODEM_DSP_DIR}/)
set(_t30 t30.cpp t4.cpp tiff.cpp)
list(TRANSFORM _t30 PREPEND ${DMODEM_DSP_DIR}/)

set(DMODEM_DSP_DIR ${DMODEM_DSP_DIR} PARENT_SCOPE)
set(DMODEM_DSP_SRCS ${_dsp} PARENT_SCOPE)
set(DMODEM_T30_SRCS ${_t30} PARENT_SCOPE)
