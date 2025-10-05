# VAL Protocol - Integration Examples

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance. Test thoroughly in your target environment.

---

## Complete TCP Integration (Windows/Linux)

### Platform-Agnostic TCP Wrapper

```c
// tcp_transport.h
#ifndef TCP_TRANSPORT_H
#define TCP_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

typedef struct tcp_context tcp_context_t;

// Create TCP context
tcp_context_t* tcp_create(void);
void tcp_destroy(tcp_context_t *ctx);

// Client: connect to server
int tcp_connect(tcp_context_t *ctx, const char *host, uint16_t port);

// Server: bind, listen, accept
int tcp_listen(tcp_context_t *ctx, uint16_t port);
int tcp_accept(tcp_context_t *ctx, tcp_context_t **client_ctx);

// Close connection
void tcp_close(tcp_context_t *ctx);

// VAL-compatible send/recv
int tcp_val_send(void *ctx, const void *data, size_t len);
int tcp_val_recv(void *ctx, void *buffer, size_t size,
                 size_t *received, uint32_t timeout_ms);

#endif // TCP_TRANSPORT_H
```

```c
// tcp_transport_windows.c
#ifdef _WIN32

#include "tcp_transport.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

typedef struct tcp_context {
    SOCKET sock;
    int initialized;
} tcp_context_t;

static int tcp_startup(void) {
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return -1;
        }
        started = 1;
    }
    return 0;
}

tcp_context_t* tcp_create(void) {
    if (tcp_startup() < 0) return NULL;
    
    tcp_context_t *ctx = calloc(1, sizeof(tcp_context_t));
    if (ctx) {
        ctx->sock = INVALID_SOCKET;
        ctx->initialized = 1;
    }
    return ctx;
}

void tcp_destroy(tcp_context_t *ctx) {
    if (ctx) {
        tcp_close(ctx);
        free(ctx);
    }
}

int tcp_connect(tcp_context_t *ctx, const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return -1;
    }
    
    ctx->sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ctx->sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        return -1;
    }
    
    if (connect(ctx->sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
        return -1;
    }
    
    freeaddrinfo(result);
    return 0;
}

int tcp_listen(tcp_context_t *ctx, uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    ctx->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ctx->sock == INVALID_SOCKET) {
        return -1;
    }
    
    if (bind(ctx->sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
        return -1;
    }
    
    if (listen(ctx->sock, 1) == SOCKET_ERROR) {
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
        return -1;
    }
    
    return 0;
}

int tcp_accept(tcp_context_t *ctx, tcp_context_t **client_ctx) {
    *client_ctx = tcp_create();
    if (!*client_ctx) return -1;
    
    (*client_ctx)->sock = accept(ctx->sock, NULL, NULL);
    if ((*client_ctx)->sock == INVALID_SOCKET) {
        tcp_destroy(*client_ctx);
        *client_ctx = NULL;
        return -1;
    }
    
    return 0;
}

void tcp_close(tcp_context_t *ctx) {
    if (ctx && ctx->sock != INVALID_SOCKET) {
        closesocket(ctx->sock);
        ctx->sock = INVALID_SOCKET;
    }
}

int tcp_val_send(void *ctx, const void *data, size_t len) {
    tcp_context_t *tcp_ctx = (tcp_context_t*)ctx;
    size_t total_sent = 0;
    
    while (total_sent < len) {
        int sent = send(tcp_ctx->sock,
                       (const char*)data + total_sent,
                       (int)(len - total_sent),
                       0);
        if (sent <= 0) {
            return -1;
        }
        total_sent += sent;
    }
    
    return (int)total_sent;
}

int tcp_val_recv(void *ctx, void *buffer, size_t size,
                 size_t *received, uint32_t timeout_ms) {
    tcp_context_t *tcp_ctx = (tcp_context_t*)ctx;
    
    // Set timeout
    DWORD timeout = timeout_ms;
    setsockopt(tcp_ctx->sock, SOL_SOCKET, SO_RCVTIMEO,
              (const char*)&timeout, sizeof(timeout));
    
    size_t total_received = 0;
    while (total_received < size) {
        int rcvd = recv(tcp_ctx->sock,
                       (char*)buffer + total_received,
                       (int)(size - total_received),
                       0);
        
        if (rcvd == 0) {
            // Connection closed
            return -1;
        }
        if (rcvd < 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                *received = total_received;
                return 0;  // Timeout (VAL expects 0 with received=0)
            }
            return -1;
        }
        
        total_received += rcvd;
    }
    
    *received = total_received;
    return 0;
}

uint32_t get_ticks_ms(void) {
    return (uint32_t)GetTickCount64();
}

#endif // _WIN32
```

