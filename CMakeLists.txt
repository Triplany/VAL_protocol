cmake_minimum_required(VERSION 3.15)
project(val_protocol C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
elseif(UNIX)
    # Linux-specific settings
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
endif()

add_library(val_protocol STATIC
    src/val_core.c
    src/val_sender.c
    src/val_receiver.c
)
target_include_directories(val_protocol PUBLIC include)

# ---------------- Examples (cross-platform TCP) ----------------
add_library(example_tcp_common STATIC
    examples/tcp/common/tcp_util.c)
target_include_directories(example_tcp_common PUBLIC examples/tcp/common)

add_executable(val_example_send examples/tcp/val_example_send.c)
target_include_directories(val_example_send PRIVATE examples/tcp/common include)
target_link_libraries(val_example_send PRIVATE val_protocol example_tcp_common)

add_executable(val_example_receive examples/tcp/val_example_receive.c)
target_include_directories(val_example_receive PRIVATE examples/tcp/common include)
target_link_libraries(val_example_receive PRIVATE val_protocol example_tcp_common)

if (WIN32)
    target_link_libraries(example_tcp_common PUBLIC ws2_32)
endif()

# ---------------- Unit Tests ----------------
enable_testing()

add_library(test_support OBJECT
    unit_tests/support/test_support.c)
target_include_directories(test_support PUBLIC unit_tests/support include)

function(add_val_ut name src)
    add_executable(${name} ${src} $<TARGET_OBJECTS:test_support>)
    target_include_directories(${name} PRIVATE unit_tests/support include)
    target_link_libraries(${name} PRIVATE val_protocol)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_val_ut(ut_send_single unit_tests/send_receive/test_single_file.c)
add_val_ut(ut_send_multi unit_tests/send_receive/test_multi_file.c)
add_val_ut(ut_resume unit_tests/recovery/test_resume.c)
add_val_ut(ut_corruption unit_tests/recovery/test_corruption.c)

# Include the unit tests directory; all tests are plain executables registered with CTest
add_subdirectory(unit_tests)
