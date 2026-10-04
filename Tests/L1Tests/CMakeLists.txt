# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2025 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.8)

project(SceneSetL1Tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(GTest REQUIRED)
find_package(libralf CONFIG REQUIRED)

# Set the executable name
set(TEST_EXECUTABLE SceneSetL1TestsIN)

# Source files - include the actual SceneSet implementation
set(TEST_SRC
    tests/test_SceneSet.cpp
    tests/test_download_monitor.cpp
    ../../src/SceneSet.cpp
    ../../src/RalfPackageSupport.cpp
   )

# Create the test executable
add_executable(${TEST_EXECUTABLE} ${TEST_SRC})
target_compile_definitions(${TEST_EXECUTABLE} PRIVATE
    UNIT_TEST
    DAC_APP_CERT_PATH="/etc/rdk/certs"
    ENABLE_SYSTEM_CONFIG=1
    ENABLE_FIRMWARE_CHANGE_DETECTION=1
)

# WPEFramework library paths
set(WPEFRAMEWORK_LIB_DIR ${CMAKE_INSTALL_PREFIX}/lib)
set(WPEFRAMEWORK_INCLUDE_DIR ${CMAKE_INSTALL_PREFIX}/include)

#Link libraries
target_link_libraries(${TEST_EXECUTABLE}
    PRIVATE
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCore.so
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCOM.so
    pthread
    systemd
    dl
)

    # GCC < 9 requires explicit -lstdc++fs when using <filesystem> in C++17 mode.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
        target_link_libraries(${TEST_EXECUTABLE} PRIVATE stdc++fs)
    endif()

if(TARGET ralf-utils::libralf)
    target_link_libraries(${TEST_EXECUTABLE} PRIVATE ralf-utils::libralf)
elseif(TARGET libralf)
    target_link_libraries(${TEST_EXECUTABLE} PRIVATE libralf)
elseif(TARGET ralf-utils::libralf-static)
    message(WARNING "Only static libralf target available. Linking ralf-utils::libralf-static.")
    target_link_libraries(${TEST_EXECUTABLE} PRIVATE ralf-utils::libralf-static)
elseif(TARGET libralf-static)
    message(WARNING "Only static libralf target available. Linking libralf-static.")
    target_link_libraries(${TEST_EXECUTABLE} PRIVATE libralf-static)
else()
    message(FATAL_ERROR "libralf package found but no usable CMake target exported (expected ralf-utils::libralf, libralf, ralf-utils::libralf-static, or libralf-static)")
endif()

# Include directories
target_include_directories(${TEST_EXECUTABLE}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${GTEST_INCLUDE_DIRS}
    ${WPEFRAMEWORK_INCLUDE_DIR}
    ${WPEFRAMEWORK_INCLUDE_DIR}/WPEFramework
)

# Add coverage flags for gcc with-coverage builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${TEST_EXECUTABLE}
        PRIVATE
        --coverage
        -fprofile-arcs
        -ftest-coverage
        -g
        -O0
    )
    target_link_options(${TEST_EXECUTABLE}
        PRIVATE
        --coverage
    )
endif()

# Install the binary
install(TARGETS ${TEST_EXECUTABLE} DESTINATION bin)

# L1 test executable for RESTART_HOMEAPP_ALWAYS compile-time path
set(TEST_EXECUTABLE_RESTART_ALWAYS SceneSetL1TestsRestartAlwaysIN)
add_executable(${TEST_EXECUTABLE_RESTART_ALWAYS} ${TEST_SRC})
target_compile_definitions(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE
    UNIT_TEST
    DAC_APP_CERT_PATH="/etc/rdk/certs"
    ENABLE_SYSTEM_CONFIG=1
    RESTART_HOMEAPP_ALWAYS=1
)

target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS}
    PRIVATE
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCore.so
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCOM.so
    pthread
    systemd
    dl
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE stdc++fs)
endif()

if(TARGET ralf-utils::libralf)
    target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE ralf-utils::libralf)
elseif(TARGET libralf)
    target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE libralf)
elseif(TARGET ralf-utils::libralf-static)
    target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE ralf-utils::libralf-static)
elseif(TARGET libralf-static)
    target_link_libraries(${TEST_EXECUTABLE_RESTART_ALWAYS} PRIVATE libralf-static)
else()
    message(FATAL_ERROR "libralf package found but no usable CMake target exported (expected ralf-utils::libralf, libralf, ralf-utils::libralf-static, or libralf-static)")
endif()

target_include_directories(${TEST_EXECUTABLE_RESTART_ALWAYS}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${GTEST_INCLUDE_DIRS}
    ${WPEFRAMEWORK_INCLUDE_DIR}
    ${WPEFRAMEWORK_INCLUDE_DIR}/WPEFramework
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${TEST_EXECUTABLE_RESTART_ALWAYS}
        PRIVATE
        --coverage
        -fprofile-arcs
        -ftest-coverage
        -g
        -O0
    )
    target_link_options(${TEST_EXECUTABLE_RESTART_ALWAYS}
        PRIVATE
        --coverage
    )
endif()

install(TARGETS ${TEST_EXECUTABLE_RESTART_ALWAYS} DESTINATION bin)

# L1 test executable for the first-boot marker strategy (ENABLE_FIRMWARE_CHANGE_DETECTION=0)
set(TEST_EXECUTABLE_MARKER SceneSetL1TestsMarkerIN)
add_executable(${TEST_EXECUTABLE_MARKER}
    tests/test_SceneSetMarker.cpp
    ../../src/SceneSet.cpp
    ../../src/RalfPackageSupport.cpp
)
target_compile_definitions(${TEST_EXECUTABLE_MARKER} PRIVATE
    UNIT_TEST
    DAC_APP_CERT_PATH="/etc/rdk/certs"
    ENABLE_SYSTEM_CONFIG=1
    ENABLE_FIRMWARE_CHANGE_DETECTION=0
)

target_link_libraries(${TEST_EXECUTABLE_MARKER}
    PRIVATE
    GTest::gtest
    GTest::gtest_main
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCore.so
    ${WPEFRAMEWORK_LIB_DIR}/libWPEFrameworkCOM.so
    pthread
    systemd
    dl
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(${TEST_EXECUTABLE_MARKER} PRIVATE stdc++fs)
endif()

if(TARGET ralf-utils::libralf)
    target_link_libraries(${TEST_EXECUTABLE_MARKER} PRIVATE ralf-utils::libralf)
elseif(TARGET libralf)
    target_link_libraries(${TEST_EXECUTABLE_MARKER} PRIVATE libralf)
elseif(TARGET ralf-utils::libralf-static)
    target_link_libraries(${TEST_EXECUTABLE_MARKER} PRIVATE ralf-utils::libralf-static)
elseif(TARGET libralf-static)
    target_link_libraries(${TEST_EXECUTABLE_MARKER} PRIVATE libralf-static)
endif()

target_include_directories(${TEST_EXECUTABLE_MARKER}
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${GTEST_INCLUDE_DIRS}
    ${WPEFRAMEWORK_INCLUDE_DIR}
    ${WPEFRAMEWORK_INCLUDE_DIR}/WPEFramework
)

install(TARGETS ${TEST_EXECUTABLE_MARKER} DESTINATION bin)
