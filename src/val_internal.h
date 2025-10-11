#ifndef VAL_INTERNAL_H
#define VAL_INTERNAL_H

#include "val_errors.h"
#include "val_protocol.h"
#include "val_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#define VAL_PACKET_MAGIC VAL_MAGIC

// Compile-time log level: 0=OFF, 1=CRITICAL, 2=WARNING, 3=INFO, 4=DEBUG, 5=TRACE
#ifndef VAL_LOG_LEVEL
#if defined(NDEBUG)
#define VAL_LOG_LEVEL 0
#else
#define VAL_LOG_LEVEL 5
#endif
#endif

// Internal logging shims; implemented in val_core.c
void val_internal_log(val_session_t *s, int level, const char *file, int line, const char *msg);
void val_internal_logf(val_session_t *s, int level, const char *file, int line, const char *fmt, ...);

// CRITICAL
#if VAL_LOG_LEVEL >= 1
#define VAL_LOG_CRIT(s, msg) val_internal_log((s), VAL_LOG_CRITICAL, __FILE__, __LINE__, (msg))
#define VAL_LOG_CRITF(s, ...) val_internal_logf((s), VAL_LOG_CRITICAL, __FILE__, __LINE__, __VA_ARGS__)
#else
#define VAL_LOG_CRIT(s, msg) ((void)0)
#define VAL_LOG_CRITF(s, fmt, ...) ((void)0)
#endif
// WARNING
#if VAL_LOG_LEVEL >= 2
#define VAL_LOG_WARN(s, msg) val_internal_log((s), VAL_LOG_WARNING, __FILE__, __LINE__, (msg))
#define VAL_LOG_WARNF(s, ...) val_internal_logf((s), VAL_LOG_WARNING, __FILE__, __LINE__, __VA_ARGS__)
#else
#define VAL_LOG_WARN(s, msg) ((void)0)
#define VAL_LOG_WARNF(s, fmt, ...) ((void)0)
#endif
// INFO
#if VAL_LOG_LEVEL >= 3
#define VAL_LOG_INFO(s, msg) val_internal_log((s), VAL_LOG_INFO, __FILE__, __LINE__, (msg))
#define VAL_LOG_INFOF(s, ...) val_internal_logf((s), VAL_LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#else
#define VAL_LOG_INFO(s, msg) ((void)0)
#define VAL_LOG_INFOF(s, fmt, ...) ((void)0)
#endif
// DEBUG
#if VAL_LOG_LEVEL >= 4
#define VAL_LOG_DEBUG(s, msg) val_internal_log((s), VAL_LOG_DEBUG, __FILE__, __LINE__, (msg))
#define VAL_LOG_DEBUGF(s, ...) val_internal_logf((s), VAL_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define VAL_LOG_DEBUG(s, msg) ((void)0)
#define VAL_LOG_DEBUGF(s, fmt, ...) ((void)0)
#endif
// TRACE
#if VAL_LOG_LEVEL >= 5
#define VAL_LOG_TRACE(s, msg) val_internal_log((s), VAL_LOG_TRACE, __FILE__, __LINE__, (msg))
#define VAL_LOG_TRACEF(s, ...) val_internal_logf((s), VAL_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#else
#define VAL_LOG_TRACE(s, msg) ((void)0)
#define VAL_LOG_TRACEF(s, fmt, ...) ((void)0)
#endif

// Backwards compatibility aliases (ERROR -> CRITICAL)
#define VAL_LOG_ERROR(s, msg) VAL_LOG_CRIT((s), (msg))
#define VAL_LOG_ERRORF(s, ...) VAL_LOG_CRITF((s), __VA_ARGS__)

// Numeric-only error log helper (no strings)
#define VAL_LOG_ERROR_CODE(s, code, detail) VAL_LOG_CRITF((s), "ERR:%d DET:0x%08X", (int)(code), (unsigned)(detail))

// Packet Types are declared in public header (val_packet_type_t)

// Use public val_resume_action_t enum from val_protocol.h

// Minimal session struct
// Adaptive timing state (RFC 6298-inspired)
typedef struct
{
    uint32_t srtt_ms;        // smoothed RTT (ms)
    uint32_t rttvar_ms;      // RTT variance (ms)
    uint32_t min_timeout_ms; // floor
    uint32_t max_timeout_ms; // ceiling
    uint8_t samples_taken;   // number of valid samples incorporated
    uint8_t in_retransmit;   // Karn's algorithm flag (do not sample when set)
} val_timing_t;

