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
