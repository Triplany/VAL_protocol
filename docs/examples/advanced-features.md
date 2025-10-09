# VAL Protocol - Advanced Features (v0.7)

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance. Verify behavior through testing.

---

## Flow Control in v0.7

VAL uses a bounded window with cwnd-based adaptation (AIMD). Configure via `cfg.tx_flow` and monitor with `val_get_cwnd_packets()`.

```c
cfg.tx_flow.window_cap_packets = 64;    // negotiate to min(local, peer_rx_cap)
cfg.tx_flow.initial_cwnd_packets = 4;   // 0 = auto
cfg.tx_flow.degrade_error_threshold = 3;
cfg.tx_flow.recovery_success_threshold = 10;
cfg.tx_flow.retransmit_cache_enabled = true;
```

Tips:
- Keep caps small on MCUs (2–8) to minimize tracking memory
- Increase MTU for high-throughput links
- Tune timeouts based on RTT characteristics

---

## Packet Capture Hook (Debug)

```c
static void capture(void *ctx, const val_packet_record_t *r) {
    fprintf(stderr, "[%s] t=%u type=%u pay=%u off=%llu cwnd?\n",
        r->direction==VAL_DIR_TX?"TX":"RX", r->timestamp_ms, r->type,
        r->payload_len, (unsigned long long)r->offset);
}
cfg.capture.on_packet = capture;
```

---

## Resume Modes

### Full CRC Resume (Full-Prefix with Fallback)

```c
cfg.resume.mode = VAL_RESUME_TAIL;
cfg.resume.tail_cap_bytes = 8 * 1024 * 1024;  // 8 MiB cap (example)
cfg.resume.min_verify_bytes = 0;
cfg.resume.mismatch_skip = 1;  // choose skip-on-mismatch policy
```

**Behavior:**
- Verifies full existing file **up to 256 MB cap**
- Files >256 MB: uses large-tail verification (last 256 MB)
- Resume only if CRC matches sender's partial file CRC
- **Slower** but most secure for files under cap
- Best for critical data

---

### Tail CRC Resume (Balanced)

```c
cfg.resume.mode = VAL_RESUME_TAIL;
cfg.resume.tail_cap_bytes = 65536;  // Last 64 KB cap (internally clamped)
```

**Behavior:**
- Verify only last N bytes of file (capped at 2 MB to avoid timeouts)
- If CRC matches: resume from that point
- If CRC mismatch: truncate to zero and restart
- **Recommended default** - fast and reliable

---

### Fast Resume (Trust Size)

```c
cfg.resume.mode = VAL_RESUME_SIZE_MATCH_OR_ZERO;
```

**Behavior:**
- If file size matches expected: resume
- If size mismatch: truncate and restart
- No CRC verification (fast but risky)

---

### Always Overwrite

```c
cfg.resume.mode = VAL_RESUME_NEVER;
```

**Behavior:**
- Existing files always overwritten from start
- Use for development/testing

---

### Custom Resume Decision

```c
int my_resume_decision(void *ctx, const val_resume_info_t *info) {
    // info->filename, info->file_size, info->existing_size, etc.
    
    if (info->existing_size == info->file_size) {
        // File complete, skip it
        return VAL_RESUME_SKIP;
    }
    
    if (info->existing_size > info->file_size) {
        // Corrupted? Start over
        return VAL_RESUME_ZERO;
    }
    
    if (info->existing_size < 1024) {
        // Too small to bother resuming
        return VAL_RESUME_ZERO;
    }
    
    // Trust the size
    return VAL_RESUME_SIZE;
}

cfg.resume.custom_resume_handler = my_resume_decision;
cfg.resume.mode = VAL_RESUME_CALLBACK;
```

---

## Metrics and Diagnostics

### Enable Metrics (Compile-Time)

```bash
cmake -B build -DVAL_ENABLE_METRICS=ON
cmake --build build
```

### Query Metrics

```c
#ifdef VAL_ENABLE_METRICS

val_metrics_t metrics = {0};
val_get_metrics(session, &metrics);

printf("=== VAL Protocol Metrics ===\n");
printf("Packets sent:        %u\n", metrics.packets_sent);
printf("Packets received:    %u\n", metrics.packets_received);
printf("Retransmissions:     %u\n", metrics.retransmissions);
printf("Payload bytes:       %llu\n", metrics.payload_bytes_sent);
printf("Protocol overhead:   %llu bytes\n", metrics.protocol_overhead_bytes);

double efficiency = 100.0 * metrics.payload_bytes_sent /
                    (metrics.payload_bytes_sent + metrics.protocol_overhead_bytes);
printf("Efficiency:          %.1f%%\n", efficiency);

printf("\n=== Timing ===\n");
printf("Average RTT:         %u ms\n", metrics.average_rtt_ms);
printf("Current timeout:     %u ms\n", metrics.current_timeout_ms);
printf("Smoothed RTT:        %u ms\n", metrics.smoothed_rtt_ms);
printf("RTT variance:        %u ms\n", metrics.rtt_variance_ms);

printf("\n=== Health ===\n");
printf("Timeout events:      %u (soft=%u hard=%u)\n",
    metrics.timeouts, metrics.timeouts_soft, metrics.timeouts_hard);
printf("CRC failures:        %u\n", metrics.crc_failures);
printf("Out-of-sequence:     %u\n", metrics.out_of_sequence_count);

printf("\n=== Window ===\n");
printf("Current mode:        %s\n", mode_name(metrics.current_tx_mode));
printf("Max window used:     %u\n", metrics.max_window_size_used);

#endif
```

