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

    typedef enum
    {
        VAL_RESUME_NONE = 0,
        VAL_RESUME_APPEND = 1,
        VAL_RESUME_CRC_VERIFY = 2,
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

// Public feature bits (use in config.features.required/requested)
#define VAL_FEAT_NONE 0u
#define VAL_FEAT_CRC_RESUME (1u << 0)
#define VAL_FEAT_MULTI_FILES (1u << 1)

    // Receiver-driven resume policy (no truncation). If policy == 0, legacy resume.mode applies.
    typedef enum
    {
        VAL_RESUME_POLICY_NONE = 0, // Use legacy resume.mode behavior
        VAL_RESUME_POLICY_SAFE_DEFAULT =
            1, // Verify tail when possible; restart on mismatch; never truncate; skip when equal+match
        VAL_RESUME_POLICY_ALWAYS_START_ZERO = 2,
        VAL_RESUME_POLICY_ALWAYS_SKIP_IF_EXISTS = 3,
        VAL_RESUME_POLICY_SKIP_IF_DIFFERENT = 4, // Verify; on mismatch SKIP; never overwrite
        VAL_RESUME_POLICY_ALWAYS_SKIP = 5,       // Skip all files unconditionally
        VAL_RESUME_POLICY_STRICT_RESUME_ONLY = 6 // Only proceed if verify allows resume; otherwise ABORT_FILE
    } val_resume_policy_t;

    // Action on verify mismatch
    typedef enum
    {
        VAL_RESUME_MISMATCH_START_ZERO = 1,
        VAL_RESUME_MISMATCH_SKIP_FILE = 2,
        VAL_RESUME_MISMATCH_ABORT_FILE = 3,
    } val_resume_mismatch_action_t;

    // Action on filesystem anomaly (e.g., not a regular file, permission error)
    typedef enum
    {
        VAL_FS_ANOMALY_SKIP_FILE = 1,
        VAL_FS_ANOMALY_ABORT_FILE = 2,
    } val_fs_anomaly_action_t;

    // Verify algorithm selector (extensible). Currently only CRC32.
    typedef enum
    {
        VAL_VERIFY_ALGO_CRC32 = 1
    } val_verify_algo_t;

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
            uint32_t (*get_ticks_ms)(void);
            void (*delay_ms)(uint32_t ms);
        } system;

        // Timeouts (ms). Zero values pick sensible defaults.
        struct
        {
            uint32_t handshake_ms; // initial hello exchange
            uint32_t meta_ms;      // metadata send/receive
            uint32_t data_ms;      // data packet receive
            uint32_t ack_ms;       // ack wait per chunk
            uint32_t idle_ms;      // idle wait between files
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

        struct
        {
            val_resume_mode_t mode;
            uint32_t verify_bytes;
            // New policy-driven controls (preferred). If 'policy' is non-zero, it takes precedence over legacy 'mode'.
            val_resume_policy_t policy;                      // default SAFE_DEFAULT if 0 during session creation
            val_resume_mismatch_action_t on_verify_mismatch; // default depends on policy (START_ZERO for SAFE_DEFAULT)
            val_fs_anomaly_action_t on_fs_anomaly;           // default SKIP_FILE
            val_verify_algo_t verify_algo;                   // default CRC32
            // Optional future hook (not yet wired): application override for per-file decision
            // int (*on_resume_decide)(const val_meta_t* meta, const val_local_info_t* local, val_resume_policy_t policy);
        } resume;

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
    val_session_t *val_session_create(const val_config_t *config);
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

#ifdef __cplusplus
}
#endif

#endif // VAL_PROTOCOL_H
