# VAL Protocol - Advanced Features

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance. Verify behavior through testing.

---

## Why Use VAL Protocol?

### When VAL is the Right Choice

VAL Protocol fills a specific niche that existing protocols don't address well:

**✅ Use VAL when you need:**
- **Embedded/IoT file transfers** over UART, RS-485, CAN, USB CDC, or other non-TCP transports
- **Resume capability** with corruption detection on unreliable links
- **Adaptive performance** that automatically adjusts to network quality
- **Blocking I/O design** that integrates cleanly with bare-metal or RTOS code
- **Zero dynamic allocation** for resource-constrained systems
- **Transport agnostic** protocol that works over any reliable byte stream
- **Simple integration** without OS networking stack dependencies

### Comparison to Alternatives

| Protocol | Transport | Resume | Adaptive | Embedded-Friendly | Use Case |
|----------|-----------|--------|----------|-------------------|----------|
| **VAL** | Any byte stream | ✅ CRC-verified | ✅ Window + streaming | ✅ Zero alloc | **Embedded, custom transports, unreliable links** |
| HTTP(S) | TCP/IP only | ⚠️ Range requests | ❌ TCP-level only | ❌ Needs full stack | Web, cloud, APIs |
| FTP | TCP/IP only | ✅ REST command | ❌ Fixed | ❌ Complex protocol | Traditional file servers |
| XMODEM | Any serial | ⚠️ Basic | ❌ Stop-and-wait | ✅ Simple | Ancient terminals, bootstrap |
| YMODEM | Any serial | ⚠️ File-level | ❌ 1KB blocks | ✅ Simple | Batch files, legacy |
| ZMODEM | Any serial | ✅ Good | ⚠️ Streaming only | ⚠️ Complex CRC | Legacy serial, still common |
| TFTP | UDP/IP only | ❌ No resume | ❌ Fixed window | ⚠️ Needs UDP | Bootloaders, network boot |

### Real-World Scenarios

**🔧 Embedded Device Firmware Update**
- **Problem:** Need to update firmware over UART. HTTP requires full TCP/IP stack (too heavy). XMODEM is too slow (stop-and-wait). ZMODEM is complex and has issues with modern systems.
- **VAL Solution:** Blocking I/O design, adaptive transmission for speed, CRC-verified resume if update interrupted, works over raw UART.

**📡 Satellite/RF Link File Transfer**
- **Problem:** High latency (500ms+), occasional packet loss. TCP performs poorly. XMODEM is unusable (1 packet per second). HTTP can't adapt.
- **VAL Solution:** Adaptive transmission automatically adjusts window size based on loss. Streaming mode maintains throughput even with small windows.

**🏭 Industrial PLC Communication**
- **Problem:** Need file transfer over RS-485 Modbus network. No TCP/IP available. Must be deterministic and recover from power interruptions.
- **VAL Solution:** Works over any transport. Deterministic blocking behavior. Resume from exact byte offset after power loss.

**🚀 Space/Aerospace Data Downlink**
- **Problem:** Intermittent connection, strict memory limits, can't tolerate corruption, need every byte accounted for.
- **VAL Solution:** CRC on every packet, metrics tracking, zero dynamic allocation, proven resume modes.

**🔌 USB Device File Sync**
- **Problem:** Custom USB device (not mass storage class) needs file transfer. Can't use filesystem drivers. Need progress tracking.
- **VAL Solution:** Works over USB CDC/bulk pipes, progress callbacks, metadata validation, efficient pacing.

### What VAL is NOT

❌ **Not a replacement for HTTP/HTTPS** - If you have TCP/IP and TLS infrastructure, use that  
❌ **Not for real-time streaming** - VAL is for file transfer, not live video/audio  
❌ **Not for datagram networks** - Requires reliable, in-order byte stream (use TCP, UART, USB, etc.)  
❌ **Not for internet-scale distribution** - No built-in encryption, authentication, or CDN features  
❌ **Not production-ready yet** - Version 0.7, protocol still evolving

### The VAL Advantage: Adaptive + Streaming

Unlike XMODEM/YMODEM (fixed stop-and-wait) or pure windowed protocols, VAL combines:

1. **Adaptive windowing** - Grows/shrinks based on network quality
2. **Streaming pacing** - Keeps pipeline full even with small windows
3. **Automatic capability negotiation** - Works with asymmetric endpoints (PC ↔ MCU)

This means you can get **high throughput on good links** while maintaining **reliability on poor links**, all with **memory-constrained embedded devices**.

---

## Adaptive Transmission Control

### Basic Adaptive TX Configuration

