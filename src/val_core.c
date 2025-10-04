#include "val_internal.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A monotonic millisecond clock is required via config.system.get_ticks_ms; no built-in defaults.

// Internal logging implementation. If compile-time logging is enabled and a sink is provided in config,
// forward messages; otherwise, drop them. This function itself is a tiny call and will be removed by
// the compiler entirely when VAL_LOG_LEVEL==0 because no callers remain.
void val_internal_log(val_session_t *s, int level, const char *file, int line, const char *msg)
{
    if (!s || !s->config)
        return;
    // Runtime level filter: forward only if logger exists and level <= min_level (non-zero)
    if (s->config->debug.log)
    {
        int minlvl = s->config->debug.min_level;
        if (minlvl == 0)
            return; // OFF
        if (level <= minlvl)
            s->config->debug.log(s->config->debug.context, level, file, line, msg);
    }
}

void val_internal_logf(val_session_t *s, int level, const char *file, int line, const char *fmt, ...)
{
    char stackbuf[256];
    va_list ap, ap2;
    int needed;
    if (!s || !s->config || !s->config->debug.log || !fmt)
        return;
    // Runtime filter check before doing any formatting work
    int minlvl = s->config->debug.min_level;
    if (minlvl == 0 || level > minlvl)
        return;
    va_start(ap, fmt);
    // Try to compute required length portably
    va_copy(ap2, ap);
    needed = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
#if defined(_MSC_VER)
    if (needed < 0)
    {
        // MSVC returns -1 for insufficient buffer; use _vscprintf
        va_list ap3;
        va_copy(ap3, ap);
        needed = _vscprintf(fmt, ap3);
        va_end(ap3);
    }
#endif
    if (needed < 0)
    {
        // Give up and emit the format string
        va_end(ap);
        s->config->debug.log(s->config->debug.context, level, file, line, fmt);
        return;
    }
    if ((size_t)needed < sizeof(stackbuf))
    {
        (void)vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
        va_end(ap);
        s->config->debug.log(s->config->debug.context, level, file, line, stackbuf);
        return;
    }
    else
    {
        size_t need_bytes = (size_t)needed + 1;
        char *heapbuf = (char *)malloc(need_bytes);
        if (!heapbuf)
        {
            // fallback to truncated stack buffer
            (void)vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
            va_end(ap);
            s->config->debug.log(s->config->debug.context, level, file, line, stackbuf);
            return;
        }
        (void)vsnprintf(heapbuf, need_bytes, fmt, ap);
        va_end(ap);
        s->config->debug.log(s->config->debug.context, level, file, line, heapbuf);
        free(heapbuf);
        return;
    }
}

