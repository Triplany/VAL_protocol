#ifndef VAL_INTERNAL_H
#define VAL_INTERNAL_H

#include "val_errors.h"
#include "val_protocol.h"
#include "val_wire.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
#define VAL_LOG_LEVEL 4
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

typedef enum
{
    VAL_RESUME_ACTION_START_ZERO = 0,
    VAL_RESUME_ACTION_START_OFFSET = 1,
    VAL_RESUME_ACTION_VERIFY_FIRST = 2,
    VAL_RESUME_ACTION_SKIP_FILE = 3,
    VAL_RESUME_ACTION_ABORT_FILE = 4,
} val_resume_action_t;

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
    uint8_t handshake_done;       // handshake completed once per session
    uint32_t peer_features;       // features advertised by peer during handshake
    val_timing_t timing;
    // last error info
    val_error_t last_error;
    // --- Adaptive transmission state (Phase 1 scaffolding) ---
    // Negotiated/active mode tracking
    val_tx_mode_t current_tx_mode;     // current active window rung
    val_tx_mode_t peer_tx_mode;        // peer's last known window rung
    val_tx_mode_t min_negotiated_mode; // best performance (largest window) both sides support
    val_tx_mode_t max_negotiated_mode; // most reliable mode (stop-and-wait)
    // Streaming negotiation (directional)
    uint8_t send_streaming_allowed; // we may stream when we are sender to peer
    uint8_t recv_streaming_allowed; // we accept peer streaming to us
    // Streaming runtime state: engaged when at fastest rung and sustained successes
    uint8_t streaming_engaged;
    // Peer runtime streaming state (best-effort, from MODE_SYNC)
    uint8_t peer_streaming_engaged;
    // Performance counters
    uint32_t consecutive_errors;
    uint32_t consecutive_successes;
    uint32_t packets_since_mode_change;
    uint32_t packets_since_mode_sync;
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
    // Mode sync state
    uint32_t mode_sync_sequence;
    uint32_t last_mode_sync_time;
    // Streaming keepalive timestamps (ms since ticks)
    uint32_t last_keepalive_send_time; // last time we sent a keepalive (receiver side)
    uint32_t last_keepalive_recv_time; // last time we observed a keepalive from peer (sender side)
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

// Transmission mode helpers
static VAL_FORCE_INLINE int val_tx_mode_is_valid(val_tx_mode_t mode)
{
    switch (mode)
    {
    case VAL_TX_WINDOW_64:
    case VAL_TX_WINDOW_32:
    case VAL_TX_WINDOW_16:
    case VAL_TX_WINDOW_8:
    case VAL_TX_WINDOW_4:
    case VAL_TX_WINDOW_2:
    case VAL_TX_STOP_AND_WAIT:
        return 1;
    default:
        return 0;
    }
}

static VAL_FORCE_INLINE val_tx_mode_t val_tx_mode_sanitize(val_tx_mode_t mode)
{
    return val_tx_mode_is_valid(mode) ? mode : VAL_TX_STOP_AND_WAIT;
}

static VAL_FORCE_INLINE uint32_t val_tx_mode_window(val_tx_mode_t mode)
{
    switch (mode)
    {
    case VAL_TX_WINDOW_64:     return 64u;
    case VAL_TX_WINDOW_32:     return 32u;
    case VAL_TX_WINDOW_16:     return 16u;
    case VAL_TX_WINDOW_8:      return 8u;
    case VAL_TX_WINDOW_4:      return 4u;
    case VAL_TX_WINDOW_2:      return 2u;
    case VAL_TX_STOP_AND_WAIT: return 1u;
    default:                   return 1u;
    }
}

static VAL_FORCE_INLINE val_tx_mode_t val_tx_mode_from_window(uint32_t window)
{
    switch (window)
    {
    case 64u: return VAL_TX_WINDOW_64;
    case 32u: return VAL_TX_WINDOW_32;
    case 16u: return VAL_TX_WINDOW_16;
    case 8u:  return VAL_TX_WINDOW_8;
    case 4u:  return VAL_TX_WINDOW_4;
    case 2u:  return VAL_TX_WINDOW_2;
    case 1u:  return VAL_TX_STOP_AND_WAIT;
    default:
        if (window >= 64u)
            return VAL_TX_WINDOW_64;
        if (window >= 32u)
            return VAL_TX_WINDOW_32;
        if (window >= 16u)
            return VAL_TX_WINDOW_16;
        if (window >= 8u)
            return VAL_TX_WINDOW_8;
        if (window >= 4u)
            return VAL_TX_WINDOW_4;
        if (window >= 2u)
            return VAL_TX_WINDOW_2;
        return VAL_TX_STOP_AND_WAIT;
    }
}

