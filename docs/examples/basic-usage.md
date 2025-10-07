# VAL Protocol - Basic Usage Examples

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance. Verify against actual examples in `examples/tcp/`.

---

## Minimal Sender Example

This example shows the absolute minimum code needed to send a file using VAL Protocol.

```c
#include "val_protocol.h"
#include <stdio.h>
#include <stdint.h>

// Your transport callbacks (blocking TCP example)
int my_send(void *ctx, const void *data, size_t len) {
    int fd = *(int*)ctx;
    // TODO: Implement blocking send that sends exactly 'len' bytes
    // Return bytes sent (should equal len) or <0 on error
    return send_all(fd, data, len);
}

int my_recv(void *ctx, void *buffer, size_t size,
            size_t *received, uint32_t timeout_ms) {
    int fd = *(int*)ctx;
    // TODO: Implement blocking receive with timeout
    // On success: return 0, set *received = size
    // On timeout: return 0, set *received = 0  
    // On error: return <0
    return recv_exact(fd, buffer, size, timeout_ms, received);
}

// Monotonic millisecond clock (REQUIRED)
uint32_t get_ticks_ms(void) {
    // Windows: return GetTickCount64();
    // Linux: clock_gettime(CLOCK_MONOTONIC, ...)
    // Embedded: HAL_GetTick() or equivalent
    return platform_get_milliseconds();
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port> <file>\n", argv[0]);
        return 1;
    }
    
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *filepath = argv[3];
    
    // 1. Connect to receiver
    int sockfd = tcp_connect(host, port);
    if (sockfd < 0) {
        fprintf(stderr, "Connection failed\n");
        return 1;
    }
    
    // 2. Configure VAL session
    val_config_t cfg = {0};
    
    // Transport
    cfg.transport.send = my_send;
    cfg.transport.recv = my_recv;
    cfg.transport.io_context = &sockfd;
    
    // Filesystem (use standard C library)
    cfg.filesystem.fopen = (void*(*)(void*, const char*, const char*))fopen;
    cfg.filesystem.fread = (size_t(*)(void*, void*, size_t, size_t, void*))fread;
    cfg.filesystem.fwrite = (size_t(*)(void*, const void*, size_t, size_t, void*))fwrite;
    cfg.filesystem.fseek = (int(*)(void*, void*, long, int))fseek;
    cfg.filesystem.ftell = (long(*)(void*, void*))ftell;
    cfg.filesystem.fclose = (int(*)(void*, void*))fclose;
    
    // Clock
    cfg.system.get_ticks_ms = get_ticks_ms;
    
    // Buffers (allocate on stack or heap)
    uint8_t send_buf[4096];
    uint8_t recv_buf[4096];
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = 4096;
    
    // Timeouts (reasonable defaults for LAN)
    cfg.timeouts.min_timeout_ms = 100;
    cfg.timeouts.max_timeout_ms = 10000;
    
    // Resume (simple: always overwrite)
    cfg.resume.mode = VAL_RESUME_NEVER;
    
    // 3. Create session
    val_session_t *session = NULL;
    val_status_t status = val_session_create(&cfg, &session, NULL);
    if (status != VAL_OK) {
        fprintf(stderr, "Session creation failed: %d\n", status);
        tcp_close(sockfd);
        return 1;
    }
    
    // 4. Send file
    printf("Sending %s...\n", filepath);
    const char *files[] = { filepath };
    status = val_send_files(session, files, 1, NULL);
    
    // 5. Cleanup
    val_session_destroy(session);
    tcp_close(sockfd);
    
    if (status == VAL_OK) {
        printf("Transfer complete!\n");
        return 0;
    } else {
        fprintf(stderr, "Transfer failed: %d\n", status);
        return 1;
    }
}
```

---

## Minimal Receiver Example