// Simple CRC32 (IEEE 802.3) table
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void crc32_init_table(void)
{
    if (crc32_table_init)
        return;
    uint32_t poly = 0xEDB88320u;
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
        {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

uint32_t val_crc32(const void *data, size_t length)
{
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
    {
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t val_crc32_init_state(void)
{
    crc32_init_table();
    return 0xFFFFFFFFu;
}

uint32_t val_crc32_update_state(uint32_t state, const void *data, size_t length)
{
    crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = state;
    for (size_t i = 0; i < length; ++i)
    {
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

uint32_t val_crc32_finalize_state(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t val_get_builtin_features(void)
{
    return VAL_BUILTIN_FEATURES;
}

// Public: expose current transmitter mode (thread-safe)
val_status_t val_get_current_tx_mode(val_session_t *session, val_tx_mode_t *out_mode)
{
    if (!session || !out_mode)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_mode = session->current_tx_mode;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

// Public: expose peer's last-known transmitter mode (thread-safe)
val_status_t val_get_peer_tx_mode(val_session_t *session, val_tx_mode_t *out_mode)
{
    if (!session || !out_mode)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_mode = session->peer_tx_mode;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

// Public: expose whether streaming pacing is currently engaged (thread-safe)
val_status_t val_is_streaming_engaged(val_session_t *session, int *out_streaming_engaged)
{
    if (!session || !out_streaming_engaged)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_streaming_engaged = session->streaming_engaged ? 1 : 0;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

val_status_t val_is_peer_streaming_engaged(val_session_t *session, int *out_peer_streaming_engaged)
{
    if (!session || !out_peer_streaming_engaged)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_peer_streaming_engaged = session->peer_streaming_engaged ? 1 : 0;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

// Public: expose negotiated streaming permissions (thread-safe)
val_status_t val_get_streaming_allowed(val_session_t *session, int *out_send_allowed, int *out_recv_allowed)
{
    if (!session || !out_send_allowed || !out_recv_allowed)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_send_allowed = session->send_streaming_allowed ? 1 : 0;
    *out_recv_allowed = session->recv_streaming_allowed ? 1 : 0;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

// Public: expose effective negotiated MTU (thread-safe)
val_status_t val_get_effective_packet_size(val_session_t *session, size_t *out_packet_size)
{
    if (!session || !out_packet_size)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out_packet_size = session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

#if VAL_ENABLE_WIRE_AUDIT
static void val__audit_zero(val_session_t *s)
{
    if (!s)
        return;
    memset(&s->audit, 0, sizeof(s->audit));
}

val_status_t val_get_wire_audit(val_session_t *session, val_wire_audit_t *out)
{
    if (!session || !out)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    // Map internal compact arrays to public struct
    memset(out, 0, sizeof(*out));
    // Sent
    out->sent_hello = session->audit.sent[VAL_PKT_HELLO];
    out->sent_send_meta = session->audit.sent[VAL_PKT_SEND_META];
    out->sent_resume_req = session->audit.sent[VAL_PKT_RESUME_REQ];
    out->sent_resume_resp = session->audit.sent[VAL_PKT_RESUME_RESP];
    out->sent_verify = session->audit.sent[VAL_PKT_VERIFY];
    out->sent_data = session->audit.sent[VAL_PKT_DATA];
    out->sent_data_ack = session->audit.sent[VAL_PKT_DATA_ACK];
    out->sent_done = session->audit.sent[VAL_PKT_DONE];
    out->sent_error = session->audit.sent[VAL_PKT_ERROR];
    out->sent_eot = session->audit.sent[VAL_PKT_EOT];
    out->sent_eot_ack = session->audit.sent[VAL_PKT_EOT_ACK];
    out->sent_done_ack = session->audit.sent[VAL_PKT_DONE_ACK];
    // Recv
    out->recv_hello = session->audit.recv[VAL_PKT_HELLO];
    out->recv_send_meta = session->audit.recv[VAL_PKT_SEND_META];
    out->recv_resume_req = session->audit.recv[VAL_PKT_RESUME_REQ];
    out->recv_resume_resp = session->audit.recv[VAL_PKT_RESUME_RESP];
    out->recv_verify = session->audit.recv[VAL_PKT_VERIFY];
    out->recv_data = session->audit.recv[VAL_PKT_DATA];
    out->recv_data_ack = session->audit.recv[VAL_PKT_DATA_ACK];
    out->recv_done = session->audit.recv[VAL_PKT_DONE];
    out->recv_error = session->audit.recv[VAL_PKT_ERROR];
    out->recv_eot = session->audit.recv[VAL_PKT_EOT];
    out->recv_eot_ack = session->audit.recv[VAL_PKT_EOT_ACK];
    out->recv_done_ack = session->audit.recv[VAL_PKT_DONE_ACK];
    // Window audit
    out->max_inflight_observed = session->audit.max_inflight_observed;
    out->current_inflight = session->audit.current_inflight;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

val_status_t val_reset_wire_audit(val_session_t *session)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    val__audit_zero(session);
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}
#endif // VAL_ENABLE_WIRE_AUDIT

// Metadata validation helpers
void val_config_validation_disabled(val_config_t *config)
{
    if (!config)
        return;
    config->metadata_validation.validator = NULL;
    config->metadata_validation.validator_context = NULL;
}

void val_config_set_validator(val_config_t *config, val_metadata_validator_t validator, void *context)
{
    if (!config)
        return;
    config->metadata_validation.validator = validator;
    config->metadata_validation.validator_context = context;
}

// Session-aware CRC adapters
uint32_t val_internal_crc32(val_session_t *s, const void *data, size_t length)
{
    if (s && s->config && s->config->crc.crc32)
    {
        return s->config->crc.crc32(s->config->crc.crc_context, data, length);
    }
    return val_crc32(data, length);
}

uint32_t val_internal_crc32_init(val_session_t *s)
{
    if (s && s->config && s->config->crc.crc32_init)
    {
        return s->config->crc.crc32_init(s->config->crc.crc_context);
    }
    return val_crc32_init_state();
}

uint32_t val_internal_crc32_update(val_session_t *s, uint32_t state, const void *data, size_t length)
{
    if (s && s->config && s->config->crc.crc32_update)
    {
        return s->config->crc.crc32_update(s->config->crc.crc_context, state, data, length);
    }
    return val_crc32_update_state(state, data, length);
}

uint32_t val_internal_crc32_final(val_session_t *s, uint32_t state)
{
    if (s && s->config && s->config->crc.crc32_final)
    {
        return s->config->crc.crc32_final(s->config->crc.crc_context, state);
    }
    return val_crc32_finalize_state(state);
}

size_t val_internal_strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    if (!s)
        return 0;
    while (n < maxlen && s[n])
        ++n;
    return n;
}

void val_clean_filename(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return;
    output[0] = '\0';
    if (!input)
        return;
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i)
    {
        unsigned char c = (unsigned char)input[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            continue;
        }
        if ((c < 32) || (c == 127))
            continue;
        output[j++] = (char)c;
    }
    if (j == 0 && output_size > 1)
        output[j++] = 'f';
    output[j] = '\0';
}

void val_clean_path(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return;
    output[0] = '\0';
    if (!input)
        return;
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i)
    {
        unsigned char c = (unsigned char)input[i];
        if ((c < 32) || (c == '"') || (c == '<') || (c == '>') || (c == '|') || (c == 127))
            continue;
        output[j++] = (char)c;
    }
    output[j] = '\0';
}

void val_internal_set_last_error(val_session_t *s, val_status_t code, uint32_t detail)
{
    if (!s)
        return;
    // Protect last error fields with session lock
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
    s->last_error_code = code;
    s->last_error_detail = detail;
#if defined(_WIN32)
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}

val_status_t val_get_last_error(val_session_t *session, val_status_t *code, uint32_t *detail_mask)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
    // Protect reads with session lock
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    if (code)
        *code = session->last_error_code;
    if (detail_mask)
        *detail_mask = session->last_error_detail;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

static uint32_t validate_config_details(const val_config_t *cfg)
{
    uint32_t detail = 0;
    if (!cfg)
        return VAL_ERROR_DETAIL_INVALID_STATE; // generic invalid
    if (!cfg->transport.send || !cfg->transport.recv)
        detail |= VAL_ERROR_DETAIL_CONNECTION; // use transport-related code as proxy
    if (!cfg->filesystem.fopen || !cfg->filesystem.fread || !cfg->filesystem.fwrite || !cfg->filesystem.fseek ||
        !cfg->filesystem.ftell || !cfg->filesystem.fclose)
        detail |= VAL_ERROR_DETAIL_PERMISSION; // filesystem hooks missing
    if (!cfg->buffers.send_buffer || !cfg->buffers.recv_buffer)
        detail |= VAL_ERROR_DETAIL_PAYLOAD_SIZE; // buffers missing
    if (cfg->buffers.packet_size < VAL_MIN_PACKET_SIZE || cfg->buffers.packet_size > VAL_MAX_PACKET_SIZE)
        detail |= VAL_ERROR_DETAIL_PACKET_SIZE;
    if (!cfg->system.get_ticks_ms)
        detail |= VAL_ERROR_DETAIL_TIMEOUT_HELLO; // indicate required clock missing
    return detail;
}

static uint32_t calculate_tracking_slots(val_tx_mode_t mode)
{
    uint32_t window = val_tx_mode_window(val_tx_mode_sanitize(mode));
    return (window > 1u) ? window : 0u;
}

val_status_t val_session_create(const val_config_t *config, val_session_t **out_session, uint32_t *out_detail)
{
    if (out_detail)
        *out_detail = 0;
    if (!out_session)
        return VAL_ERR_INVALID_ARG;
    *out_session = NULL;
    uint32_t detail = validate_config_details(config);
    if (detail != 0)
    {
        if (out_detail)
            *out_detail = detail;
        return VAL_ERR_INVALID_ARG;
    }
    // Prefer user allocator when provided
    val_session_t *s = NULL;
    if (config && config->adaptive_tx.allocator.alloc)
        s = (val_session_t *)config->adaptive_tx.allocator.alloc(sizeof(val_session_t), config->adaptive_tx.allocator.context);
    else
        s = (val_session_t *)calloc(1, sizeof(val_session_t));
    if (!s)
        return VAL_ERR_NO_MEMORY;
    // store by value to decouple from caller mutability, but keep pointer for callbacks
    s->cfg = *config;
    s->config = &s->cfg;
    s->seq_counter = 1;
    s->output_directory[0] = '\0';
    s->effective_packet_size = s->cfg.buffers.packet_size; // default until handshake negotiates min
    s->handshake_done = 0;
    s->peer_features = 0;
    s->last_error_code = VAL_OK;
    s->last_error_detail = 0;
    // Initialize adaptive TX scaffolding
    s->current_tx_mode = VAL_TX_STOP_AND_WAIT;
    s->peer_tx_mode = VAL_TX_STOP_AND_WAIT;
    s->peer_streaming_engaged = 0;
    s->streaming_engaged = 0;
    s->min_negotiated_mode = VAL_TX_WINDOW_2;      // placeholder until handshake
    s->max_negotiated_mode = VAL_TX_STOP_AND_WAIT; // always supported
    s->send_streaming_allowed = 0;
    s->recv_streaming_allowed = 0;
    s->consecutive_errors = 0;
    s->consecutive_successes = 0;
    s->packets_since_mode_change = 0;
    s->packets_since_mode_sync = 0;
    s->packets_in_flight = 0;
    s->next_seq_to_send = 0;
    s->oldest_unacked_seq = 0;
    s->tracking_slots = NULL;
    s->max_tracking_slots = 0;
    s->current_file_handle = NULL;
    s->current_file_position = 0;
    s->total_file_size = 0;
    s->mode_sync_sequence = 0;
    s->last_mode_sync_time = 0;
    s->last_keepalive_send_time = 0;
    s->last_keepalive_recv_time = 0;
#if VAL_ENABLE_METRICS
    memset(&s->metrics, 0, sizeof(s->metrics));
#endif
#if VAL_ENABLE_WIRE_AUDIT
    memset(&s->audit, 0, sizeof(s->audit));
#endif
    // Initialize adaptive timing state
    // Clamp/sanitize config bounds: if invalid, swap, and default when zero.
    uint32_t min_to = config->timeouts.min_timeout_ms ? config->timeouts.min_timeout_ms : 200u;
    uint32_t max_to = config->timeouts.max_timeout_ms ? config->timeouts.max_timeout_ms : 8000u;
    if (min_to == 0)
        min_to = 200u;
    if (max_to == 0)
        max_to = 8000u;
    if (min_to > max_to)
    {
        uint32_t tmp = min_to;
        min_to = max_to;
        max_to = tmp;
    }
    s->timing.min_timeout_ms = min_to;
    s->timing.max_timeout_ms = max_to;
    s->timing.srtt_ms = max_to / 2u;   // conservative initial SRTT
    s->timing.rttvar_ms = max_to / 4u; // initial variance heuristic
    s->timing.samples_taken = 0;
    s->timing.in_retransmit = 0;
    // Clock presence is guaranteed by validate_config(); no runtime fallback paths.
    // No legacy resume policy defaults; simplified resume config has only mode and crc_verify_bytes.
#if VAL_LOG_LEVEL == 0
    s->cfg.debug.min_level = 0; // OFF in builds without logging
#else
    if (s->cfg.debug.min_level == 0)
    {
        // Default runtime threshold to compile-time level if caller leaves it 0
        s->cfg.debug.min_level = VAL_LOG_LEVEL;
    }
#endif
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
    *out_session = s;
    // Allocate tracking slots based on configured max performance window rung
    uint32_t slots = calculate_tracking_slots(config->adaptive_tx.max_performance_mode);
    if (slots > 0)
    {
        size_t bytes = sizeof(val_inflight_packet_t) * (size_t)slots;
        void *mem = NULL;
        if (config->adaptive_tx.allocator.alloc)
            mem = config->adaptive_tx.allocator.alloc(bytes, config->adaptive_tx.allocator.context);
        else
            mem = calloc(1, bytes);
        if (!mem)
        {
            // Free session and return OOM
#if defined(_WIN32)
            DeleteCriticalSection(&s->lock);
#else
            pthread_mutex_destroy(&s->lock);
#endif
            if (config->adaptive_tx.allocator.free && config->adaptive_tx.allocator.alloc)
                config->adaptive_tx.allocator.free(s, config->adaptive_tx.allocator.context);
            else
                free(s);
            return VAL_ERR_NO_MEMORY;
        }
        s->tracking_slots = (val_inflight_packet_t *)mem;
        s->max_tracking_slots = slots;
        // zero-initialize
        memset(s->tracking_slots, 0, bytes);
    }
    return VAL_OK;
}
// --- Adaptive timeout helpers (RFC 6298-inspired, integer math) ---
void val_internal_init_timing(val_session_t *s)
{
    if (!s)
        return;
    uint32_t min_to = s->config->timeouts.min_timeout_ms ? s->config->timeouts.min_timeout_ms : 200u;
    uint32_t max_to = s->config->timeouts.max_timeout_ms ? s->config->timeouts.max_timeout_ms : 8000u;
    if (min_to > max_to)
    {
        uint32_t t = min_to;
        min_to = max_to;
        max_to = t;
    }
    s->timing.min_timeout_ms = min_to;
    s->timing.max_timeout_ms = max_to;
    s->timing.srtt_ms = max_to / 2u;
    s->timing.rttvar_ms = max_to / 4u;
    s->timing.samples_taken = 0;
    s->timing.in_retransmit = 0;
}

static inline uint32_t val__clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void val_internal_record_rtt(val_session_t *s, uint32_t measured_rtt_ms)
{
    if (!s)
        return;
    // If retransmission occurred, skip sampling (Karn's algorithm)
    if (s->timing.in_retransmit)
        return;
    uint32_t rtt = measured_rtt_ms;
    if (rtt == 0)
        rtt = 1; // avoid zeros
    // First sample initialization per RFC6298
    if (s->timing.samples_taken == 0)
    {
        s->timing.srtt_ms = rtt;
        s->timing.rttvar_ms = rtt / 2u;
        s->timing.samples_taken = 1;
        val_metrics_inc_rtt_sample(s);
        return;
    }
    // Subsequent samples: RTTVAR = (1 - beta)*RTTVAR + beta*|SRTT - RTT|, beta=1/4
    // SRTT = (1 - alpha)*SRTT + alpha*RTT, alpha=1/8
    uint32_t srtt = s->timing.srtt_ms;
    uint32_t rttvar = s->timing.rttvar_ms;
    uint32_t diff = (srtt > rtt) ? (srtt - rtt) : (rtt - srtt);
    // rttvar = 3/4*rttvar + 1/4*diff
    rttvar = (uint32_t)((3u * (uint64_t)rttvar + diff) >> 2);
    // srtt = 7/8*srtt + 1/8*rtt
    srtt = (uint32_t)((7u * (uint64_t)srtt + rtt) >> 3);
    s->timing.rttvar_ms = rttvar;
    s->timing.srtt_ms = srtt;
    if (s->timing.samples_taken < 0xFF)
        s->timing.samples_taken++;
    val_metrics_inc_rtt_sample(s);
}

uint32_t val_internal_get_timeout(val_session_t *s, val_operation_type_t op)
{
    if (!s)
        return 1000u;
    // Compute base RTO = SRTT + 4*RTTVAR
    uint64_t base = (uint64_t)s->timing.srtt_ms + (4ull * (uint64_t)s->timing.rttvar_ms);
    // Per-operation multiplier
    uint32_t mul = 1;
    switch (op)
    {
    case VAL_OP_HANDSHAKE:
        mul = 5; // more conservative
        break;
    case VAL_OP_META:
        mul = 4;
        break;
    case VAL_OP_DATA_ACK:
        mul = 3;
        break;
    case VAL_OP_VERIFY:
        mul = 3;
        break;
    case VAL_OP_DONE_ACK:
        mul = 4;
        break;
    case VAL_OP_EOT_ACK:
        mul = 4;
        break;
    case VAL_OP_DATA_RECV:
        mul = 6; // watchdog for inbound data
        break;
    default:
        mul = 3;
        break;
    }
    uint64_t rto = base * (uint64_t)mul;
    if (rto > 0xFFFFFFFFull)
        rto = 0xFFFFFFFFull;
    uint32_t rto32 = (uint32_t)rto;
    // Clamp to [min,max]
    uint32_t min_to = s->timing.min_timeout_ms ? s->timing.min_timeout_ms : 200u;
    uint32_t max_to = s->timing.max_timeout_ms ? s->timing.max_timeout_ms : 8000u;
    if (min_to > max_to)
    {
        uint32_t t = min_to;
        min_to = max_to;
        max_to = t;
    }
    return val__clamp_u32(rto32, min_to, max_to);
}

void val_session_destroy(val_session_t *session)
{
    if (!session)
        return;
#if defined(_WIN32)
    DeleteCriticalSection(&session->lock);
#else
    pthread_mutex_destroy(&session->lock);
#endif
    // Free tracking slots
    if (session->tracking_slots)
    {
        if (session->cfg.adaptive_tx.allocator.free && session->cfg.adaptive_tx.allocator.alloc)
            session->cfg.adaptive_tx.allocator.free(session->tracking_slots, session->cfg.adaptive_tx.allocator.context);
        else
            free(session->tracking_slots);
    }
    // Free session
    if (session->cfg.adaptive_tx.allocator.free && session->cfg.adaptive_tx.allocator.alloc)
        session->cfg.adaptive_tx.allocator.free(session, session->cfg.adaptive_tx.allocator.context);
    else
        free(session);
}

// Packet helpers
static void fill_header(val_packet_header_t *h, val_packet_type_t type, uint32_t payload_len, uint64_t offset, uint32_t seq)
{
    memset(h, 0, sizeof(*h));
    h->type = (uint8_t)type;
    h->wire_version = 0; // base/core protocol reserves this byte; always 0 for now
    h->reserved2 = 0;
    h->payload_len = payload_len;
    h->seq = seq;
    h->offset = offset;
    h->header_crc = 0;
}

int val_internal_send_packet(val_session_t *s, val_packet_type_t type, const void *payload, uint32_t payload_len, uint64_t offset)
{
    // Serialize low-level send operations; recursive with public API locks
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
    // Optional preflight connection check
    if (!val_internal_transport_is_connected(s))
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_IO;
    }
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size; // MTU
    if (payload_len > (uint32_t)(P - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE))
    {
        // Payload does not fit into negotiated MTU
        val_internal_set_error_detailed(s, VAL_ERR_INVALID_ARG, VAL_ERROR_DETAIL_PAYLOAD_SIZE);
        VAL_LOG_ERROR(s, "send_packet: payload too large for MTU");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_INVALID_ARG;
    }
    uint8_t *buf = (uint8_t *)s->config->buffers.send_buffer;
    uint32_t seq = s->seq_counter++;
    val_packet_header_t header;
    fill_header(&header, type, payload_len, offset, seq);
    val_serialize_header(&header, buf);
    uint32_t header_crc = val_internal_crc32(s, buf, VAL_WIRE_HEADER_SIZE);
    header.header_crc = header_crc;
    val_serialize_header(&header, buf);
    uint8_t *payload_dst = buf + VAL_WIRE_HEADER_SIZE;
    if (payload_len && payload)
    {
        memcpy(payload_dst, payload, payload_len);
    }
    // Trailer CRC over Header+Data
    size_t used = VAL_WIRE_HEADER_SIZE + payload_len;
    uint32_t pkt_crc = val_internal_crc32(s, buf, used);
    VAL_PUT_LE32(buf + used, pkt_crc);
    size_t total_len = used + VAL_WIRE_TRAILER_SIZE;
    int rc = s->config->transport.send(s->config->transport.io_context, buf, total_len);
#if defined(_WIN32)
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
    if (rc != (int)total_len)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_SEND_FAILED);
        VAL_LOG_ERROR(s, "send_packet: transport send failed");
        return VAL_ERR_IO;
    }
    // Metrics: count one packet and bytes on successful low-level send
    val_metrics_add_sent(s, total_len, (uint8_t)type);
#if VAL_ENABLE_WIRE_AUDIT
    if ((unsigned)type < 16u)
    {
        // Bump outside of the lock to keep overhead minimal; tearing is acceptable for test-only stats
        s->audit.sent[(unsigned)type]++;
    }
#endif
    // Best-effort flush after control packets where timely delivery matters
    if (type == VAL_PKT_DONE || type == VAL_PKT_EOT || type == VAL_PKT_HELLO || type == VAL_PKT_ERROR || type == VAL_PKT_CANCEL)
    {
        val_internal_transport_flush(s);
    }
    return VAL_OK;
}

int val_internal_recv_packet(val_session_t *s, val_packet_type_t *type, void *payload_out, uint32_t payload_cap,
                             uint32_t *payload_len_out, uint64_t *offset_out, uint32_t timeout_ms)
{
    // Serialize low-level recv operations; recursive with public API locks
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size; // MTU
    uint8_t *buf = (uint8_t *)s->config->buffers.recv_buffer;
    size_t got = 0;
    // Read header first
    int rc = s->config->transport.recv(s->config->transport.io_context, buf, VAL_WIRE_HEADER_SIZE, &got, timeout_ms);
    if (rc < 0)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
        VAL_LOG_ERROR(s, "recv_packet: transport error on header");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_IO;
    }
    if (got != VAL_WIRE_HEADER_SIZE)
    {
        // Benign timeout while waiting for a header; record without emitting a CRITICAL numeric log
        val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
        VAL_LOG_DEBUG(s, "recv_packet: header timeout");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        val_metrics_inc_timeout(s);
        return VAL_ERR_TIMEOUT;
    }
    uint8_t header_scratch[VAL_WIRE_HEADER_SIZE];
    for (;;)
    {
        memcpy(header_scratch, buf, VAL_WIRE_HEADER_SIZE);
        uint32_t header_crc_expected = VAL_GET_LE32(buf + 20);
        VAL_PUT_LE32(header_scratch + 20, 0u);
        uint32_t header_crc_calc = val_internal_crc32(s, header_scratch, VAL_WIRE_HEADER_SIZE);
        if (header_crc_calc == header_crc_expected)
        {
            break;
        }

        // Header corruption detected. Increment metrics and attempt to resynchronize to the next valid header
        // boundary by sliding a 1-byte window until a plausible header CRC matches and basic sanity checks pass.
        VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_CRC_HEADER);
        VAL_LOG_ERROR(s, "recv_packet: header CRC mismatch (attempting resync)");
#if VAL_ENABLE_METRICS
        s->metrics.crc_errors++;
#endif
        uint8_t window[VAL_WIRE_HEADER_SIZE];
        memcpy(window, buf, VAL_WIRE_HEADER_SIZE);
        size_t bytes_scanned = 0;
        const size_t scan_limit = P; // don't scan more than one MTU worth of extra bytes
        for (;;)
        {
            memmove(window, window + 1, VAL_WIRE_HEADER_SIZE - 1);
            size_t gotb = 0;
            int rc2 = s->config->transport.recv(s->config->transport.io_context, window + VAL_WIRE_HEADER_SIZE - 1, 1,
                                                 &gotb, timeout_ms);
            if (rc2 < 0)
            {
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
                VAL_LOG_ERROR(s, "recv_packet: transport error during resync");
#if defined(_WIN32)
                LeaveCriticalSection(&s->lock);
#else
                pthread_mutex_unlock(&s->lock);
#endif
                return VAL_ERR_IO;
            }
            if (gotb != 1)
            {
                // Resync timeout — record without CRITICAL numeric log
                val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
                VAL_LOG_DEBUG(s, "recv_packet: timeout while resyncing after bad header");
#if defined(_WIN32)
                LeaveCriticalSection(&s->lock);
#else
                pthread_mutex_unlock(&s->lock);
#endif
                val_metrics_inc_timeout(s);
                return VAL_ERR_TIMEOUT;
            }
            bytes_scanned++;

            uint8_t tmp[VAL_WIRE_HEADER_SIZE];
            memcpy(tmp, window, VAL_WIRE_HEADER_SIZE);
            uint32_t exp = VAL_GET_LE32(window + 20);
            VAL_PUT_LE32(tmp + 20, 0u);
            uint32_t calc = val_internal_crc32(s, tmp, VAL_WIRE_HEADER_SIZE);
            if (calc == exp)
            {
                val_packet_header_t candidate;
                val_deserialize_header(window, &candidate);
                if (candidate.wire_version == 0 &&
                    candidate.payload_len <= (uint32_t)(P - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE))
                {
                    memcpy(buf, window, VAL_WIRE_HEADER_SIZE);
                    break;
                }
            }
            if (bytes_scanned > scan_limit)
            {
                VAL_LOG_ERROR(s, "recv_packet: resync failed after scanning limit");
#if defined(_WIN32)
                LeaveCriticalSection(&s->lock);
#else
                pthread_mutex_unlock(&s->lock);
#endif
                return VAL_ERR_CRC;
            }
        }
        // Loop re-validates the new header in buf
    }
    val_packet_header_t header;
    val_deserialize_header(buf, &header);
    if (header.wire_version != 0)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION_MAJOR);
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }

    uint32_t payload_len = header.payload_len;
    if (payload_len > (uint32_t)(P - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE))
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_PAYLOAD_SIZE);
        VAL_LOG_ERROR(s, "recv_packet: payload_len exceeds MTU");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_PROTOCOL;
    }

    if (payload_len > 0)
    {
        size_t got2 = 0;
        rc = s->config->transport.recv(s->config->transport.io_context, buf + VAL_WIRE_HEADER_SIZE, payload_len, &got2,
                                       timeout_ms);
        if (rc < 0)
        {
            VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
            VAL_LOG_ERROR(s, "recv_packet: transport error on payload");
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return VAL_ERR_IO;
        }
        if (got2 != payload_len)
        {
            val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
            VAL_LOG_DEBUG(s, "recv_packet: payload timeout");
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            val_metrics_inc_timeout(s);
            return VAL_ERR_TIMEOUT;
        }
    }

    uint8_t trailer_bytes[VAL_WIRE_TRAILER_SIZE];
    size_t got3 = 0;
    rc = s->config->transport.recv(s->config->transport.io_context, trailer_bytes, VAL_WIRE_TRAILER_SIZE, &got3,
                                   timeout_ms);
    if (rc < 0)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
        VAL_LOG_ERROR(s, "recv_packet: transport error on trailer");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_IO;
    }
    if (got3 != VAL_WIRE_TRAILER_SIZE)
    {
        val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
        VAL_LOG_DEBUG(s, "recv_packet: trailer timeout");
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        val_metrics_inc_timeout(s);
        return VAL_ERR_TIMEOUT;
    }

    uint32_t trailer_crc = VAL_GET_LE32(trailer_bytes);
    uint32_t calc_crc = val_internal_crc32(s, buf, VAL_WIRE_HEADER_SIZE + payload_len);
    if (trailer_crc != calc_crc)
    {
        VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_CRC_TRAILER);
        VAL_LOG_ERROR(s, "recv_packet: trailer CRC mismatch");
#if VAL_ENABLE_METRICS
        s->metrics.crc_errors++;
#endif
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return VAL_ERR_CRC;
    }

    uint8_t type_byte = header.type;
    if (type)
        *type = (val_packet_type_t)type_byte;
    if (offset_out)
        *offset_out = header.offset;
    if (payload_len_out)
        *payload_len_out = payload_len;

    if (header.type == (uint8_t)VAL_PKT_CANCEL)
    {
        val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
        VAL_LOG_WARN(s, "recv_packet: observed CANCEL on wire");
    }

    if (payload_out && payload_cap)
    {
        if (payload_len > payload_cap)
        {
            val_internal_set_error_detailed(s, VAL_ERR_INVALID_ARG, VAL_ERROR_DETAIL_PAYLOAD_SIZE);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return VAL_ERR_INVALID_ARG;
        }
        memcpy(payload_out, buf + VAL_WIRE_HEADER_SIZE, payload_len);
    }