struct val_session_s
{
    val_config_t cfg;
    const val_config_t *config; // keep pointer to external for callbacks
    char output_directory[VAL_MAX_PATH + 1];
    uint32_t seq_counter;
    size_t effective_packet_size; // negotiated packet size after handshake
    bool handshake_done;          // handshake completed once per session
    uint32_t peer_features;       // features advertised by peer during handshake
    val_timing_t timing;
    // last error info
    val_error_t last_error;
    // --- Bounded-window flow control state ---
    // Negotiated caps and dynamic window
    uint16_t negotiated_window_packets; // min(local desired_tx, peer rx_max)
    uint16_t current_window_packets;    // dynamic window used by sender (AIMD-tuned)
    uint16_t peer_tx_window_packets;    // best-effort: peer's tx cap from HELLO (for observability)
    uint16_t ack_stride_packets;        // receiver's preferred ACK cadence (0 => window)
    // Performance counters
    uint32_t consecutive_errors;
    uint32_t consecutive_successes;
    uint32_t packets_since_mode_change;
    // Window/sequence tracking
    uint32_t packets_in_flight;
    uint32_t next_seq_to_send;
    uint32_t oldest_unacked_seq;
    // In-flight packet tracking array (allocated at session create)
    struct val_inflight_packet_s *tracking_slots;
    uint32_t max_tracking_slots;
    // File state for retransmissions (sender side)
    void *current_file_handle;
    uint64_t current_file_position;
    uint64_t total_file_size;
    // Keepalive timestamps (ms since ticks) – optional
    uint32_t last_keepalive_send_time;
    uint32_t last_keepalive_recv_time;
    // Connection health monitoring (graceful failure on extreme conditions)
    struct
    {
        uint32_t operations; // Total operations attempted (incremented each send/recv/wait)
        uint32_t retries;    // Total retries across all operations
        uint32_t soft_trips; // Number of soft performance trips observed (reset on progress)
    } health;
#if VAL_ENABLE_METRICS
    // Metrics counters (zeroed at session create)
    val_metrics_t metrics;
#endif
    // No wire audit; use capture hook via config
    // thread-safety primitive (coarse serialization per session)
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

// In-flight packet tracking slot
typedef struct val_inflight_packet_s
{
    uint32_t sequence;       // sequence number
    uint64_t file_offset;    // file offset
    uint32_t payload_length; // data length
    uint32_t send_timestamp; // ticks at send
    uint8_t retransmit_count;
    uint8_t state; // 0=SENT,1=ACKED,2=TIMEOUT
} val_inflight_packet_t;

// (Legacy transmission mode helpers removed in 0.7)

// Mode synchronization payloads
// Extended handshake payload with adaptive fields declared in val_wire.h

// Internal locking helpers (recursive on POSIX via mutex attr; CRITICAL_SECTION is recursive on Windows)
static VAL_FORCE_INLINE void val_internal_lock_init(val_session_t *s)
{
#if defined(_WIN32)
    InitializeCriticalSection(&s->lock);
#else
    // Initialize a recursive mutex so internal helpers can lock even when public API already holds the lock
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#if defined(PTHREAD_MUTEX_RECURSIVE)
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#else
    // Fallback for platforms using the NP constant
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
#endif
    pthread_mutex_init(&s->lock, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

static VAL_FORCE_INLINE void val_internal_lock(val_session_t *s)
{
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}
static VAL_FORCE_INLINE void val_internal_unlock(val_session_t *s)
{
#if defined(_WIN32)
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}

static VAL_FORCE_INLINE void val_internal_lock_destroy(val_session_t *s)
{
#if defined(_WIN32)
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
#endif
}

// Cross-platform path helpers
static VAL_FORCE_INLINE char val_internal_path_sep(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

// Joins dir and name using the platform path separator. If dir is empty or NULL, just writes name.
// Returns the number of characters written (snprintf-style) or a negative value on error.
static VAL_FORCE_INLINE int val_internal_join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    if (!dst || cap == 0)
        return -1;
    if (!name)
        name = "";
    if (dir && dir[0])
        return snprintf(dst, cap, "%s%c%s", dir, val_internal_path_sep(), name);
    return snprintf(dst, cap, "%s", name);
}

// Internal helpers
int val_internal_send_packet(val_session_t *s, val_packet_type_t type, const void *payload, uint32_t payload_len,
                             uint64_t offset);
// Extended variant: for VAL_PKT_DATA, include_data_offset controls whether to include the explicit 8-byte offset
// in the frame content and set VAL_DATA_OFFSET_PRESENT. Ignored for other packet types. When not included, the
// receiver must treat the DATA packet as having an implied offset equal to its current next-expected position.
int val_internal_send_packet_ex(val_session_t *s, val_packet_type_t type, const void *payload, uint32_t payload_len,
                                uint64_t offset, int include_data_offset);
int val_internal_recv_packet(val_session_t *s, val_packet_type_t *type, void *payload_out, uint32_t payload_cap,
                             uint32_t *payload_len_out, uint64_t *offset_out, uint32_t timeout_ms);

// Micro-poll until a packet arrives or the absolute deadline passes, slicing waits to remain cancel-responsive.
// Returns VAL_OK and fills out_type/len/off on packet; VAL_ERR_TIMEOUT when deadline elapses; VAL_ERR_ABORTED on cancel;
// any other error is returned immediately. TIMEOUT/CRC within slices are treated as benign and do not end early.
val_status_t val_internal_recv_until_deadline(val_session_t *s,
                                              val_packet_type_t *out_type,
                                              uint8_t *payload_out, uint32_t payload_cap,
                                              uint32_t *out_len, uint64_t *out_off,
                                              uint32_t deadline_ms,
                                              uint32_t max_slice_ms);

// Generic control wait helper callbacks
typedef int (*val_ctrl_accept_fn)(val_session_t *s, val_packet_type_t t,
                                  const uint8_t *payload, uint32_t len, uint64_t off, void *ctx);
typedef val_status_t (*val_ctrl_on_timeout_fn)(val_session_t *s, void *ctx);

// Wait for a specific control condition using micro-polling, retries, and optional backoff/resend.
// - Calls recv-until-deadline in short slices to remain cancel-responsive.
// - CANCEL => VAL_ERR_ABORTED; ERROR => VAL_ERR_PROTOCOL.
// - On TIMEOUT/CRC: optional on_timeout() is invoked (e.g., to resend), then backoff doubles per retry.
// - accept() returns 1 to accept and stop, 0 to ignore and continue, <0 to treat as protocol error.
// On success, returns VAL_OK and writes the last observed packet to out_*.
val_status_t val_internal_wait_control(val_session_t *s,
                                       uint32_t timeout_ms,
                                       uint8_t retries,
                                       uint32_t backoff_ms_base,
                                       uint8_t *scratch, uint32_t scratch_cap,
                                       val_packet_type_t *out_type,
                                       uint32_t *out_len,
                                       uint64_t *out_off,
                                       val_ctrl_accept_fn accept,
                                       val_ctrl_on_timeout_fn on_timeout,
                                       void *ctx);

// Centralized control ACK waits
val_status_t val_internal_wait_done_ack(val_session_t *s, uint64_t file_size);
val_status_t val_internal_wait_eot_ack(val_session_t *s);

// Centralized VERIFY result wait with resend-on-timeout. Caller should have already
// constructed the VERIFY payload bytes (e.g., serialized val_resume_resp_t with verify_crc/len).
// On success, returns VAL_OK and writes the resolved resume offset to out_resume_offset as follows:
//  - Peer OK     -> out_resume_offset = end_off (resume at requested end)
//  - Peer SKIP   -> out_resume_offset = UINT64_MAX (sentinel to skip file)
//  - Peer MISMATCH -> out_resume_offset = 0 (restart from zero)
// If the peer returns a non-OK error status, that status is propagated as the function return.
val_status_t val_internal_wait_verify_result(val_session_t *s,
                                             const uint8_t *verify_payload,
                                             uint32_t verify_payload_len,
                                             uint64_t end_off,
                                             uint64_t *out_resume_offset);

// Centralized RESUME_RESP wait with resend-on-timeout policy.
// On success, writes the RESUME_RESP payload bytes into payload_out (up to payload_cap) and sets out_len/off.
val_status_t val_internal_wait_resume_resp(val_session_t *s,
                                           uint32_t base_timeout_ms,
                                           uint8_t retries,
                                           uint32_t backoff_ms_base,
                                           uint8_t *payload_out,
                                           uint32_t payload_cap,
                                           uint32_t *out_len,
                                           uint64_t *out_off);

// Receiver-side: wait for a VERIFY request from sender (after we signaled VERIFY_FIRST).
// If a stray RESUME_REQ arrives while waiting, re-send the provided RESUME_RESP payload.
// On success, write the VERIFY payload into payload_out and set out_len/off.
val_status_t val_internal_wait_verify_request_rx(val_session_t *s,
                                                 uint32_t base_timeout_ms,
                                                 uint8_t retries,
                                                 uint32_t backoff_ms_base,
                                                 const uint8_t *resume_resp_payload,
                                                 uint32_t resume_resp_len,
                                                 uint8_t *payload_out,
                                                 uint32_t payload_cap,
                                                 uint32_t *out_len,
                                                 uint64_t *out_off);

// Backoff helper: delay current backoff, double with ceiling, and decrement retries.
static VAL_FORCE_INLINE void val_internal_backoff_step(val_session_t *s, uint32_t *backoff_ms, uint8_t *retries)
{
    if (!s || !backoff_ms || !retries)
        return;
    if (*retries == 0)
        return;
    if (*backoff_ms && s->config && s->config->system.delay_ms)
        s->config->system.delay_ms(*backoff_ms);
    // Cap exponential growth to a sane upper bound (e.g., 4 seconds) to avoid very long sleeps
    uint32_t next = (*backoff_ms == 0) ? 0 : (*backoff_ms << 1);
    if (next > 4000u)
        next = 4000u;
    *backoff_ms = next;
    --(*retries);
}

// Tiny sugar wrappers over val_internal_wait_control to centralize policy and shrink call sites.
// DONE_ACK/EOT_ACK also record RTT when rtt_start_ms!=0 and we did not retransmit.
val_status_t val_internal_wait_done_ack_ex(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                           uint32_t backoff_ms_base, uint32_t rtt_start_ms);
val_status_t val_internal_wait_eot_ack_ex(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                          uint32_t backoff_ms_base, uint32_t rtt_start_ms);

// HELLO wait: if sender_or_receiver!=0 (sender), will re-send HELLO on timeouts; receiver uses no resend.
val_status_t val_internal_wait_hello(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                     uint32_t backoff_ms_base, int sender_or_receiver);

// VERIFY wait with optional handling of stray RESUME_REQ: if resend_ctx provided and contains a RESUME_RESP
// payload, the wrapper will re-send that payload whenever a benign RESUME_REQ arrives while waiting.
typedef struct val_verify_wait_ctx_s {
    const uint8_t *verify_payload;
    uint32_t verify_payload_len;
    const uint8_t *resume_resp_payload; // optional; if non-null and len>0, sent on stray RESUME_REQ
    uint32_t resume_resp_len;
    uint64_t end_off;                 // desired resume offset if OK
    uint64_t *out_resume_offset;      // where to write the mapping (OK->end_off, SKIPPED->UINT64_MAX, MISMATCH->0)
} val_verify_wait_ctx_t;

val_status_t val_internal_wait_verify(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                      uint32_t backoff_ms_base, const val_verify_wait_ctx_t *resend_ctx);

// Operation kinds for timeout selection
typedef enum
{
    VAL_OP_HANDSHAKE = 1,
    VAL_OP_META = 2,
    VAL_OP_DATA_ACK = 3,
    VAL_OP_VERIFY = 4,
    VAL_OP_DONE_ACK = 5,
    VAL_OP_EOT_ACK = 6,
    VAL_OP_DATA_RECV = 7
} val_operation_type_t;

// Adaptive timeout API (RFC6298)
void val_internal_init_timing(val_session_t *s);
void val_internal_record_rtt(val_session_t *s, uint32_t measured_rtt_ms);
uint32_t val_internal_get_timeout(val_session_t *s, val_operation_type_t op);

#if VAL_ENABLE_METRICS
// Internal helpers to update metrics; compile to no-ops when disabled
static VAL_FORCE_INLINE void val_metrics_inc_timeout(val_session_t *s)
{
    if (s)
    {
        s->metrics.timeouts++;
    }
}
// Explicit soft timeout increment (interim slice/backoff events)
// Soft timeout metrics removed - only hard timeouts are meaningful
// Explicit hard timeout increment (terminal operation failure after retries)
static VAL_FORCE_INLINE void val_metrics_inc_timeout_hard(val_session_t *s)
{
    if (s)
    {
        s->metrics.timeouts++;
        s->metrics.timeouts_hard++;
    }
}
static VAL_FORCE_INLINE void val_metrics_inc_retrans(val_session_t *s)
{
    if (s)
        s->metrics.retransmits++;
}
static VAL_FORCE_INLINE void val_metrics_inc_crcerr(val_session_t *s)
{
    if (s)
        s->metrics.crc_errors++;
}
static VAL_FORCE_INLINE void val_metrics_inc_rtt_sample(val_session_t *s)
{
    if (s)
        s->metrics.rtt_samples++;
}
static VAL_FORCE_INLINE void val_metrics_note_handshake(val_session_t *s)
{
    if (s)
        s->metrics.handshakes++;
}
static VAL_FORCE_INLINE void val_metrics_inc_files_sent(val_session_t *s)
{
    if (s)
        s->metrics.files_sent++;
}
static VAL_FORCE_INLINE void val_metrics_inc_files_recv(val_session_t *s)
{
    if (s)
        s->metrics.files_recv++;
}
static VAL_FORCE_INLINE void val_metrics_add_sent(val_session_t *s, size_t bytes, uint8_t type)
{
    if (s)
    {
        s->metrics.packets_sent++;
        s->metrics.bytes_sent += (uint64_t)bytes;
        s->metrics.send_by_type[(unsigned)(type & 31u)]++;
    }
}
static VAL_FORCE_INLINE void val_metrics_add_recv(val_session_t *s, size_t bytes, uint8_t type)
{
    if (s)
    {
        s->metrics.packets_recv++;
        s->metrics.bytes_recv += (uint64_t)bytes;
        s->metrics.recv_by_type[(unsigned)(type & 31u)]++;
    }
}
#else
static VAL_FORCE_INLINE void val_metrics_inc_timeout(val_session_t *s)
{
    (void)s;
}
// Soft timeout metrics removed - only hard timeouts are meaningful
static VAL_FORCE_INLINE void val_metrics_inc_timeout_hard(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_inc_retrans(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_inc_crcerr(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_inc_rtt_sample(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_note_handshake(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_inc_files_sent(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_inc_files_recv(val_session_t *s)
{
    (void)s;
}
static VAL_FORCE_INLINE void val_metrics_add_sent(val_session_t *s, size_t bytes, uint8_t type)
{
    (void)s;
    (void)bytes;
    (void)type;
}
static VAL_FORCE_INLINE void val_metrics_add_recv(val_session_t *s, size_t bytes, uint8_t type)
{
    (void)s;
    (void)bytes;
    (void)type;
}
#endif

// Optional transport helpers (safe wrappers)
static VAL_FORCE_INLINE int val_internal_transport_is_connected(val_session_t *s)
{
    if (!s || !s->config)
        return 0;
    if (s->config->transport.is_connected)
    {
        int r = s->config->transport.is_connected(s->config->transport.io_context);
        // Treat negative/unknown as connected to avoid false disconnects; only 0 is definite false
        return (r == 0) ? 0 : 1;
    }
    // No hook -> assume connected
    return 1;
}

static VAL_FORCE_INLINE void val_internal_transport_flush(val_session_t *s)
{
    if (s && s->config && s->config->transport.flush)
    {
        s->config->transport.flush(s->config->transport.io_context);
    }
}

// Resume helpers
val_status_t val_internal_handle_file_resume(val_session_t *session, const char *filename, const char *sender_path,
                                             uint64_t file_size, uint64_t *out_resume_offset);

// String utils
size_t val_internal_strnlen(const char *s, size_t maxlen);

// Handshake payload is defined in val_wire.h

// Feature bits are defined publicly in val_protocol.h
// VAL_BUILTIN_FEATURES should include ONLY negotiable/optional features compiled into this build.
#ifndef VAL_BUILTIN_FEATURES
#define VAL_BUILTIN_FEATURES (0u)
#endif

// Handshake helpers
val_status_t val_internal_do_handshake_sender(val_session_t *s);
val_status_t val_internal_do_handshake_receiver(val_session_t *s);

// Error helpers
val_status_t val_internal_send_error(val_session_t *s, val_status_t code, uint32_t detail);

// Record last error in session (unified)
void val_internal_set_last_error(val_session_t *s, val_status_t code, uint32_t detail);
// Full setter including op/site string
void val_internal_set_last_error_full(val_session_t *s, val_status_t code, uint32_t detail, const char *op);
// Compatibility helper: previous API name; now routes to full setter with op=__FUNCTION__ and logs numeric-only
static VAL_FORCE_INLINE void val_internal_set_error_detailed(val_session_t *s, val_status_t code, uint32_t detail)
{
    val_internal_set_last_error_full(s, code, detail, __FUNCTION__);
    VAL_LOG_ERROR_CODE(s, code, detail);
}

// Convenience macros
#define VAL_SET_NETWORK_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_IO, (detail))
#define VAL_SET_CRC_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_CRC, (detail))
#define VAL_SET_TIMEOUT_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_TIMEOUT, (detail))
#define VAL_SET_PROTOCOL_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_PROTOCOL, (detail))
#define VAL_SET_FEATURE_ERROR(s, missing_mask)                                                                                   \
    val_internal_set_error_detailed((s), VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE((missing_mask)))
#define VAL_SET_PERFORMANCE_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_PERFORMANCE, (detail))

// Connection health monitoring (diagnostics). When VAL_BUILD_DIAGNOSTICS is not defined,
// these become no-ops to save code size and runtime.
#ifdef VAL_BUILD_DIAGNOSTICS
// Tracks retry rate and aborts if connection quality is unacceptable
static inline val_status_t val_internal_check_health(val_session_t *s)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;

    // Wait for initial settling period (handshake, metadata exchange)
    // Require a larger sample size before enforcing health thresholds to avoid false positives
    // during normal loss/retransmit bursts at start-up.
    // Use total attempts (operations + retries) as the sample size so that frequent retries
    // still advance the sampler and avoid dividing by an inappropriately small operations count.
    uint32_t attempts = s->health.operations + s->health.retries;
    // Do not enforce until we have a meaningful window of observations
    if (attempts < 64u)
    {
        VAL_LOG_DEBUGF(s, "health: check skipped, ops=%u < 64 (attempts=%u)", s->health.operations, attempts);
        return VAL_OK;
    }

    VAL_LOG_DEBUGF(s, "health: checking (v2) ops=%u retries=%u attempts=%u", s->health.operations, s->health.retries, attempts);

    // Check retry rate: if >= ~25% of total attempts are retries, treat as unusable.
    // This avoids tripping under moderate loss while still catching sustained extreme conditions.
    // Use shifts to approximate 25%: attempts / 4 == attempts >> 2
    // Also require an absolute minimum of a few retries to avoid tripping on tiny samples that
    // barely exceed the ratio threshold.
    // Use a stricter ratio (>=50%) and absolute minimum (>=8 retries) to classify as excessive.
    if (s->health.retries >= 8u && (s->health.retries * 2u) > attempts)
    {
        VAL_LOG_CRITF(s, "health: excessive retries ops=%u retries=%u attempts=%u",
                      s->health.operations, s->health.retries, attempts);
        VAL_SET_PERFORMANCE_ERROR(s, VAL_ERROR_DETAIL_EXCESSIVE_RETRIES);
        return VAL_ERR_PERFORMANCE;
    }

    VAL_LOG_DEBUG(s, "health: check passed");
    return VAL_OK;
}

// Health tracking helpers
#define VAL_HEALTH_RECORD_OPERATION(s) do { \
    (s)->health.operations++; \
    VAL_LOG_DEBUGF((s), "health: operation recorded, total ops=%u retries=%u", \
                   (s)->health.operations, (s)->health.retries); \
} while(0)

#define VAL_HEALTH_RECORD_RETRY(s) do { \
    (s)->health.retries++; \
    VAL_LOG_DEBUGF((s), "health: retry recorded, total ops=%u retries=%u", \
                   (s)->health.operations, (s)->health.retries); \
} while(0)
#else
static inline val_status_t val_internal_check_health(val_session_t *s)
{
    (void)s;
    return VAL_OK;
}
#define VAL_HEALTH_RECORD_OPERATION(s) do { (void)(s); } while(0)
#define VAL_HEALTH_RECORD_RETRY(s) do { (void)(s); } while(0)
#endif

// Internal CRC incremental helpers (for file-level CRC)
uint32_t val_crc32_init_state(void);
uint32_t val_crc32_update_state(uint32_t state, const void *data, size_t length);
uint32_t val_crc32_finalize_state(uint32_t state);

// Session-aware CRC adapter (prefer user provider if present)
uint32_t val_internal_crc32(val_session_t *s, const void *data, size_t length);

// Compute CRC32 over a file region using the session's configured filesystem and recv buffer.
// Reads [start_offset, start_offset+length) from the given open file handle in chunk sizes based
// on the negotiated/effective packet size, updating the CRC incrementally. Returns VAL_OK and
// writes the CRC to out_crc on success. On error, returns VAL_ERR_IO or VAL_ERR_INVALID_ARG.
val_status_t val_internal_crc32_region(val_session_t *s, void *file_handle, uint64_t start_offset,
                                       uint64_t length, uint32_t *out_crc);

// Adaptive transmission mode management
void val_internal_record_transmission_error(val_session_t *s);
void val_internal_record_transmission_success(val_session_t *s);

#endif // VAL_INTERNAL_H
