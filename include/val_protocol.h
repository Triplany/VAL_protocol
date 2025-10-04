/**
 * @file val_protocol.h
 * @brief VAL Protocol - Versatile Adaptive Link Protocol
 * 
 * A robust, blocking-I/O file transfer protocol library designed for reliable
 * file transfers across diverse network conditions, from high-speed LANs to
 * constrained embedded systems.
 * 
 * Features:
 * - Adaptive transmission with window-based flow control (1-64 packets)
 * - Six resume modes with CRC-verified partial transfers
 * - Embedded-friendly: zero dynamic allocations in steady state
 * - Transport agnostic: works over TCP, UART, USB, or any reliable byte stream
 * - Comprehensive error handling with detailed diagnostic masks
 * - Optional metrics collection and wire audit trails
 * 
 * @version 0.7.0
 * @date 2025
 * @copyright MIT License - Copyright 2025 Arthur T Lee
 * 
 * @note EARLY DEVELOPMENT - READY FOR TESTING, NOT PRODUCTION READY
 *       Backward compatibility is not guaranteed until v1.0.
 * 
 * Dedicated to Valerie Lee - for all her support over the years
 * allowing me to chase my ideas.
 */

#ifndef VAL_PROTOCOL_H
#define VAL_PROTOCOL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "val_errors.h"
#include <stddef.h>
#include <stdint.h>