#if defined(_WIN32)
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif

    val_metrics_add_recv(s, (size_t)(VAL_WIRE_HEADER_SIZE + payload_len + VAL_WIRE_TRAILER_SIZE), type_byte);
#if VAL_ENABLE_WIRE_AUDIT
    if ((unsigned)type_byte < 16u)
    {
        s->audit.recv[(unsigned)type_byte]++;
    }
#endif
    return VAL_OK;
}

// The actual sending/receiving logic is implemented in separate compilation units
// progress_ctx is opaque to core; defined/used in sender implementation for batch progress
extern val_status_t val_internal_send_file(val_session_t *session, const char *filepath, const char *sender_path,
                                           void *progress_ctx);
// Public: Emergency cancel (best-effort)
val_status_t val_emergency_cancel(val_session_t *session)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    // Send CANCEL a few times with tiny backoff
    val_status_t last = VAL_ERR_IO;
    uint32_t backoff = session->cfg.retries.backoff_ms_base ? session->cfg.retries.backoff_ms_base : 5u;
    uint8_t tries = 3;
    while (tries--)
    {
        val_status_t st = val_internal_send_packet(session, VAL_PKT_CANCEL, NULL, 0, 0);
        fprintf(stdout, "[VAL] emergency_cancel: send CANCEL attempt=%u st=%d\n", (unsigned)(3 - tries), (int)st);
        fflush(stdout);
        if (st == VAL_OK)
            last = VAL_OK;
        if (session->cfg.system.delay_ms)
            session->cfg.system.delay_ms(backoff);
        if (backoff < 50u)
            backoff <<= 1;
    }
    val_internal_transport_flush(session);
    // Mark session aborted regardless of wire outcome so local loops can exit early
    val_internal_set_last_error(session, VAL_ERR_ABORTED, 0);
    fprintf(stdout, "[VAL] emergency_cancel: marked session aborted (last_error set)\n");
    fflush(stdout);
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return last;
}