```c
val_config_t cfg = {0};

// Basic configuration
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_32;  // Up to 32 outstanding packets
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_4;  // Start with 4 packets
cfg.adaptive_tx.allow_streaming = 1;                       // Enable streaming overlay
```

**Important:** During handshake, both sides exchange their capabilities. The protocol automatically uses the **minimum capabilities** from both sides. If the receiver only supports `VAL_TX_WINDOW_2` and no streaming, that's what will be used regardless of sender's max settings.

**Window Modes:**
- `VAL_TX_SINGLE` - Stop-and-wait (1 packet in flight)
- `VAL_TX_WINDOW_2`, `VAL_TX_WINDOW_4`, `VAL_TX_WINDOW_8` - Fixed window sizes
- `VAL_TX_WINDOW_16`, `VAL_TX_WINDOW_32`, `VAL_TX_WINDOW_64` - Large windows

---

### How Capability Negotiation Works

During the handshake, both sender and receiver exchange their capabilities. The protocol **automatically selects the minimum** from both sides:

**Example: High-Performance PC ↔ Low-End MCU**

```c
// PC (sender) - High capabilities
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
cfg.adaptive_tx.allow_streaming = 1;

// MCU (receiver) - Limited capabilities  
cfg.adaptive_tx.max_performance_mode = VAL_TX_SINGLE;  // Stop-and-wait only
cfg.adaptive_tx.allow_streaming = 0;                   // No streaming
```

**Negotiated Result:**
- Max mode: `VAL_TX_SINGLE` (lowest of 64 and 1)
- Streaming: `DISABLED` (both must support it)

The sender will **never** use more than single-packet mode, regardless of network quality. The MCU's limits become the session limits.

**Key Principle:** The handshake ensures both sides operate within the constraints of the **weakest link**. No configuration needed - it's automatic.

---

### Aggressive Configuration (High-Speed LAN)

```c
// Maximize throughput on reliable, low-latency networks
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_16;
cfg.adaptive_tx.allow_streaming = 1;

cfg.adaptive_tx.escalation_success_threshold = 5;  // Escalate quickly
cfg.adaptive_tx.deescalation_loss_threshold = 2;   // De-escalate on first sign of trouble
cfg.adaptive_tx.min_stable_rounds = 2;             // Short stabilization period

cfg.timeouts.min_timeout_ms = 50;   // Fast local network
cfg.timeouts.max_timeout_ms = 5000;

cfg.buffers.packet_size = 8192;  // Large packets
```

**Use Case:** Gigabit LAN, fiber links, local loopback

---

### Conservative Configuration (Unreliable/High-Latency Networks)

```c
// Minimize retransmissions on lossy/slow networks
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_4;   // Small window
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_2;
cfg.adaptive_tx.allow_streaming = 0;                       // Disable streaming

cfg.adaptive_tx.escalation_success_threshold = 20;  // Escalate slowly
cfg.adaptive_tx.deescalation_loss_threshold = 1;    // De-escalate aggressively
cfg.adaptive_tx.min_stable_rounds = 10;             // Long stabilization

cfg.timeouts.min_timeout_ms = 1000;   // Slow/variable latency
cfg.timeouts.max_timeout_ms = 60000;  // Very patient

cfg.buffers.packet_size = 1024;  // Small packets
```

**Use Case:** Satellite, cellular, long-distance WAN, RF links

---

### Adaptive TX Monitoring

```c
void on_adaptive_change(val_adaptive_tx_mode_t old_mode,
                        val_adaptive_tx_mode_t new_mode,
                        val_adaptive_change_reason_t reason) {
    const char *reasons[] = {
        "Escalation", "De-escalation", "Re-escalation",
        "Timeout", "Protocol", "User", "Receiver"
    };
    
    printf("Adaptive TX: %s → %s (%s)\n",
           mode_name(old_mode),
           mode_name(new_mode),
           reasons[reason]);
}

cfg.callbacks.on_adaptive_change = on_adaptive_change;
```

**Example Output:**
```
Adaptive TX: WINDOW_4 → WINDOW_8 (Escalation)
Adaptive TX: WINDOW_8 → WINDOW_16 (Escalation)
Adaptive TX: WINDOW_16 → WINDOW_8 (De-escalation)
Adaptive TX: WINDOW_8 → WINDOW_16 (Re-escalation)
```

---

## Streaming Mode - Continuous Non-Blocking Transfer

### What is Streaming Mode?

Streaming mode is VAL's **continuous transmission mode** that fundamentally changes how the sender operates. Instead of waiting for ACKs to fill the window, the sender **continuously sends packets until NAK or EOF**, using ACKs as **keepalive heartbeats** rather than flow control signals.

