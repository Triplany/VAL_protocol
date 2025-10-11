cmake_minimum_required(VERSION 3.15)

# Always build unit test executables and register them with CTest.

# Place unit test executables into the shared bin/unit_tests folder so they
# don't clutter the top-level bin directory where examples live.
set(VAL_UT_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/unit_tests)
file(MAKE_DIRECTORY ${VAL_UT_OUTPUT_DIR})

function(add_ctest_exe name src)
    # Include common test support and transport simulation helpers in all unit tests
    add_executable(${name} ${src} support/test_support.c support/transport_profiles.c)
    # Ensure unit tests emit into the bin/unit_tests output directory
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${VAL_UT_OUTPUT_DIR}
    )
    # Allow unit tests to include public and selected internal headers for white-box testing
    target_include_directories(${name} PRIVATE support include ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE val_protocol)
    # Register test using the generated target file to ensure CTest uses the actual path
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

# Add unit tests

add_ctest_exe(ut_sr_check send_receive/suite_send_receive_check.c)
add_ctest_exe(ut_recovery_check recovery/suite_recovery_check.c)
add_ctest_exe(ut_resume_policies recovery/test_resume_policies.c)
set_property(TEST ut_resume_policies PROPERTY LABELS "quick")

# Comprehensive resume modes coverage
add_ctest_exe(ut_resume_modes recovery/test_resume_modes.c)
set_property(TEST ut_resume_modes PROPERTY LABELS "quick")

# Resume + validation interaction tests (Phase 2 comprehensive coverage)
add_ctest_exe(ut_resume_with_validation recovery/test_resume_with_validation.c)
set_property(TEST ut_resume_with_validation PROPERTY LABELS "normal")

add_ctest_exe(ut_error_system core/test_error_system.c)
add_ctest_exe(ut_transport_optional core/test_transport_optional.c)
add_ctest_exe(ut_packet_negotiation core/test_packet_negotiation.c)
set_property(TEST ut_packet_negotiation PROPERTY LABELS "normal")
add_ctest_exe(ut_metadata_validation core/test_metadata_validation.c)
set_property(TEST ut_metadata_validation PROPERTY LABELS "quick")

# Adaptive timeout behavior
add_ctest_exe(ut_adaptive_timeouts core/test_adaptive_timeouts.c)
set_property(TEST ut_adaptive_timeouts PROPERTY LABELS "quick")

## Legacy adaptive/streaming tests removed in 0.7

# Streaming overlay removed: related tests removed

# Wire auditing is not part of the unit test surface; no corresponding test

# Wire serialization roundtrip tests
add_ctest_exe(ut_wire_roundtrip core/test_wire_roundtrip.c)
set_property(TEST ut_wire_roundtrip PROPERTY LABELS "quick")

# Big-endian simulation roundtrip (forces big-endian code path via define)
add_ctest_exe(ut_wire_big_endian_sim core/test_wire_big_endian_sim.c)
target_compile_definitions(ut_wire_big_endian_sim PRIVATE VAL_FORCE_BIG_ENDIAN=1)
set_property(TEST ut_wire_big_endian_sim PROPERTY LABELS "quick")

# Clock requirement behavior
add_ctest_exe(ut_clock_requirement core/test_clock_requirement.c)
set_property(TEST ut_clock_requirement PROPERTY LABELS "quick")

# Transport fragmentation and reordering/jitter behavior
add_ctest_exe(ut_transport_fragmentation core/test_transport_fragmentation.c)
set_property(TEST ut_transport_fragmentation PROPERTY LABELS "quick")

# Label heavier suites to allow selective runs
set_property(TEST ut_sr_check PROPERTY LABELS "quick")
set_property(TEST ut_recovery_check PROPERTY LABELS "quick")

# Cancel tests
add_ctest_exe(ut_cancel_mid_data_receiver send_receive/test_cancel_mid_data_receiver.c)
set_property(TEST ut_cancel_mid_data_receiver PROPERTY LABELS "quick")
add_ctest_exe(ut_cancel_mid_data_sender send_receive/test_cancel_mid_data_sender.c)
set_property(TEST ut_cancel_mid_data_sender PROPERTY LABELS "quick")

# Integration tests using TCP example binaries over localhost
add_ctest_exe(it_tcp_single integration/test_tcp_single.c)
set_property(TEST it_tcp_single PROPERTY LABELS "normal")
add_ctest_exe(it_tcp_multi integration/test_tcp_multi.c)
set_property(TEST it_tcp_multi PROPERTY LABELS "normal")

# Metrics-specific tests (only meaningful when metrics are compiled in)
if(VAL_ENABLE_METRICS)
    add_ctest_exe(ut_metrics core/test_metrics.c)
    set_property(TEST ut_metrics PROPERTY LABELS "quick")
    add_ctest_exe(ut_metrics_crc core/test_metrics_crc.c)
    set_property(TEST ut_metrics_crc PROPERTY LABELS "normal")
    add_ctest_exe(ut_metrics_rtt core/test_metrics_rtt.c)
    set_property(TEST ut_metrics_rtt PROPERTY LABELS "quick")
    add_ctest_exe(ut_metrics_timeout core/test_metrics_timeout.c)
    set_property(TEST ut_metrics_timeout PROPERTY LABELS "quick")
endif()

# Transport profile simulation tests
# These tests simulate realistic transport conditions (UART, WiFi, Satellite)
# with deterministic behavior for reproducible testing
function(add_transport_profile_test name src)
    add_executable(${name} ${src} support/test_support.c support/transport_profiles.c)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${VAL_UT_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${VAL_UT_OUTPUT_DIR}
    )
    target_include_directories(${name} PRIVATE support include ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE val_protocol)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

add_transport_profile_test(ut_uart_profile transport/test_uart_profile.c)
set_property(TEST ut_uart_profile PROPERTY LABELS "heavy")

add_transport_profile_test(ut_wifi_profile transport/test_wifi_profile.c)
set_property(TEST ut_wifi_profile PROPERTY LABELS "heavy")

add_transport_profile_test(ut_satellite_profile transport/test_satellite_profile.c)
set_property(TEST ut_satellite_profile PROPERTY LABELS "quick")

# Diagnostic test for satellite packet loss investigation
# Opt-in diagnostics: excluded from default builds/runs to keep CI fast and green
option(VAL_BUILD_DIAGNOSTICS "Build diagnostic tests (slow/long-running)." OFF)
if(VAL_BUILD_DIAGNOSTICS)
    add_transport_profile_test(sat_diagnostic transport/test_satellite_diagnostic.c)
    set_property(TEST sat_diagnostic PROPERTY LABELS "diagnostic;heavy")
    # Allow more time if the diagnostic is explicitly enabled
    set_tests_properties(sat_diagnostic PROPERTIES TIMEOUT 120)
endif()

# Stress tests for extreme network conditions (>5% loss, graceful failure validation)
add_transport_profile_test(ut_transport_stress transport/test_transport_stress.c)
set_property(TEST ut_transport_stress PROPERTY LABELS "stress")

# Assert graceful failure occurs within a bounded time under extreme loss
add_transport_profile_test(ut_timebound_failures transport/test_timebound_failures.c)
set_property(TEST ut_timebound_failures PROPERTY LABELS "stress;heavy")