```c
// tcp_transport_linux.c
#ifndef _WIN32

#include "tcp_transport.h"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct tcp_context {
    int sock;
} tcp_context_t;

tcp_context_t* tcp_create(void) {
    tcp_context_t *ctx = calloc(1, sizeof(tcp_context_t));
    if (ctx) {
        ctx->sock = -1;
    }
    return ctx;
}

void tcp_destroy(tcp_context_t *ctx) {
    if (ctx) {
        tcp_close(ctx);
        free(ctx);
    }
}

int tcp_connect(tcp_context_t *ctx, const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return -1;
    }
    
    ctx->sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (ctx->sock < 0) {
        freeaddrinfo(result);
        return -1;
    }
    
    if (connect(ctx->sock, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        close(ctx->sock);
        ctx->sock = -1;
        return -1;
    }
    
    freeaddrinfo(result);
    return 0;
}

int tcp_listen(tcp_context_t *ctx, uint16_t port) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->sock < 0) {
        return -1;
    }
    
    int reuse = 1;
    setsockopt(ctx->sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    if (bind(ctx->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(ctx->sock);
        ctx->sock = -1;
        return -1;
    }
    
    if (listen(ctx->sock, 1) < 0) {
        close(ctx->sock);
        ctx->sock = -1;
        return -1;
    }
    
    return 0;
}

int tcp_accept(tcp_context_t *ctx, tcp_context_t **client_ctx) {
    *client_ctx = tcp_create();
    if (!*client_ctx) return -1;
    
    (*client_ctx)->sock = accept(ctx->sock, NULL, NULL);
    if ((*client_ctx)->sock < 0) {
        tcp_destroy(*client_ctx);
        *client_ctx = NULL;
        return -1;
    }
    
    return 0;
}

void tcp_close(tcp_context_t *ctx) {
    if (ctx && ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
}

int tcp_val_send(void *ctx, const void *data, size_t len) {
    tcp_context_t *tcp_ctx = (tcp_context_t*)ctx;
    size_t total_sent = 0;
    
    while (total_sent < len) {
        ssize_t sent = send(tcp_ctx->sock,
                           (const char*)data + total_sent,
                           len - total_sent,
                           0);
        if (sent <= 0) {
            return -1;
        }
        total_sent += sent;
    }
    
    return (int)total_sent;
}

int tcp_val_recv(void *ctx, void *buffer, size_t size,
                 size_t *received, uint32_t timeout_ms) {
    tcp_context_t *tcp_ctx = (tcp_context_t*)ctx;
    
    // Set timeout using select
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    size_t total_received = 0;
    while (total_received < size) {
        FD_ZERO(&readfds);
        FD_SET(tcp_ctx->sock, &readfds);
        
        int ready = select(tcp_ctx->sock + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            return -1;
        }
        if (ready == 0) {
            // Timeout
            *received = total_received;
            return 0;
        }
        
        ssize_t rcvd = recv(tcp_ctx->sock,
                           (char*)buffer + total_received,
                           size - total_received,
                           0);
        
        if (rcvd == 0) {
            return -1;  // Connection closed
        }
        if (rcvd < 0) {
            return -1;
        }
        
        total_received += rcvd;
    }
    
    *received = total_received;
    return 0;
}

uint32_t get_ticks_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif // !_WIN32
```

### Using the TCP Transport

```c
#include "val_protocol.h"
#include "tcp_transport.h"

int main(int argc, char **argv) {
    tcp_context_t *tcp = tcp_create();
    
    if (tcp_connect(tcp, "192.168.1.100", 5555) < 0) {
        fprintf(stderr, "Connection failed\n");
        return 1;
    }
    
    val_config_t cfg = {0};
    cfg.transport.send = tcp_val_send;
    cfg.transport.recv = tcp_val_recv;
    cfg.transport.io_context = tcp;
    cfg.system.get_ticks_ms = get_ticks_ms;
    
    // ... rest of VAL setup ...
    
    val_session_t *session = NULL;
    val_session_create(&cfg, &session, NULL);
    
    const char *files[] = { "test.bin" };
    val_send_files(session, files, 1, NULL);
    
    val_session_destroy(session);
    tcp_destroy(tcp);
    
    return 0;
}
```

---

## STM32 + FatFS + FreeRTOS Integration

### FatFS Wrappers

