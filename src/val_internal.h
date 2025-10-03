#ifndef VAL_INTERNAL_H
#define VAL_INTERNAL_H

#include "val_errors.h"
#include "val_protocol.h"
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

#pragma pack(push, 1)
typedef struct
{
    uint8_t type; // val_packet_type_t
    // Reserved wire version byte for future on-wire framing changes. Always 0 in base protocol.
    uint8_t wire_version;
    uint16_t reserved2;
    uint32_t payload_len; // bytes valid in payload
    uint32_t seq;         // monotonically increasing per file
    uint64_t offset;      // for data/resume; for DATA_ACK this is the next expected offset (cumulative ACK)
    uint32_t header_crc;  // crc of header without header_crc and trailer crc
} val_packet_header_t;    // header is followed by payload, zero pad, then trailer crc32 (full packet)
#pragma pack(pop)

// Error packet payload (compact: no message strings on wire)
typedef struct
{
    int32_t code;    // val_status_t-compatible negative code
    uint32_t detail; // optional extra information (mask or extra code)
} val_error_payload_t;

// Shared payload structs are declared publicly in val_protocol.h

typedef struct
{
    uint32_t action; // val_resume_action_t
    uint64_t resume_offset;
    uint32_t verify_crc;
    uint64_t verify_len; // 64-bit per design prompt
} val_resume_resp_t;

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
    val_status_t last_error_code;
    uint32_t last_error_detail;
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
#if VAL_ENABLE_METRICS
    // Metrics counters (zeroed at session create)
    val_metrics_t metrics;
#endif
#if VAL_ENABLE_WIRE_AUDIT
    // Wire audit counters (zeroed at session create)
    struct
    {
        // Packet counters
        uint64_t sent[16];
        uint64_t recv[16];
        // Inflight/window audit (sender perspective)
        uint32_t max_inflight_observed;
        uint32_t current_inflight;
    } audit;
#endif
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

// Mode synchronization payloads
typedef struct
{
    uint32_t current_mode;        // val_tx_mode_t
    uint32_t sequence;            // sync sequence
    uint32_t consecutive_errors;  // recent error count
    uint32_t consecutive_success; // recent success count
    uint32_t flags;               // bit0: streaming_engaged (sender side)
} val_mode_sync_t;

typedef struct
{
    uint32_t ack_sequence;    // sequence from sync
    uint32_t agreed_mode;     // agreed val_tx_mode_t
    uint32_t receiver_errors; // receiver view of errors
} val_mode_sync_ack_t;

// Extended handshake payload with adaptive fields
typedef struct
{
    uint32_t magic;        // VAL_MAGIC
    uint8_t version_major; // protocol version
    uint8_t version_minor; // protocol version
    uint16_t reserved;     // keep alignment
    uint32_t packet_size;  // MTU
    uint32_t features;     // optional features (negotiable)
    uint32_t required;     // optional features required by peer
    uint32_t requested;    // optional features requested by peer
    // Adaptive TX negotiation
    uint8_t max_performance_mode;   // window rung cap (val_tx_mode_t)
    uint8_t preferred_initial_mode; // initial window rung (val_tx_mode_t)
    uint16_t mode_sync_interval;    // reserved
    uint8_t streaming_flags;        // bit0: tx_stream_capable, bit1: rx_stream_accept, other bits 0
    uint8_t reserved_streaming[3];  // pad to 4-byte boundary
    uint16_t supported_features16;  // reserved 16-bit future
    uint16_t required_features16;   // reserved 16-bit future
    uint16_t requested_features16;  // reserved 16-bit future
    uint32_t reserved2;
} val_handshake_t;

// Endianness helpers: on-wire is Little Endian. Convert to/from host as needed.
static inline int val_is_little_endian(void)
{
    const uint16_t x = 0x0102u;
    return (*((const uint8_t *)&x) == 0x02u);
}

static inline uint16_t val_bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t val_bswap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}
static inline uint64_t val_bswap64(uint64_t v)
{
    return ((uint64_t)val_bswap32((uint32_t)(v & 0xFFFFFFFFu)) << 32) | (uint64_t)val_bswap32((uint32_t)(v >> 32));
}

static inline uint16_t val_htole16(uint16_t v)
{
    return val_is_little_endian() ? v : val_bswap16(v);
}
static inline uint32_t val_htole32(uint32_t v)
{
    return val_is_little_endian() ? v : val_bswap32(v);
}
static inline uint64_t val_htole64(uint64_t v)
{
    return val_is_little_endian() ? v : val_bswap64(v);
}
static inline uint16_t val_letoh16(uint16_t v)
{
    return val_is_little_endian() ? v : val_bswap16(v);
}
static inline uint32_t val_letoh32(uint32_t v)
{
    return val_is_little_endian() ? v : val_bswap32(v);
}
static inline uint64_t val_letoh64(uint64_t v)
{
    return val_is_little_endian() ? v : val_bswap64(v);
}