// Protocol constants
#define VAL_MAGIC 0x56414C00u // "VAL\0"
#define VAL_VERSION_MAJOR 0u
#define VAL_VERSION_MINOR 7u
#define VAL_MIN_PACKET_SIZE 512u
#define VAL_MAX_PACKET_SIZE 65536u
#define VAL_MAX_FILENAME 127u
#define VAL_MAX_PATH 127u
// Emergency cancel (ASCII CAN)
#define VAL_PKT_CANCEL 0x18u

    // Public packet type identifiers (on-wire type field). Values must match core.
    typedef enum
    {
        VAL_PKT_HELLO = 1,       // session/version negotiation
        VAL_PKT_SEND_META = 2,   // filename, size, path
        VAL_PKT_RESUME_REQ = 3,  // sender asks resume options
        VAL_PKT_RESUME_RESP = 4, // receiver responds with action
        VAL_PKT_DATA = 5,        // file data chunk
        VAL_PKT_DATA_ACK = 6,    // ack for data chunk (cumulative)
        VAL_PKT_VERIFY = 7,      // crc verify request/response
        VAL_PKT_DONE = 8,        // file complete
        VAL_PKT_ERROR = 9,       // error report
        VAL_PKT_EOT = 10,        // end of transmission (batch)
        VAL_PKT_EOT_ACK = 11,    // ack for end of transmission
        VAL_PKT_DONE_ACK = 12,   // ack for end of file
        // Adaptive mode sync heartbeat/control
        VAL_PKT_MODE_SYNC = 13,
        VAL_PKT_MODE_SYNC_ACK = 14,
        VAL_PKT_DATA_NAK = 15,   // negative ack with next_expected_offset and reason bits
    } val_packet_type_t;

    // Optional flags for DATA_ACK payload semantics (when payload is present)
    // For now, most ACKs remain header-only with offset used as rx_highwater.
    // These flags are reserved for future use to distinguish heartbeat vs EOF.
    typedef enum
    {
        VAL_ACK_FLAG_HEARTBEAT = 1u << 0,
        VAL_ACK_FLAG_EOF       = 1u << 1,
    } val_ack_flags_t;
    // Metadata payload shared with application callbacks (public)
    typedef struct
    {
        // Sanitized basename only (no directories). Sender strips paths and cleans unsafe chars.
        char filename[VAL_MAX_FILENAME + 1];
        // Reported/original path hint from the sender (sanitized). For receiver information only — do not trust for output paths.
        // Receivers should decide their own output directory and must not concatenate this blindly to avoid path traversal.
        char sender_path[VAL_MAX_PATH + 1];
        uint64_t file_size;
        uint32_t file_crc32; // whole-file CRC for integrity verification
    } val_meta_payload_t;

    // Enhanced progress information for professional UX
    typedef struct
    {
        uint64_t bytes_transferred;   // Total bytes transferred in this batch/session so far (cumulative)
        uint64_t total_bytes;         // Total bytes expected in batch (0 if unknown)
        uint64_t current_file_bytes;  // Bytes transferred for the current file
        uint32_t files_completed;     // Files completed in the batch so far
        uint32_t total_files;         // Total files in the batch (0 if unknown)
        uint32_t transfer_rate_bps;   // Average transfer rate (bytes per second)
        uint32_t eta_seconds;         // Estimated time remaining for batch (0 if unknown)
        const char *current_filename; // Pointer to current filename (valid for duration of callback)
    } val_progress_info_t;

    // Validation response actions
    typedef enum
    {
        VAL_VALIDATION_ACCEPT = 0, // File is valid, proceed with transfer
        VAL_VALIDATION_SKIP = 1,   // Skip this file, continue session with next file
        VAL_VALIDATION_ABORT = 2,  // Abort entire session immediately
    } val_validation_action_t;

    // Single metadata validation callback type
    // Parameters:
    //   meta: File metadata from SEND_META packet
    //   target_path: Full constructed path where file will be saved
    //   context: User-provided context pointer
    // Returns: VAL_VALIDATION_ACCEPT/SKIP/ABORT
    typedef val_validation_action_t (*val_metadata_validator_t)(const val_meta_payload_t *meta, const char *target_path,
                                                                void *context);

    // val_status_t is defined in val_errors.h

    // New resume modes - replaces all existing resume enums
    typedef enum
    {
        VAL_RESUME_NEVER = 0,         // Always overwrite from zero
        VAL_RESUME_SKIP_EXISTING = 1, // Skip any existing file (no verification)
        // Tail modes: receiver requests a CRC over the last N bytes of the local file (N = min(crc_verify_bytes, local_size)).
        //   - If CRC matches the sender's tail at the same offset, resume from local_size.
        //   - If CRC mismatches OR the local file is larger than the incoming file size, treat as mismatch.
        //     TAIL -> skip the file; TAIL_OR_ZERO -> restart from offset 0.
        VAL_RESUME_CRC_TAIL = 2,         // Resume on tail match; skip on mismatch
        VAL_RESUME_CRC_TAIL_OR_ZERO = 3, // Resume on tail match; overwrite from zero on mismatch
        // Full modes (full-prefix semantics): receiver requests a CRC over the entire local file (prefix) when local_size ≤
        // incoming_size.
        //   - If CRC matches this full local prefix:
        //       • If local_size == incoming_size, skip the file entirely (already complete).
        //       • Otherwise, resume from offset local_size.
        //   - If local_size > incoming_size there is no possible match; treat as mismatch.
        //     FULL -> skip the file; FULL_OR_ZERO -> overwrite from zero.
        //   - Core applies a verification cap for responsiveness: if the local file exceeds the cap, FULL falls back to a
        //     "large tail" verify over the last CAP bytes (resume and mismatch policies remain those of FULL/FULL_OR_ZERO).
        VAL_RESUME_CRC_FULL = 4,         // Skip only when full-prefix matches exactly; otherwise skip on mismatch
        VAL_RESUME_CRC_FULL_OR_ZERO = 5, // Skip only when full-prefix matches exactly; otherwise overwrite from zero
    } val_resume_mode_t;

    typedef struct val_session_s val_session_t;

    // Optional memory allocator used by VAL for dynamic session/tracking allocation.
    // If not provided (alloc == NULL), VAL falls back to standard calloc/free.
    typedef struct
    {
        void *(*alloc)(size_t size, void *context); // allocate 'size' bytes
        void (*free)(void *ptr, void *context);     // free previously allocated pointer
        void *context;                              // user context passed to hooks
    } val_memory_allocator_t;

    // Log levels for runtime filtering (lower = higher priority)
    typedef enum
    {
        VAL_LOG_OFF = 0,
        VAL_LOG_CRITICAL = 1,
        VAL_LOG_WARNING = 2,
        VAL_LOG_INFO = 3,
        VAL_LOG_DEBUG = 4,
        VAL_LOG_TRACE = 5,
    } val_log_level_t;