```c
// fatfs_val_wrappers.c
#include "val_protocol.h"
#include "ff.h"
#include <string.h>

void* fatfs_fopen(void *ctx, const char *path, const char *mode) {
    FIL *fp = malloc(sizeof(FIL));
    if (!fp) return NULL;
    
    BYTE fatfs_mode = 0;
    if (strchr(mode, 'r')) fatfs_mode |= FA_READ;
    if (strchr(mode, 'w')) fatfs_mode |= FA_WRITE | FA_CREATE_ALWAYS;
    if (strchr(mode, 'a')) fatfs_mode |= FA_WRITE | FA_OPEN_APPEND;
    if (strchr(mode, '+')) fatfs_mode |= FA_READ | FA_WRITE;
    
    if (f_open(fp, path, fatfs_mode) != FR_OK) {
        free(fp);
        return NULL;
    }
    
    return fp;
}

int fatfs_fread(void *ctx, void *buffer, size_t size, size_t count, void *fp) {
    UINT bytes_read = 0;
    FRESULT res = f_read((FIL*)fp, buffer, size * count, &bytes_read);
    return (res == FR_OK) ? (int)(bytes_read / size) : 0;
}

int fatfs_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *fp) {
    UINT bytes_written = 0;
    FRESULT res = f_write((FIL*)fp, buffer, size * count, &bytes_written);
    return (res == FR_OK) ? (int)(bytes_written / size) : 0;
}

int fatfs_fseek(void *ctx, void *fp, long offset, int origin) {
    FIL *file = (FIL*)fp;
    FSIZE_t new_pos;
    
    if (origin == SEEK_SET) {
        new_pos = offset;
    } else if (origin == SEEK_CUR) {
        new_pos = f_tell(file) + offset;
    } else { // SEEK_END
        new_pos = f_size(file) + offset;
    }
    
    return (f_lseek(file, new_pos) == FR_OK) ? 0 : -1;
}

long fatfs_ftell(void *ctx, void *fp) {
    return (long)f_tell((FIL*)fp);
}

int fatfs_fclose(void *ctx, void *fp) {
    FRESULT res = f_close((FIL*)fp);
    free(fp);
    return (res == FR_OK) ? 0 : EOF;
}
```

### UART Transport for STM32

```c
// uart_val_transport.c
#include "main.h"  // HAL headers
#include "val_protocol.h"

extern UART_HandleTypeDef huart1;

typedef struct {
    UART_HandleTypeDef *huart;
} uart_ctx_t;

int uart_val_send(void *ctx, const void *data, size_t len) {
    uart_ctx_t *uctx = (uart_ctx_t*)ctx;
    HAL_StatusTypeDef status = HAL_UART_Transmit(uctx->huart, (uint8_t*)data, len, 10000);
    return (status == HAL_OK) ? (int)len : -1;
}

int uart_val_recv(void *ctx, void *buffer, size_t size,
                  size_t *received, uint32_t timeout_ms) {
    uart_ctx_t *uctx = (uart_ctx_t*)ctx;
    HAL_StatusTypeDef status = HAL_UART_Receive(uctx->huart, buffer, size, timeout_ms);
    
    if (status == HAL_TIMEOUT) {
        *received = 0;
        return 0;  // VAL expects 0 on timeout
    }
    if (status != HAL_OK) {
        return -1;
    }
    
    *received = size;
    return 0;
}
```

### FreeRTOS Task

```c
// val_receiver_task.c
#include "FreeRTOS.h"
#include "task.h"
#include "val_protocol.h"

extern CRC_HandleTypeDef hcrc;
extern UART_HandleTypeDef huart1;

void val_receiver_task(void *pvParameters) {
    uart_ctx_t uart_ctx = { &huart1 };
    
    val_config_t cfg = {0};
    
    // Transport: UART
    cfg.transport.send = uart_val_send;
    cfg.transport.recv = uart_val_recv;
    cfg.transport.io_context = &uart_ctx;
    
    // Filesystem: FatFS
    cfg.filesystem.fopen = fatfs_fopen;
    cfg.filesystem.fread = fatfs_fread;
    cfg.filesystem.fwrite = fatfs_fwrite;
    cfg.filesystem.fseek = fatfs_fseek;
    cfg.filesystem.ftell = fatfs_ftell;
    cfg.filesystem.fclose = fatfs_fclose;
    
    // Clock
    cfg.system.get_ticks_ms = (uint32_t(*)(void))HAL_GetTick;
    
    // CRC: Hardware
    cfg.crc.crc32 = stm32_crc32;
    cfg.crc.crc_context = &hcrc;
    
    // Buffers (static allocation)
    static uint8_t send_buf[2048];
    static uint8_t recv_buf[2048];
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = 2048;
    
    // Timeouts (conservative for UART)
    cfg.timeouts.min_timeout_ms = 500;
    cfg.timeouts.max_timeout_ms = 30000;
    
    // Adaptive TX (small window)
    cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_4;
    cfg.adaptive_tx.allow_streaming = 0;
    
    // Resume
    cfg.resume.mode = VAL_RESUME_TAIL;
    cfg.resume.tail_cap_bytes = 2048;
    cfg.resume.min_verify_bytes = 0;
    cfg.resume.mismatch_skip = 0;
    
    // Create session
    val_session_t *session = NULL;
    if (val_session_create(&cfg, &session, NULL) != VAL_OK) {
        Error_Handler();
    }
    
    // Receive files to SD card
    HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_SET);
    
    val_status_t status = val_receive_files(session, "0:/downloads");
    
    val_session_destroy(session);
    
    HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_RESET);
    
    if (status == VAL_OK) {
        HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(LED_RED_GPIO_Port, LED_RED_Pin, GPIO_PIN_SET);
    }
    
    // Task complete
    vTaskDelete(NULL);
}
```