int val_check_for_cancel(val_session_t *session)
{
    if (!session)
        return 0;
    return (session->last_error_code == VAL_ERR_ABORTED) ? 1 : 0;
}

extern val_status_t val_internal_receive_files(val_session_t *session, const char *output_directory);

// Single-file public sends are intentionally removed; use val_send_files with count=1.

val_status_t val_receive_files(val_session_t *session, const char *output_directory)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    if (output_directory)
    {
        size_t n = val_internal_strnlen(output_directory, VAL_MAX_PATH);
        if (n > VAL_MAX_PATH)
            n = VAL_MAX_PATH;
        memcpy(session->output_directory, output_directory, n);
        session->output_directory[n] = '\0';
    }
    else
    {
        session->output_directory[0] = '\0';
    }
    // Ensure handshake once per session on first receive call
    val_status_t hs = val_internal_do_handshake_receiver(session);
    if (hs != VAL_OK)
    {
#if defined(_WIN32)
        LeaveCriticalSection(&session->lock);
#else
        pthread_mutex_unlock(&session->lock);
#endif
        return hs;
    }
    val_status_t rs = val_internal_receive_files(session, session->output_directory);
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return rs;
}

// EOT is internal and sent automatically by val_send_files when all files are sent.

// def_or(v, d) was an earlier helper; no longer used

