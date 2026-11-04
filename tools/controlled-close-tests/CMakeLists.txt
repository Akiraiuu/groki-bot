# Boundary tests execute production methods with declared SDK/RTOS substitutes.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(OVERLAY "${STOPWATCH_PROJECT_ROOT}/patches/stopwatch-controlled-close")
set(SCOPE "${OVERLAY}/idf/candidate/components/esp-tls")
set(SDK "${OVERLAY}/sdk/candidate/esp_websocket_client.c")
set(DNS "${OVERLAY}/lwip/candidate/src/core/dns.c")
set(HTTP "${OVERLAY}/idf/candidate/components/esp_http_client/esp_http_client.c")
function(extract output source)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_methods.py"
            "${source}" "${CMAKE_CURRENT_BINARY_DIR}/${output}" ${ARGN}
        DEPENDS "${source}" extract_methods.py VERBATIM)
endfunction()
extract(dns_methods.inc "${DNS}" dns_call_found dns_cancel_request)
extract(http_methods.inc "${HTTP}" http_scope_config_valid http_scope_begin esp_http_client_close esp_http_client_cleanup)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sdk_methods.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_sdk_methods.py"
        "${SDK}" "${CMAKE_CURRENT_BINARY_DIR}/sdk_methods.inc"
    DEPENDS "${SDK}" generate_sdk_methods.py VERBATIM)

foreach(security RANGE 0 3)
    add_executable(controlled_dns_${security} test_dns_cancel.c "${CMAKE_CURRENT_BINARY_DIR}/dns_methods.inc")
    target_include_directories(controlled_dns_${security} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(controlled_dns_${security} PRIVATE LWIP_DNS_SECURE=${security})
    target_compile_options(controlled_dns_${security} PRIVATE -Wall -Wextra -Werror -Wno-unused-function)
    add_test(NAME controlled_dns_${security} COMMAND controlled_dns_${security})
endforeach()
add_executable(controlled_http test_http_scope.c "${CMAKE_CURRENT_BINARY_DIR}/http_methods.inc")
target_include_directories(controlled_http PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(controlled_http PRIVATE conversation_scope_host)
target_compile_options(controlled_http PRIVATE -Wall -Wextra -Werror -Wno-unused-function -Wno-misleading-indentation)
add_test(NAME controlled_http COMMAND controlled_http)
foreach(separate RANGE 0 1)
    add_executable(controlled_sdk_${separate} test_sdk_stop.c "${CMAKE_CURRENT_BINARY_DIR}/sdk_methods.inc")
    target_include_directories(controlled_sdk_${separate} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(controlled_sdk_${separate} PRIVATE conversation_scope_host)
    target_compile_features(controlled_sdk_${separate} PRIVATE c_std_17)
    target_compile_options(controlled_sdk_${separate} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-error=sign-compare)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # The upstream SDK uses a GNU C label followed by a declaration.
        target_compile_options(controlled_sdk_${separate} PRIVATE -Wno-error=c23-extensions)
    endif()
    if(separate)
        target_compile_definitions(controlled_sdk_${separate} PRIVATE CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=1)
    endif()
    add_test(NAME controlled_sdk_${separate} COMMAND controlled_sdk_${separate})
endforeach()
add_executable(controlled_resolver test_resolver.cpp "${SCOPE}/esp_tls_scope_resolver.c")
target_include_directories(controlled_resolver PRIVATE resolver-stubs)
target_link_libraries(controlled_resolver PRIVATE conversation_scope_host Threads::Threads)
if(WIN32)
    target_link_libraries(controlled_resolver PRIVATE ws2_32)
endif()
target_compile_options(controlled_resolver PRIVATE -Wall -Wextra -Werror)
add_test(NAME controlled_resolver COMMAND controlled_resolver)
set_tests_properties(controlled_resolver controlled_http controlled_sdk_0 controlled_sdk_1
    controlled_dns_0 controlled_dns_1 controlled_dns_2 controlled_dns_3 PROPERTIES TIMEOUT 15)
