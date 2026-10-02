# Runs a real watch with stdout piped while another process adds a task,
# and checks the plain frames. execute_process runs its COMMANDs
# concurrently; watch is last so its output is captured, and TIMEOUT
# bounds it because watch only stops on a signal.
if(DEFINED ADD)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
  execute_process(
    COMMAND "${TASKGLANCE}" add "${ADD}"
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY
  )
  return()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(env
  "${CMAKE_COMMAND}" -E env
  "XDG_DATA_HOME=${WORK_DIR}/data"
  "XDG_CONFIG_HOME=${WORK_DIR}/config"
  "XDG_STATE_HOME=${WORK_DIR}/state"
)
execute_process(
  COMMAND ${env} "${TASKGLANCE}" add "Before watch"
  OUTPUT_QUIET
  COMMAND_ERROR_IS_FATAL ANY
)
file(TIMESTAMP "${WORK_DIR}/data/taskglance/tasks.tsv" persisted_clock
     "%H:%M:%S")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
execute_process(
  COMMAND ${env} "${CMAKE_COMMAND}" "-DTASKGLANCE=${TASKGLANCE}"
          "-DADD=Added while watching" -P "${CMAKE_CURRENT_LIST_FILE}"
  COMMAND ${env} "${TASKGLANCE}" watch --interval 0.1
  OUTPUT_VARIABLE output
  TIMEOUT 4
)

string(ASCII 27 esc)
string(FIND "${output}" "${esc}" escape)
if(NOT escape EQUAL -1)
  message(FATAL_ERROR "Piped watch emitted an escape sequence:\n${output}")
endif()
string(REGEX MATCHALL "My Tasks · [0-9]+ active" headers "${output}")
list(LENGTH headers frames)
if(NOT frames EQUAL 2 OR
   NOT output MATCHES "1 active.*Before watch.*2 active.*Added while")
  message(FATAL_ERROR "Expected an initial frame and one change frame, "
                      "got ${frames}:\n${output}")
endif()

string(FIND "${output}" "updated ${persisted_clock}" timestamp_position)
if(timestamp_position EQUAL -1 OR persisted_clock STREQUAL "")
  message(FATAL_ERROR "Watch did not retain the persisted list timestamp")
endif()