// Public feature bits
// Negotiation covers only optional features; core functionality is implicit and not represented by bits.
// Currently no optional features are defined; all core features (windowing, streaming, resume) are implicit.
//
#define VAL_FEAT_NONE 0u
#define VAL_BUILTIN_FEATURES VAL_FEAT_NONE

    // Simple resume config - replaces complex resume struct
    typedef struct
    {
        val_resume_mode_t mode;
        // Tail verification window size. Used only by TAIL modes. 0 = implementation-chosen default.
        // Default cap: the core clamps tail verification to a small window (currently 2 MiB) to keep operations fast
        // on slow/embedded storage. Larger requests are reduced to this cap. FULL modes ignore this value.
        uint32_t crc_verify_bytes; // For tail modes only (0 = auto-calculate)
    } val_resume_config_t;

    // Adaptive transmission window rungs (fastest has the lowest numeric value)
    typedef enum
    {
        VAL_TX_WINDOW_64 = 64,     // 64-packet window
        VAL_TX_WINDOW_32 = 32,     // 32-packet window
        VAL_TX_WINDOW_16 = 16,     // 16-packet window
        VAL_TX_WINDOW_8  = 8,      // 8-packet window
        VAL_TX_WINDOW_4  = 4,      // 4-packet window
        VAL_TX_WINDOW_2  = 2,      // 2-packet window
        VAL_TX_STOP_AND_WAIT = 1,  // 1 in flight (stop-and-wait)
    } val_tx_mode_t;

    typedef struct
    {
        // Window rungs (discrete). These replace any prior mixed "streaming" enum.
        val_tx_mode_t max_performance_mode;   // Max window rung supported by this endpoint (cap)
        val_tx_mode_t preferred_initial_mode; // Initial rung (clamped to cap). If out of range, defaults to cap
        // Streaming policy: single switch governs both sending and accepting streaming.
        // If 1, we will stream when sending and we allow the peer to stream to us.
        // If 0, we will not stream when sending and we require the peer not to stream to us.
        uint8_t allow_streaming; // 0 = disallow streaming in either direction for this session; 1 = allow
        // Optional +1 MTU retransmit cache for faster Go-Back-N recovery (MCU default: 0)
        uint8_t retransmit_cache_enabled; // 0/1
        uint8_t reserved0;
        // Adaptive stepping thresholds
        uint16_t degrade_error_threshold;    // Errors before degrading one rung
        uint16_t recovery_success_threshold; // Successes before upgrading one rung
        uint16_t mode_sync_interval;         // Packets between mode sync messages (reserved; optional)
        val_memory_allocator_t allocator;    // Allocator for session and tracking structures
    } val_adaptive_tx_config_t;

    typedef struct
    {
        // Thread-safety: Unless otherwise stated, a single val_session_t must not be used concurrently from multiple threads.
        // Create one session per direction/worker, or add your own external synchronization around public APIs. Different
        // sessions can be used in parallel safely. Callbacks must be non-reentrant (do not call back into the same session).
        struct
        {
            // Send exactly 'len' bytes (may be smaller than packet_size for final frames). Return bytes sent or <0 on error.
            int (*send)(void *ctx, const void *data, size_t len);
            // Receive exactly 'buffer_size' bytes, blocking until filled or timeout. Return 0 on success, <0 on error.
            // The protocol reads header, then payload_len, then trailer CRC in separate calls.
            int (*recv)(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms);
            // Optional: report whether the underlying transport is currently connected.
            // Return 1 if connected/usable, 0 if not connected, and <0 if unknown. When absent (NULL), the
            // protocol assumes the transport is connected.
            int (*is_connected)(void *ctx);
            // Optional: flush any buffered data in the transport to the wire (best-effort). When absent (NULL),
            // this is treated as a no-op.
            void (*flush)(void *ctx);
            void *io_context;
        } transport;

        struct
        {
            void *(*fopen)(void *ctx, const char *path, const char *mode);
            int (*fread)(void *ctx, void *buffer, size_t size, size_t count, void *file);
            int (*fwrite)(void *ctx, const void *buffer, size_t size, size_t count, void *file);
            int (*fseek)(void *ctx, void *file, long offset, int whence);
            long (*ftell)(void *ctx, void *file);
            int (*fclose)(void *ctx, void *file);
            void *fs_context;
        } filesystem;

        // Optional CRC32 provider (e.g., hardware-accelerated). If any pointer is NULL, built-in software is used.
        struct
        {
            uint32_t (*crc32)(void *ctx, const void *data, size_t length); // one-shot
            uint32_t (*crc32_init)(void *ctx);                             // returns initial state
            uint32_t (*crc32_update)(void *ctx, uint32_t state, const void *data, size_t length);
            uint32_t (*crc32_final)(void *ctx, uint32_t state); // returns final CRC32
            void *crc_context;                                  // user context passed to hooks
        } crc;

        struct
        {
            // Monotonic millisecond clock. Always required by VAL; no built-in defaults.
            uint32_t (*get_ticks_ms)(void);
            // Optional delay helper for backoff/sleeps between retries. When NULL, the
            // implementation uses a minimal spin/yield or platform fallback where applicable.
            void (*delay_ms)(uint32_t ms);
        } system;

        // Adaptive timeout bounds (ms). Required.
        // The protocol computes per-operation timeouts adaptively using RFC6298-like RTT estimation,
        // clamped to [min_timeout_ms, max_timeout_ms].
        struct
        {
            uint32_t min_timeout_ms; // Minimum allowed timeout (floor)
            uint32_t max_timeout_ms; // Maximum allowed timeout (ceiling)
        } timeouts;

        // Feature requirements and requests (used during handshake)
        // Note: Actual supported features are baked in at compile time and not user-editable.
        struct
        {
            uint32_t required;  // features that must be present on peer; masked to built-in features internally
            uint32_t requested; // features requested if peer supports; masked to built-in features internally
        } features;

        // Retry policy and backoff for timeouts
        struct
        {
            uint8_t meta_retries;      // retries for waiting metadata (on timeout)
            uint8_t data_retries;      // retries for waiting data (on timeout)
            uint8_t ack_retries;       // retries for waiting ACK (on timeout)
            uint8_t handshake_retries; // retries for waiting handshake hello (on timeout)
            uint32_t backoff_ms_base;  // base backoff between retries (exponential: base, 2*base, ...)
        } retries;

        struct
        {
            void *send_buffer;  // At least packet_size bytes; used as staging for header+payload+crc
            void *recv_buffer;  // At least packet_size bytes; used to read header and payload
            size_t packet_size; // MTU (max frame size), not a strict per-packet size
        } buffers;

        // Simple resume configuration
        val_resume_config_t resume;

        // Adaptive transmission configuration (scaffolding; Phase 1)
        val_adaptive_tx_config_t adaptive_tx;

        struct
        {
            // Filenames and sender_path are treated as UTF-8 on wire and in callbacks. Sanitizers preserve non-ASCII bytes
            // but may truncate at a byte boundary to fit size limits. Applications should treat these as UTF-8 strings.
            void (*on_file_start)(const char *filename, const char *sender_path, uint64_t file_size, uint64_t resume_offset);
            void (*on_file_complete)(const char *filename, const char *sender_path, val_status_t result);
            // Enhanced progress callback (replaces the previous 2-argument form)
            void (*on_progress)(const val_progress_info_t *info);
        } callbacks;

        // Simple metadata validation configuration (optional)
        struct
        {
            // Single validation callback — NULL means accept all files
            val_metadata_validator_t validator;
            // Context pointer passed to validator callback
            void *validator_context;
        } metadata_validation;

        // Optional debug logging sink. Logging is compile-time gated by VAL_LOG_LEVEL macro (0..5).
        // If VAL_LOG_LEVEL == 0, all logging macros compile to no-ops with zero runtime overhead.
        // If enabled (>0), and this sink is provided, messages will be sent here; otherwise they are dropped.
        struct
        {
            // level passed to log(): one of val_log_level_t values above
            void (*log)(void *ctx, int level, const char *file, int line, const char *message);
            void *context;
            // Runtime threshold: messages with level <= min_level are forwarded (subject to compile-time gating).
            // If set to 0 (OFF), no logs are forwarded at runtime. If left 0 by caller, the session will default it
            // to the compile-time VAL_LOG_LEVEL when created.
            int min_level;
        } debug;
    } val_config_t;

    // API
    // Create a session. On success, returns VAL_OK and writes a session pointer to out_session.
    // On failure, returns a specific error code (e.g., VAL_ERR_INVALID_ARG, VAL_ERR_NO_MEMORY).
    // If out_detail is provided, a 32-bit detail mask is written to indicate the specific init issue(s).
    val_status_t val_session_create(const val_config_t *config, val_session_t **out_session, uint32_t *out_detail);
    void val_session_destroy(val_session_t *session);

    // Send multiple files; array of paths (you can pass 1 or many). Stops on first error.
    val_status_t val_send_files(val_session_t *session, const char *const *filepaths, size_t file_count, const char *sender_path);
    val_status_t val_receive_files(val_session_t *session, const char *output_directory);

    // Utilities
    void val_clean_filename(const char *input, char *output, size_t output_size);
    void val_clean_path(const char *input, char *output, size_t output_size);
    uint32_t val_crc32(const void *data, size_t length);

    // Adaptive TX helpers
    // Get the sender's current adaptive transmission mode.
    // Returns VAL_OK and writes to out_mode on success; VAL_ERR_INVALID_ARG on bad inputs.
    val_status_t val_get_current_tx_mode(val_session_t *session, val_tx_mode_t *out_mode);

    // Query whether streaming pacing is currently engaged for this session when we are the sender.
    // Returns VAL_OK and writes 0/1 to out_streaming_engaged on success; VAL_ERR_INVALID_ARG on bad inputs.
    // Note: Streaming is an overlay on top of the fastest window rung and may toggle based on runtime conditions
    // (e.g., sustained successes at max rung engage streaming; any error disengages it).
    val_status_t val_is_streaming_engaged(val_session_t *session, int *out_streaming_engaged);

    // Best-effort: Query whether the peer has engaged streaming pacing (observed via MODE_SYNC flags).
    // Returns VAL_OK and writes 0/1 to out_peer_streaming_engaged on success; VAL_ERR_INVALID_ARG on bad inputs.
    val_status_t val_is_peer_streaming_engaged(val_session_t *session, int *out_peer_streaming_engaged);

    // Get the peer's last-known adaptive transmission mode (as reported via handshake or mode sync).
    // This reflects the other side's TX window rung. Returns VAL_OK and writes to out_mode on success.
    val_status_t val_get_peer_tx_mode(val_session_t *session, val_tx_mode_t *out_mode);

    // Query negotiated streaming permissions for this session.
    // On success, writes 0/1 to out_send_allowed (we may stream when sending) and out_recv_allowed (we accept peer streaming).
    // Returns VAL_ERR_INVALID_ARG on bad inputs.
    val_status_t val_get_streaming_allowed(val_session_t *session, int *out_send_allowed, int *out_recv_allowed);

    // Get the effective negotiated packet size (MTU) for this session.
    // Returns VAL_OK and writes to out_packet_size on success; VAL_ERR_INVALID_ARG on bad inputs.
    val_status_t val_get_effective_packet_size(val_session_t *session, size_t *out_packet_size);

    // Query compiled-in features (what this build supports)
    uint32_t val_get_builtin_features(void);

    // Retrieve last error info recorded by the session (code and optional detail mask)
    val_status_t val_get_last_error(val_session_t *session, val_status_t *code, uint32_t *detail_mask);

    // Emergency cancel API (best-effort, +0 RAM)
    // Sends a CANCEL packet to the peer and marks the session as aborted.
    // Returns VAL_OK if at least one send succeeded; VAL_ERR_IO if all sends failed.
    val_status_t val_emergency_cancel(val_session_t *session);
    // Convenience helper to query if session is in cancelled state (last_error_code == VAL_ERR_ABORTED)
    int val_check_for_cancel(val_session_t *session);

    // Metadata validation helpers
    // Initialize with no validation (default - accept all files)
    void val_config_validation_disabled(val_config_t *config);
    // Set custom validator with context
    void val_config_set_validator(val_config_t *config, val_metadata_validator_t validator, void *context);

