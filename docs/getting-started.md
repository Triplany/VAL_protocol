# Getting Started with VAL Protocol

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors. Please verify against source code.

---

## Quick Start

This guide will help you get started with VAL Protocol in under 15 minutes.

### Why VAL Protocol?

**VAL excels at:**
- **Embedded file transfers** over UART, USB, RS-485, CAN - where HTTP/FTP aren't available
- **Complete abstraction** - implement custom encryption, compression, hardware CRC, any byte source
- **Adaptive performance** - bounded-window cwnd adjusts to link quality (localhost to satellite)
- **Robust resume** - CRC-verified resume modes with corruption detection

**Not for:** Internet-scale distribution, real-time streaming media, or datagram networks.

## Prerequisites

### Required Tools

- **C Compiler**: GCC, Clang, or MSVC
- **CMake**: Version 3.15 or higher
- **Git**: For cloning the repository

### Supported Platforms

- ✅ Windows (Visual Studio 2017+, MinGW)
- ✅ Linux (Ubuntu, Debian, Fedora, etc.)
- ✅ WSL (Windows Subsystem for Linux)
- ✅ macOS (experimental)
- ✅ Embedded systems with C99 compiler

## Installation

### 1. Clone the Repository

```bash
git clone https://github.com/Triplany/VAL_protocol.git
cd VAL_protocol
```

### 2. Build on Windows

```powershell
# Configure with Visual Studio generator
cmake -S . -B build\windows-debug -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build\windows-debug --config Debug -j

# Run tests
ctest --test-dir build\windows-debug --build-config Debug --output-on-failure
```

### 3. Build on Linux/WSL

```bash
# Configure
cmake -S . -B build/linux-debug

# Build
cmake --build build/linux-debug -j

# Run tests
ctest --test-dir build/linux-debug --output-on-failure
```

### 4. Using CMake Presets

The project includes convenient presets:

```bash
# Windows
cmake --preset windows-debug
cmake --build --preset windows-debug

# Linux
cmake --preset linux-debug
cmake --build --preset linux-debug
```

## Your First File Transfer

### Basic Receiver

```c
#include "val_protocol.h"
#include <stdio.h>
#include <stdint.h>

// Minimal transport: blocking TCP recv wrapper
int my_recv(void *ctx, void *buffer, size_t len, size_t *received, uint32_t timeout_ms) {
    int fd = *(int*)ctx;
    // Your blocking recv implementation here
    // Return 0 and set *received = len on success
    // Return 0 and set *received = 0 on timeout
    // Return <0 on fatal error
    return tcp_recv_exact(fd, buffer, len, timeout_ms, received);
}

int my_send(void *ctx, const void *data, size_t len) {
    int fd = *(int*)ctx;
    return tcp_send_all(fd, data, len);
}

// Monotonic millisecond clock
uint32_t get_ticks_ms(void) {
    // Your platform's monotonic clock
    return platform_get_milliseconds();
}

int main(void) {
    val_config_t cfg = {0};
    
    // Transport
    int sockfd = accept_connection(9000); // Your TCP code
    cfg.transport.send = my_send;
    cfg.transport.recv = my_recv;
    cfg.transport.io_context = &sockfd;
    
    // Filesystem (standard C)
    cfg.filesystem.fopen = (void*(*)(void*, const char*, const char*))fopen;
    cfg.filesystem.fread = (size_t(*)(void*, void*, size_t, size_t, void*))fread;
    cfg.filesystem.fwrite = (size_t(*)(void*, const void*, size_t, size_t, void*))fwrite;
    cfg.filesystem.fseek = my_fseek64;  // 64-bit wrapper function
    cfg.filesystem.ftell = my_ftell64;  // 64-bit wrapper function
    cfg.filesystem.fclose = (int(*)(void*, void*))fclose;
    
    // Clock
    cfg.system.get_ticks_ms = get_ticks_ms;
    
    // Buffers (4 KB MTU)
    uint8_t send_buf[4096], recv_buf[4096];
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = 4096;
    
    // Timeouts (conservative defaults)
    cfg.timeouts.min_timeout_ms = 100;
    cfg.timeouts.max_timeout_ms = 10000;
    
    // Resume: tail verification (robust default)
    cfg.resume.mode = VAL_RESUME_TAIL;
    cfg.resume.tail_cap_bytes = 16384; // 16 KB cap for tail window
    cfg.resume.min_verify_bytes = 0;   // optional
    cfg.resume.mismatch_skip = 0;      // restart on mismatch (set 1 to skip file)
    
    // Create session
    val_session_t *session = NULL;
    val_status_t status = val_session_create(&cfg, &session, NULL);
    if (status != VAL_OK) {
        fprintf(stderr, "Failed to create session: %d\n", status);
        return 1;
    }
    
    // Receive files to output directory
    status = val_receive_files(session, "./received");
    
    // Cleanup
    val_session_destroy(session);
    close(sockfd);
    
    return (status == VAL_OK) ? 0 : 1;
}
```

### Basic Sender

```c
#include "val_protocol.h"
#include <stdio.h>

int main(void) {
    val_config_t cfg = {0};
    
    // ... same transport/filesystem/clock setup as receiver ...
    
    int sockfd = connect_to_receiver("192.168.1.100", 9000);
    cfg.transport.send = my_send;
    cfg.transport.recv = my_recv;
    cfg.transport.io_context = &sockfd;
    
    // ... buffers, timeouts, filesystem ...
    
    val_session_t *session = NULL;
    val_status_t status = val_session_create(&cfg, &session, NULL);
    if (status != VAL_OK) {
        fprintf(stderr, "Failed to create session: %d\n", status);
        return 1;
    }
    
    // Send multiple files
    const char *files[] = {
        "/path/to/file1.bin",
        "/path/to/file2.txt",
        "/path/to/file3.dat"
    };
    
    status = val_send_files(session, files, 3, "/original/path");
    
    val_session_destroy(session);
    close(sockfd);
    
    return (status == VAL_OK) ? 0 : 1;
}
```

