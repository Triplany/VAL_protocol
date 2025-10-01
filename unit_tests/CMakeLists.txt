cmake_minimum_required(VERSION 3.15)

# Always build unit test executables and register them with CTest.

function(add_ctest_exe name src)
    add_executable(${name} ${src} support/test_support.c)
    target_include_directories(${name} PRIVATE support include)
    target_link_libraries(${name} PRIVATE val_protocol)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endfunction()

# Add unit tests

add_ctest_exe(ut_sr_check send_receive/suite_send_receive_check.c)
add_ctest_exe(ut_recovery_check recovery/suite_recovery_check.c)
add_ctest_exe(ut_resume_policies recovery/test_resume_policies.c)
set_property(TEST ut_resume_policies PROPERTY LABELS "heavy")

add_ctest_exe(ut_error_system core/test_error_system.c)
add_ctest_exe(ut_transport_optional core/test_transport_optional.c)
add_ctest_exe(ut_packet_negotiation core/test_packet_negotiation.c)
set_property(TEST ut_packet_negotiation PROPERTY LABELS "quick;heavy")
add_ctest_exe(ut_metadata_validation core/test_metadata_validation.c)
set_property(TEST ut_metadata_validation PROPERTY LABELS "quick")

# Adaptive timeout behavior
add_ctest_exe(ut_adaptive_timeouts core/test_adaptive_timeouts.c)
set_property(TEST ut_adaptive_timeouts PROPERTY LABELS "quick")

# Clock requirement behavior
add_ctest_exe(ut_clock_requirement core/test_clock_requirement.c)
set_property(TEST ut_clock_requirement PROPERTY LABELS "quick")

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
