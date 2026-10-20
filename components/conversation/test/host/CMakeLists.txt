# SPDX-FileCopyrightText: 2026 sefuzhou770801-hub
# SPDX-License-Identifier: BSL-1.0
#
# Host tests for the XiaoZhi control-message dispatch (control_dispatch.hpp):
# the gateway's `head` message and the pass-through of every other type.
#
# cJSON comes from ESP-IDF when IDF_PATH is set (the same copy the firmware
# links); otherwise CMake downloads the matching upstream release.

cmake_minimum_required(VERSION 3.18)
project(conversation_host_test C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CONVERSATION_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
    set(CJSON_DIR "$ENV{IDF_PATH}/components/json/cJSON")
else()
    include(FetchContent)
    # SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so
    # MakeAvailable only downloads; cJSON.c is compiled below.
    FetchContent_Declare(cjson
        URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR no-cmake-build
    )
    FetchContent_MakeAvailable(cjson)
    set(CJSON_DIR ${cjson_SOURCE_DIR})
endif()

add_library(cjson_host STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson_host PUBLIC ${CJSON_DIR})

enable_testing()

add_executable(conversation_test_control_dispatch test_control_dispatch.cpp)
target_include_directories(conversation_test_control_dispatch PRIVATE ${CONVERSATION_ROOT}/include)
target_link_libraries(conversation_test_control_dispatch PRIVATE cjson_host)
target_compile_options(conversation_test_control_dispatch PRIVATE -Wall -Wextra)
add_test(NAME control_dispatch COMMAND conversation_test_control_dispatch)

# Protocol payloads and settings use the production implementations.
set(CONFIG_ROOT ${CONVERSATION_ROOT}/../config_service)
add_executable(conversation_test_realtime_session
    test_realtime_session.cpp
    ${CONFIG_ROOT}/settings_registry.cpp
    ${CONFIG_ROOT}/staged_config.cpp)
target_include_directories(conversation_test_realtime_session PRIVATE
    ${CONVERSATION_ROOT} ${CONVERSATION_ROOT}/include
    ${CONVERSATION_ROOT}/../tl_expected/expected/include
    ${CONFIG_ROOT}/include)
target_link_libraries(conversation_test_realtime_session PRIVATE cjson_host)
target_compile_options(conversation_test_realtime_session PRIVATE -Wall -Wextra)
add_test(NAME realtime_session COMMAND conversation_test_realtime_session)

# Real client + codecs with only the ESP transport/RTOS replaced by test doubles.
find_path(MBEDTLS_INCLUDE_DIR mbedtls/base64.h REQUIRED)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto REQUIRED)
add_executable(conversation_test_realtime_client
    test_realtime_client.cpp
    ${CONVERSATION_ROOT}/openai_realtime_client.cpp
    ${CONVERSATION_ROOT}/base64.cpp)
target_include_directories(conversation_test_realtime_client PRIVATE
    stubs ${CONVERSATION_ROOT} ${CONVERSATION_ROOT}/include
    ${CONVERSATION_ROOT}/../tl_expected/expected/include ${MBEDTLS_INCLUDE_DIR})
target_compile_definitions(conversation_test_realtime_client PRIVATE CONFIG_STACKCHAN_CONV_WS_RX_BUFFER=131072)
target_link_libraries(conversation_test_realtime_client PRIVATE cjson_host ${MBEDCRYPTO_LIBRARY})
add_test(NAME realtime_client COMMAND conversation_test_realtime_client)

add_executable(conversation_test_saved_agent
    test_saved_agent.cpp
    ${CONVERSATION_ROOT}/saved_agent.cpp
    ${CONVERSATION_ROOT}/openai_realtime_client.cpp
    ${CONVERSATION_ROOT}/base64.cpp)
target_include_directories(conversation_test_saved_agent PRIVATE
    stubs ${CONVERSATION_ROOT} ${CONVERSATION_ROOT}/include
    ${CONVERSATION_ROOT}/../tl_expected/expected/include ${MBEDTLS_INCLUDE_DIR})
target_compile_definitions(conversation_test_saved_agent PRIVATE CONFIG_STACKCHAN_CONV_WS_RX_BUFFER=131072)
target_link_libraries(conversation_test_saved_agent PRIVATE cjson_host ${MBEDCRYPTO_LIBRARY})
add_test(NAME saved_agent COMMAND conversation_test_saved_agent)
