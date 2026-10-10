# The data link layer (C++, namespace dlink): the V.42 detection phase,
# V.42 LAPM over HDLC, V.42bis, and asynchronous characters.  It does not
# depend on the modem core; the core is its Owner (link.h).  Like the data
# pumps, the sources are compiled into each program that uses them (the
# interop test programs are 32 bit).
#
#   DMODEM_LINK_SRCS, DMODEM_LINK_DIR

set(DMODEM_LINK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(_link hdlc.cpp lapm.cpp v42bis.cpp link.cpp)
list(TRANSFORM _link PREPEND ${DMODEM_LINK_DIR}/)
set(DMODEM_LINK_DIR ${DMODEM_LINK_DIR} PARENT_SCOPE)
set(DMODEM_LINK_SRCS ${_link} PARENT_SCOPE)

if(DMODEM_TESTS)
	add_executable(link_test test/link_test.cpp ${_link})
	target_include_directories(link_test PRIVATE ${DMODEM_LINK_DIR})
	target_compile_options(link_test PRIVATE -Wall -Wextra -O2 -g)
	add_test(NAME link_test COMMAND link_test)
	set_tests_properties(link_test PROPERTIES LABELS quick)
endif()
