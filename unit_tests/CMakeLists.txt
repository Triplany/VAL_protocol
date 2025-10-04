cmake_minimum_required(VERSION 3.15)

# Always build unit test executables and register them with CTest.

function(add_ctest_exe name src)
    # Include common test support and transport simulation helpers in all unit tests
    add_executable(${name} ${src} support/test_support.c support/transport_profiles.c)
    # Allow unit tests to include public and selected internal headers for white-box testing
    target_include_directories(${name} PRIVATE support include ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE val_protocol)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

# Add unit tests

add_ctest_exe(ut_sr_check send_receive/suite_send_receive_check.c)
add_ctest_exe(ut_recovery_check recovery/suite_recovery_check.c)
add_ctest_exe(ut_resume_policies recovery/test_resume_policies.c)
set_property(TEST ut_resume_policies PROPERTY LABELS "heavy")

# Comprehensive resume modes coverage
add_ctest_exe(ut_resume_modes recovery/test_resume_modes.c)
set_property(TEST ut_resume_modes PROPERTY LABELS "heavy")

add_ctest_exe(ut_error_system core/test_error_system.c)
add_ctest_exe(ut_transport_optional core/test_transport_optional.c)
add_ctest_exe(ut_packet_negotiation core/test_packet_negotiation.c)
set_property(TEST ut_packet_negotiation PROPERTY LABELS "quick;heavy")
add_ctest_exe(ut_metadata_validation core/test_metadata_validation.c)
set_property(TEST ut_metadata_validation PROPERTY LABELS "quick")

# Adaptive timeout behavior
add_ctest_exe(ut_adaptive_timeouts core/test_adaptive_timeouts.c)
set_property(TEST ut_adaptive_timeouts PROPERTY LABELS "quick")

# Adaptive transmission behavior
add_ctest_exe(ut_adaptive_transmission core/test_adaptive_transmission.c)
set_property(TEST ut_adaptive_transmission PROPERTY LABELS "heavy")

# Split adaptive streaming transitions into focused tests
add_ctest_exe(ut_adaptive_escalate core/test_adaptive_escalate.c)
set_property(TEST ut_adaptive_escalate PROPERTY LABELS "quick")
add_ctest_exe(ut_adaptive_deescalate core/test_adaptive_deescalate.c)
set_property(TEST ut_adaptive_deescalate PROPERTY LABELS "quick")
add_ctest_exe(ut_adaptive_reescalate core/test_adaptive_reescalate.c)
set_property(TEST ut_adaptive_reescalate PROPERTY LABELS "quick")

# Adaptive: escalate to streaming, degrade to windowed, and recover back
# Remove the combined escalation test in favor of the split ones

# Adaptive transmission demo (lightweight, quick)
add_ctest_exe(ut_adaptive_demo core/test_adaptive_demo.c)
set_property(TEST ut_adaptive_demo PROPERTY LABELS "quick")

# Initial mode negotiation behavior
add_ctest_exe(ut_initial_mode_negotiation core/test_initial_mode_negotiation.c)
set_property(TEST ut_initial_mode_negotiation PROPERTY LABELS "quick")

# Streaming negotiation/veto behavior
add_ctest_exe(ut_streaming_veto core/test_streaming_veto.c)
set_property(TEST ut_streaming_veto PROPERTY LABELS "quick")

# Positive streaming negotiation (both sides allow streaming)
add_ctest_exe(ut_streaming_allowed core/test_streaming_allowed.c)
set_property(TEST ut_streaming_allowed PROPERTY LABELS "quick")

# Streaming ACK cadence (requires wire audit to assert counts; otherwise still runs as integrity check)
add_ctest_exe(ut_streaming_ack_cadence core/test_streaming_ack_cadence.c)
set_property(TEST ut_streaming_ack_cadence PROPERTY LABELS "quick")

# Wire audit (requires VAL_ENABLE_WIRE_AUDIT build option to assert invariants)
add_ctest_exe(ut_wire_audit core/test_wire_audit.c)
set_property(TEST ut_wire_audit PROPERTY LABELS "quick")

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
add_ctest_exe(ut_transport_reorder_jitter core/test_transport_reorder_jitter.c)
set_property(TEST ut_transport_reorder_jitter PROPERTY LABELS "quick")

# Label heavier suites to allow selective runs
set_property(TEST ut_sr_check PROPERTY LABELS "heavy")
set_property(TEST ut_recovery_check PROPERTY LABELS "heavy")

# Cancel tests
add_ctest_exe(ut_cancel_mid_data_receiver send_receive/test_cancel_mid_data_receiver.c)
set_property(TEST ut_cancel_mid_data_receiver PROPERTY LABELS "heavy")
add_ctest_exe(ut_cancel_mid_data_sender send_receive/test_cancel_mid_data_sender.c)
set_property(TEST ut_cancel_mid_data_sender PROPERTY LABELS "heavy")

# Integration tests using TCP example binaries over localhost
add_ctest_exe(it_tcp_single integration/test_tcp_single.c)
set_property(TEST it_tcp_single PROPERTY LABELS "heavy")
add_ctest_exe(it_tcp_multi integration/test_tcp_multi.c)
set_property(TEST it_tcp_multi PROPERTY LABELS "heavy")

# Metrics-specific tests (only meaningful when metrics are compiled in)
if(VAL_ENABLE_METRICS)
    add_ctest_exe(ut_metrics core/test_metrics.c)
    set_property(TEST ut_metrics PROPERTY LABELS "quick")
    add_ctest_exe(ut_metrics_crc core/test_metrics_crc.c)
    set_property(TEST ut_metrics_crc PROPERTY LABELS "quick")
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
    target_include_directories(${name} PRIVATE support include ${CMAKE_CURRENT_LIST_DIR}/../src ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE val_protocol)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

add_transport_profile_test(ut_uart_profile transport/test_uart_profile.c)
set_property(TEST ut_uart_profile PROPERTY LABELS "quick")

add_transport_profile_test(ut_wifi_profile transport/test_wifi_profile.c)
set_property(TEST ut_wifi_profile PROPERTY LABELS "quick")

add_transport_profile_test(ut_satellite_profile transport/test_satellite_profile.c)
set_property(TEST ut_satellite_profile PROPERTY LABELS "heavy")

# Diagnostic test for satellite packet loss investigation
add_transport_profile_test(sat_diagnostic transport/test_satellite_diagnostic.c)

# Stress tests for extreme network conditions (>5% loss, graceful failure validation)
add_transport_profile_test(ut_transport_stress transport/test_transport_stress.c)
set_property(TEST ut_transport_stress PROPERTY LABELS "stress")