## Try the TCP Examples

The repository includes complete TCP examples with all features enabled.

### Start Receiver

```bash
# Windows
.\build\windows-debug\bin\Debug\val_example_receive_tcp.exe 9000 .\received

# Linux
./build/linux-debug/bin/val_example_receive 9000 ./received
```

### Send Files

```bash
# Windows
.\build\windows-debug\bin\Debug\val_example_send_tcp.exe 127.0.0.1 9000 file1.bin file2.txt

# Linux
./build/linux-debug/bin/val_example_send 127.0.0.1 9000 file1.bin file2.txt
```

### With Advanced Options

```bash
# Large MTU, tail resume, verbose logging (bounded-window)
./val_example_send --mtu 32768 --resume tail --log-level debug 192.168.1.100 9000 bigfile.iso

./val_example_receive --mtu 32768 --resume tail --log-level debug 9000 ./downloads
```

## Common Configuration Patterns

### Embedded/MCU Configuration

```c
// Minimal footprint: 1 KB MTU, stop-and-wait
cfg.buffers.packet_size = 1024;
cfg.tx_flow.window_cap_packets = 1;
cfg.tx_flow.initial_cwnd_packets = 1;
cfg.resume.mode = VAL_RESUME_TAIL;
cfg.resume.tail_cap_bytes = 1024; // Small tail cap
```

### High-Speed LAN

```c
// Maximize throughput: large MTU, larger window cap
cfg.buffers.packet_size = (2*1024*1024); // 2 MB
cfg.tx_flow.window_cap_packets = 128;   // if memory allows
cfg.tx_flow.initial_cwnd_packets = 4;   // start conservatively
cfg.resume.mode = VAL_RESUME_TAIL;
```

### Unreliable Link (WiFi/Cellular)

```c
// Conservative: moderate MTU, aggressive error recovery
cfg.buffers.packet_size = 4096;
cfg.tx_flow.window_cap_packets = 16;
cfg.tx_flow.initial_cwnd_packets = 4;
cfg.tx_flow.degrade_error_threshold = 2;     // Quick downgrade
cfg.tx_flow.recovery_success_threshold = 20; // Slow upgrade
cfg.timeouts.min_timeout_ms = 500;
cfg.timeouts.max_timeout_ms = 30000;
```

## Adding Callbacks

### Progress Monitoring

```c
void on_progress(const val_progress_info_t *info) {
    printf("Progress: %llu / %llu bytes (%.1f%%) - %s\n",
        info->bytes_transferred,
        info->total_bytes,
        100.0 * info->bytes_transferred / info->total_bytes,
        info->current_filename);
}

cfg.callbacks.on_progress = on_progress;
```

### File Events

```c
void on_file_start(const char *filename, const char *sender_path,
                   uint64_t file_size, uint64_t resume_offset) {
    printf("Starting: %s (%llu bytes, resume at %llu)\n",
        filename, file_size, resume_offset);
}

void on_file_complete(const char *filename, const char *sender_path,
                      val_status_t result) {
    printf("Completed: %s - %s\n",
        filename, 
        result == VAL_OK ? "SUCCESS" : "FAILED");
}

cfg.callbacks.on_file_start = on_file_start;
cfg.callbacks.on_file_complete = on_file_complete;
```

### Metadata Validation

```c
val_validation_action_t my_validator(const val_meta_payload_t *meta,
                                     const char *target_path,
                                     void *context) {
    // Reject files over 100 MB
    if (meta->file_size > 100 * 1024 * 1024) {
        printf("Rejecting large file: %s (%llu bytes)\n",
            meta->filename, meta->file_size);
        return VAL_VALIDATION_SKIP;
    }
    
    // Reject executable files
    const char *ext = strrchr(meta->filename, '.');
    if (ext && (strcmp(ext, ".exe") == 0 || strcmp(ext, ".dll") == 0)) {
        printf("Rejecting executable: %s\n", meta->filename);
        return VAL_VALIDATION_SKIP;
    }
    
    return VAL_VALIDATION_ACCEPT;
}

val_config_set_validator(&cfg, my_validator, NULL);
```

## Next Steps

- Read the [API Reference](api-reference.md) for detailed function documentation
- See [Implementation Guide](implementation-guide.md) for architecture details
- Check [Examples](examples/) for advanced usage patterns
- Review [Message Formats](message-formats.md) for protocol internals

## Troubleshooting

### Common Issues

**Session creation fails with VAL_ERR_INVALID_ARG**
- Check that all required callbacks are set (transport.send, transport.recv, filesystem.*, system.get_ticks_ms)
- Verify buffer pointers are non-NULL
- Ensure packet_size is within [512, 2*1024*1024] range

**Timeouts during handshake**
- Increase min_timeout_ms (try 500-1000 ms)
- Check network connectivity
- Verify both sides are using compatible packet sizes

**Resume always fails**
- Ensure VAL_RESUME_TAIL mode is selected
- Increase resume.tail_cap_bytes to cover modified regions
- Use VAL_RESUME_NEVER to disable resume and overwrite

See [Troubleshooting Guide](troubleshooting.md) for more help.
