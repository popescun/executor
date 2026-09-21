# Copyright (c) 2026 Nicolae Popescu. MIT License.

cmake_minimum_required(VERSION 4.1)

project(test_executor)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# googletest and the tests both want a thread library; ask for it explicitly rather than relying on
# googletest's own find_package call having run first.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# googletest is fetched at configure time rather than vendored or added as a second submodule:
# `async` is a submodule because it is what this project is built out of, while googletest is a
# pinned third-party release that nothing here ever edits. Same version as the async repo pins.
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.18.0
  GIT_SHALLOW    TRUE
)

# Without this, googletest links the static CRT on MSVC while this project links the dynamic one,
# and the two object files will not go together. It is a no-op everywhere else.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# gmock is built here, unlike in the async repo: a task is a callable the pool is meant to invoke
# exactly once, which is a call expectation rather than a counter the test keeps itself.
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# enable_testing() creates the CTest targets, and writes the CTestTestfile.cmake that
# `ctest --test-dir` needs.
enable_testing()

# ../          - executor.hpp
# ../async     - async.hpp, and the actuator/ directory async.hpp includes from
include_directories(
  ../
  ../async
)

if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../async/async.hpp)
  message(FATAL_ERROR
    "async/async.hpp is missing; run: git submodule update --init --recursive")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

# A pool is where the threading defects show, so the sanitizers are how they get diagnosed rather
# than guessed at: a test can only observe a queue that has already gone wrong, while
# -fsanitize=thread names the two racing accesses. Off by default - a sanitized build is several
# times slower, and TSan does not ship for every toolchain the CI matrix covers.
set(EXECUTOR_SANITIZE "" CACHE STRING "Build the tests with a sanitizer: address, thread, undefined, or empty")

# Offered to cmake-gui and ccmake as a drop-down rather than a free-text field.
set_property(CACHE EXECUTOR_SANITIZE PROPERTY STRINGS "" address thread undefined)

# Without this, a typo goes straight through as -fsanitize=adress and the compiler reports it a
# hundred lines later, in terms of a flag the caller never typed.
set(EXECUTOR_KNOWN_SANITIZERS address thread undefined)
if (EXECUTOR_SANITIZE AND NOT EXECUTOR_SANITIZE IN_LIST EXECUTOR_KNOWN_SANITIZERS)
  message(FATAL_ERROR
    "EXECUTOR_SANITIZE is '${EXECUTOR_SANITIZE}'; expected one of ${EXECUTOR_KNOWN_SANITIZERS}, or empty.")
endif()

# address and thread instrument the same accesses in incompatible ways and cannot be combined; one
# configuration is one sanitizer, so build them in separate directories.
if (EXECUTOR_SANITIZE)
  if (MSVC)
    message(FATAL_ERROR "EXECUTOR_SANITIZE uses -fsanitize=, which MSVC does not accept.")
  endif()
  message(STATUS "Sanitizer: ${EXECUTOR_SANITIZE}")
else()
  message(STATUS "Sanitizer: none (configure with -DEXECUTOR_SANITIZE=address|thread|undefined)")
endif()

# executor_smoke_test walks the pool through the scenarios the prototype was written to answer and
# prints what it observed; it states only what does not depend on timing. executor_tests is the
# suite that states expectations.
add_executable(executor_smoke_test executor_smoke_test.cpp)
add_executable(executor_tests executor_tests.cpp)

foreach(target executor_smoke_test executor_tests)
  # gmock_main brings gtest_main with it, so both binaries take the same entry point.
  target_link_libraries(${target} PRIVATE GTest::gmock_main)

  if (UNIX)
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endif (UNIX)

  if (EXECUTOR_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=${EXECUTOR_SANITIZE} -g -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=${EXECUTOR_SANITIZE})
  endif()
endforeach()

# gtest_discover_tests registers every TEST() as its own CTest entry, which is what keeps these
# suites reportable: a defect in the pool can wedge or abort the process, and one test per process
# means that takes down the single case that provoked it instead of every case after it.
include(GoogleTest)
foreach(target executor_smoke_test executor_tests)
  gtest_discover_tests(${target}
    PROPERTIES TIMEOUT 120
    DISCOVERY_TIMEOUT 60
  )
endforeach()