### Window Mode vs Streaming Mode (How It Actually Works)

**Window Mode (without streaming):**
```
WINDOW_4 example:
┌─────────────────────────────────────────────────────────────┐
│ Send pkt 1,2,3,4 → WAIT for ACKs → Send pkt 5,6,7,8 → WAIT │
│                    ↑ BLOCKED ↑                  ↑ BLOCKED ↑ │
└─────────────────────────────────────────────────────────────┘
```
**Logic:** 
```c
while (inflight < window && !eof) {
    send_packet();
}
wait_for_ack();  // BLOCKS until ACK received
```

**Streaming Mode (continuous send):**
```
┌──────────────────────────────────────────────────────────────┐
│ Send pkt 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → ... → EOF     │
│    ↓         ↓         ↓         ↓                            │
│  ACK 1     ACK 4     ACK 8     ACK 12  (heartbeats)         │
└──────────────────────────────────────────────────────────────┘
```
**Logic:**
```c
while (!eof) {
    send_packet();        // Keep sending
    poll_for_ack(2-20ms); // Non-blocking, short timeout
    if (nak_received) {
        retransmit_from_nak_offset();
    }
}
// ACKs are processed but don't gate sending
```

### Key Implementation Details (From Code Analysis)

**1. Window Constraint Removed**
```c
// val_sender.c line 868
while ((s->streaming_engaged ? 1u : inflight) < 
       (s->streaming_engaged ? UINT32_MAX : win) && next_to_send < size)
{
    send_data_packet();  // In streaming: UINT32_MAX effectively means "no limit"
}
```
In streaming mode, the sender ignores the window constraint and sends continuously.

**2. Non-Blocking ACK Wait**
```c
// val_sender.c line 361-372
uint32_t poll_ms = streaming_mode ? 
    (srtt_ms / 4) : // Short poll: 2-20ms based on RTT/4
    full_timeout;   // Full timeout in window mode

val_internal_recv_packet(s, ..., poll_ms);

if (streaming_mode && (st == VAL_ERR_TIMEOUT || st == VAL_ERR_CRC)) {
    return VAL_OK;  // Timeout is OK - just keep sending
}
```
Streaming uses very short polling intervals (SRTT/4, clamped 2-20ms) and treats timeouts as "keep going" signals.

**3. ACKs as Keepalive Heartbeats**
```c
// val_sender.c line 400
// Treat any DATA_ACK (even stale) as a keepalive to extend streaming deadlines
if (s->config->system.get_ticks_ms)
    s->last_keepalive_recv_time = s->config->system.get_ticks_ms();
```
ACKs don't block progress - they just prove the receiver is alive.

**4. Receiver Coalesces ACKs**
```c
// val_receiver.c line 746-750
if (s->peer_streaming_engaged) {
    // Send ACK once per sender's window size (e.g., every 4 packets for WINDOW_4)
    // Plus sparse heartbeats every ~3 seconds when idle
}
```
Receiver doesn't ACK every packet in streaming mode - only periodically (once per window) to reduce overhead.

**5. NAK Triggers Retransmission**
```c
// val_sender.c line 384-394
if (t == VAL_PKT_DATA_NAK) {
    handle_nak_retransmit(...);  // Rewind to NAK offset
    *restart_window = 1;
    return VAL_OK;
}
```
Out-of-order or corrupted packet triggers NAK from receiver, causing sender to rewind and retransmit.

**6. No Window Rewind on Timeout**
```c
// val_sender.c line 494-495
// In streaming mode, avoid rewinding window on timeout; continue sending
if (!streaming_mode) {
    rewind_window();  // Window mode: rewind and retransmit
}
```
Streaming doesn't rewind on timeout - just keeps pushing forward.

### Performance Impact

**Why Streaming is Dramatically Faster**

| Configuration | Throughput | RAM Usage | Notes |
|---------------|------------|-----------|-------|
| WINDOW_64, 4KB packets | ~2-5 MB/s | ~256 KB | Maximum performance & adaptation |
| WINDOW_64 + streaming | ~2-6 MB/s | ~256 KB | Slight improvement |
| WINDOW_4, 4KB packets | ~200-500 KB/s | ~16 KB | Significant RAM savings |
| WINDOW_4 + streaming | ~1-3 MB/s | ~16 KB | Best RAM/performance balance |
| WINDOW_2 + streaming | ~500 KB - 2 MB/s | ~8 KB | Minimal RAM, limited adaptation |

**Performance Depends on Window Size:**
- **Large windows (WINDOW_32/64)**: Streaming provides modest improvement (~10-20%)
- **Small windows (WINDOW_2/4)**: Streaming provides dramatic improvement (2-5x)
- **Window mode**: Blocks waiting for ACKs, underutilizes good links
- **Streaming mode**: Continuous send, ACKs as heartbeats only