// Ensure local config.required/requested only contains negotiable (optional) features compiled-in; sanitize requested
static inline uint32_t val__negotiable_mask(void)
{
    // Negotiable bits are exactly the compiled-in optional feature mask
    return VAL_BUILTIN_FEATURES;
}

static val_status_t val_internal_validate_local_features(const val_config_t *cfg, uint32_t *out_requested_sanitized)
{
    if (!cfg)
        return VAL_ERR_INVALID_ARG;
    // Ignore any core/reserved bits the app may have set by mistake; we only negotiate optional bits
    uint32_t negotiable = val__negotiable_mask();
    uint32_t required_sanitized = cfg->features.required & negotiable;
    uint32_t missing_required = required_sanitized & ~negotiable; // always zero but keep shape for clarity
    if (missing_required)
    {
        // Local build does not support some required features; abort before handshake
        return VAL_ERR_FEATURE_NEGOTIATION;
    }
    if (out_requested_sanitized)
    {
        *out_requested_sanitized = cfg->features.requested & negotiable;
    }
    return VAL_OK;
}

// Handshake implementations
val_status_t val_internal_do_handshake_sender(val_session_t *s)
{
    if (s->handshake_done)
        return VAL_OK;
    uint32_t requested_sanitized = 0;
    // Pre-validate local features: fail fast if required contains unsupported bits
    val_status_t vr = val_internal_validate_local_features(s->config, &requested_sanitized);
    if (vr != VAL_OK)
    {
        // Encode which required features are missing locally
        uint32_t missing_required = s->config->features.required & ~VAL_BUILTIN_FEATURES;
        if (missing_required)
        {
            VAL_SET_FEATURE_ERROR(s, missing_required);
        }
        return vr;
    }
    val_handshake_t hello;
    // Prepare handshake message
    memset(&hello, 0, sizeof(hello));
    hello.magic = VAL_MAGIC;
    hello.version_major = (uint8_t)VAL_VERSION_MAJOR;
    hello.version_minor = (uint8_t)VAL_VERSION_MINOR;
    // Propose our configured size; effective size will be min on negotiation
    hello.packet_size = (uint32_t)s->config->buffers.packet_size;
    // Advertise only negotiable optional features; core features are implicit
    uint32_t negotiable = val__negotiable_mask();
    hello.features = negotiable;
    // Mask required/requested to negotiable bits only
    hello.required = s->config->features.required & negotiable;
    hello.requested = requested_sanitized; // already masked to negotiable
    // Adaptive fields from config (window rungs + streaming flags)
    hello.max_performance_mode = (uint8_t)s->cfg.adaptive_tx.max_performance_mode;
    hello.preferred_initial_mode = (uint8_t)s->cfg.adaptive_tx.preferred_initial_mode;
    hello.mode_sync_interval = s->cfg.adaptive_tx.mode_sync_interval;
    // Single policy: if we allow streaming, advertise both TX-capable and RX-accept bits
    hello.streaming_flags = (uint8_t)(s->cfg.adaptive_tx.allow_streaming ? 0x3u : 0x0u);
    hello.reserved_streaming[0] = 0;
    hello.reserved_streaming[1] = 0;
    hello.reserved_streaming[2] = 0;
    hello.supported_features16 = 0;
    hello.required_features16 = 0;
    hello.requested_features16 = 0;
    hello.reserved2 = 0;
    uint8_t hello_wire[VAL_WIRE_HANDSHAKE_SIZE];
    val_serialize_handshake(&hello, hello_wire);
    VAL_LOG_TRACE(s, "handshake(sender): sending HELLO");
    val_status_t st = val_internal_send_packet(s, VAL_PKT_HELLO, hello_wire, VAL_WIRE_HANDSHAKE_SIZE, 0);
    if (st != VAL_OK)
        return st;
    uint32_t len = 0;
    uint64_t off = 0;
    val_packet_type_t t = 0;
    uint8_t peer_wire[VAL_WIRE_HANDSHAKE_SIZE];
    val_handshake_t peer_h;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
    uint8_t tries = s->config->retries.handshake_retries ? s->config->retries.handshake_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    for (;;)
    {
        VAL_HEALTH_RECORD_OPERATION(s);
        val_status_t health = val_internal_check_health(s);
        if (health != VAL_OK)
            return health;
            
        VAL_LOG_TRACEF(s, "handshake(sender): waiting for HELLO (to=%u ms, tries=%u)", (unsigned)to, (unsigned)tries);
        st = val_internal_recv_packet(s, &t, peer_wire, VAL_WIRE_HANDSHAKE_SIZE, &len, &off, to);
        if (st == VAL_OK)
            break;
        if (st != VAL_ERR_TIMEOUT || tries == 0)
        {
            if (st == VAL_ERR_TIMEOUT && tries == 0)
            {
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_HELLO);
            }
            VAL_LOG_DEBUGF(s, "handshake(sender): wait failed st=%d tries=%u", (int)st, (unsigned)tries);
            return st;
        }
        // Retransmit HELLO and backoff
    val_status_t rs = val_internal_send_packet(s, VAL_PKT_HELLO, hello_wire, VAL_WIRE_HANDSHAKE_SIZE, 0);
        if (rs != VAL_OK)
            return rs;
        VAL_HEALTH_RECORD_RETRY(s);
        VAL_LOG_DEBUG(s, "handshake(sender): timeout -> retransmit HELLO");
        if (backoff && s->config->system.delay_ms)
            s->config->system.delay_ms(backoff);
        if (backoff)
            backoff <<= 1;
        --tries;
    }
    VAL_LOG_TRACEF(s, "handshake(sender): got pkt t=%u len=%u", (unsigned)t, (unsigned)len);
    if (t != VAL_PKT_HELLO || len < VAL_WIRE_HANDSHAKE_SIZE)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    val_deserialize_handshake(peer_wire, &peer_h);
    if (peer_h.magic != VAL_MAGIC)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    if (peer_h.version_major != VAL_VERSION_MAJOR)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION_MAJOR);
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }
    // Adopt the smaller packet size so both can operate; both peers independently take min
    size_t negotiated =
        (peer_h.packet_size < s->config->buffers.packet_size) ? peer_h.packet_size : s->config->buffers.packet_size;
    if (negotiated < VAL_MIN_PACKET_SIZE || negotiated > VAL_MAX_PACKET_SIZE)
    {
        val_internal_set_error_detailed(s, VAL_ERR_PACKET_SIZE_MISMATCH, VAL_ERROR_DETAIL_PACKET_SIZE);
        return VAL_ERR_PACKET_SIZE_MISMATCH;
    }
    s->effective_packet_size = negotiated;
    // Enforce optional required features: our negotiable 'required' must be present on peer
    s->peer_features = peer_h.features;
    uint32_t missing_on_peer = hello.required & ~peer_h.features;
    if (missing_on_peer)
    {
        VAL_SET_FEATURE_ERROR(s, missing_on_peer);
        (void)val_internal_send_error(s, VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE(missing_on_peer));
        return VAL_ERR_FEATURE_NEGOTIATION;
    }
    // Optionally adjust behavior based on requested ∧ peer.features later
    s->handshake_done = 1;
