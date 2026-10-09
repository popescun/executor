# Copyright (c) 2026 Nicolae Popescu. MIT License.

cmake_minimum_required(VERSION 3.21)

project(qt_pool_vs_this LANGUAGES CXX)

# As the tests: executor.hpp and async.hpp use <print>.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A benchmark is only worth running optimised.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Where Qt is installed, for this platform: <Qt>/<version>/<platform>.
set(QT_PATH "/Users/popescun/Qt/6.11.2/macos" CACHE PATH "Qt installation, e.g. ~/Qt/6.11.2/macos")

if (NOT EXISTS "${QT_PATH}/lib/cmake/Qt6")
  message(FATAL_ERROR
    "QT_PATH '${QT_PATH}' is not a Qt 6 installation: ${QT_PATH}/lib/cmake/Qt6 does not exist.\n"
    "Point it at <Qt>/<version>/<platform>, e.g. -DQT_PATH=$HOME/Qt/6.11.2/macos")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${QT_PATH}")
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Concurrent)
qt_standard_project_setup(REQUIRES 6.5)

if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../async/async.hpp)
  message(FATAL_ERROR
    "async/async.hpp is missing; run: git submodule update --init --recursive")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

qt_add_executable(qt_pool_vs_this qt_pool_vs_this.cpp)

# ../ - executor.hpp; ../async - async.hpp, and the actuator/ directory it includes from.
target_include_directories(qt_pool_vs_this PRIVATE .. ../async)
target_link_libraries(qt_pool_vs_this PRIVATE Qt6::Core Qt6::Concurrent Threads::Threads)

# As in fluxcpp's Qt prototypes: Qt's keyword macros stay out of headers that are not Qt's.
target_compile_definitions(qt_pool_vs_this PRIVATE QT_NO_KEYWORDS)
set_target_properties(qt_pool_vs_this PROPERTIES MACOSX_BUNDLE OFF)