#if VAL_ENABLE_WIRE_AUDIT
    // Optional compile-time wire audit: per-packet counters and inflight tracking to assert protocol invariants.
    typedef struct
    {
        // Packet counters
        uint64_t sent_hello;
        uint64_t sent_send_meta;
        uint64_t sent_resume_req;
        uint64_t sent_resume_resp;
        uint64_t sent_verify;
        uint64_t sent_data;
        uint64_t sent_data_ack;
        uint64_t sent_done;
        uint64_t sent_error;
        uint64_t sent_eot;
        uint64_t sent_eot_ack;
        uint64_t sent_done_ack;
        // Recv packet counters
        uint64_t recv_hello;
        uint64_t recv_send_meta;
        uint64_t recv_resume_req;
        uint64_t recv_resume_resp;
        uint64_t recv_verify;
        uint64_t recv_data;
        uint64_t recv_data_ack;
        uint64_t recv_done;
        uint64_t recv_error;
        uint64_t recv_eot;
        uint64_t recv_eot_ack;
        uint64_t recv_done_ack;
        // Inflight and window audit (sender perspective)
        uint32_t max_inflight_observed; // maximum simultaneous packets in flight during a file
        uint32_t current_inflight;      // current inflight at last update
    } val_wire_audit_t;

    // Retrieve a snapshot of the current wire audit stats.
    // Returns VAL_OK when auditing is compiled in; VAL_ERR_INVALID_ARG on bad inputs.
    val_status_t val_get_wire_audit(val_session_t *session, val_wire_audit_t *out);
    // Reset wire audit counters to zero.
    val_status_t val_reset_wire_audit(val_session_t *session);