#if VAL_ENABLE_METRICS
    s->metrics.handshakes++;
#endif
    // Adaptive TX negotiation (window rung + streaming flags)
    val_tx_mode_t local_max = val_tx_mode_sanitize(s->cfg.adaptive_tx.max_performance_mode);
    val_tx_mode_t peer_max = val_tx_mode_sanitize((val_tx_mode_t)peer_h.max_performance_mode);
    uint32_t local_max_window = val_tx_mode_window(local_max);
    uint32_t peer_max_window = val_tx_mode_window(peer_max);
    uint32_t shared_window = (local_max_window < peer_max_window) ? local_max_window : peer_max_window;
    val_tx_mode_t negotiated_cap = val_tx_mode_from_window(shared_window);
    s->min_negotiated_mode = negotiated_cap;       // best performance both support (largest window rung allowed)
    s->max_negotiated_mode = VAL_TX_STOP_AND_WAIT; // smallest rung always supported
    // Streaming allowed from us to peer if we allow streaming and peer accepts incoming streaming
    uint8_t peer_rx_accept = (peer_h.streaming_flags & 2u) ? 1u : 0u;
    uint8_t local_allow = (s->cfg.adaptive_tx.allow_streaming ? 1u : 0u);
    s->send_streaming_allowed = (uint8_t)(local_allow && peer_rx_accept);
    // Streaming we accept when peer sends to us (single policy governs acceptance)
    s->recv_streaming_allowed = (uint8_t)(s->cfg.adaptive_tx.allow_streaming ? 1u : 0u);
    // Initial window rung selection (conservative): pick the slower (numerically larger) of our preferred
    // and the peer's preferred, clamped to the negotiated cap. This avoids jumping to a rung faster than
    // the peer would like immediately after handshake.
    val_tx_mode_t local_pref = val_tx_mode_sanitize(s->cfg.adaptive_tx.preferred_initial_mode);
    val_tx_mode_t peer_pref = val_tx_mode_sanitize((val_tx_mode_t)peer_h.preferred_initial_mode);
    if (val_tx_mode_window(local_pref) > shared_window)
        local_pref = negotiated_cap;
    if (val_tx_mode_window(peer_pref) > shared_window)
        peer_pref = negotiated_cap;
    uint32_t init_window = (val_tx_mode_window(local_pref) < val_tx_mode_window(peer_pref))
                               ? val_tx_mode_window(local_pref)
                               : val_tx_mode_window(peer_pref);
    val_tx_mode_t init_mode = val_tx_mode_from_window(init_window);
    s->current_tx_mode = init_mode;
    s->peer_tx_mode = s->current_tx_mode;
    // features compatibility: ensure required features supported. For now, just note the peer features; could gate behavior
    // later.
    return VAL_OK;
}

