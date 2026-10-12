# hsf_obj.cmake - one of the HSF modem's objects (hsfengine, hsfmc97ich,
# hsfsoar) as an object half_hsf links: its .ctors and its
# .gnu.linkonce.* sections renamed for the object (.<name>_ctors,
# .<name>.<rest>).  Each object has the same generic names (its C++
# vtables and the like) and the linker would keep only one of them.
#
#     cmake -DIN=<hsfengine.o> -DOUT=<out.o> -DNAME=hsfengine
#           -DOBJDUMP=<objdump> -DOBJCOPY=<objcopy> -P hsf_obj.cmake
execute_process(COMMAND ${OBJDUMP} -h ${IN} OUTPUT_VARIABLE _h
	RESULT_VARIABLE _r)
if(_r)
	message(FATAL_ERROR "objdump -h ${IN} failed")
endif()
set(_args --rename-section .ctors=.${NAME}_ctors)
string(REGEX MATCHALL " \\.gnu\\.linkonce\\.[^ \t\n]+" _secs "${_h}")
list(REMOVE_DUPLICATES _secs)
foreach(_s ${_secs})
	string(STRIP "${_s}" _s)
	string(SUBSTRING "${_s}" 13 -1 _rest)
	list(APPEND _args --rename-section ${_s}=.${NAME}${_rest})
endforeach()
execute_process(COMMAND ${OBJCOPY} ${_args} ${IN} ${OUT} RESULT_VARIABLE _r)
if(_r)
	message(FATAL_ERROR "objcopy ${IN} failed")
endif()
