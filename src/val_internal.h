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

// Packet Types
typedef enum
{
    VAL_PKT_HELLO = 1,       // session/version negotiation
    VAL_PKT_SEND_META = 2,   // filename, size, path
    VAL_PKT_RESUME_REQ = 3,  // sender asks resume options
    VAL_PKT_RESUME_RESP = 4, // receiver responds with action
    VAL_PKT_DATA = 5,        // file data chunk
    VAL_PKT_DATA_ACK = 6,    // ack for data chunk
    VAL_PKT_VERIFY = 7,      // crc verify request/response
    VAL_PKT_DONE = 8,        // file complete
    VAL_PKT_ERROR = 9,
    VAL_PKT_EOT = 10,      // end of transmission
    VAL_PKT_EOT_ACK = 11,  // ack for end of transmission
    VAL_PKT_DONE_ACK = 12, // ack for end of file
} val_packet_type_t;

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
    uint64_t verify_len;
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
#if VAL_ENABLE_METRICS
    // Metrics counters (zeroed at session create)
    val_metrics_t metrics;
#endif
    // thread-safety primitive (coarse serialization per session)
#if defined(_WIN32)
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

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
static inline void val_metrics_add_sent(val_session_t *s, size_t bytes)
{
    if (s)
    {
        s->metrics.packets_sent++;
        s->metrics.bytes_sent += (uint64_t)bytes;
    }
}
static inline void val_metrics_add_recv(val_session_t *s, size_t bytes)
{
    if (s)
    {
        s->metrics.packets_recv++;
        s->metrics.bytes_recv += (uint64_t)bytes;
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
static inline void val_metrics_add_sent(val_session_t *s, size_t bytes)
{
    (void)s;
    (void)bytes;
}
static inline void val_metrics_add_recv(val_session_t *s, size_t bytes)
{
    (void)s;
    (void)bytes;
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

// Handshake payload
typedef struct
{
    uint32_t magic; // VAL_MAGIC
    uint8_t version_major;
    uint8_t version_minor;
    uint16_t reserved;
    uint32_t packet_size; // required packet size for this endpoint
    uint32_t features;    // bitfield for future capability negotiation all compiled in features
    uint32_t required;    // bitfield for future capability negotiation user set on required features to use. if one side doesn't
                          // have transfer aborts
    uint32_t requested;   // bitfield for future capability negotiation user set on requested features to use if peer supports.
} val_handshake_t;

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

#endif // VAL_INTERNAL_H
