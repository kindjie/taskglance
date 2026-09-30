cmake_minimum_required(VERSION 3.24)

project(taskglance VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TASKGLANCE_BUILD_TESTS "Build taskglance tests" ON)

set(TASKGLANCE_ZSH_COMPLETION_DIR
  "${CMAKE_INSTALL_DATADIR}/zsh/site-functions"
)
set(TASKGLANCE_BASH_COMPLETION_DIR
  "${CMAKE_INSTALL_DATADIR}/bash-completion/completions"
)
set(TASKGLANCE_FISH_COMPLETION_DIR
  "${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d"
)

add_library(taskglance_core
  src/config.cpp
  src/legacy.cpp
  src/prompt.cpp
  src/render.cpp
  src/store.cpp
  src/util.cpp
  src/watch.cpp
)

target_include_directories(taskglance_core PUBLIC include)

target_compile_features(taskglance_core PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(taskglance_core PRIVATE /W4)
else()
  target_compile_options(taskglance_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(taskglance src/main.cpp src/watch_terminal.cpp)
target_link_libraries(taskglance PRIVATE taskglance_core)

install(TARGETS taskglance RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(CODE "
set(taskglance_exe \"$<TARGET_FILE:taskglance>\")
set(taskglance_prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
execute_process(
  COMMAND \"\${CMAKE_COMMAND}\" -E make_directory
          \"\${taskglance_prefix}/${TASKGLANCE_ZSH_COMPLETION_DIR}\"
          \"\${taskglance_prefix}/${TASKGLANCE_BASH_COMPLETION_DIR}\"
          \"\${taskglance_prefix}/${TASKGLANCE_FISH_COMPLETION_DIR}\"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND \"\${taskglance_exe}\" completions zsh
  OUTPUT_FILE
    \"\${taskglance_prefix}/${TASKGLANCE_ZSH_COMPLETION_DIR}/_taskglance\"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND \"\${taskglance_exe}\" completions bash
  OUTPUT_FILE
    \"\${taskglance_prefix}/${TASKGLANCE_BASH_COMPLETION_DIR}/taskglance\"
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND \"\${taskglance_exe}\" completions fish
  OUTPUT_FILE
    \"\${taskglance_prefix}/${TASKGLANCE_FISH_COMPLETION_DIR}/taskglance.fish\"
  COMMAND_ERROR_IS_FATAL ANY
)
")

if(TASKGLANCE_BUILD_TESTS)
  enable_testing()
  add_executable(taskglance_tests
    tests/test_main.cpp
  )
  find_package(Threads REQUIRED)
  target_link_libraries(taskglance_tests
    PRIVATE taskglance_core Threads::Threads
  )
  add_test(NAME taskglance_tests COMMAND taskglance_tests)
  add_executable(taskglance_watch_tests tests/test_watch.cpp)
  target_link_libraries(taskglance_watch_tests PRIVATE taskglance_core)
  add_test(NAME taskglance_watch_tests COMMAND taskglance_watch_tests)
  add_test(NAME taskglance_watch_arguments
    COMMAND ${CMAKE_COMMAND}
            -DTASKGLANCE=$<TARGET_FILE:taskglance>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/watch_arguments.cmake
  )
  set_tests_properties(taskglance_watch_arguments PROPERTIES TIMEOUT 25)
  add_test(NAME taskglance_concurrent_cli
    COMMAND ${CMAKE_COMMAND}
            -DTASKGLANCE=$<TARGET_FILE:taskglance>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/concurrent-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrent_cli.cmake
  )
endif()
