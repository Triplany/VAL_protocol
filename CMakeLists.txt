cmake_minimum_required(VERSION 3.15)
project(val_protocol C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Place built artifacts under a flat bin/lib layout inside the build tree
# This avoids Visual Studio's extra per-config subfolder (e.g., Debug/) so
# executables are in <build>/bin and libraries in <build>/lib.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# Ensure multi-config generators (VS) don't append the config subdir
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFGU)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFGU} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFGU} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFGU} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# Build options
option(VAL_ENABLE_ERROR_STRINGS "Build optional error string utilities (host-only)" ON)
option(VAL_ENABLE_METRICS "Enable compile-time protocol metrics counters and timing" OFF)

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
    src/val_wire.c
)
target_include_directories(val_protocol PUBLIC include)
# Build diagnostics and logging controls
# - Logging is private to the library and compiled out (0) for non-Debug configs.
# - Examples/tests can still enable their own logging, but it won’t affect the already-compiled core.
option(VAL_BUILD_DIAGNOSTICS "Enable extra diagnostics (health checks, extra tracing)" OFF)
target_compile_definitions(val_protocol PRIVATE $<$<BOOL:${VAL_BUILD_DIAGNOSTICS}>:VAL_BUILD_DIAGNOSTICS=1>)

# Compile-time log level for the core library: 5 in Debug, 0 otherwise (Release/RelWithDebInfo/MinSizeRel)
target_compile_definitions(val_protocol PRIVATE $<$<CONFIG:Debug>:VAL_LOG_LEVEL=5>)
target_compile_definitions(val_protocol PRIVATE $<$<NOT:$<CONFIG:Debug>>:VAL_LOG_LEVEL=0>)

if(VAL_ENABLE_METRICS)
    target_compile_definitions(val_protocol PUBLIC VAL_ENABLE_METRICS=1)
endif()

 



if(VAL_ENABLE_ERROR_STRINGS)
    add_library(val_error_strings STATIC src/val_error_strings.c)
    target_include_directories(val_error_strings PUBLIC include)
    target_compile_definitions(val_error_strings PUBLIC VAL_ENABLE_ERROR_STRINGS=1)
    # Link into examples/tests but not into the core library to keep MCU footprint minimal
endif()

# ---------------- Examples (cross-platform TCP) ----------------
add_library(example_tcp_common STATIC
    examples/tcp/common/tcp_util.c)
target_include_directories(example_tcp_common PUBLIC examples/tcp/common)

add_executable(val_example_send examples/tcp/val_example_send.c)
target_include_directories(val_example_send PRIVATE examples/tcp/common include)
target_link_libraries(val_example_send PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_send PRIVATE val_error_strings)
endif()
# Always compile the examples with maximum VAL_LOG_LEVEL so core logs are not compiled out
target_compile_definitions(val_example_send PRIVATE VAL_LOG_LEVEL=5)
# Build examples lean even in Debug: optimize and define NDEBUG to avoid full debug noise
if (MSVC)
    target_compile_options(val_example_send PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_send PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_send PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

add_executable(val_example_receive examples/tcp/val_example_receive.c)
target_include_directories(val_example_receive PRIVATE examples/tcp/common include)
target_link_libraries(val_example_receive PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_receive PRIVATE val_error_strings)
endif()
# Always compile the examples with maximum VAL_LOG_LEVEL so core logs are not compiled out
target_compile_definitions(val_example_receive PRIVATE VAL_LOG_LEVEL=5)
if (MSVC)
    target_compile_options(val_example_receive PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_receive PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_receive PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

if (WIN32)
    target_link_libraries(example_tcp_common PUBLIC ws2_32)
endif()

# ---------------- Unit Tests ----------------
enable_testing()

add_library(test_support OBJECT
    unit_tests/support/test_support.c
    unit_tests/support/transport_profiles.c)
target_include_directories(test_support PUBLIC unit_tests/support include)

function(add_val_ut name src)
    add_executable(${name} ${src} $<TARGET_OBJECTS:test_support>)
    target_include_directories(${name} PRIVATE unit_tests/support include)
    target_link_libraries(${name} PRIVATE val_protocol)
    if(VAL_ENABLE_ERROR_STRINGS)
        target_link_libraries(${name} PRIVATE val_error_strings)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_val_ut(ut_send_single unit_tests/send_receive/test_single_file.c)
add_val_ut(ut_send_multi unit_tests/send_receive/test_multi_file.c)
add_val_ut(ut_resume unit_tests/recovery/test_resume.c)
add_val_ut(ut_corruption unit_tests/recovery/test_corruption.c)

# Include the unit tests directory; all tests are plain executables registered with CTest
add_subdirectory(unit_tests)