**Memory Trade-off:**
- WINDOW_64: ~256KB buffer (64 × 4KB packets)
- WINDOW_4: ~16KB buffer (4 × 4KB packets) 
- WINDOW_4 + streaming ≈ WINDOW_64 performance with 16x less RAM

### Automatic Activation

**Streaming engages when:**
1. Both sides allow streaming (`cfg.adaptive_tx.allow_streaming = 1`)
2. Sender reaches maximum negotiated window mode (fastest rung)
3. Sustained clean transmission (10+ consecutive successful packet groups)
4. Protocol sends `MODE_SYNC` with streaming flag set

```c
// val_core.c line 1749-1764
if (!s->streaming_engaged && s->current_tx_mode == s->min_negotiated_mode) {
    if (s->consecutive_successes >= 10) {  // Default threshold
        s->streaming_engaged = 1;
        // Notify peer
        send_mode_sync_with_streaming_flag();
    }
}
```

**Streaming disengages when:**
- NAK received (corruption detected)
- Timeout exceeds threshold
- Any transmission error
- Protocol falls back to conservative windowed mode

### Configuration

```c
cfg.adaptive_tx.allow_streaming = 1;  // Enable streaming capability

// Streaming activates automatically after reaching max window + clean transmission
// No manual tuning needed - protocol adapts
```

### Monitoring

```c
void on_adaptive_change(val_adaptive_tx_mode_t old_mode,
                        val_adaptive_tx_mode_t new_mode,
                        val_adaptive_change_reason_t reason) {
    printf("Mode: %s → %s (%s)\n", 
           mode_name(old_mode), mode_name(new_mode), reason_name(reason));
}

// Output when streaming engages:
// "adaptive: engaging streaming pacing at max window rung"

// Check streaming status
int is_streaming = 0;
val_is_streaming_engaged(session, &is_streaming);
```

### Why Streaming is Powerful

**For Embedded Systems:**
- Small window (WINDOW_2 or WINDOW_4) for limited RAM
- Streaming mode still achieves high throughput
- No need for huge buffers to get good performance

**For High-Latency Links:**
- 500ms satellite link with WINDOW_8
- Window mode: 16KB per 500ms = 32 KB/s
- Streaming mode: Continuous send = MB/s (link-limited)

**Efficiency:**
- Reduces ACK overhead (coalesced ACKs)
- Receiver only sends ACK every N packets + periodic heartbeats
- Sender never blocks waiting for ACKs

### Key Takeaway

**Streaming mode transforms VAL from a windowed protocol into a continuous streaming protocol** while maintaining the same error recovery mechanisms (NAK-triggered retransmission). ACKs become heartbeats that prove liveness, not flow control gates. This allows **small-window embedded devices to achieve high throughput** on good links.

---

## Window Size Selection - RAM vs Performance vs Adaptation

### Memory Requirements

**Per-Window RAM Usage** (with 4KB packets):
```
WINDOW_1:  ~8 KB   (send + recv buffers only)
WINDOW_2:  ~8 KB   (same buffers, 2 packets in-flight)
WINDOW_4:  ~16 KB  (4 × 4KB packets)
WINDOW_8:  ~32 KB
WINDOW_16: ~64 KB
WINDOW_32: ~128 KB
WINDOW_64: ~256 KB
```

### Recommended Configurations

**Severely Constrained MCU (<32 KB RAM)**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_2;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_SINGLE;
cfg.adaptive_tx.allow_streaming = 1;  // Essential for performance
cfg.buffers.packet_size = 1024;       // Smaller packets
```
**Pros:** Minimal RAM usage  
**Cons:** Limited adaptation to network errors (only 2-3 rungs)

---

**Moderate MCU (64-128 KB RAM) - RECOMMENDED**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_8;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_4;
cfg.adaptive_tx.allow_streaming = 1;
cfg.buffers.packet_size = 2048;
```
**Pros:** Good adaptation range (5 rungs), reasonable RAM  
**Cons:** None - best balance for most embedded systems

---

**High-Performance MCU/PC (>256 KB RAM)**
```c
cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_16;
cfg.adaptive_tx.allow_streaming = 1;
cfg.buffers.packet_size = 8192;
```
**Pros:** Maximum adaptation range (8 rungs), highest throughput  
**Cons:** Higher RAM usage

---

### Why Minimum WINDOW_4 Recommended

