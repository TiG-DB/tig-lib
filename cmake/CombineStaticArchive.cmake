if(NOT DEFINED OUTPUT OR NOT DEFINED PRIMARY OR NOT DEFINED DEPENDENCY)
  message(FATAL_ERROR "OUTPUT, PRIMARY, and DEPENDENCY are required")
endif()

set(_output "${OUTPUT}")
set(_tmp "${OUTPUT}.standalone.tmp")

file(REMOVE "${_tmp}")

if(WIN32)
  execute_process(
    COMMAND lib /NOLOGO /OUT:${_tmp} "${PRIMARY}" "${DEPENDENCY}"
    RESULT_VARIABLE _result
  )
elseif(APPLE)
  find_program(_libtool NAMES libtool REQUIRED)
  execute_process(
    COMMAND "${_libtool}" -static -o "${_tmp}" "${PRIMARY}" "${DEPENDENCY}"
    RESULT_VARIABLE _result
  )
else()
  if(NOT CMAKE_AR)
    message(FATAL_ERROR "CMAKE_AR is required to combine static archives")
  endif()

  set(_mri "${OUTPUT}.standalone.mri")
  file(WRITE "${_mri}" "CREATE ${_tmp}\n")
  file(APPEND "${_mri}" "ADDLIB ${PRIMARY}\n")
  file(APPEND "${_mri}" "ADDLIB ${DEPENDENCY}\n")
  file(APPEND "${_mri}" "SAVE\n")
  file(APPEND "${_mri}" "END\n")

  execute_process(
    COMMAND "${CMAKE_AR}" -M
    INPUT_FILE "${_mri}"
    RESULT_VARIABLE _result
  )
  file(REMOVE "${_mri}")

  if(_result EQUAL 0 AND CMAKE_RANLIB)
    execute_process(COMMAND "${CMAKE_RANLIB}" "${_tmp}" RESULT_VARIABLE _result)
  endif()
endif()

if(NOT _result EQUAL 0)
  file(REMOVE "${_tmp}")
  message(FATAL_ERROR "Failed to combine static archives into ${_output}")
endif()

file(RENAME "${_tmp}" "${_output}")
