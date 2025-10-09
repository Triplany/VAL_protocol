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
# Allow the library to include internal headers located in src/ when building the target.
# On some toolchains (notably Unix GCC/Clang) the private source headers must be explicitly
# added to the target include path so public headers that include internal headers will
# resolve correctly during compilation.
target_include_directories(val_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src)
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

# Streaming overlay removed in protocol v0.7 — no build-time switch remains

 



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

add_executable(val_example_send_tcp examples/tcp/val_example_send_tcp.c)
target_include_directories(val_example_send_tcp PRIVATE examples/tcp/common include)
target_link_libraries(val_example_send_tcp PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_send_tcp PRIVATE val_error_strings)
endif()
# Always compile the examples with maximum VAL_LOG_LEVEL so core logs are not compiled out
target_compile_definitions(val_example_send_tcp PRIVATE VAL_LOG_LEVEL=5)
# Build examples lean even in Debug: optimize and define NDEBUG to avoid full debug noise
if (MSVC)
    target_compile_options(val_example_send_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_send_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_send_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

add_executable(val_example_receive_tcp examples/tcp/val_example_receive_tcp.c)
target_include_directories(val_example_receive_tcp PRIVATE examples/tcp/common include)
target_link_libraries(val_example_receive_tcp PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_receive_tcp PRIVATE val_error_strings)
endif()
# Always compile the examples with maximum VAL_LOG_LEVEL so core logs are not compiled out
target_compile_definitions(val_example_receive_tcp PRIVATE VAL_LOG_LEVEL=5)
if (MSVC)
    target_compile_options(val_example_receive_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_receive_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_receive_tcp PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# COM/Serial examples (PTY on Linux, COM ports on Windows)
add_executable(val_example_send_com examples/tcp/val_example_send_com.c)
target_include_directories(val_example_send_com PRIVATE examples/tcp/common include)
target_link_libraries(val_example_send_com PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_send_com PRIVATE val_error_strings)
endif()
target_compile_definitions(val_example_send_com PRIVATE VAL_LOG_LEVEL=5)
if (MSVC)
    target_compile_options(val_example_send_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_send_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_send_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

add_executable(val_example_receive_com examples/tcp/val_example_receive_com.c)
target_include_directories(val_example_receive_com PRIVATE examples/tcp/common include)
target_link_libraries(val_example_receive_com PRIVATE val_protocol example_tcp_common)
if(VAL_ENABLE_ERROR_STRINGS)
    target_link_libraries(val_example_receive_com PRIVATE val_error_strings)
endif()
target_compile_definitions(val_example_receive_com PRIVATE VAL_LOG_LEVEL=5)
if (MSVC)
    target_compile_options(val_example_receive_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(val_example_receive_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
endif()
target_compile_definitions(val_example_receive_com PRIVATE $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

if (WIN32)
    target_link_libraries(example_tcp_common PUBLIC ws2_32)
endif()

# ---------------- Unit Tests ----------------
# Unit tests are configured in the unit_tests subdirectory which places
# their executables under ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/unit_tests.
enable_testing()
add_subdirectory(unit_tests)