---

### Health Monitoring Callback

```c
void on_health_check(const val_health_status_t *health) {
    if (health->error_rate_percent > 10.0) {
        fprintf(stderr, "WARNING: Error rate %.1f%% (threshold 10%%)\n",
                health->error_rate_percent);
    }
    
    if (health->timeout_rate_percent > 5.0) {
        fprintf(stderr, "WARNING: Timeout rate %.1f%% (threshold 5%%)\n",
                health->timeout_rate_percent);
    }
    
    if (health->retransmit_rate_percent > 15.0) {
        fprintf(stderr, "WARNING: Retransmit rate %.1f%% (threshold 15%%)\n",
                health->retransmit_rate_percent);
    }
    
    // Optional: Force more conservative mode
    if (health->error_rate_percent > 20.0) {
    }
}

cfg.callbacks.on_health_check = on_health_check;
```

---

----

## Custom Memory Allocators

### Stack-Based Allocation

```c
void* stack_alloc(void *ctx, size_t size) {
    // VAL doesn't actually allocate anything dynamically by default
    // This is for custom user data if needed
    return NULL;
}

void stack_free(void *ctx, void *ptr) {
    // No-op
}

cfg.system.malloc = stack_alloc;
cfg.system.free = stack_free;
```

**Note:** VAL Protocol does NOT dynamically allocate memory by default. All buffers are provided by user in `val_config_t`.

---

### Custom Context for User Data

```c
typedef struct {
    int connection_id;
    uint64_t start_time_ms;
    FILE *log_file;
} app_context_t;

app_context_t app_ctx = {
    .connection_id = 42,
    .start_time_ms = get_time_ms(),
    .log_file = fopen("transfer.log", "w")
};

// Attach to session
val_status_t status = val_session_create(&cfg, &session, &app_ctx);

// Access in callbacks
void on_progress(const val_progress_info_t *info) {
    app_context_t *ctx = (app_context_t*)val_get_user_context(session);
    fprintf(ctx->log_file, "[%d] Progress: %.1f%%\n",
            ctx->connection_id,
            100.0 * info->bytes_transferred / info->total_bytes);
}
```

---

## Hardware CRC Acceleration

### STM32 Example

```c
CRC_HandleTypeDef hcrc;

uint32_t stm32_crc32(uint32_t seed, const void *data, size_t len) {
    // Reset and set initial value
    __HAL_CRC_DR_RESET(&hcrc);
    HAL_CRC_Accumulate(&hcrc, &seed, 1);
    
    // Process in 32-bit words
    uint32_t result = HAL_CRC_Calculate(&hcrc, (uint32_t*)data, len / 4);
    
    // Handle remaining bytes if len not multiple of 4
    if (len % 4) {
        uint32_t tail = 0;
        memcpy(&tail, (const uint8_t*)data + (len & ~3), len % 4);
        result = HAL_CRC_Accumulate(&hcrc, &tail, 1);
    }
    
    return result;
}

cfg.crc32_provider = stm32_crc32;
```

**Performance:** ~10x faster than software CRC on typical MCU

---

### ESP32 Example

```c
#include "esp32/rom/crc.h"

uint32_t esp32_crc32(uint32_t seed, const void *data, size_t len) {
    return crc32_le(seed, data, len) ^ 0xFFFFFFFF;
}

cfg.crc32_provider = esp32_crc32;
```

---

## High-Resolution Timing (Optional)

### Microsecond Clock (for sub-millisecond RTT)

```c
uint64_t get_ticks_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

// Still provide millisecond interface
uint32_t get_ticks_ms(void) {
    return (uint32_t)(get_ticks_us() / 1000);
}

cfg.system.get_ticks_ms = get_ticks_ms;

// Adaptive TX will benefit from accurate RTT measurement
```

**Use Case:** Ultra-low-latency networks (< 1 ms RTT)

---

## Verbose Logging

### Enable Logging (Compile-Time)

```bash
# Set log level (0=OFF, 5=VERBOSE)
export VAL_LOG_LEVEL=5
cmake --build build
ctest --test-dir build
```

**Log Levels:**
- `0` - No logging
- `1` - Errors only
- `2` - Warnings
- `3` - Info
- `4` - Debug
- `5` - Verbose (all packets)

---

## See Also

- [Basic Usage](basic-usage.md) - Getting started examples
- [Integration Examples](integration-examples.md) - Platform-specific code
- [Implementation Guide](../implementation-guide.md) - Deep dive into features
- [API Reference](../api-reference.md) - Complete API docs
