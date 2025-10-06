# VAL Protocol Implementation Guide

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors. Please verify against source code.

---

_Dedicated to Valerie Lee - for all her support over the years allowing me to chase my ideas._

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Building from Source](#building-from-source)
3. [Integration Patterns](#integration-patterns)
4. [Platform-Specific Considerations](#platform-specific-considerations)
5. [Performance Tuning](#performance-tuning)
6. [Memory Management](#memory-management)
7. [Threading and Concurrency](#threading-and-concurrency)
8. [Security Considerations](#security-considerations)

---

## Architecture Overview

### Abstraction Layer - VAL's Key Advantage

VAL Protocol achieves complete separation between protocol logic and platform implementation through three callback interfaces:

**1. Transport Layer Abstraction**
```c
cfg.transport.send = my_send;    // Any byte stream: TCP, UART, USB, SPI
cfg.transport.recv = my_recv;    // Blocking receive with timeout
cfg.transport.io_context = ctx;  // Your custom context
```
**Enables:** Encryption wrapper, compression, custom framing, any transport hardware

**2. Filesystem Layer Abstraction**
```c
cfg.filesystem.fopen = my_fopen;   // Any byte source/sink
cfg.filesystem.fread = my_fread;   // Files, RAM, flash, network buffers
cfg.filesystem.fwrite = my_fwrite; // Streaming compression/decompression
cfg.filesystem.fseek = my_fseek;
cfg.filesystem.ftell = my_ftell;
cfg.filesystem.fclose = my_fclose;
```
**Enables:** In-memory transfers, encrypted storage, compressed files, custom formats

**3. System Layer Abstraction**
```c
cfg.system.get_ticks_ms = my_clock;  // Monotonic clock
cfg.crc.crc32 = my_crc;              // Hardware CRC acceleration
cfg.system.malloc = my_alloc;        // Custom allocators
```
**Enables:** Hardware acceleration, deterministic timing, zero-allocation operation

### Core Components

```
val_protocol/
├── include/              # Public API headers
│   ├── val_protocol.h    # Main API (abstraction interfaces)
│   ├── val_errors.h      # Error codes and detail masks
│   └── val_error_strings.h # Optional string utilities (host-only)
├── src/                  # Implementation
│   ├── val_core.c        # Session management, streaming mode, adaptive TX
│   ├── val_sender.c      # Sender-side logic, continuous transmission
│   ├── val_receiver.c    # Receiver-side logic, ACK coalescing
│   ├── val_error_strings.c # Optional error strings
│   └── val_internal.h    # Internal structures
└── examples/             # Example implementations
    └── tcp/              # TCP transport examples
```

### Module Responsibilities

**val_core.c**:
- Session creation/destruction
- CRC computation (software fallback)
- String sanitization
- Logging infrastructure
- Adaptive timeout (RFC 6298)
- Low-level packet send/receive

**val_sender.c**:
- Handshake (sender role)
- File metadata transmission
- Resume negotiation (sender side)
- Windowed data transmission
- Adaptive mode management
- Progress tracking

**val_receiver.c**:
- Handshake (receiver role)
- Metadata validation
- Resume decision logic
- Data reception and ACKing
- CRC verification

---

## Building from Source

### CMake Build System

**Project Structure:**
```
CMakeLists.txt              # Root configuration
CMakePresets.json           # Build presets
unit_tests/CMakeLists.txt   # Test configuration
```

**Build Options:**
```cmake
VAL_ENABLE_ERROR_STRINGS=ON   # Build error string utilities (default: ON)
VAL_ENABLE_METRICS=OFF        # Enable metrics collection (default: OFF)
# Use the runtime packet capture hook via config.capture.on_packet for non-intrusive wire observation.
VAL_LOG_LEVEL=4               # Compile-time log level 0-5 (default: 4 debug, 0 release)
```

### Windows Build (Visual Studio)

```powershell
# Configure
cmake -S . -B build\windows-debug -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build\windows-debug --config Debug -j

# Install (optional)
cmake --install build\windows-debug --prefix .\install

# Run tests
ctest --test-dir build\windows-debug --build-config Debug -j
```

**With Metrics:**
```powershell
cmake -S . -B build\windows-metrics ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DVAL_ENABLE_METRICS=ON
cmake --build build\windows-metrics --config Release -j
```

### Linux Build (GCC/Clang)

```bash
# Configure
cmake -S . -B build/linux-debug -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build/linux-debug -j$(nproc)

# Install
sudo cmake --install build/linux-debug

# Run tests
ctest --test-dir build/linux-debug -j$(nproc)
```

**Cross-Compilation Example (ARM):**
```bash
cmake -S . -B build/arm-debug \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVAL_ENABLE_METRICS=OFF

cmake --build build/arm-debug -j
```

### Embedded Build (Bare Metal)

For bare-metal systems, integrate source files directly:

**Files to Include:**
```
src/val_core.c
src/val_sender.c
src/val_receiver.c
```

**Optional:**
```
src/val_error_strings.c  (only if VAL_ENABLE_ERROR_STRINGS=1)
```

**Build Flags:**
```
-DVAL_LOG_LEVEL=0          # Disable logging
-DVAL_ENABLE_METRICS=0     # Disable metrics
```

**Example Makefile:**
```makefile
CFLAGS += -DVAL_LOG_LEVEL=0 -DVAL_ENABLE_METRICS=0
CFLAGS += -Ipath/to/val_protocol/include

SOURCES += val_protocol/src/val_core.c
SOURCES += val_protocol/src/val_sender.c
SOURCES += val_protocol/src/val_receiver.c

OBJECTS = $(SOURCES:.c=.o)

myapp: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)
```

---

## Integration Patterns

### 1. TCP Integration

See `examples/tcp/` for complete reference implementation.

**Key Components:**
- `tcp_util.c/h`: Blocking TCP wrappers
- `val_example_send.c`: Sender with full feature set
- `val_example_receive.c`: Receiver with validation

**Transport Callbacks:**
```c
int tp_send(void *ctx, const void *data, size_t len) {
    int fd = *(int*)ctx;
    return tcp_send_all(fd, data, len) == 0 ? (int)len : -1;
}

int tp_recv(void *ctx, void *buffer, size_t size,
            size_t *received, uint32_t timeout_ms) {
    int fd = *(int*)ctx;
    int rc = tcp_recv_exact(fd, buffer, size, timeout_ms);
    if (rc != 0) {
        *received = 0;  // Timeout
        return 0;
    }
    *received = size;
    return 0;
}

int tp_is_connected(void *ctx) {
    int fd = *(int*)ctx;
    return tcp_is_connected(fd);
}

void tp_flush(void *ctx) {
    int fd = *(int*)ctx;
    tcp_flush(fd);
}
```

### 2. UART Integration (Embedded)

**Example for STM32:**
```c
typedef struct {
    UART_HandleTypeDef *huart;
    uint32_t timeout_default;
} uart_context_t;

int uart_send(void *ctx, const void *data, size_t len) {
    uart_context_t *uctx = (uart_context_t*)ctx;
    HAL_StatusTypeDef status = HAL_UART_Transmit(
        uctx->huart, (uint8_t*)data, len, uctx->timeout_default);
    return (status == HAL_OK) ? (int)len : -1;
}

int uart_recv(void *ctx, void *buffer, size_t size,
              size_t *received, uint32_t timeout_ms) {
    uart_context_t *uctx = (uart_context_t*)ctx;
    HAL_StatusTypeDef status = HAL_UART_Receive(
        uctx->huart, (uint8_t*)buffer, size, timeout_ms);
    
    if (status == HAL_TIMEOUT) {
        *received = 0;
        return 0;  // Protocol will retry
    }
    if (status != HAL_OK) {
        return -1;  // Fatal error
    }
    *received = size;
    return 0;
}

uint32_t get_ticks_ms(void) {
    return HAL_GetTick();
}
```

### 3. USB CDC Integration

**Example for USB serial:**
```c
int usb_send(void *ctx, const void *data, size_t len) {
    // USB CDC transmit
    uint8_t result = CDC_Transmit_FS((uint8_t*)data, len);
    return (result == USBD_OK) ? (int)len : -1;
}

int usb_recv(void *ctx, void *buffer, size_t size,
             size_t *received, uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    size_t got = 0;
    
    while (got < size) {
        if ((HAL_GetTick() - start) >= timeout_ms) {
            *received = 0;
            return 0;  // Timeout
        }
        
        // Check USB receive buffer
        uint8_t byte;
        if (usb_cdc_get_byte(&byte)) {
            ((uint8_t*)buffer)[got++] = byte;
        } else {
            // Small delay to avoid busy-wait
            HAL_Delay(1);
        }
    }
    
    *received = size;
    return 0;
}
```

### 4. File I/O Integration

**Standard C Library:**
```c
cfg.filesystem.fopen = (void*(*)(void*, const char*, const char*))fopen;
cfg.filesystem.fread = (size_t(*)(void*, void*, size_t, size_t, void*))fread;
cfg.filesystem.fwrite = (size_t(*)(void*, const void*, size_t, size_t, void*))fwrite;
cfg.filesystem.fseek = (int(*)(void*, void*, long, int))fseek;
cfg.filesystem.ftell = (long(*)(void*, void*))ftell;
cfg.filesystem.fclose = (int(*)(void*, void*))fclose;
cfg.filesystem.fs_context = NULL;
```

**Custom Filesystem (e.g., FatFS):**
```c
void* fatfs_fopen(void *ctx, const char *path, const char *mode) {
    FIL *fp = malloc(sizeof(FIL));
    if (!fp) return NULL;
    
    BYTE open_mode = 0;
    if (strchr(mode, 'r')) open_mode |= FA_READ;
    if (strchr(mode, 'w')) open_mode |= FA_WRITE | FA_CREATE_ALWAYS;
    if (strchr(mode, '+')) open_mode |= FA_READ | FA_WRITE;
    
    if (f_open(fp, path, open_mode) != FR_OK) {
        free(fp);
        return NULL;
    }
    return fp;
}

int fatfs_fread(void *ctx, void *buffer, size_t size,
                size_t count, void *file) {
    FIL *fp = (FIL*)file;
    UINT br;
    size_t total = size * count;
    if (f_read(fp, buffer, total, &br) != FR_OK) return -1;
    return (int)(br / size);
}

// ... similar for fwrite, fseek, ftell, fclose ...

cfg.filesystem.fopen = fatfs_fopen;
cfg.filesystem.fread = fatfs_fread;
// ... etc ...
```

---

## Platform-Specific Considerations

### Windows (MSVC)

**Clock Implementation:**
```c
#include <windows.h>

uint32_t windows_get_ticks_ms(void) {
    return (uint32_t)GetTickCount64();
}
```

**Delay:**
```c
void windows_delay_ms(uint32_t ms) {
    Sleep(ms);
}
```

### Linux/POSIX

**Clock Implementation:**
```c
#include <time.h>

uint32_t linux_get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
```

**Delay:**
```c
#include <time.h>

void linux_delay_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}
```

### STM32 (Bare Metal)

**Clock (HAL):**
```c
uint32_t stm32_get_ticks_ms(void) {
    return HAL_GetTick();
}
```

**Delay:**
```c
void stm32_delay_ms(uint32_t ms) {
    HAL_Delay(ms);
}
```

**CRC Hardware Acceleration (STM32 with hardware CRC):**
```c
uint32_t stm32_crc32(void *ctx, const void *data, size_t length) {
    CRC_HandleTypeDef *hcrc = (CRC_HandleTypeDef*)ctx;
    
    // Reset CRC peripheral
    __HAL_CRC_DR_RESET(hcrc);
    
    // Calculate CRC
    uint32_t crc = HAL_CRC_Calculate(hcrc, (uint32_t*)data, length / 4);
    
    // Handle remainder bytes if any
    // ... (implementation depends on HW CRC configuration)
    
    return crc;
}

cfg.crc.crc32 = stm32_crc32;
cfg.crc.crc_context = &hcrc_instance;
```

### ESP32 (ESP-IDF)

**Clock:**
```c
#include "esp_timer.h"

uint32_t esp32_get_ticks_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
```

**Delay:**
```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void esp32_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
```

---

## Performance Tuning

### MTU Selection

**Choosing Packet Size:**

| Environment | Recommended MTU | Rationale |
|-------------|----------------|-----------|
| Ethernet LAN | 8192-32768 | Minimize overhead, max throughput |
| WiFi | 4096-8192 | Balance throughput and packet loss |
| 4G/LTE | 2048-4096 | Account for varying conditions |
| UART/Serial | 512-2048 | Limited buffer sizes |
| Satellite | 1024-2048 | High latency, minimize retransmit cost |

**Trade-offs:**
- Larger MTU = higher throughput, but larger retransmit penalty
- Smaller MTU = more overhead, but faster recovery from errors

### Timeout Configuration

**Guidelines by Link Type:**

| Link Type | min_timeout_ms | max_timeout_ms |
|-----------|----------------|----------------|
| Localhost/Loopback | 50 | 2000 |
| Fast LAN | 100 | 5000 |
| WiFi/WAN | 200 | 10000 |
| Cellular/4G | 500 | 20000 |
| Satellite | 1000 | 60000 |

### Adaptive TX Tuning

**High-Speed, Low-Loss Links (LAN):**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_32;
cfg.adaptive_tx.allow_streaming = 1;
cfg.adaptive_tx.degrade_error_threshold = 5;
cfg.adaptive_tx.recovery_success_threshold = 10;
```

**Moderate Links (WiFi):**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_32;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
cfg.adaptive_tx.allow_streaming = 1;
cfg.adaptive_tx.degrade_error_threshold = 3;
cfg.adaptive_tx.recovery_success_threshold = 15;
```

**Constrained/Unreliable (Cellular, Embedded):**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_8;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_2;
cfg.adaptive_tx.allow_streaming = 0;
cfg.adaptive_tx.degrade_error_threshold = 2;
cfg.adaptive_tx.recovery_success_threshold = 20;
```

---

## Memory Management

### Static Memory Profile

**Per-Session Overhead:**
```c
sizeof(val_session_t) ≈ 500 bytes (platform-dependent)
```

**Tracking Slots (Adaptive TX):**
```
max_window_size * sizeof(val_inflight_packet_t)
= max_window * 32 bytes

Example: 64-packet window = 2 KB
```

**Total Static Footprint:**
```
Session structure:        ~500 bytes
Tracking slots (64):      2 KB
Send buffer (user):       MTU bytes (e.g., 4 KB)
Recv buffer (user):       MTU bytes (e.g., 4 KB)
----------------------------------------------
Total:                    ~10.5 KB for 4K MTU
```

### Custom Allocators

**Provide Custom Allocator:**
```c
void* my_alloc(size_t size, void *context) {
    // Your allocation logic
    return custom_malloc(size);
}

void my_free(void *ptr, void *context) {
    custom_free(ptr);
}

cfg.adaptive_tx.allocator.alloc = my_alloc;
cfg.adaptive_tx.allocator.free = my_free;
cfg.adaptive_tx.allocator.context = &my_heap;
```

**Embedded Pool Allocator Example:**
```c
typedef struct {
    uint8_t pool[8192];
    size_t used;
} pool_allocator_t;

void* pool_alloc(size_t size, void *context) {
    pool_allocator_t *pool = (pool_allocator_t*)context;
    if (pool->used + size > sizeof(pool->pool)) {
        return NULL;  // Out of memory
    }
    void *ptr = &pool->pool[pool->used];
    pool->used += size;
    return ptr;
}

void pool_free(void *ptr, void *context) {
    // Simple pool: no individual frees
    // Reset entire pool between sessions
}

static pool_allocator_t my_pool = {0};
cfg.adaptive_tx.allocator.alloc = pool_alloc;
cfg.adaptive_tx.allocator.free = pool_free;
cfg.adaptive_tx.allocator.context = &my_pool;
```

---

## Threading and Concurrency

### Thread Safety

**Session is NOT Thread-Safe:**
- Do not call session functions from multiple threads simultaneously
- Each session should be used by a single thread

**Safe Patterns:**

**1. Single-Threaded:**
```c
// All operations in one thread
main_thread() {
    val_session_t *session = ...;
    val_send_files(session, files, count, path);
    val_session_destroy(session);
}
```

**2. Worker Thread:**
```c
// Session lives in dedicated thread
void* transfer_thread(void *arg) {
    val_session_t *session = ...;
    val_send_files(session, files, count, path);
    val_session_destroy(session);
    return NULL;
}

main() {
    pthread_t thread;
    pthread_create(&thread, NULL, transfer_thread, NULL);
    pthread_join(thread, NULL);
}
```

**3. Multiple Sessions (Parallel):**
```c
// Each session in its own thread (safe)
void* sender_thread(void *arg) {
    val_session_t *session1 = create_session(&cfg1);
    val_send_files(session1, ...);
    val_session_destroy(session1);
}

void* receiver_thread(void *arg) {
    val_session_t *session2 = create_session(&cfg2);
    val_receive_files(session2, ...);
    val_session_destroy(session2);
}
```

**Callback Reentrancy:**
- Callbacks execute in session's thread
- Do NOT call session functions from within callbacks
- Callbacks should be quick (no blocking operations)

---

## Security Considerations

### Path Traversal Prevention

**Receiver-Side Protection:**
```c
// NEVER do this:
char bad_path[512];
snprintf(bad_path, sizeof(bad_path), "%s/%s",
         output_dir, meta->sender_path);  // UNSAFE!

// Always do this:
char sanitized[128];
val_clean_filename(meta->filename, sanitized, sizeof(sanitized));

char safe_path[512];
snprintf(safe_path, sizeof(safe_path), "%s/%s",
         output_dir, sanitized);  // SAFE
```

**Validation Example:**
```c
val_validation_action_t secure_validator(
    const val_meta_payload_t *meta,
    const char *target_path,
    void *context) {
    
    // Reject absolute paths
    if (meta->filename[0] == '/' || meta->filename[0] == '\\') {
        return VAL_VALIDATION_ABORT;
    }
    
    // Reject parent directory references
    if (strstr(meta->filename, "..")) {
        return VAL_VALIDATION_ABORT;
    }
    
    // Reject files outside allowed directory
    char resolved[PATH_MAX];
    if (realpath(target_path, resolved) == NULL) {
        return VAL_VALIDATION_ABORT;
    }
    
    char allowed[PATH_MAX];
    realpath("/allowed/directory", allowed);
    
    if (strncmp(resolved, allowed, strlen(allowed)) != 0) {
        return VAL_VALIDATION_ABORT;
    }
    
    return VAL_VALIDATION_ACCEPT;
}
```

### Transport Security

**TLS Integration Example:**
```c
// Use OpenSSL or mbedTLS for TLS
int tls_send(void *ctx, const void *data, size_t len) {
    SSL *ssl = (SSL*)ctx;
    int sent = SSL_write(ssl, data, len);
    return (sent > 0) ? sent : -1;
}

int tls_recv(void *ctx, void *buffer, size_t size,
             size_t *received, uint32_t timeout_ms) {
    SSL *ssl = (SSL*)ctx;
    
    // Set socket timeout
    set_socket_timeout(SSL_get_fd(ssl), timeout_ms);
    
    int r = SSL_read(ssl, buffer, size);
    if (r == (int)size) {
        *received = size;
        return 0;
    }
    
    // Check if timeout vs error
    int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        *received = 0;
        return 0;  // Timeout
    }
    
    return -1;  // Fatal error
}
```

### Resource Limits

**File Size Limits:**
```c
val_validation_action_t size_limiter(
    const val_meta_payload_t *meta,
    const char *target_path,
    void *context) {
    
    const uint64_t MAX_FILE_SIZE = 1ULL * 1024 * 1024 * 1024; // 1 GB
    
    if (meta->file_size > MAX_FILE_SIZE) {
        log_security("Rejected oversized file: %s (%llu bytes)",
                    meta->filename, meta->file_size);
        return VAL_VALIDATION_SKIP;
    }
    
    return VAL_VALIDATION_ACCEPT;
}
```

**Rate Limiting:**
```c
// Implement at transport or application level
typedef struct {
    uint32_t bytes_per_second_limit;
    uint32_t bytes_this_second;
    uint32_t last_reset_time;
} rate_limiter_t;

int rate_limited_send(void *ctx, const void *data, size_t len) {
    rate_limiter_t *rl = (rate_limiter_t*)ctx;
    
    uint32_t now = get_ticks_ms();
    if ((now - rl->last_reset_time) >= 1000) {
        rl->bytes_this_second = 0;
        rl->last_reset_time = now;
    }
    
    if (rl->bytes_this_second + len > rl->bytes_per_second_limit) {
        // Rate limit exceeded; delay or drop
        return -1;
    }
    
    int result = actual_send(data, len);
    if (result > 0) {
        rl->bytes_this_second += result;
    }
    return result;
}
```

---

## See Also

- [API Reference](api-reference.md)
- [Protocol Specification](protocol-specification.md)
- [Getting Started](getting-started.md)
- [Troubleshooting](troubleshooting.md)
