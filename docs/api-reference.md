# VAL Protocol - API Reference

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors. Please verify against source code in `include/val_protocol.h`.

---

## Key API Concepts

**VAL's Abstraction Layer** provides complete separation between protocol and implementation:

- **Transport Callbacks** (`send`, `recv`): Work with ANY byte stream - TCP, UART, USB, SPI, encrypted channels
- **Filesystem Callbacks** (`fopen`, `fread`, `fwrite`, etc.): Use ANY byte source - files, RAM, flash, network buffers, compression
- **System Callbacks** (`get_ticks_ms`, `malloc`, `crc32`): Enable hardware CRC acceleration, custom allocators, platform clocks
- **Result**: Implement encryption, compression, custom protocols, in-memory transfers without touching core protocol

## Table of Contents

1. [Session Management](#session-management)
2. [File Transfer Operations](#file-transfer-operations)
3. [Configuration](#configuration)
4. [Callbacks](#callbacks)
5. [Adaptive Transmission](#adaptive-transmission)
    uint32_t *out_detail
);
```

**Description:**  
Creates a new VAL protocol session with the specified configuration.

**Parameters:**
- `config`: Pointer to configuration structure (must remain valid for session lifetime)
- `out_session`: Receives pointer to created session on success
- `out_detail`: Optional; receives detail mask on failure (can be NULL)

**Returns:**
- `VAL_OK` on success
- `VAL_ERR_INVALID_ARG` if configuration is invalid
**Example:**
```c
val_config_t cfg = {0};
// ... configure cfg ...

val_session_t *session = NULL;
uint32_t detail = 0;
val_status_t status = val_session_create(&cfg, &session, &detail);
if (status != VAL_OK) {
    fprintf(stderr, "Session creation failed: %d (detail: 0x%08X)\n",
```

**Description:**  
Destroys a session and frees all associated resources.

**Parameters:**
- `session`: Session to destroy (can be NULL)

**Example:**
```c
val_session_destroy(session);
session = NULL;
```

**Notes:**
- Safe to call with NULL
- Does not close transport or free user buffers
- Frees tracking slots allocated via custom allocator (if provided)

---

## File Transfer Operations

### val_send_files

**Signature:**
```c
val_status_t val_send_files(
    val_session_t *session,
    const char *const *filepaths,
    size_t file_count,
    const char *sender_path
);
```

**Description:**  
Sends multiple files to the receiver. Performs handshake on first call, then sends all files in sequence.

**Parameters:**
- `session`: Active session
- `filepaths`: Array of file paths to send
- `file_count`: Number of files in array
- `sender_path`: Optional path hint sent to receiver (can be NULL)

**Returns:**
- `VAL_OK` if all files sent successfully
- `VAL_SKIPPED` if some files were skipped by receiver
- `VAL_ERR_*` on error (stops at first failure)

**Example:**
```c
const char *files[] = {
    "/data/file1.bin",
    "/data/file2.txt",
    "/data/file3.dat"
};

val_status_t status = val_send_files(session, files, 3, "/data");
if (status != VAL_OK && status != VAL_SKIPPED) {
    fprintf(stderr, "Transfer failed: %d\n", status);
}
```

**Notes:**
- Handshake occurs automatically on first call
- Files sent in array order
- Stops on first error
- Sends EOT packet after all files
- Can be called multiple times on same session to send batches

---

### val_receive_files

**Signature:**
```c
val_status_t val_receive_files(
    val_session_t *session,
    const char *output_directory
);
```

**Description:**  
Receives files from sender and saves to specified directory.

**Parameters:**
- `session`: Active session
- `output_directory`: Directory where files will be saved

**Returns:**
- `VAL_OK` on success
- `VAL_ERR_*` on error

**Example:**
```c
val_status_t status = val_receive_files(session, "./downloads");
if (status != VAL_OK) {
    fprintf(stderr, "Receive failed: %d\n", status);
}
```

**Notes:**
- Handshake occurs automatically on first call
- Receives files until EOT packet
- Filenames are sanitized automatically
- Never uses `sender_path` from metadata for output path
- Output paths are: `output_directory / sanitized_filename`

---

## Configuration

### val_config_t Structure

**Definition:**
```c
typedef struct {
    // Transport callbacks (REQUIRED)
    struct {
        int (*send)(void *ctx, const void *data, size_t len);
        int (*recv)(void *ctx, void *buffer, size_t size,
                   size_t *received, uint32_t timeout_ms);
        int (*is_connected)(void *ctx);      // Optional
        void (*flush)(void *ctx);            // Optional
        void *io_context;
    } transport;
    
    // Filesystem callbacks (REQUIRED)
    struct {
        void *(*fopen)(void *ctx, const char *path, const char *mode);
    size_t (*fread)(void *ctx, void *buf, size_t sz, size_t cnt, void *file);
    size_t (*fwrite)(void *ctx, const void *buf, size_t sz, size_t cnt, void *file);
        int (*fseek)(void *ctx, void *file, long off, int whence);
        int64_t (*ftell)(void *ctx, void *file);
        int (*fclose)(void *ctx, void *file);
        void *fs_context;
    } filesystem;
    
    // CRC provider (optional - uses built-in if NULL)
    // Single-function interface for hardware acceleration
    crc32_func_t crc32_provider;  // uint32_t (*)(uint32_t seed, const void *buf, size_t len)
    
    // System callbacks (REQUIRED)
    struct {
        uint32_t (*get_ticks_ms)(void);     // REQUIRED: monotonic milliseconds
        void (*delay_ms)(uint32_t ms);      // Optional
    } system;
    
    // Adaptive timeout bounds (REQUIRED)
    struct {
        uint32_t min_timeout_ms;            // Floor (e.g., 100)
        uint32_t max_timeout_ms;            // Ceiling (e.g., 10000)
    } timeouts;
    
    // Feature negotiation (optional)
    struct {
        uint32_t required;                  // Required features from peer
        uint32_t requested;                 // Requested features from peer
    } features;
    
    // Retry policy
    struct {
        uint8_t meta_retries;               // Default: 4
        uint8_t data_retries;               // Default: 3
        uint8_t ack_retries;                // Default: 3
        uint8_t handshake_retries;          // Default: 3
        uint32_t backoff_ms_base;           // Default: 100
    } retries;
    
    // Buffers (REQUIRED)
    struct {
        void *send_buffer;                  // At least packet_size bytes
        void *recv_buffer;                  // At least packet_size bytes
    size_t packet_size;                 // MTU [512 .. 2*1024*1024]
    } buffers;
    
    // Resume configuration
    val_resume_config_t resume;
    
    // Flow control (bounded-window, single knob)
    val_tx_flow_config_t tx_flow;
    
    // Callbacks (optional)
    struct {
        void (*on_file_start)(const char *filename, const char *sender_path,
                             uint64_t file_size, uint64_t resume_offset);
        void (*on_file_complete)(const char *filename, const char *sender_path,
                                val_status_t result);
        void (*on_progress)(const val_progress_info_t *info);
    } callbacks;
    
    // Metadata validation (optional)
    struct {
        val_metadata_validator_t validator;
        void *validator_context;
    } metadata_validation;
    
    // Debug logging (optional)
    struct {
        void (*log)(void *ctx, int level, const char *file,
                   int line, const char *message);
        void *context;
        int min_level;                      // Runtime threshold
    } debug;
} val_config_t;
```

**Transport Callbacks:**

`send(ctx, data, len)`:
- Must send exactly `len` bytes
- Return bytes sent (should equal `len`) or <0 on error
- Blocking send required

`recv(ctx, buffer, size, received, timeout_ms)`:
- Must receive exactly `size` bytes or timeout
- On success: return 0, set `*received = size`
- On timeout: return 0, set `*received = 0`
- On error: return <0
- Blocking receive required

`is_connected(ctx)`: (Optional)
- Return 1 if connected/usable
- Return 0 if definitively disconnected
- Return <0 if unknown
- If NULL, assumes always connected

`flush(ctx)`: (Optional)
- Best-effort flush of buffered data to wire
- Called after control packets (HELLO, DONE, EOT, ERROR)
- If NULL, treated as no-op

**Filesystem Callbacks:**
- Should map to standard C file I/O (fopen, fread, fwrite, fseek, ftell, fclose)
- `ctx` parameter allows custom context
- For standard C library, cast function pointers appropriately

**System Callbacks:**

`get_ticks_ms()`: **(REQUIRED)**
- Must return monotonic millisecond timestamp
- Used for timeouts and RTT measurement
- Never goes backward

`delay_ms(ms)`: (Optional)
- Sleep/delay for specified milliseconds
- Used for backoff between retries
- If NULL, protocol uses minimal spin/yield

---

### val_resume_config_t

**Definition:**
```c
typedef struct {
    val_resume_mode_t mode;         // Resume mode
    uint32_t tail_cap_bytes;        // Tail verification cap (0 = default, clamped)
    uint32_t min_verify_bytes;      // Optional minimum verification window (0 = none)
    uint8_t  mismatch_skip;         // 0 = restart on mismatch; 1 = skip file
} val_resume_config_t;
```

**Resume Modes:**
```c
typedef enum {
    VAL_RESUME_NEVER = 0,           // Always overwrite from zero
    VAL_RESUME_SKIP_EXISTING = 1,   // Skip any existing file
    VAL_RESUME_TAIL = 2,            // Verify tail window; resume on match
} val_resume_mode_t;
```

**Mode Selection Guide:**

- **NEVER**: Always start from beginning (clean copy, no resume)
- **SKIP_EXISTING**: Skip any existing file (no verification, useful for batch re-transfer)
- **TAIL**: CRC-verified resume using trailing verification window (cap via `tail_cap_bytes`); on mismatch, restart file or skip based on `mismatch_skip`

**Notes**: 
- TAIL mode verifies trailing data up to the configured cap (default clamps to safe maximum)
- Adjust `resume.tail_cap_bytes` and `resume.mismatch_skip` to control verification window and mismatch behavior

---


**Type:**
```c
typedef void (*on_progress_callback_t)(const val_progress_info_t *info);
```

**Progress Info:**
```c
typedef struct {
    uint64_t bytes_transferred;     // Total bytes transferred in batch
    uint64_t total_bytes;           // Total bytes expected (0 if unknown)
    uint64_t current_file_bytes;    // Bytes transferred for current file
    uint32_t files_completed;       // Files completed so far
    uint32_t total_files;           // Total files in batch (0 if unknown)
    uint32_t transfer_rate_bps;     // Average rate (bytes/sec)
    uint32_t eta_seconds;           // ETA for batch (0 if unknown)
    const char *current_filename;   // Current filename (valid during callback only)
} val_progress_info_t;
```

**Example:**
```c
void on_progress(const val_progress_info_t *info) {
    if (info->total_bytes > 0) {
        double pct = 100.0 * info->bytes_transferred / info->total_bytes;
        printf("\r[%.1f%%] %s - %u KB/s (ETA %u sec)   ",
               pct,
               info->current_filename,
               info->transfer_rate_bps / 1024,
               info->eta_seconds);
        fflush(stdout);
    }
}

cfg.callbacks.on_progress = on_progress;
```

---

### File Event Callbacks

**File Start:**
```c
void on_file_start(const char *filename,
                   const char *sender_path,
                   uint64_t file_size,
                   uint64_t resume_offset);
```

**File Complete:**
```c
void on_file_complete(const char *filename,
                      const char *sender_path,
                      val_status_t result);
```

**Example:**
```c
void on_file_start(const char *filename, const char *sender_path,
                   uint64_t file_size, uint64_t resume_offset) {
    printf("Starting: %s (%llu bytes)\n", filename, file_size);
    if (resume_offset > 0) {
        printf("  Resuming from offset %llu\n", resume_offset);
    }
}

void on_file_complete(const char *filename, const char *sender_path,
                      val_status_t result) {
    if (result == VAL_OK) {
        printf("Completed: %s\n", filename);
    } else if (result == VAL_SKIPPED) {
        printf("Skipped: %s\n", filename);
    } else {
        printf("Failed: %s (error %d)\n", filename, result);
    }
}

cfg.callbacks.on_file_start = on_file_start;
cfg.callbacks.on_file_complete = on_file_complete;
```

---

### Metadata Validation

**Type:**
```c
typedef val_validation_action_t (*val_metadata_validator_t)(
    const val_meta_payload_t *meta,
    const char *target_path,
    void *context
);
```

**Actions:**
```c
typedef enum {
    VAL_VALIDATION_ACCEPT = 0,      // Accept file
    VAL_VALIDATION_SKIP = 1,        // Skip file, continue session
    VAL_VALIDATION_ABORT = 2        // Abort entire session
} val_validation_action_t;
```

**Metadata:**
```c
typedef struct {
    char filename[128];             // Sanitized basename
    char sender_path[128];          // Original path hint
    uint64_t file_size;             // File size
    // Note: whole-file CRC32 is not part of metadata
} val_meta_payload_t;
```

**Example:**
```c
val_validation_action_t my_validator(const val_meta_payload_t *meta,
                                     const char *target_path,
                                     void *context) {
    // Reject files over 100 MB
    if (meta->file_size > 100 * 1024 * 1024) {
        return VAL_VALIDATION_SKIP;
    }
    
    // Accept only .txt and .dat files
    const char *ext = strrchr(meta->filename, '.');
    if (!ext || (strcmp(ext, ".txt") != 0 && strcmp(ext, ".dat") != 0)) {
        return VAL_VALIDATION_SKIP;
    }
    
    return VAL_VALIDATION_ACCEPT;
}

val_config_set_validator(&cfg, my_validator, NULL);
```

**Helper Functions:**
```c
void val_config_validation_disabled(val_config_t *config);
void val_config_set_validator(val_config_t *config,
                              val_metadata_validator_t validator,
                              void *context);
```

---

## Adaptive Transmission

### val_get_cwnd_packets

**Signature:**
```c
val_status_t val_get_cwnd_packets(val_session_t *session, uint32_t *out_cwnd);
```

**Description:**  
Gets the current congestion window (cwnd) in packets.

---

### val_get_peer_tx_cap_packets

**Signature:**
```c
val_status_t val_get_peer_tx_cap_packets(val_session_t *session, uint32_t *out_cap);
```

**Description:**  
Gets the peer's advertised TX capability (packets) from handshake.


### val_get_effective_packet_size

**Signature:**
```c
val_status_t val_get_effective_packet_size(
    val_session_t *session,
    size_t *out_packet_size
);
```

**Description:**  
Gets the negotiated MTU for this session.

---

## Error Handling

### val_get_last_error

**Signature:**
```c
val_status_t val_get_last_error(
    val_session_t *session,
    val_status_t *code,
    uint32_t *detail_mask
);
```

**Description:**  
Retrieves last error recorded by session.

**Parameters:**
- `code`: Receives error code (can be NULL)
- `detail_mask`: Receives detail mask (can be NULL)

**Example:**
```c
val_status_t code = 0;
uint32_t detail = 0;
val_get_last_error(session, &code, &detail);

if (VAL_ERROR_IS_NETWORK_RELATED(detail)) {
    printf("Network error detected\n");
}
if (VAL_ERROR_IS_CRC_RELATED(detail)) {
    printf("CRC error detected\n");
}
```

**Detail Mask Helpers:**
```c
VAL_ERROR_IS_NETWORK_RELATED(detail)
VAL_ERROR_IS_CRC_RELATED(detail)
VAL_ERROR_IS_PROTOCOL_RELATED(detail)
VAL_ERROR_IS_FILESYSTEM_RELATED(detail)
```

---

### val_emergency_cancel

**Signature:**
```c
val_status_t val_emergency_cancel(val_session_t *session);
```

**Description:**  
Sends emergency CANCEL packet and marks session as aborted.

**Returns:**
- `VAL_OK` if at least one send succeeded
- `VAL_ERR_IO` if all sends failed

**Example:**
```c
// In signal handler or abort path:
val_emergency_cancel(session);
```

---

### val_check_for_cancel

**Signature:**
```c
int val_check_for_cancel(val_session_t *session);
```

**Description:**  
Convenience helper to check if session is in cancelled state.

**Returns:**
- 1 if session has `VAL_ERR_ABORTED` as last error
- 0 otherwise

---

## Utilities

### val_clean_filename

**Signature:**
```c
void val_clean_filename(const char *input, char *output, size_t output_size);
```

**Description:**  
Sanitizes filename by removing directory separators and unsafe characters.

**Example:**
```c
char clean[128];
val_clean_filename("../../etc/passwd", clean, sizeof(clean));
// clean = "etcpasswd"
```

---

### val_clean_path

**Signature:**
```c
void val_clean_path(const char *input, char *output, size_t output_size);
```

**Description:**  
Sanitizes path by removing control characters but preserving directory separators.

---

### val_crc32

**Signature:**
```c
uint32_t val_crc32(const void *data, size_t length);
```

**Description:**  
Computes CRC-32 (IEEE 802.3) of data.

**Example:**
```c
const char *str = "Hello, World!";
uint32_t crc = val_crc32(str, strlen(str));
printf("CRC: 0x%08X\n", crc);
```

---

### val_get_builtin_features

**Signature:**
```c
uint32_t val_get_builtin_features(void);
```

**Description:**  
Returns bitmask of optional features compiled into this build.

**Example:**
```c
uint32_t features = val_get_builtin_features();
// Currently always returns 0 (VAL_FEAT_NONE) - all features are implicit
printf("Built-in features: 0x%08X\n", features);
```

---

## Diagnostics

### val_get_metrics (Optional)

**Signature:**
```c
val_status_t val_get_metrics(val_session_t *session, val_metrics_t *out);
```

**Description:**  
Retrieves metrics snapshot (requires `VAL_ENABLE_METRICS=ON` at build time).

**Metrics Structure:**
```c
typedef struct {
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t send_by_type[32];      // Per-type counters
    uint64_t recv_by_type[32];
    uint32_t timeouts;       // total = soft + hard
    uint32_t timeouts_soft;  // interim slice/backoff timeouts
    uint32_t timeouts_hard;  // terminal operation timeouts
    uint32_t retransmits;
    uint32_t crc_errors;
    uint32_t handshakes;
    uint32_t files_sent;
    uint32_t files_recv;
    uint32_t rtt_samples;
} val_metrics_t;
```

**Example:**
```c
#if VAL_ENABLE_METRICS
val_metrics_t metrics;
if (val_get_metrics(session, &metrics) == VAL_OK) {
    printf("Packets: sent=%llu recv=%llu\n",
           metrics.packets_sent, metrics.packets_recv);
    printf("Timeouts: %u, Retransmits: %u\n",
           metrics.timeouts, metrics.retransmits);
}
#endif
```

---

### val_reset_metrics (Optional)

**Signature:**
```c
val_status_t val_reset_metrics(val_session_t *session);
```

**Description:**  
Resets all metrics counters to zero.

---

### Packet Capture Hook (Optional)

Set `cfg.capture.on_packet` to observe packet metadata (direction, type, lengths, offset, timestamp). No payload is exposed and overhead is near-zero when unset.

---

## Error Codes Reference

```c
typedef enum {
    VAL_OK = 0,
    VAL_SKIPPED = 1,                        // File skipped (not an error)
    VAL_ERR_INVALID_ARG = -1,
    VAL_ERR_NO_MEMORY = -2,
    VAL_ERR_IO = -3,
    VAL_ERR_TIMEOUT = -4,
    VAL_ERR_PROTOCOL = -5,
    VAL_ERR_CRC = -6,
    VAL_ERR_RESUME_VERIFY = -7,
    VAL_ERR_INCOMPATIBLE_VERSION = -8,
    VAL_ERR_PACKET_SIZE_MISMATCH = -9,
    VAL_ERR_FEATURE_NEGOTIATION = -10,
    VAL_ERR_ABORTED = -11,
    VAL_ERR_MODE_NEGOTIATION_FAILED = -12,
    VAL_ERR_UNSUPPORTED_TX_MODE = -14,
    VAL_ERR_PERFORMANCE = -15               // Connection quality too poor
} val_status_t;
```

---

## Constants

```c
#define VAL_MAGIC 0x56414C00u              // "VAL\0"
#define VAL_VERSION_MAJOR 0u
#define VAL_VERSION_MINOR 7u
#define VAL_MIN_PACKET_SIZE 512u
#define VAL_MAX_PACKET_SIZE (2u * 1024u * 1024u)
#define VAL_MAX_FILENAME 127u
#define VAL_MAX_PATH 127u
#define VAL_PKT_CANCEL 0x18u               // ASCII CAN

// Feature bits
#define VAL_FEAT_NONE 0u
// Currently no optional features defined; all core features are implicit
```

---

## See Also

- [Getting Started Guide](getting-started.md)
- [Protocol Specification](protocol-specification.md)
- [Implementation Guide](implementation-guide.md)
- [Troubleshooting](troubleshooting.md)