---

## ESP32 WiFi Integration

```c
// esp32_wifi_val.c
#include "val_protocol.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "VAL";

typedef struct {
    int sock;
} wifi_ctx_t;

int wifi_val_send(void *ctx, const void *data, size_t len) {
    wifi_ctx_t *wctx = (wifi_ctx_t*)ctx;
    size_t total_sent = 0;
    
    while (total_sent < len) {
        int sent = send(wctx->sock, (const char*)data + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            ESP_LOGE(TAG, "Send failed");
            return -1;
        }
        total_sent += sent;
    }
    
    return total_sent;
}

int wifi_val_recv(void *ctx, void *buffer, size_t size,
                  size_t *received, uint32_t timeout_ms) {
    wifi_ctx_t *wctx = (wifi_ctx_t*)ctx;
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(wctx->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    size_t total_received = 0;
    while (total_received < size) {
        int rcvd = recv(wctx->sock, (char*)buffer + total_received, size - total_received, 0);
        
        if (rcvd == 0) {
            ESP_LOGE(TAG, "Connection closed");
            return -1;
        }
        if (rcvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                *received = total_received;
                return 0;  // Timeout
            }
            ESP_LOGE(TAG, "Recv error: %d", errno);
            return -1;
        }
        
        total_received += rcvd;
    }
    
    *received = total_received;
    return 0;
}

uint32_t esp_get_ticks_ms(void) {
    return esp_timer_get_time() / 1000;
}

void val_task(void *pvParameters) {
    // Connect to WiFi first (not shown)
    
    // Create TCP connection
    wifi_ctx_t wifi_ctx;
    wifi_ctx.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5555);
    inet_pton(AF_INET, "192.168.1.100", &server_addr.sin_addr);
    
    if (connect(wifi_ctx.sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Connect failed");
        close(wifi_ctx.sock);
        vTaskDelete(NULL);
        return;
    }
    
    // Configure VAL
    val_config_t cfg = {0};
    cfg.transport.send = wifi_val_send;
    cfg.transport.recv = wifi_val_recv;
    cfg.transport.io_context = &wifi_ctx;
    cfg.system.get_ticks_ms = esp_get_ticks_ms;
    
    // ... rest of configuration ...
    
    val_session_t *session = NULL;
    val_session_create(&cfg, &session, NULL);
    
    const char *files[] = { "/spiffs/data.bin" };
    val_status_t status = val_send_files(session, files, 1, "/spiffs");
    
    val_session_destroy(session);
    close(wifi_ctx.sock);
    
    ESP_LOGI(TAG, "Transfer %s", status == VAL_OK ? "OK" : "FAILED");
    
    vTaskDelete(NULL);
}
```

---

## Python Integration (via ctypes)

```python
# val_protocol.py
import ctypes
import os
from ctypes import *

# Load shared library
val_lib = CDLL("./libval_protocol.so")  # or .dll on Windows

# Define structures
class ValConfig(Structure):
    pass  # TODO: Match C structure layout

class ValSession(Structure):
    pass

# Function prototypes
val_session_create = val_lib.val_session_create
val_session_create.argtypes = [POINTER(ValConfig), POINTER(POINTER(ValSession)), c_void_p]
val_session_create.restype = c_int

val_send_files = val_lib.val_send_files
val_send_files.argtypes = [POINTER(ValSession), POINTER(c_char_p), c_size_t, c_char_p]
val_send_files.restype = c_int

val_session_destroy = val_lib.val_session_destroy
val_session_destroy.argtypes = [POINTER(ValSession)]

# Example usage
def send_file(host, port, filepath):
    # TODO: Implement transport callbacks in Python
    # This requires wrapping Python functions as C callbacks
    pass
```

---

## See Also

- [Basic Usage](basic-usage.md) - Simple examples
- [Advanced Features](advanced-features.md) - Adaptive TX, metrics, debugging
- [Implementation Guide](../implementation-guide.md) - Detailed integration instructions
- [API Reference](../api-reference.md) - Complete API
