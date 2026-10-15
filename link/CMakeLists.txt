# The data link layer (C++, namespace dlink): the V.42 detection phase,
# V.42 LAPM over HDLC, MNP 2 to 4 and 5, V.42bis, V.44, and asynchronous
# characters.  It does not depend on the modem core; the core is its
# Owner (link.h).
#
#   dmodem_link        object library
#   DMODEM_LINK_SRCS   the same as a source list (32 bit interop tests)
#   DMODEM_LINK_DIR

set(DMODEM_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_link hdlc.cpp lapm.cpp mnp.cpp mnp5.cpp v42bis.cpp v44.cpp link.cpp)
list(TRANSFORM _link PREPEND ${DMODEM_LINK_DIR}/)
add_library(dmodem_link OBJECT ${_link})
target_include_directories(dmodem_link PUBLIC ${DMODEM_LINK_DIR})
target_compile_options(dmodem_link PRIVATE -Wall -Wextra -g -O2)
set_target_properties(dmodem_link PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(DMODEM_LINK_DIR ${DMODEM_LINK_DIR} PARENT_SCOPE)
set(DMODEM_LINK_SRCS ${_link} PARENT_SCOPE)

if(DMODEM_TESTS)
	add_executable(link_test test/link_test.cpp)
	target_link_libraries(link_test PRIVATE dmodem_link)
	target_compile_options(link_test PRIVATE -Wall -Wextra -O2 -g)
	dmodem_gtest(link_test LABELS quick)
endif()
