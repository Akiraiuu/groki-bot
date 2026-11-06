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

find_package(Threads REQUIRED)
add_executable(conversation_test_paid_voice_test test_paid_voice_test.cpp)
target_include_directories(conversation_test_paid_voice_test PRIVATE
    ${CONVERSATION_ROOT}/include ${CONVERSATION_ROOT}/../tl_expected/expected/include)
target_link_libraries(conversation_test_paid_voice_test PRIVATE cjson_host Threads::Threads)
target_compile_options(conversation_test_paid_voice_test PRIVATE -Wall -Wextra)
add_test(NAME paid_voice_test COMMAND conversation_test_paid_voice_test)

add_executable(conversation_test_wake_confirmation test_wake_confirmation.cpp)
target_include_directories(conversation_test_wake_confirmation PRIVATE ${CONVERSATION_ROOT}/include)
target_link_libraries(conversation_test_wake_confirmation PRIVATE Threads::Threads)
target_compile_options(conversation_test_wake_confirmation PRIVATE -Wall -Wextra)
add_test(NAME wake_confirmation COMMAND conversation_test_wake_confirmation)

add_executable(conversation_test_local_wake_worker
    test_local_wake_worker.cpp ${CONVERSATION_ROOT}/../../main/local_wake.cpp)
target_include_directories(conversation_test_local_wake_worker PRIVATE
    wake_stubs ${CONVERSATION_ROOT}/include ${CONVERSATION_ROOT}/../../main)
target_link_libraries(conversation_test_local_wake_worker PRIVATE Threads::Threads)
target_compile_options(conversation_test_local_wake_worker PRIVATE -Wall -Wextra)
add_test(NAME local_wake_worker COMMAND conversation_test_local_wake_worker)

# Bounded event admission and close-ownership policy; no device or network.
add_executable(conversation_test_voice_control_policy test_voice_control_policy.cpp)
target_include_directories(conversation_test_voice_control_policy PRIVATE ${CONVERSATION_ROOT}/include/conversation)
target_link_libraries(conversation_test_voice_control_policy PRIVATE Threads::Threads)
target_compile_options(conversation_test_voice_control_policy PRIVATE -Wall -Wextra -Werror)
add_test(NAME voice_control_policy COMMAND conversation_test_voice_control_policy)

# Compile/execute the ESP adapter branch against task API doubles on the host.
# This is not an Xtensa build or a hardware scheduling/timing test.
add_executable(conversation_test_voice_control_policy_esp_boundary test_voice_control_policy.cpp)
target_include_directories(conversation_test_voice_control_policy_esp_boundary PRIVATE
    policy_esp_stubs ${CONVERSATION_ROOT}/include/conversation)
target_compile_definitions(conversation_test_voice_control_policy_esp_boundary PRIVATE ESP_PLATFORM=1)
target_link_libraries(conversation_test_voice_control_policy_esp_boundary PRIVATE Threads::Threads)
target_compile_options(conversation_test_voice_control_policy_esp_boundary PRIVATE -Wall -Wextra -Werror)
add_test(NAME voice_control_policy_esp_boundary COMMAND conversation_test_voice_control_policy_esp_boundary)

# Queue backend/allocator doubles, with the actual paid permission state header.
add_executable(conversation_test_event_inbox test_event_inbox.cpp)
target_include_directories(conversation_test_event_inbox PRIVATE ${CONVERSATION_ROOT}/include/conversation)
target_link_libraries(conversation_test_event_inbox PRIVATE Threads::Threads)
target_compile_options(conversation_test_event_inbox PRIVATE -Wall -Wextra -Werror)
add_test(NAME event_inbox COMMAND conversation_test_event_inbox)

# Actual controlled close implementation with synthetic transport/task boundary.
add_executable(conversation_test_controlled_close test_controlled_close.cpp
    ${CONVERSATION_ROOT}/openai_realtime_client.cpp ${CONVERSATION_ROOT}/base64.cpp)
