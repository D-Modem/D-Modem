# sm56_obj.cmake - Motorola's sm56.ko as an object half_sm56 links: its
# imports (the kernel's) renamed k_<name>, which half_sm56.c provides, and
# its tty entry points made global.
#
#     cmake -DKO=<sm56.ko> -DOUT=<sm56k.o> -DNM=<nm> -DOBJCOPY=<objcopy>
#           -P sm56_obj.cmake
execute_process(COMMAND ${NM} -u ${KO} OUTPUT_VARIABLE _u
	RESULT_VARIABLE _r)
if(_r)
	message(FATAL_ERROR "nm -u ${KO} failed")
endif()
string(REGEX MATCHALL "U [A-Za-z0-9_]+" _syms "${_u}")
set(_redef "")
foreach(_s ${_syms})
	string(SUBSTRING "${_s}" 2 -1 _n)
	string(APPEND _redef "${_n} k_${_n}\n")
endforeach()
file(WRITE ${OUT}.redef "${_redef}")
execute_process(COMMAND ${OBJCOPY} --redefine-syms=${OUT}.redef
	--globalize-symbol=mot_open --globalize-symbol=mot_write
	--globalize-symbol=mot_writeroom ${KO} ${OUT} RESULT_VARIABLE _r)
if(_r)
	message(FATAL_ERROR "objcopy ${KO} failed")
endif()
