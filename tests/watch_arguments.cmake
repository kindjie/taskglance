# Even malformed watch commands must be bounded if a regression starts
# polling instead of rejecting the argument.
foreach(value IN ITEMS 0 -1 bogus 1x nan inf 1e999 0.05)
  execute_process(
    COMMAND "${TASKGLANCE}" watch --interval "${value}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 2
  )
  if(NOT result STREQUAL "2" OR NOT error MATCHES "at least 0\\.1" OR
     NOT output STREQUAL "")
    message(FATAL_ERROR "watch accepted '${value}': ${result}, ${error}")
  endif()
endforeach()

foreach(option IN ITEMS --interactive -i)
  execute_process(
    COMMAND "${TASKGLANCE}" watch "${option}" --all --interval 0.1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 2
  )
  if(WIN32)
    set(expected "interactive mode is not supported on Windows yet")
  else()
    set(expected "interactive watch requires stdin and stdout to be TTYs")
  endif()
  if(NOT result STREQUAL "2" OR NOT error MATCHES "${expected}" OR
     NOT output STREQUAL "")
    message(FATAL_ERROR "interactive watch: ${result}, ${output}, ${error}")
  endif()
endforeach()

foreach(option IN ITEMS --interval --unknown)
  execute_process(
    COMMAND "${TASKGLANCE}" watch "${option}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
    TIMEOUT 2
  )
  if(NOT result STREQUAL "2" OR error STREQUAL "")
    message(FATAL_ERROR "watch accepted '${option}': ${result}, ${error}")
  endif()
endforeach()
