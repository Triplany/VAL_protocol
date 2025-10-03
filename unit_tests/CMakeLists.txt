cmake_minimum_required(VERSION 3.15)

# Always build unit test executables and register them with CTest.

function(add_ctest_exe name src)
    add_executable(${name} ${src} support/test_support.c)
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

# Wire audit (requires VAL_ENABLE_WIRE_AUDIT build option to assert invariants)
add_ctest_exe(ut_wire_audit core/test_wire_audit.c)
set_property(TEST ut_wire_audit PROPERTY LABELS "quick")

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