**Problem with WINDOW_2 or SINGLE only:**
- Limited rungs available for adaptation (2-3 total)
- Protocol can't effectively "find" optimal transmission rate
- Poor response to variable network conditions
- Streaming becomes critical (not just helpful)

**With WINDOW_8 as maximum:**
- 5 rungs: SINGLE → 2 → 4 → 8
- Better error adaptation (can de-escalate gradually)
- More granular response to network conditions
- Example: Start 4 → escalate to 8 → error → de-escalate to 4 → recover → re-escalate to 8

**With WINDOW_16+ as maximum:**
- 6+ rungs for fine-grained adaptation
- Protocol navigates complex networks effectively
- Can find optimal "sweet spot" between rungs

### Trade-off Summary

| Max Window | RAM | Rungs | Adaptation Quality | Best For |
|------------|-----|-------|-------------------|----------|
| WINDOW_2 | 8 KB | 2-3 | Poor | Severely constrained, stable links |
| WINDOW_4 | 16 KB | 4 | Marginal | Small MCUs, simple networks |
| WINDOW_8 | 32 KB | 5 | **Good** | **Recommended minimum** |
| WINDOW_16 | 64 KB | 6 | Better | Good RAM/perf balance |
| WINDOW_32 | 128 KB | 7 | Very Good | High-perf with adaptation |
| WINDOW_64 | 256 KB | 8 | Excellent | Maximum capability |

---

## Resume Modes

### Full CRC Resume (Full-Prefix with Fallback)

```c
cfg.resume.mode = VAL_RESUME_CRC_FULL;
cfg.resume.crc_verify_bytes = 0;  // Not used for FULL mode
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
cfg.resume.mode = VAL_RESUME_CRC_TAIL_OR_ZERO;
cfg.resume.crc_verify_bytes = 65536;  // Last 64 KB (max 2 MB enforced)
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
printf("Timeout events:      %u\n", metrics.timeout_count);
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
        // val_force_adaptive_mode(session, VAL_TX_WINDOW_2);
    }
}

cfg.callbacks.on_health_check = on_health_check;
```

---

## Wire Auditing (Debug Feature)

### Enable Wire Audit (Compile-Time)

```bash
cmake -B build -DVAL_ENABLE_WIRE_AUDIT=ON
cmake --build build
```

### Capture All Wire Traffic

```c
#ifdef VAL_ENABLE_WIRE_AUDIT

FILE *audit_file = NULL;

void audit_callback(void *ctx, int is_send,
                   const void *data, size_t len) {
    FILE *f = (FILE*)ctx;
    const char *dir = is_send ? "SEND" : "RECV";
    
    fprintf(f, "[%s %zu bytes]\n", dir, len);
    
    // Hex dump
    const uint8_t *bytes = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        fprintf(f, "%02X ", bytes[i]);
        if ((i + 1) % 16 == 0) fprintf(f, "\n");
    }
    fprintf(f, "\n\n");
    fflush(f);
}

// Setup
audit_file = fopen("wire_audit.log", "w");
val_set_wire_audit_callback(session, audit_callback, audit_file);

// ... run transfer ...

// Cleanup
fclose(audit_file);

#endif
```

**Use Case:** Debugging protocol issues, verifying packet formats

---

## Custom Memory Allocators

### Stack-Based Allocation

```c
void* stack_alloc(void *ctx, size_t size) {
    // VAL doesn't actually allocate anything dynamically by default
    // This is for custom user data if needed
    return NULL;  // Not used
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

uint32_t hw_crc32(void *ctx, const void *data, size_t len) {
    CRC_HandleTypeDef *hcrc = (CRC_HandleTypeDef*)ctx;
    
    // STM32 CRC peripheral uses IEEE 802.3 polynomial by default
    __HAL_CRC_DR_RESET(hcrc);
    
    // Process in 32-bit words
    uint32_t result = HAL_CRC_Calculate(hcrc, (uint32_t*)data, len / 4);
    
    // Handle remaining bytes if len not multiple of 4
    if (len % 4) {
        uint32_t tail = 0;
        memcpy(&tail, (const uint8_t*)data + (len & ~3), len % 4);
        result = HAL_CRC_Accumulate(hcrc, &tail, 1);
    }
    
    return result;
}

cfg.crc.crc32 = hw_crc32;
cfg.crc.crc_context = &hcrc;
```

**Performance:** ~10x faster than software CRC on typical MCU

---

### ESP32 Example

```c
#include "esp32/rom/crc.h"

uint32_t esp32_crc32(void *ctx, const void *data, size_t len) {
    return crc32_le(0xFFFFFFFF, data, len) ^ 0xFFFFFFFF;
}

cfg.crc.crc32 = esp32_crc32;
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