```c
#include "val_protocol.h"
#include <stdio.h>

// (Same transport and clock implementations as sender)

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <output_dir>\n", argv[0]);
        return 1;
    }
    
    int port = atoi(argv[1]);
    const char *output_dir = argv[2];
    
    // 1. Listen for connection
    int listen_fd = tcp_listen(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Listen failed\n");
        return 1;
    }
    
    printf("Listening on port %d...\n", port);
    int sockfd = tcp_accept(listen_fd);
    tcp_close(listen_fd);
    
    if (sockfd < 0) {
        fprintf(stderr, "Accept failed\n");
        return 1;
    }
    
    printf("Connection accepted\n");
    
    // 2. Configure VAL session (same as sender, except resume mode)
    val_config_t cfg = {0};
    
    // Transport
    cfg.transport.send = my_send;
    cfg.transport.recv = my_recv;
    cfg.transport.io_context = &sockfd;
    
    // Filesystem
    cfg.filesystem.fopen = (void*(*)(void*, const char*, const char*))fopen;
    cfg.filesystem.fread = (size_t(*)(void*, void*, size_t, size_t, void*))fread;
    cfg.filesystem.fwrite = (size_t(*)(void*, const void*, size_t, size_t, void*))fwrite;
    cfg.filesystem.fseek = (int(*)(void*, void*, long, int))fseek;
    cfg.filesystem.ftell = (long(*)(void*, void*))ftell;
    cfg.filesystem.fclose = (int(*)(void*, void*))fclose;
    
    // Clock
    cfg.system.get_ticks_ms = get_ticks_ms;
    
    // Buffers
    uint8_t send_buf[4096];
    uint8_t recv_buf[4096];
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = 4096;
    
    // Timeouts
    cfg.timeouts.min_timeout_ms = 100;
    cfg.timeouts.max_timeout_ms = 10000;
    
    // Resume (tail-or-zero: robust default)
    cfg.resume.mode = VAL_RESUME_TAIL;
    cfg.resume.tail_cap_bytes = 16384;  // 16 KB cap
    cfg.resume.min_verify_bytes = 0;
    cfg.resume.mismatch_skip = 0;       // restart on mismatch
    
    // 3. Create session
    val_session_t *session = NULL;
    val_status_t status = val_session_create(&cfg, &session, NULL);
    if (status != VAL_OK) {
        fprintf(stderr, "Session creation failed: %d\n", status);
        tcp_close(sockfd);
        return 1;
    }
    
    // 4. Receive files
    printf("Ready to receive files to %s\n", output_dir);
    status = val_receive_files(session, output_dir);
    
    // 5. Cleanup
    val_session_destroy(session);
    tcp_close(sockfd);
    
    if (status == VAL_OK) {
        printf("Transfer complete!\n");
        return 0;
    } else {
        fprintf(stderr, "Transfer failed: %d\n", status);
        return 1;
    }
}
```

---

## Adding Progress Callback

```c
void on_progress(const val_progress_info_t *info) {
    if (info->total_bytes > 0) {
        double percent = 100.0 * info->bytes_transferred / info->total_bytes;
        uint32_t rate_kbps = info->transfer_rate_bps / 1024;
        
        printf("\r[%.1f%%] %s - %u KB/s (ETA %u sec)   ",
               percent,
               info->current_filename,
               rate_kbps,
               info->eta_seconds);
        fflush(stdout);
    }
}

// In main():
cfg.callbacks.on_progress = on_progress;
```

**Output:**
```
[45.2%] largefile.bin - 2048 KB/s (ETA 120 sec)
```

---

## Adding File Event Callbacks

```c
void on_file_start(const char *filename, const char *sender_path,
                   uint64_t file_size, uint64_t resume_offset) {
    printf("\n=== Starting: %s ===\n", filename);
    printf("Size: %llu bytes\n", file_size);
    if (resume_offset > 0) {
        printf("Resuming from: %llu bytes\n", resume_offset);
    }
}

void on_file_complete(const char *filename, const char *sender_path,
                      val_status_t result) {
    if (result == VAL_OK) {
        printf("\n✓ Completed: %s\n", filename);
    } else if (result == VAL_SKIPPED) {
        printf("\n⊘ Skipped: %s\n", filename);
    } else {
        printf("\n✗ Failed: %s (error %d)\n", filename, result);
    }
}

// In main():
cfg.callbacks.on_file_start = on_file_start;
cfg.callbacks.on_file_complete = on_file_complete;
```

---

## Multiple Files in One Session

```c
const char *files[] = {
    "/data/file1.bin",
    "/data/file2.txt",
    "/data/file3.dat",
    "/data/file4.jpg"
};

val_status_t status = val_send_files(session, files, 4, "/data");

// All files sent in one session with single handshake
```

---

## Resume config: simple, robust defaults

- Modes:
    - VAL_RESUME_NEVER: always restart from 0 and overwrite local files.
    - VAL_RESUME_SKIP_EXISTING: if a local file exists with non-zero size, skip it.
    - VAL_RESUME_TAIL: verify a tail window of the existing local file and resume from its end on match.

- Recommended defaults (LAN/USB):
    - cfg.resume.mode = VAL_RESUME_TAIL
    - cfg.resume.tail_cap_bytes = 8 * 1024 * 1024  // 8 MiB cap
    - cfg.resume.min_verify_bytes = 0               // no floor beyond tail cap
    - cfg.resume.mismatch_skip = 0                  // on mismatch, restart from 0

- For embedded/flash-constrained targets:
    - Use smaller caps (e.g., 64 KiB to 1 MiB) to reduce read I/O during verify.
    - Keep mismatch_skip = 0 to ensure a clean restart if verification fails.

- For high-latency/WAN links where re-sending is expensive:
    - Increase tail_cap_bytes (e.g., 16–64 MiB) to make false matches less likely.
    - Optionally set mismatch_skip = 1 to emulate "skip on mismatch" behavior.

Notes:
- The receiver uses only the configured recv buffer and negotiated packet size to compute the tail CRC. No full-file revalidation occurs at DONE.
- If the local file is larger than the incoming size, the mismatch policy applies: restart (0) or skip depending on mismatch_skip.