target_include_directories(conversation_test_controlled_close PRIVATE stubs ${CONVERSATION_ROOT}
    ${CONVERSATION_ROOT}/include ${CONVERSATION_ROOT}/../tl_expected/expected/include ${MBEDTLS_INCLUDE_DIR})
target_compile_definitions(conversation_test_controlled_close PRIVATE CONFIG_STACKCHAN_CONV_WS_RX_BUFFER=131072)
target_link_libraries(conversation_test_controlled_close PRIVATE cjson_host ${MBEDCRYPTO_LIBRARY} Threads::Threads)
add_test(NAME controlled_close COMMAND conversation_test_controlled_close)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CLOSE_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/generated_controlled_close.inc")
add_custom_command(OUTPUT "${CLOSE_FIXTURE}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated_local_speech_guard.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_controlled_close_fixture.py"
        "${CONVERSATION_ROOT}/../../main/conversation_task.cpp" "${CLOSE_FIXTURE}"
    DEPENDS generate_controlled_close_fixture.py "${CONVERSATION_ROOT}/../../main/conversation_task.cpp"
        "${CONVERSATION_ROOT}/../../main/settings_sinks.cpp" "${CONVERSATION_ROOT}/../../main/app_main.cpp")
add_executable(conversation_test_close_monitor test_close_monitor.cpp "${CLOSE_FIXTURE}")
target_include_directories(conversation_test_close_monitor PRIVATE ${CONVERSATION_ROOT}/include
    ${CONVERSATION_ROOT}/../tl_expected/expected/include "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(conversation_test_close_monitor PRIVATE Threads::Threads)
target_compile_options(conversation_test_close_monitor PRIVATE -Wall -Wextra -Werror)
add_test(NAME close_monitor COMMAND conversation_test_close_monitor)
set_tests_properties(controlled_close close_monitor PROPERTIES TIMEOUT 15)

add_executable(conversation_test_session_operation_gate test_session_operation_gate.cpp)
target_include_directories(conversation_test_session_operation_gate PRIVATE ${CONVERSATION_ROOT}/include)
target_link_libraries(conversation_test_session_operation_gate PRIVATE Threads::Threads)
target_compile_options(conversation_test_session_operation_gate PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_operation_gate COMMAND conversation_test_session_operation_gate)
set_tests_properties(session_operation_gate PROPERTIES TIMEOUT 15)

add_executable(conversation_test_session_operation_gate_esp_boundary test_session_operation_gate.cpp)
target_include_directories(conversation_test_session_operation_gate_esp_boundary PRIVATE
    policy_esp_stubs ${CONVERSATION_ROOT}/include)
target_compile_definitions(conversation_test_session_operation_gate_esp_boundary PRIVATE ESP_PLATFORM=1)
target_link_libraries(conversation_test_session_operation_gate_esp_boundary PRIVATE Threads::Threads)
target_compile_options(conversation_test_session_operation_gate_esp_boundary PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_operation_gate_esp_boundary COMMAND conversation_test_session_operation_gate_esp_boundary)
set_tests_properties(session_operation_gate_esp_boundary PROPERTIES TIMEOUT 15)

# The production scope implementation is shared with the firmware overlay.
get_filename_component(STOPWATCH_PROJECT_ROOT "${CONVERSATION_ROOT}/../.." ABSOLUTE)
set(STOPWATCH_SCOPE_ROOT "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close/idf/candidate/components/esp-tls")
add_library(conversation_scope_host STATIC "${STOPWATCH_SCOPE_ROOT}/esp_tls_scope.c")
target_compile_features(conversation_scope_host PUBLIC c_std_11)
# Match the firmware GCC rule that exposed the scope indentation error.
target_compile_options(conversation_scope_host PRIVATE -Wall -Wextra -Werror=misleading-indentation)
target_include_directories(conversation_scope_host PUBLIC "${STOPWATCH_SCOPE_ROOT}")
foreach(target conversation_test_realtime_client conversation_test_saved_agent conversation_test_controlled_close)
    target_link_libraries(${target} PRIVATE conversation_scope_host)
endforeach()
add_subdirectory("${STOPWATCH_PROJECT_ROOT}/tools/controlled-close-tests" "${CMAKE_CURRENT_BINARY_DIR}/controlled-close-tests")

# Actual ticket/client implementations and extracted coordinator connect body.
set(CONNECT_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/generated_connect.inc")
add_custom_command(OUTPUT "${CONNECT_FIXTURE}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated_voice_status.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_connect_fixture.py"
        "${CONVERSATION_ROOT}/../../main/conversation_task.cpp" "${CONNECT_FIXTURE}"
    DEPENDS generate_connect_fixture.py "${CONVERSATION_ROOT}/../../main/conversation_task.cpp"
        "${CONVERSATION_ROOT}/../wifi_config_service/http_handlers.cpp")
add_executable(conversation_test_connect_diagnostics test_connect_diagnostics.cpp
    "${CONNECT_FIXTURE}" ${CONVERSATION_ROOT}/saved_agent.cpp
    ${CONVERSATION_ROOT}/openai_realtime_client.cpp ${CONVERSATION_ROOT}/base64.cpp)
target_include_directories(conversation_test_connect_diagnostics PRIVATE stubs
    ${CONVERSATION_ROOT} ${CONVERSATION_ROOT}/include ${CMAKE_CURRENT_BINARY_DIR}
    ${CONVERSATION_ROOT}/../tl_expected/expected/include ${MBEDTLS_INCLUDE_DIR})
target_compile_definitions(conversation_test_connect_diagnostics PRIVATE CONFIG_STACKCHAN_CONV_WS_RX_BUFFER=131072)
target_compile_options(conversation_test_connect_diagnostics PRIVATE -Wall -Wextra)
target_link_libraries(conversation_test_connect_diagnostics PRIVATE conversation_scope_host cjson_host ${MBEDCRYPTO_LIBRARY} Threads::Threads)
add_test(NAME connect_diagnostics COMMAND conversation_test_connect_diagnostics)
set_tests_properties(connect_diagnostics PROPERTIES TIMEOUT 20)
add_test(NAME connect_cleanup_failure COMMAND conversation_test_connect_diagnostics cleanup)
add_test(NAME connect_first_fault_before_cleanup COMMAND conversation_test_connect_diagnostics cleanup-after-auth)
set_tests_properties(connect_cleanup_failure connect_first_fault_before_cleanup PROPERTIES TIMEOUT 20)

# HTTP/SSL/TLS admission using exact frozen method bodies, no network or crypto.
set(TICKET_PATH_DIR "${CMAKE_CURRENT_BINARY_DIR}/ticket-http-path")
add_custom_command(OUTPUT "${TICKET_PATH_DIR}/tls.inc" "${TICKET_PATH_DIR}/ssl.inc" "${TICKET_PATH_DIR}/http.inc" "${TICKET_PATH_DIR}/http-init.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_ticket_http_path.py"
        "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close" "${TICKET_PATH_DIR}"
    DEPENDS generate_ticket_http_path.py
        "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close/idf/candidate/components/esp-tls/esp_tls.c"
        "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close/idf/candidate/components/tcp_transport/transport_ssl.c"
        "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close/idf/candidate/components/esp_http_client/esp_http_client.c"
        "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close/overlay/overlay-manifest-v03.json")
add_executable(conversation_test_ticket_http_path test_ticket_http_path.c
    "${TICKET_PATH_DIR}/tls.inc" "${TICKET_PATH_DIR}/ssl.inc" "${TICKET_PATH_DIR}/http.inc" "${TICKET_PATH_DIR}/http-init.inc")
target_include_directories(conversation_test_ticket_http_path PRIVATE "${TICKET_PATH_DIR}")
target_link_libraries(conversation_test_ticket_http_path PRIVATE conversation_scope_host)
target_compile_options(conversation_test_ticket_http_path PRIVATE -Wall -Wextra -Werror -Wno-unused-function)
add_test(NAME ticket_http_path COMMAND conversation_test_ticket_http_path)
set_tests_properties(ticket_http_path PROPERTIES TIMEOUT 10)
