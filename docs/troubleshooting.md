# VAL Protocol Troubleshooting Guide

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors.

---

## Table of Contents

1. [Common Issues](#common-issues)
2. [Error Code Reference](#error-code-reference)
3. [Debugging Techniques](#debugging-techniques)
4. [Performance Issues](#performance-issues)
5. [FAQ](#frequently-asked-questions)

---

## Common Issues

### Session Creation Fails

**Symptom:** `val_session_create()` returns `VAL_ERR_INVALID_ARG`

**Causes & Solutions:**

1. **Missing Required Callbacks**
   ```c
   // Check that ALL required callbacks are set:
   if (!cfg.transport.send || !cfg.transport.recv ||
       !cfg.filesystem.fopen || !cfg.filesystem.fread ||
       !cfg.filesystem.fwrite || !cfg.filesystem.fseek ||
       !cfg.filesystem.ftell || !cfg.filesystem.fclose ||
       !cfg.system.get_ticks_ms) {
       // Missing required callback!
   }
   ```

2. **Invalid Packet Size**
   ```c
   // Must be in range [512, 2*1024*1024]
   cfg.buffers.packet_size = 4096;  // Valid
   // cfg.buffers.packet_size = 256;   // TOO SMALL!
   // cfg.buffers.packet_size = 100000; // TOO LARGE!
   ```

3. **NULL Buffers**
   ```c
   // Both buffers required
   cfg.buffers.send_buffer = send_buf;  // Must be non-NULL
   cfg.buffers.recv_buffer = recv_buf;  // Must be non-NULL
   ```

4. **Invalid Timeout Configuration**
   ```c
   cfg.timeouts.min_timeout_ms = 100;    // Must be > 0
   cfg.timeouts.max_timeout_ms = 10000;  // Must be >= min
   ```

**Get Detailed Error:**
```c
uint32_t detail = 0;
val_status_t status = val_session_create(&cfg, &session, &detail);
if (status != VAL_OK) {
    if (detail & VAL_ERROR_DETAIL_CONNECTION) {
        fprintf(stderr, "Transport callbacks missing\n");
    }
    if (detail & VAL_ERROR_DETAIL_PERMISSION) {
        fprintf(stderr, "Filesystem callbacks missing\n");
    }
    if (detail & VAL_ERROR_DETAIL_PACKET_SIZE) {
        fprintf(stderr, "Invalid packet size\n");
    }
}
```

---

### Handshake Timeouts

**Symptom:** Transfer fails immediately with `VAL_ERR_TIMEOUT`

**Causes & Solutions:**

1. **Network Connectivity**
   - Verify network connection
   - Check firewall/NAT settings
   - Test with `ping` or `telnet`

2. **Packet Size Mismatch**
   ```c
   // Both sides must support common MTU
   // Sender: packet_size = 8192
   // Receiver: packet_size = 4096
   // Negotiated: min(8192, 4096) = 4096
   ```

3. **Timeout Too Short**
   ```c
   // Increase for high-latency links
   cfg.timeouts.min_timeout_ms = 500;   // Was: 100
   cfg.timeouts.max_timeout_ms = 30000; // Was: 10000
   ```

4. **Clock Not Implemented**
   ```c
   // MUST provide monotonic clock
   cfg.system.get_ticks_ms = my_get_ticks_ms;
   ```

**Debug with Logging:**
```c
void my_log(void *ctx, int level, const char *file, int line, const char *msg) {
    fprintf(stderr, "[%s:%d] %s\n", file, line, msg);
}

cfg.debug.log = my_log;
cfg.debug.min_level = VAL_LOG_DEBUG;  // Or VAL_LOG_TRACE
```

---

### Data Transfer Stalls

**Symptom:** Transfer starts but hangs partway through

**Causes & Solutions:**

1. **Transport recv() Broken**
   ```c
   // recv() must return 0 and set *received = 0 on timeout
   // NOT return error on timeout!
   
   int my_recv(void *ctx, void *buf, size_t size,
               size_t *received, uint32_t timeout_ms) {
       // WRONG:
       // if (timeout) return -1;
       
       // CORRECT:
       if (timeout) {
           *received = 0;
           return 0;  // Let protocol retry
       }
       
       *received = size;
       return 0;
   }
   ```

2. **File I/O Blocking**
   - Check disk space
   - Verify file permissions
   - Test filesystem separately

3. **Window Too Large for Link**
   ```c
   // Reduce window size
   cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_8;
   cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_2;
   ```

4. **Excessive Packet Loss**
   - Protocol will downgrade automatically
   - Check with diagnostics:
   ```c
   #if VAL_ENABLE_METRICS
   val_metrics_t m;
   val_get_metrics(session, &m);
   printf("Timeouts: %u, Retransmits: %u, CRC errors: %u\n",
          m.timeouts, m.retransmits, m.crc_errors);
   #endif
   ```

---

### Resume Always Fails

**Symptom:** Files always restart from beginning, never resume

**Causes & Solutions:**

1. **Wrong Resume Mode**
   ```c
   // Use forgiving mode:
    cfg.resume.mode = VAL_RESUME_TAIL;  // Recommended
    cfg.resume.tail_cap_bytes = 16384;  // 16 KB (was: 1024)
    cfg.resume.min_verify_bytes = 0;    // optional lower bound
    cfg.resume.mismatch_skip = 0;       // restart from zero on mismatch; set 1 to skip file
    // Or set `cfg.resume.mismatch_skip = 0` to restart on mismatch (default)
   ```

2. **Tail Window Too Small**
   ```c
   // If file modified in middle, small tail may not detect
   cfg.resume.tail_cap_bytes = 16384;  // 16 KB (was: 1024)
   ```

3. **File Modified**
   - If local file was modified, CRC will mismatch
   - Use `VAL_RESUME_NEVER` to always overwrite
   - Or set `cfg.resume.mismatch_skip = 0` to restart on mismatch

4. **Filesystem fseek/ftell Issues**
   ```c
   // Verify filesystem callbacks work correctly
   void *f = cfg.filesystem.fopen(ctx, "test.txt", "rb");
   cfg.filesystem.fseek(ctx, f, 0, SEEK_END);
   long size = cfg.filesystem.ftell(ctx, f);
   printf("File size: %ld\n", size);
   cfg.filesystem.fclose(ctx, f);
   ```

---

### CRC Errors

**Symptom:** `VAL_ERR_CRC` during transfer

**Causes & Solutions:**

1. **Corrupt Transport**
   - Check cable/connector quality
   - Test transport independently
   - Use error detection in transport layer

2. **Endianness Issues**
   - VAL uses little-endian on wire
   - Built-in converters handle this automatically
   - If using custom CRC provider, ensure correct byte order

3. **CRC Provider Mismatch**
   ```c
   // If providing custom CRC, must match IEEE 802.3:
   // Polynomial: 0xEDB88320 (reflected)
   // Initial: 0xFFFFFFFF
   // Final XOR: 0xFFFFFFFF
   
   // Test against known value:
   const char *test = "123456789";
   uint32_t crc = val_crc32(test, strlen(test));
   assert(crc == 0xCBF43926);  // Expected CRC-32
   ```

4. **Buffer Corruption**
   - Verify buffers are not shared/reused unsafely
   - Check for buffer overruns in transport code

---

### File Corruption

**Symptom:** Received file differs from sent file

**Causes & Solutions:**

1. **Transport Corruption**
   - Enable logging to check for CRC errors
   - Add transport-level checksums if possible

2. **Filesystem Writes Incomplete**
   ```c
   // Ensure fwrite() completes:
   int my_fwrite(void *ctx, const void *buf, size_t size,
                 size_t count, void *file) {
       size_t written = fwrite(buf, size, count, (FILE*)file);
       if (written != count) {
           return -1;  // Signal error to protocol
       }
       return (int)written;
   }
   ```

3. **Resume Logic Bug**
   - Disable resume temporarily to test:
   ```c
   cfg.resume.mode = VAL_RESUME_NEVER;
   ```

4. **Verify End-to-End**
   ```c
   // After transfer, compare CRCs:
   uint32_t sent_crc = val_crc32(sent_data, sent_size);
   uint32_t recv_crc = val_crc32(recv_data, recv_size);
   if (sent_crc != recv_crc) {
       fprintf(stderr, "File corruption detected!\n");
   }
   ```

---

## Error Code Reference

### VAL_ERR_INVALID_ARG (-1)
- **Cause**: Invalid parameter passed to API
- **Check**: NULL pointers, out-of-range values
- **Fix**: Validate all parameters before calling

### VAL_ERR_NO_MEMORY (-2)
- **Cause**: Memory allocation failed
- **Check**: Heap exhaustion, allocator issues
- **Fix**: Increase heap size, provide custom allocator

### VAL_ERR_IO (-3)
- **Cause**: Transport or filesystem I/O error
- **Detail Mask**: Check `VAL_ERROR_DETAIL_*` flags
- **Fix**: Check transport connectivity, disk space, permissions

### VAL_ERR_TIMEOUT (-4)
- **Cause**: Operation timed out
- **Detail Mask**: 
  - `VAL_ERROR_DETAIL_TIMEOUT_HELLO`: Handshake timeout
  - `VAL_ERROR_DETAIL_TIMEOUT_META`: Metadata timeout
  - `VAL_ERROR_DETAIL_TIMEOUT_DATA`: Data timeout
  - `VAL_ERROR_DETAIL_TIMEOUT_ACK`: ACK timeout
- **Fix**: Increase timeouts, check network quality

### VAL_ERR_PROTOCOL (-5)
- **Cause**: Protocol violation or unexpected packet
- **Detail Mask**:
  - `VAL_ERROR_DETAIL_MALFORMED_PKT`: Packet structure invalid
  - `VAL_ERROR_DETAIL_UNKNOWN_TYPE`: Unknown packet type
  - `VAL_ERROR_DETAIL_INVALID_STATE`: Operation in wrong state
- **Fix**: Check protocol version compatibility, enable logging

### VAL_ERR_CRC (-6)
- **Cause**: CRC mismatch detected
- **Detail Mask**:
  - `VAL_ERROR_DETAIL_CRC_HEADER`: Header CRC failed
  - `VAL_ERROR_DETAIL_CRC_TRAILER`: Trailer CRC failed
   - 0x00000400 (CRC_FILE): removed; whole-file CRC is no longer part of the protocol
- **Fix**: Check transport reliability, test CRC implementation

### VAL_ERR_RESUME_VERIFY (-7)
- **Cause**: Resume verification failed (CRC mismatch)
- **Fix**: Use `*_OR_ZERO` resume mode, or disable resume

### VAL_ERR_INCOMPATIBLE_VERSION (-8)
- **Cause**: Protocol version mismatch
- **Detail Mask**:
  - `VAL_ERROR_DETAIL_VERSION_MAJOR`: Major version differs
  - `VAL_ERROR_DETAIL_VERSION_MINOR`: Minor version differs
- **Fix**: Ensure both sides use compatible versions

### VAL_ERR_PACKET_SIZE_MISMATCH (-9)
- **Cause**: Cannot negotiate common MTU
- **Fix**: Ensure both sides support compatible packet sizes

### VAL_ERR_FEATURE_NEGOTIATION (-10)
- **Cause**: Required feature not supported by peer
- **Detail Mask**: Use `VAL_GET_MISSING_FEATURE(detail)` to get bitmask
- **Fix**: Remove required features or upgrade peer

### VAL_ERR_ABORTED (-11)
- **Cause**: Session aborted (user-initiated or peer cancel)
- **Fix**: Normal for emergency cancellation

### VAL_ERR_PERFORMANCE (-15)
- **Cause**: Connection quality too poor (excessive retries)
- **Fix**: Improve link quality, reduce window size

---

## Debugging Techniques

### Enable Logging

**Compile-Time:**
```c
// In CMakeLists.txt or compiler flags:
-DVAL_LOG_LEVEL=5  // TRACE (most verbose)
```

**Runtime:**
```c
void my_log(void *ctx, int level, const char *file, int line, const char *msg) {
    const char *lvl_str[] = {"OFF", "CRIT", "WARN", "INFO", "DEBUG", "TRACE"};
    fprintf(stderr, "[%s] %s:%d: %s\n",
            lvl_str[level], file, line, msg);
}

cfg.debug.log = my_log;
cfg.debug.min_level = VAL_LOG_TRACE;  // Maximum verbosity
```

### Enable Metrics

**Build with metrics:**
```bash
cmake -DVAL_ENABLE_METRICS=ON ...
```

**Query during transfer:**
```c
#if VAL_ENABLE_METRICS
val_metrics_t m;
val_get_metrics(session, &m);

printf("Packets: sent=%llu recv=%llu\n", m.packets_sent, m.packets_recv);
printf("Bytes: sent=%llu recv=%llu\n", m.bytes_sent, m.bytes_recv);
printf("Errors: timeouts=%u retrans=%u crc=%u\n",
       m.timeouts, m.retransmits, m.crc_errors);
printf("RTT samples: %u\n", m.rtt_samples);
#endif
```

### Packet capture hook

Set a capture callback to observe packets without payloads:
```c
static void my_capture(void *ctx, const val_packet_record_t *r) {
    (void)ctx;
    fprintf(stderr, "CAP %s type=%u len=%u off=%llu t=%u\n",
    (r->direction==VAL_DIR_TX?"TX":"RX"), r->type, r->wire_len,
    (unsigned long long)r->offset, r->timestamp_ms);
}

cfg.capture.on_packet = my_capture;
cfg.capture.context = NULL;
```
This is runtime-only (no build flags) and has near-zero overhead when unset.

### Network Packet Capture

**Use Wireshark/tcpdump:**
```bash
# Capture on port 9000
sudo tcpdump -i eth0 -w val_capture.pcap port 9000

# Analyze in Wireshark
wireshark val_capture.pcap
```

**Look for:**
- Packet loss (missing sequence numbers)
- Retransmissions (duplicate data)
- Out-of-order delivery
- Large gaps/delays

---

## Performance Issues

### Slow Transfer Speed

**Diagnostic Steps:**

1. **Check Effective Window Size**
   ```c
   val_tx_mode_t mode;
   val_get_current_tx_mode(session, &mode);
   printf("Current window: %u packets\n", (unsigned)mode);
   
   // If stuck at low window, check for errors
   ```

2. **Monitor Streaming Engagement**
   ```c
   int streaming = 0;
   val_is_streaming_engaged(session, &streaming);
   printf("Streaming: %s\n", streaming ? "YES" : "NO");
   
   // Should engage at fastest rung on good links
   ```

3. **Check Transfer Rate**
   ```c
   void on_progress(const val_progress_info_t *info) {
       printf("Rate: %u KB/s\n", info->transfer_rate_bps / 1024);
   }
   ```

**Optimization:**

1. **Increase MTU** (if link supports)
   ```c
   cfg.buffers.packet_size = 32768;  // 32 KB
   ```

2. **Increase Max Window**
   ```c
   cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
   ```

3. **Enable Streaming**
   ```c
   cfg.adaptive_tx.allow_streaming = 1;
   ```

4. **Reduce Timeouts** (on low-latency links)
   ```c
   cfg.timeouts.min_timeout_ms = 50;
   cfg.timeouts.max_timeout_ms = 2000;
   ```

### High CPU Usage

**Causes:**

1. **Busy-Wait in Transport**
   - Ensure `recv()` blocks properly
   - Provide `delay_ms()` for backoff

2. **Excessive Logging**
   - Reduce log level in production
   - Compile with `-DVAL_LOG_LEVEL=0`

3. **Small MTU with Large Files**
   - Increase packet size to reduce per-packet overhead

---

## Frequently Asked Questions

### Q: Can I use VAL over UDP?

**A:** Not directly. VAL requires a reliable, ordered byte stream. However, VAL's abstraction layer lets you wrap UDP with a reliability layer (like QUIC, KCP, or custom ARQ) and present it as a blocking byte stream via the transport callbacks.

### Q: Does VAL support encryption?

**A:** VAL's abstraction layer makes encryption easy. Three approaches:

1. **Transport Layer**: Wrap transport in TLS/DTLS (most common)
2. **Filesystem Layer**: Implement encrypted `fwrite`/`fread` callbacks
3. **Hybrid**: Custom transport wrapper for channel encryption + filesystem callbacks for at-rest encryption

See [Implementation Guide](implementation-guide.md#transport-security) for examples.

### Q: Can I transfer files bidirectionally?

**A:** Not in a single session. Each session has one sender and one receiver. For bidirectional transfers, use two sessions (one per direction) or sequential transfers.

### Q: What's the maximum file size?

**A:** 2^64 - 1 bytes (16 exabytes) theoretically. Practical limits depend on filesystem and available storage.

### Q: Can I cancel a transfer midway?

**A:** Yes, use `val_emergency_cancel(session)`. This sends a best-effort CANCEL packet and marks the session as aborted.

### Q: How do I send multiple files in one session?

**A:** Pass an array to `val_send_files()`:
```c
const char *files[] = {"file1.bin", "file2.txt", "file3.dat"};
val_send_files(session, files, 3, "/path");
```

### Q: Can the receiver reject specific files?

**A:** Yes, use a metadata validator:
```c
val_config_set_validator(&cfg, my_validator, NULL);
```
Return `VAL_VALIDATION_SKIP` to skip a file, `VAL_VALIDATION_ABORT` to abort the session.

### Q: What happens if connection drops mid-transfer?

**A:** The protocol will timeout and return an error. On reconnection, use resume to continue from where it left off (if resume is enabled).

### Q: Can I use VAL on an RTOS?

**A:** Yes. VAL has no threading or OS dependencies. Provide appropriate clock, transport, and filesystem implementations for your RTOS.

### Q: How do I integrate with FreeRTOS?

**A:** See [Implementation Guide - ESP32](implementation-guide.md#esp32-esp-idf) for FreeRTOS example.

### Q: Can I customize the retry strategy?

**A:** Yes, configure retry counts and backoff:
```c
cfg.retries.ack_retries = 5;
cfg.retries.backoff_ms_base = 200;
```

### Q: Does VAL work with lossy links?

**A:** Yes. The adaptive transmission system automatically downgrades window size on errors. However, extremely lossy links (>50% loss) may trigger performance errors.

### Q: Can I get progress updates?

**A:** Yes, set `cfg.callbacks.on_progress`:
```c
void on_progress(const val_progress_info_t *info) {
    printf("\r%.1f%%", 100.0 * info->bytes_transferred / info->total_bytes);
    fflush(stdout);
}
cfg.callbacks.on_progress = on_progress;
```

### Q: What is streaming mode and why is it fast?

**A:** Streaming mode is VAL's continuous transmission mode that removes ACK blocking:
- **Removes window constraint**: Sender continuously transmits until NAK or EOF
- **ACKs as heartbeats**: ACKs prove liveness, don't gate transmission
- **Non-blocking**: Uses short polling (2-20ms) instead of full timeout waits
- **Performance gain varies**: Modest with large windows (WINDOW_64), dramatic with small windows (WINDOW_2/4)
- **Automatic**: Engages after sustained clean transmission at max window

**Key Benefit:** Enables memory-constrained devices (WINDOW_2/4 + streaming) to achieve throughput comparable to WINDOW_64 with far less RAM.

### Q: Can I implement compression?

**A:** Yes! VAL's filesystem abstraction makes this easy:
```c
// Implement compressed write wrapper
int compressed_fwrite(void *ctx, const void *data, size_t size, size_t count, void *fp) {
    compress_and_write(fp, data, size * count);
    return count;
}

cfg.filesystem.fwrite = compressed_fwrite;
cfg.filesystem.fread = compressed_fread;  // Decompress on read
```
The protocol handles integrity checking via CRC - you just handle compression in callbacks.

### Q: Can I transfer data from RAM instead of files?

**A:** Absolutely! Implement memory-based filesystem callbacks:
```c
// Map "filename" to memory buffer
void* mem_fopen(void *ctx, const char *name, const char *mode) {
    return get_buffer_by_name(name);  // Your lookup
}

int mem_fread(void *ctx, void *buf, size_t size, size_t count, void *fp) {
    memcpy(buf, ((mem_buffer_t*)fp)->data + offset, size * count);
    return count;
}

cfg.filesystem.fopen = mem_fopen;
cfg.filesystem.fread = mem_fread;
// etc...
```

### Q: Is VAL production-ready?

**A:** VAL is currently in **early development** (v0.7) and ready for testing. It is **not yet production-ready**. Backward compatibility is not guaranteed until v1.0.

---

## Getting More Help

- **Documentation**: [docs/README.md](README.md)
- **Source Code**: Check `src/` and `include/` for implementation details
- **Examples**: See `examples/tcp/` for complete reference implementations
- **Issues**: [GitHub Issues](https://github.com/Triplany/VAL_protocol/issues)

---

**Still stuck?** Open an issue with:
1. VAL version
2. Platform (OS, compiler, architecture)
3. Configuration used
4. Full error log (with `VAL_LOG_LEVEL=5`)
5. Minimal reproduction case