---

## Error Handling

```c
val_status_t status = val_send_files(session, files, count, path);

if (status != VAL_OK && status != VAL_SKIPPED) {
    // Get detailed error information
    val_status_t code = 0;
    uint32_t detail = 0;
    val_get_last_error(session, &code, &detail);
    
    fprintf(stderr, "Transfer failed: code=%d detail=0x%08X\n", code, detail);
    
    // Check error categories
    if (VAL_ERROR_IS_NETWORK_RELATED(detail)) {
        fprintf(stderr, "Network error detected\n");
        if (detail & VAL_ERROR_DETAIL_TIMEOUT_ACK) {
            fprintf(stderr, "ACK timeout - check network latency\n");
        }
    }
    
    if (VAL_ERROR_IS_CRC_RELATED(detail)) {
        fprintf(stderr, "CRC error - possible data corruption\n");
    }
    
    if (VAL_ERROR_IS_FILESYSTEM_RELATED(detail)) {
        fprintf(stderr, "Filesystem error\n");
        if (detail & VAL_ERROR_DETAIL_DISK_FULL) {
            fprintf(stderr, "Disk full\n");
        }
    }
    
    return 1;
}
```

---

## Embedded Example (STM32)

```c
// Minimal embedded receiver for STM32

UART_HandleTypeDef huart1;
CRC_HandleTypeDef hcrc;

typedef struct {
    UART_HandleTypeDef *huart;
} uart_ctx_t;

static uart_ctx_t uart_ctx = { &huart1 };

int uart_send(void *ctx, const void *data, size_t len) {
    uart_ctx_t *uctx = (uart_ctx_t*)ctx;
    HAL_StatusTypeDef st = HAL_UART_Transmit(uctx->huart, (uint8_t*)data, len, 5000);
    return (st == HAL_OK) ? (int)len : -1;
}

int uart_recv(void *ctx, void *buffer, size_t size,
              size_t *received, uint32_t timeout_ms) {
    uart_ctx_t *uctx = (uart_ctx_t*)ctx;
    HAL_StatusTypeDef st = HAL_UART_Receive(uctx->huart, buffer, size, timeout_ms);
    
    if (st == HAL_TIMEOUT) {
        *received = 0;
        return 0;
    }
    if (st != HAL_OK) {
        return -1;
    }
    *received = size;
    return 0;
}

uint32_t stm32_get_ticks_ms(void) {
    return HAL_GetTick();
}

// Optional: Hardware CRC acceleration
uint32_t stm32_crc32(uint32_t seed, const void *data, size_t len) {
    __HAL_CRC_DR_RESET(&hcrc_instance);
    HAL_CRC_Accumulate(&hcrc_instance, &seed, 1);
    return HAL_CRC_Calculate(&hcrc_instance, (uint32_t*)data, len / 4);
}

void receiver_task(void) {
    val_config_t cfg = {0};
    
    // Transport: UART
    cfg.transport.send = uart_send;
    cfg.transport.recv = uart_recv;
    cfg.transport.io_context = &uart_ctx;
    
    // Filesystem: FatFS (implement wrappers)
    cfg.filesystem.fopen = fatfs_fopen;
    cfg.filesystem.fread = fatfs_fread;
    cfg.filesystem.fwrite = fatfs_fwrite;
    cfg.filesystem.fseek = fatfs_fseek;
    cfg.filesystem.ftell = fatfs_ftell;
    cfg.filesystem.fclose = fatfs_fclose;
    
    // Clock
    cfg.system.get_ticks_ms = stm32_get_ticks_ms;
    
    // CRC: Hardware accelerated
    cfg.crc.crc32 = stm32_crc32;
    cfg.crc.crc_context = &hcrc;
    
    // Small buffers for MCU (1 KB)
    static uint8_t send_buf[1024];
    static uint8_t recv_buf[1024];
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = 1024;
    
    // Conservative timeouts for UART
    cfg.timeouts.min_timeout_ms = 500;
    cfg.timeouts.max_timeout_ms = 30000;
    
    // Conservative adaptive TX (small window)
    cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_4;
    cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_2;
    cfg.adaptive_tx.allow_streaming = 0;
    
    // Simple resume
    cfg.resume.mode = VAL_RESUME_TAIL;
    cfg.resume.tail_cap_bytes = 1024;
    
    // Create session
    val_session_t *session = NULL;
    if (val_session_create(&cfg, &session, NULL) != VAL_OK) {
        Error_Handler();
    }
    
    // Receive to SD card
    val_status_t status = val_receive_files(session, "0:/downloads");
    
    val_session_destroy(session);
    
    if (status == VAL_OK) {
        HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);
    }
}
```

---

## See Also

- [Advanced Features](advanced-features.md) - Adaptive TX, streaming, diagnostics
- [Integration Examples](integration-examples.md) - Platform-specific implementations
- [API Reference](../api-reference.md) - Complete API documentation
- [Getting Started](../getting-started.md) - Full setup guide
