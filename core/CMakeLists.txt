# The modem core (C++, namespace dcore): the AT command interpreter, the
# S-registers, the result codes, the call's state machine and its timers
# (dcore.h).  It runs the data pumps of ../dsp through their interface
# (interface.cpp) and the data link layer of ../link.
#
#   dmodem_core        object library
#   DMODEM_CORE_NEW_DIR

set(DMODEM_CORE_NEW_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_core modem.cpp at.cpp interface.cpp log.cpp homolog_data.cpp)
list(TRANSFORM _core PREPEND ${DMODEM_CORE_NEW_DIR}/)
add_library(dmodem_core OBJECT ${_core})
target_include_directories(dmodem_core PUBLIC ${DMODEM_CORE_NEW_DIR}
	${DMODEM_DSP_DIR} ${DMODEM_LINK_DIR})
target_compile_options(dmodem_core PRIVATE -Wall -Wextra -g -O2)
set_target_properties(dmodem_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(DMODEM_CORE_NEW_DIR ${DMODEM_CORE_NEW_DIR} PARENT_SCOPE)