val_status_t val_internal_do_handshake_receiver(val_session_t *s)
{
    if (s->handshake_done)
    {
        // If already done, do nothing
        return VAL_OK;
    }
    // Receive sender hello
    uint32_t len = 0;
    uint64_t off = 0;
    val_packet_type_t t = 0;
    uint8_t peer_wire[VAL_WIRE_HANDSHAKE_SIZE];
    val_handshake_t peer_h;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
    uint8_t tries = s->config->retries.handshake_retries ? s->config->retries.handshake_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    val_status_t st = VAL_OK;
    for (;;)
    {
        VAL_HEALTH_RECORD_OPERATION(s);
        val_status_t health = val_internal_check_health(s);
        if (health != VAL_OK)
            return health;
            
        st = val_internal_recv_packet(s, &t, peer_wire, VAL_WIRE_HANDSHAKE_SIZE, &len, &off, to);
        if (st == VAL_OK)
            break;
        if (st != VAL_ERR_TIMEOUT || tries == 0)
        {
            if (st == VAL_ERR_TIMEOUT && tries == 0)
            {
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_HELLO);
            }
            return st;
        }
        // Just backoff and continue waiting; receiver doesn't send until hello is received
        VAL_HEALTH_RECORD_RETRY(s);
        if (backoff && s->config->system.delay_ms)
            s->config->system.delay_ms(backoff);
        if (backoff)
            backoff <<= 1;
        --tries;
    }
    if (t != VAL_PKT_HELLO || len < VAL_WIRE_HANDSHAKE_SIZE)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    val_deserialize_handshake(peer_wire, &peer_h);
    if (peer_h.magic != VAL_MAGIC)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    if (peer_h.version_major != VAL_VERSION_MAJOR)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION_MAJOR);
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }
    size_t negotiated =
        (peer_h.packet_size < s->config->buffers.packet_size) ? peer_h.packet_size : s->config->buffers.packet_size;
    if (negotiated < VAL_MIN_PACKET_SIZE || negotiated > VAL_MAX_PACKET_SIZE)
    {
        val_internal_set_error_detailed(s, VAL_ERR_PACKET_SIZE_MISMATCH, VAL_ERROR_DETAIL_PACKET_SIZE);
        return VAL_ERR_PACKET_SIZE_MISMATCH;
    }
    s->effective_packet_size = negotiated;

    // Send our hello back
    val_handshake_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = VAL_MAGIC;
    hello.version_major = (uint8_t)VAL_VERSION_MAJOR;
    hello.version_minor = (uint8_t)VAL_VERSION_MINOR;
    hello.packet_size = (uint32_t)s->effective_packet_size;
    // Sanitize to negotiable optional features; core features are implicit and ignored
    uint32_t negotiable = val__negotiable_mask();
    uint32_t requested_sanitized = s->config->features.requested & negotiable;
    uint32_t missing_required = (s->config->features.required & negotiable) & ~negotiable; // always zero
    if (missing_required)
    {
        VAL_SET_FEATURE_ERROR(s, missing_required);
        (void)val_internal_send_error(s, VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE(missing_required));
        return VAL_ERR_FEATURE_NEGOTIATION;
    }
    hello.features = negotiable;
    hello.required = s->config->features.required & negotiable;
    hello.requested = requested_sanitized;
    // Adaptive fields from config (window rungs + streaming flags)
    hello.max_performance_mode = (uint8_t)s->cfg.adaptive_tx.max_performance_mode;
    hello.preferred_initial_mode = (uint8_t)s->cfg.adaptive_tx.preferred_initial_mode;
    hello.mode_sync_interval = s->cfg.adaptive_tx.mode_sync_interval;
    // Single policy: if we allow streaming, advertise both TX-capable and RX-accept bits
    hello.streaming_flags = (uint8_t)(s->cfg.adaptive_tx.allow_streaming ? 0x3u : 0x0u);
    hello.reserved_streaming[0] = 0;
    hello.reserved_streaming[1] = 0;
    hello.reserved_streaming[2] = 0;
    hello.supported_features16 = 0;
    hello.required_features16 = 0;
    hello.requested_features16 = 0;
    hello.reserved2 = 0;
    s->peer_features = peer_h.features;
    // Enforce peer's required optional features: must be subset of our advertised optional features
    uint32_t missing_local = (peer_h.required & negotiable) & ~hello.features;
    if (missing_local)
    {
        VAL_SET_FEATURE_ERROR(s, missing_local);
        (void)val_internal_send_error(s, VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE(missing_local));
        return VAL_ERR_FEATURE_NEGOTIATION;
    }
    uint8_t hello_wire[VAL_WIRE_HANDSHAKE_SIZE];
    val_serialize_handshake(&hello, hello_wire);
    VAL_LOG_TRACE(s, "handshake(receiver): sending HELLO response");
    st = val_internal_send_packet(s, VAL_PKT_HELLO, hello_wire, VAL_WIRE_HANDSHAKE_SIZE, 0);
    if (st == VAL_OK)
    {
        s->handshake_done = 1;
#if VAL_ENABLE_METRICS
        s->metrics.handshakes++;
#endif
        // Adaptive TX negotiation (window rung + streaming flags)
        val_tx_mode_t local_max = val_tx_mode_sanitize(s->cfg.adaptive_tx.max_performance_mode);
        val_tx_mode_t peer_max = val_tx_mode_sanitize((val_tx_mode_t)peer_h.max_performance_mode);
        uint32_t local_max_window = val_tx_mode_window(local_max);
        uint32_t peer_max_window = val_tx_mode_window(peer_max);
        uint32_t shared_window = (local_max_window < peer_max_window) ? local_max_window : peer_max_window;
        val_tx_mode_t negotiated_cap = val_tx_mode_from_window(shared_window);
        s->min_negotiated_mode = negotiated_cap;
        s->max_negotiated_mode = VAL_TX_STOP_AND_WAIT;
    // Streaming allowed from us to peer if we allow streaming and peer accepts
    uint8_t peer_rx_accept = (peer_h.streaming_flags & 2u) ? 1u : 0u;
    uint8_t local_allow = (s->cfg.adaptive_tx.allow_streaming ? 1u : 0u);
    s->send_streaming_allowed = (uint8_t)(local_allow && peer_rx_accept);
    // What we accept for inbound is governed by our single policy
    s->recv_streaming_allowed = (uint8_t)(s->cfg.adaptive_tx.allow_streaming ? 1u : 0u);
        // Initial window rung selection (conservative): pick the slower (numerically larger) of our preferred
        // and the peer's preferred, clamped to the negotiated cap.
        val_tx_mode_t local_pref = val_tx_mode_sanitize(s->cfg.adaptive_tx.preferred_initial_mode);
        val_tx_mode_t peer_pref = val_tx_mode_sanitize((val_tx_mode_t)peer_h.preferred_initial_mode);
        if (val_tx_mode_window(local_pref) > shared_window)
            local_pref = negotiated_cap;
        if (val_tx_mode_window(peer_pref) > shared_window)
            peer_pref = negotiated_cap;
        uint32_t init_window = (val_tx_mode_window(local_pref) < val_tx_mode_window(peer_pref))
                                   ? val_tx_mode_window(local_pref)
                                   : val_tx_mode_window(peer_pref);
        val_tx_mode_t init_mode = val_tx_mode_from_window(init_window);
        s->current_tx_mode = init_mode;
        s->peer_tx_mode = s->current_tx_mode;
    }
    return st;
}