// --- Handshake negotiation helpers (shared) ---
// Compute the negotiated transmission cap (largest common window rung) from local config and peer handshake.
static VAL_FORCE_INLINE val_tx_mode_t val_negotiated_tx_cap(const val_config_t *local_cfg, const val_handshake_t *peer_hs)
{
    if (!local_cfg || !peer_hs)
        return VAL_TX_STOP_AND_WAIT;
    val_tx_mode_t local_max = val_tx_mode_sanitize(local_cfg->adaptive_tx.max_performance_mode);
    val_tx_mode_t peer_max = val_tx_mode_sanitize((val_tx_mode_t)peer_hs->max_performance_mode);
    uint32_t local_w = val_tx_mode_window(local_max);
    uint32_t peer_w = val_tx_mode_window(peer_max);
    uint32_t shared_w = (local_w < peer_w) ? local_w : peer_w;
    return val_tx_mode_from_window(shared_w);
}

// Select a conservative initial mode based on each side's preference and the negotiated cap.
// Logic: clamp both preferences to the cap, then choose the slower (smaller window) of the two.
static VAL_FORCE_INLINE val_tx_mode_t val_select_initial_mode(val_tx_mode_t local_pref,
                                                             val_tx_mode_t peer_pref,
                                                             val_tx_mode_t shared_cap)
{
    val_tx_mode_t lp = val_tx_mode_sanitize(local_pref);
    val_tx_mode_t pp = val_tx_mode_sanitize(peer_pref);
    uint32_t cap_w = val_tx_mode_window(shared_cap);
    // Clamp preferences to cap
    if (val_tx_mode_window(lp) > cap_w)
        lp = shared_cap;
    if (val_tx_mode_window(pp) > cap_w)
        pp = shared_cap;
    // Choose the slower (smaller window) of the two
    uint32_t init_w = (val_tx_mode_window(lp) < val_tx_mode_window(pp))
                          ? val_tx_mode_window(lp)
                          : val_tx_mode_window(pp);
    return val_tx_mode_from_window(init_w);
}

// Mode synchronization payloads
// Extended handshake payload with adaptive fields declared in val_wire.h

// Internal locking helpers (recursive on POSIX via mutex attr; CRITICAL_SECTION is recursive on Windows)
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

// Internal helpers
int val_internal_send_packet(val_session_t *s, val_packet_type_t type, const void *payload, uint32_t payload_len,
                             uint64_t offset);
int val_internal_recv_packet(val_session_t *s, val_packet_type_t *type, void *payload_out, uint32_t payload_cap,
                             uint32_t *payload_len_out, uint64_t *offset_out, uint32_t timeout_ms);

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
        s->metrics.timeouts++;
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

// Handshake payload is defined later with extended fields for adaptive TX (val_handshake_t)

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
    if (attempts < 2u)
    {
        VAL_LOG_DEBUGF(s, "health: check skipped, ops=%u < 2 (attempts=%u)", s->health.operations, attempts);
        return VAL_OK;
    }

    VAL_LOG_DEBUGF(s, "health: checking (v2) ops=%u retries=%u attempts=%u", s->health.operations, s->health.retries, attempts);

    // Check retry rate: if >= ~25% of total attempts are retries, treat as unusable.
    // This avoids tripping under moderate loss while still catching sustained extreme conditions.
    // Use shifts to approximate 25%: attempts / 4 == attempts >> 2
    if (s->health.retries > (attempts >> 2))
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

// Session-aware CRC adapters (prefer user provider if present)
uint32_t val_internal_crc32(val_session_t *s, const void *data, size_t length);
uint32_t val_internal_crc32_init(val_session_t *s);
uint32_t val_internal_crc32_update(val_session_t *s, uint32_t state, const void *data, size_t length);
uint32_t val_internal_crc32_final(val_session_t *s, uint32_t state);

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