// Internal locking helpers (recursive on POSIX via mutex attr; CRITICAL_SECTION is recursive on Windows)
static inline void val_internal_lock(val_session_t *s)
{
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}
static inline void val_internal_unlock(val_session_t *s)
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
static inline void val_metrics_inc_timeout(val_session_t *s)
{
    if (s)
        s->metrics.timeouts++;
}
static inline void val_metrics_inc_retrans(val_session_t *s)
{
    if (s)
        s->metrics.retransmits++;
}
static inline void val_metrics_inc_crcerr(val_session_t *s)
{
    if (s)
        s->metrics.crc_errors++;
}
static inline void val_metrics_inc_rtt_sample(val_session_t *s)
{
    if (s)
        s->metrics.rtt_samples++;
}
static inline void val_metrics_note_handshake(val_session_t *s)
{
    if (s)
        s->metrics.handshakes++;
}
static inline void val_metrics_inc_files_sent(val_session_t *s)
{
    if (s)
        s->metrics.files_sent++;
}
static inline void val_metrics_inc_files_recv(val_session_t *s)
{
    if (s)
        s->metrics.files_recv++;
}
static inline void val_metrics_add_sent(val_session_t *s, size_t bytes, uint8_t type)
{
    if (s)
    {
        s->metrics.packets_sent++;
        s->metrics.bytes_sent += (uint64_t)bytes;
        s->metrics.send_by_type[(unsigned)(type & 31u)]++;
    }
}
static inline void val_metrics_add_recv(val_session_t *s, size_t bytes, uint8_t type)
{
    if (s)
    {
        s->metrics.packets_recv++;
        s->metrics.bytes_recv += (uint64_t)bytes;
        s->metrics.recv_by_type[(unsigned)(type & 31u)]++;
    }
}
#else
static inline void val_metrics_inc_timeout(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_inc_retrans(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_inc_crcerr(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_inc_rtt_sample(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_note_handshake(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_inc_files_sent(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_inc_files_recv(val_session_t *s)
{
    (void)s;
}
static inline void val_metrics_add_sent(val_session_t *s, size_t bytes, uint8_t type)
{
    (void)s;
    (void)bytes;
    (void)type;
}
static inline void val_metrics_add_recv(val_session_t *s, size_t bytes, uint8_t type)
{
    (void)s;
    (void)bytes;
    (void)type;
}
#endif

// Optional transport helpers (safe wrappers)
static inline int val_internal_transport_is_connected(val_session_t *s)
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

static inline void val_internal_transport_flush(val_session_t *s)
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

// Record last error in session
void val_internal_set_last_error(val_session_t *s, val_status_t code, uint32_t detail);
// New: detailed setter
static inline void val_internal_set_error_detailed(val_session_t *s, val_status_t code, uint32_t detail)
{
    if (!s)
        return;
    val_internal_set_last_error(s, code, detail);
    // Numeric-only logging; on MCU builds avoid string lookups
    VAL_LOG_ERROR_CODE(s, code, detail);
}

// Convenience macros
#define VAL_SET_NETWORK_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_IO, (detail))
#define VAL_SET_CRC_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_CRC, (detail))
#define VAL_SET_TIMEOUT_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_TIMEOUT, (detail))
#define VAL_SET_PROTOCOL_ERROR(s, detail) val_internal_set_error_detailed((s), VAL_ERR_PROTOCOL, (detail))
#define VAL_SET_FEATURE_ERROR(s, missing_mask)                                                                                   \
    val_internal_set_error_detailed((s), VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE((missing_mask)))

// Internal CRC incremental helpers (for file-level CRC)
uint32_t val_crc32_init_state(void);
uint32_t val_crc32_update_state(uint32_t state, const void *data, size_t length);
uint32_t val_crc32_finalize_state(uint32_t state);

// Session-aware CRC adapters (prefer user provider if present)
uint32_t val_internal_crc32(val_session_t *s, const void *data, size_t length);
uint32_t val_internal_crc32_init(val_session_t *s);
uint32_t val_internal_crc32_update(val_session_t *s, uint32_t state, const void *data, size_t length);
uint32_t val_internal_crc32_final(val_session_t *s, uint32_t state);

// Adaptive transmission mode management
void val_internal_record_transmission_error(val_session_t *s);
void val_internal_record_transmission_success(val_session_t *s);

#endif // VAL_INTERNAL_H