val_status_t val_internal_send_error(val_session_t *s, val_status_t code, uint32_t detail)
{
    val_error_payload_t payload;
    payload.code = (int32_t)code;
    payload.detail = detail;
    uint8_t wire[VAL_WIRE_ERROR_PAYLOAD_SIZE];
    val_serialize_error_payload(&payload, wire);
    return val_internal_send_packet(s, VAL_PKT_ERROR, wire, VAL_WIRE_ERROR_PAYLOAD_SIZE, 0);
}

// Adaptive transmission mode management
static val_tx_mode_t degrade_mode(val_tx_mode_t current)
{
    uint32_t window = val_tx_mode_window(current);
    if (window <= 1u)
        return VAL_TX_STOP_AND_WAIT;
    if (window <= 2u)
        return VAL_TX_STOP_AND_WAIT;
    window /= 2u;
    if (window < 2u)
        window = 2u;
    return val_tx_mode_from_window(window);
}

static val_tx_mode_t upgrade_mode(val_tx_mode_t current, val_tx_mode_t max_allowed)
{
    uint32_t current_window = val_tx_mode_window(current);
    uint32_t max_window = val_tx_mode_window(max_allowed);
    if (current_window >= max_window)
        return val_tx_mode_from_window(max_window);
    if (current_window == 0u)
        current_window = 1u;
    uint32_t next_window = current_window * 2u;
    if (next_window > max_window)
        next_window = max_window;
    return val_tx_mode_from_window(next_window);
}

void val_internal_record_transmission_error(val_session_t *s)
{
    if (!s)
        return;

    s->consecutive_errors++;
    s->consecutive_successes = 0; // reset success counter
    s->streaming_engaged = 0; // drop back to non-streaming pacing on any error

    // Check if we should degrade mode
    uint16_t threshold = s->cfg.adaptive_tx.degrade_error_threshold;
    if (threshold == 0)
        threshold = 3; // default threshold

    if (s->consecutive_errors >= threshold && s->current_tx_mode != VAL_TX_STOP_AND_WAIT)
    {
        val_tx_mode_t new_mode = degrade_mode(s->current_tx_mode);
        VAL_LOG_INFOF(s, "adaptive: degrading mode from %u to %u after %u errors", (unsigned)s->current_tx_mode,
                      (unsigned)new_mode, s->consecutive_errors);
    s->current_tx_mode = new_mode;
    s->peer_tx_mode = s->current_tx_mode; // local view: our peers may want to know our new mode
    // Best-effort mode sync (Phase 1: fire-and-forget)
    val_mode_sync_t ms = {0};
    ms.current_mode = (uint32_t)new_mode;
    ms.sequence = ++s->mode_sync_sequence;
    ms.consecutive_errors = s->consecutive_errors;
    ms.consecutive_success = s->consecutive_successes;
    ms.flags = s->streaming_engaged ? 1u : 0u;
    uint8_t ms_wire[VAL_WIRE_MODE_SYNC_SIZE];
    val_serialize_mode_sync(&ms, ms_wire);
    (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC, ms_wire, VAL_WIRE_MODE_SYNC_SIZE, 0);
        s->consecutive_errors = 0; // reset after mode change
        s->packets_since_mode_change = 0;

        // Reallocate tracking slots if needed
        uint32_t new_slots = calculate_tracking_slots(new_mode);
        if (new_slots != s->max_tracking_slots)
        {
            // For simplicity, just reset tracking when changing modes
            if (s->tracking_slots)
                memset(s->tracking_slots, 0, sizeof(val_inflight_packet_t) * s->max_tracking_slots);
        }
    }
}

void val_internal_record_transmission_success(val_session_t *s)
{
    if (!s)
        return;

    s->consecutive_successes++;
    s->consecutive_errors = 0; // reset error counter

    // Check if we should upgrade mode
    uint16_t threshold = s->cfg.adaptive_tx.recovery_success_threshold;
    if (threshold == 0)
        threshold = 10; // default threshold

    if (s->consecutive_successes >= threshold && s->current_tx_mode != s->min_negotiated_mode)
    {
        val_tx_mode_t new_mode = upgrade_mode(s->current_tx_mode, s->min_negotiated_mode);
        if (new_mode != s->current_tx_mode)
        {
            VAL_LOG_INFOF(s, "adaptive: upgrading mode from %u to %u after %u successes", (unsigned)s->current_tx_mode,
                          (unsigned)new_mode, s->consecutive_successes);
            s->current_tx_mode = new_mode;
            s->peer_tx_mode = s->current_tx_mode; // local view: our peers may want to know our new mode
            // Best-effort mode sync (Phase 1: fire-and-forget)
            val_mode_sync_t ms = {0};
            ms.current_mode = (uint32_t)new_mode;
            ms.sequence = ++s->mode_sync_sequence;
            ms.consecutive_errors = s->consecutive_errors;
            ms.consecutive_success = s->consecutive_successes;
            ms.flags = s->streaming_engaged ? 1u : 0u;
            uint8_t ms_wire[VAL_WIRE_MODE_SYNC_SIZE];
            val_serialize_mode_sync(&ms, ms_wire);
            (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC, ms_wire, VAL_WIRE_MODE_SYNC_SIZE, 0);
            s->consecutive_successes = 0; // reset after mode change
            s->packets_since_mode_change = 0;

            // Reallocate tracking slots if needed
            uint32_t new_slots = calculate_tracking_slots(new_mode);
            if (new_slots != s->max_tracking_slots)
            {
                // For simplicity, just reset tracking when changing modes
                if (s->tracking_slots)
                    memset(s->tracking_slots, 0, sizeof(val_inflight_packet_t) * s->max_tracking_slots);
            }
        }
    }
    // Escalate to streaming pacing after reaching fastest rung and sustained successes
    if (s->send_streaming_allowed && s->cfg.adaptive_tx.allow_streaming)
    {
        // Engage streaming only when at fastest negotiated rung and one more threshold worth of clean successes
        if (!s->streaming_engaged && s->current_tx_mode == s->min_negotiated_mode)
        {
            uint16_t stream_threshold = s->cfg.adaptive_tx.recovery_success_threshold ? s->cfg.adaptive_tx.recovery_success_threshold : 10;
            if (s->consecutive_successes >= stream_threshold)
            {
                s->streaming_engaged = 1;
                // Notify peer via mode sync with streaming flag set
                val_mode_sync_t ms2 = {0};
                ms2.current_mode = (uint32_t)s->current_tx_mode;
                ms2.sequence = ++s->mode_sync_sequence;
                ms2.consecutive_errors = s->consecutive_errors;
                ms2.consecutive_success = s->consecutive_successes;
                ms2.flags = 1u; // streaming engaged
                uint8_t ms_wire2[VAL_WIRE_MODE_SYNC_SIZE];
                val_serialize_mode_sync(&ms2, ms_wire2);
                (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC, ms_wire2, VAL_WIRE_MODE_SYNC_SIZE, 0);
                VAL_LOG_INFO(s, "adaptive: engaging streaming pacing at max window rung");
                s->consecutive_successes = 0; // reset after escalation
            }
        }
    }
}

#if VAL_ENABLE_METRICS
#include <string.h>
val_status_t val_get_metrics(val_session_t *session, val_metrics_t *out)
{
    if (!session || !out)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    *out = session->metrics;
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}

val_status_t val_reset_metrics(val_session_t *session)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
#if defined(_WIN32)
    EnterCriticalSection(&session->lock);
#else
    pthread_mutex_lock(&session->lock);
#endif
    memset(&session->metrics, 0, sizeof(session->metrics));
#if defined(_WIN32)
    LeaveCriticalSection(&session->lock);
#else
    pthread_mutex_unlock(&session->lock);
#endif
    return VAL_OK;
}
#endif // VAL_ENABLE_METRICS
