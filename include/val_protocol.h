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
#define VAL_VERSION_MINOR 5u
#define VAL_MIN_PACKET_SIZE 512u
#define VAL_MAX_PACKET_SIZE 65536u
#define VAL_MAX_FILENAME 127u
#define VAL_MAX_PATH 127u
// Emergency cancel (ASCII CAN)
#define VAL_PKT_CANCEL 0x18u
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
// Optional/negotiable features start at bit 0.
//
#define VAL_FEAT_NONE 0u
#ifdef VAL_ENABLE_ADVANCED_TX
#define VAL_FEAT_ADVANCED_TX (1u << 0)
#define VAL_BUILTIN_FEATURES VAL_FEAT_ADVANCED_TX
#else
#define VAL_BUILTIN_FEATURES VAL_FEAT_NONE
#endif

    // Simple resume config - replaces complex resume struct
    typedef struct
    {
        val_resume_mode_t mode;
        // Tail verification window size. Used only by TAIL modes. 0 = implementation-chosen default.
        // Default cap: the core clamps tail verification to a small window (currently 2 MiB) to keep operations fast
        // on slow/embedded storage. Larger requests are reduced to this cap. FULL modes ignore this value.
        uint32_t crc_verify_bytes; // For tail modes only (0 = auto-calculate)
    } val_resume_config_t;

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

#if VAL_ENABLE_METRICS
    // Optional compile-time metrics collection (enabled when VAL_ENABLE_METRICS=1 at build time)
    typedef struct
    {
        // Packets and bytes on wire
        uint64_t packets_sent;
        uint64_t packets_recv;
        uint64_t bytes_sent;
        uint64_t bytes_recv;
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
