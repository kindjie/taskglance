# Starts many taskglance processes at once and checks that every add
# survives. execute_process runs all of its COMMANDs concurrently as one
# pipeline, which makes this portable across platforms. Each stage re-runs
# this script in ADD mode so the add's own output is swallowed there instead
# of being piped into a stage that may already have exited.
if(DEFINED ADD)
  execute_process(
    COMMAND "${TASKGLANCE}" add "${ADD}"
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY
  )
  return()
endif()

set(writers 20)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(env
  "${CMAKE_COMMAND}" -E env
  "XDG_DATA_HOME=${WORK_DIR}/data"
  "XDG_CONFIG_HOME=${WORK_DIR}/config"
  "XDG_STATE_HOME=${WORK_DIR}/state"
)

set(commands)
foreach(index RANGE 1 ${writers})
  list(APPEND commands COMMAND ${env} "${CMAKE_COMMAND}"
    "-DTASKGLANCE=${TASKGLANCE}" "-DADD=task ${index}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
  )
endforeach()
execute_process(${commands} RESULTS_VARIABLE results OUTPUT_QUIET)
foreach(result IN LISTS results)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "A concurrent add failed: ${results}")
  endif()
endforeach()

execute_process(
  COMMAND ${env} "${TASKGLANCE}" list
  OUTPUT_VARIABLE listing
  COMMAND_ERROR_IS_FATAL ANY
)
string(REGEX MATCHALL "active" active "${listing}")
list(LENGTH active count)
if(NOT count EQUAL writers)
  message(FATAL_ERROR "Expected ${writers} tasks, found ${count}:\n${listing}")
endif()