#endif // VAL_ENABLE_WIRE_AUDIT

#if VAL_ENABLE_METRICS
    // Optional compile-time metrics collection (enabled when VAL_ENABLE_METRICS=1 at build time)
    typedef struct
    {
        // Packets and bytes on wire
        uint64_t packets_sent;
        uint64_t packets_recv;
        uint64_t bytes_sent;
        uint64_t bytes_recv;
        // Per-packet-type counters (index by on-wire type byte; sized for safety)
        // Includes core packet types (HELLO..MODE_SYNC_ACK) and CANCEL (0x18)
        uint64_t send_by_type[32];
        uint64_t recv_by_type[32];
        // Reliability/timing
        uint32_t timeouts;
        uint32_t retransmits;
        uint32_t crc_errors;
        // Handshake and session
        uint32_t handshakes;
        uint32_t files_sent;
        uint32_t files_recv;
        // Adaptive timeout samples
        uint32_t rtt_samples;
    } val_metrics_t;

    // Get a snapshot of current session metrics. Returns VAL_OK and writes to out if enabled.
    val_status_t val_get_metrics(val_session_t *session, val_metrics_t *out);
    // Reset all counters to zero.
    val_status_t val_reset_metrics(val_session_t *session);
#endif // VAL_ENABLE_METRICS

#ifdef __cplusplus
}
#endif

#endif // VAL_PROTOCOL_H
