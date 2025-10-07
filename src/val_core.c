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
    if (!s || !s->config || !s->config->debug.log)
        return;
#if VAL_LOG_LEVEL == 0
    (void)level; (void)file; (void)line; (void)msg;
    return;
#else
    // Forward directly; compile-time VAL_LOG_LEVEL and call-site macros gate noise
    int minlvl = s->config->debug.min_level;
    if (minlvl == 0)
        return;
    if (level <= minlvl)
        s->config->debug.log(s->config->debug.context, level, file, line, msg);
#endif
}

void val_internal_logf(val_session_t *s, int level, const char *file, int line, const char *fmt, ...)
{
    char stackbuf[256];
    va_list ap, ap2;
    int needed;
    if (!s || !s->config || !s->config->debug.log || !fmt)
        return;
    // Avoid formatting work when OFF or filtered out
#if VAL_LOG_LEVEL == 0
    return;
#else
    int minlvl = s->config->debug.min_level;
    if (minlvl == 0 || level > minlvl)
        return;
#endif
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

// --- Small internal utilities ---
size_t val_internal_strnlen(const char *s, size_t maxlen)
{
    if (!s)
        return 0;
    const char *p = s;
    size_t n = 0;
    while (n < maxlen && *p)
    {
        ++p;
        ++n;
    }
    return n;
}

void val_internal_set_last_error_full(val_session_t *s, val_status_t code, uint32_t detail, const char *op)
{
    if (!s)
        return;
    s->last_error.code = code;
    s->last_error.detail = detail;
    s->last_error.op = op;
}

void val_internal_set_last_error(val_session_t *s, val_status_t code, uint32_t detail)
{
    val_internal_set_last_error_full(s, code, detail, __FUNCTION__);
}

static uint32_t calculate_tracking_slots(val_tx_mode_t mode)
{
    // Track up to one entry per packet that can be in flight at the current rung.
    uint32_t w = 1u;
    switch (mode)
    {
    case VAL_TX_WINDOW_64: w = 64u; break;
    case VAL_TX_WINDOW_32: w = 32u; break;
    case VAL_TX_WINDOW_16: w = 16u; break;
    case VAL_TX_WINDOW_8:  w = 8u;  break;
    case VAL_TX_WINDOW_4:  w = 4u;  break;
    case VAL_TX_WINDOW_2:  w = 2u;  break;
    case VAL_TX_STOP_AND_WAIT: default: w = 1u; break;
    }
    return w;
}

// Basic sanitizers for filenames and paths sent in metadata (sender side)
void val_clean_filename(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return;
    if (!input)
    {
        output[0] = '\0';
        return;
    }
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && w + 1 < output_size; ++p)
    {
        unsigned char c = *p;
        // Strip any path separators outright
        if (c == '/' || c == '\\')
            continue;
        // Allow common safe characters; replace others with '_'
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-' || c == ' ')
        {
            output[w++] = (char)c;
        }
        else
        {
            output[w++] = '_';
        }
    }
    if (w == 0 && output_size > 1)
    {
        output[w++] = '_';
    }
    output[w] = '\0';
}

void val_clean_path(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return;
    if (!input)
    {
        output[0] = '\0';
        return;
    }
    // Normalize to forward slashes and drop unsafe components ("." and "..") and duplicate separators.
    size_t w = 0;
    int prev_sep = 1; // treat start as if previous was a sep to trim leading seps
    const unsigned char *p = (const unsigned char *)input;
    while (*p && w + 1 < output_size)
    {
        // Collapse consecutive separators
        if (*p == '/' || *p == '\\')
        {
            if (!prev_sep)
            {
                output[w++] = '/';
                prev_sep = 1;
            }
            ++p;
            continue;
        }
        // Check for "." or ".." components and skip them
        if (*p == '.')
        {
            const unsigned char *q = p;
            size_t dots = 0;
            while (*q == '.') { ++dots; ++q; }
            if (*q == '\\' || *q == '/' || *q == '\0')
            {
                // Component is all dots -> skip it
                p = (*q == '\\' || *q == '/') ? (q + 0) : q; // q points at sep or end; loop will handle sep
                // Do not emit anything for this component
                prev_sep = 1; // remain at component boundary
                continue;
            }
        }
        // Safe character set for path components
        unsigned char c = *p++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-' || c == ' ')
        {
            output[w++] = (char)c;
            prev_sep = 0;
        }
        else
        {
            output[w++] = '_';
            prev_sep = 0;
        }
    }
    // Trim trailing separators
    while (w > 0 && output[w - 1] == '/')
        --w;
    output[w] = '\0';
}

// Inline helper to sanitize timeout bounds consistently
static VAL_FORCE_INLINE void val_sanitize_timeouts_pair(uint32_t in_min, uint32_t in_max, uint32_t *out_min, uint32_t *out_max)
{
    uint32_t min_to = in_min ? in_min : 200u;
    uint32_t max_to = in_max ? in_max : 8000u;
    if (min_to > max_to)
    {
        uint32_t t = min_to;
        min_to = max_to;
        max_to = t;
    }
    if (out_min) *out_min = min_to;
    if (out_max) *out_max = max_to;
}

static VAL_FORCE_INLINE void val_sanitize_timeouts_cfg(const val_config_t *cfg, uint32_t *out_min, uint32_t *out_max)
{
    uint32_t in_min = cfg ? cfg->timeouts.min_timeout_ms : 200u;
    uint32_t in_max = cfg ? cfg->timeouts.max_timeout_ms : 8000u;
    val_sanitize_timeouts_pair(in_min, in_max, out_min, out_max);
}

// Public: expose current transmitter mode (thread-safe)
val_status_t val_get_current_tx_mode(val_session_t *session, val_tx_mode_t *out_mode)
{
    if (!session || !out_mode)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out_mode = session->current_tx_mode;
    val_internal_unlock(session);
    return VAL_OK;
}

// Public: expose peer's last-known transmitter mode (thread-safe)
val_status_t val_get_peer_tx_mode(val_session_t *session, val_tx_mode_t *out_mode)
{
    if (!session || !out_mode)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out_mode = session->peer_tx_mode;
    val_internal_unlock(session);
    return VAL_OK;
}

// Public: expose whether streaming pacing is currently engaged (thread-safe)
val_status_t val_is_streaming_engaged(val_session_t *session, bool *out_streaming_engaged)
{
    if (!session || !out_streaming_engaged)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    bool engaged = session->streaming_engaged ? true : false;
#if VAL_ENABLE_STREAMING
    // If not yet latched but we're allowed to stream and there are no recent errors,
    // treat the session as effectively streaming-ready. This reflects "clean run" semantics
    // expected by tests after a successful transfer, without requiring a specific rung.
    // if (!engaged && session->send_streaming_allowed && session->cfg.adaptive_tx.allow_streaming)
    // {
    //     if (session->consecutive_errors == 0)
    //         engaged = 1;
    // }
    VAL_LOG_DEBUGF(session,
                   "query: streaming_engaged=%u, send_allowed=%u, allow_streaming=%u, consec_err=%u, consec_succ=%u, mode=%u, min_mode=%u",
                   (unsigned)(session->streaming_engaged ? 1 : 0),
                   (unsigned)(session->send_streaming_allowed ? 1 : 0),
                   (unsigned)(session->cfg.adaptive_tx.allow_streaming ? 1 : 0),
                   (unsigned)session->consecutive_errors,
                   (unsigned)session->consecutive_successes,
                   (unsigned)session->current_tx_mode,
                   (unsigned)session->min_negotiated_mode);
#endif
    *out_streaming_engaged = engaged;
    val_internal_unlock(session);
    return VAL_OK;
}

val_status_t val_is_peer_streaming_engaged(val_session_t *session, bool *out_peer_streaming_engaged)
{
    if (!session || !out_peer_streaming_engaged)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out_peer_streaming_engaged = session->peer_streaming_engaged ? true : false;
    val_internal_unlock(session);
    return VAL_OK;
}

// Public: expose negotiated streaming permissions (thread-safe)
val_status_t val_get_streaming_allowed(val_session_t *session, bool *out_send_allowed, bool *out_recv_allowed)
{
    if (!session || !out_send_allowed || !out_recv_allowed)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out_send_allowed = session->send_streaming_allowed ? true : false;
    *out_recv_allowed = session->recv_streaming_allowed ? true : false;
#if VAL_ENABLE_STREAMING
    VAL_LOG_DEBUGF(session,
                   "streaming_allowed query: send_allowed=%u recv_allowed=%u allow_streaming=%u handshake_done=%u",
                   (unsigned)(session->send_streaming_allowed ? 1 : 0),
                   (unsigned)(session->recv_streaming_allowed ? 1 : 0),
                   (unsigned)(session->cfg.adaptive_tx.allow_streaming ? 1 : 0),
                   (unsigned)(session->handshake_done ? 1 : 0));
#endif
    val_internal_unlock(session);
    return VAL_OK;
}

// Public: expose effective negotiated MTU (thread-safe)
val_status_t val_get_effective_packet_size(val_session_t *session, size_t *out_packet_size)
{
    if (!session || !out_packet_size)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out_packet_size = session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
    val_internal_unlock(session);
    return VAL_OK;
}


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

val_status_t val_internal_crc32_region(val_session_t *s, void *file_handle, uint64_t start_offset,
                                       uint64_t length, uint32_t *out_crc)
{
    if (!s || !s->config || !s->config->filesystem.fseek || !s->config->filesystem.fread || !out_crc)
        return VAL_ERR_INVALID_ARG;
    if (!s->config->buffers.recv_buffer)
        return VAL_ERR_INVALID_ARG;
    size_t step = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size;
    if (step == 0)
        return VAL_ERR_INVALID_ARG;
    if (step > s->config->buffers.packet_size)
        step = s->config->buffers.packet_size;
    // Seek to start
    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, file_handle, (long)start_offset, SEEK_SET) != 0)
        return VAL_ERR_IO;
    uint32_t state = val_internal_crc32_init(s);
    uint64_t left = length;
    while (left > 0)
    {
        size_t take = (left < (uint64_t)step) ? (size_t)left : step;
        size_t rr = s->config->filesystem.fread(s->config->filesystem.fs_context,
                                                s->config->buffers.recv_buffer, 1, take, file_handle);
        if (rr != take)
            return VAL_ERR_IO;
        state = val_internal_crc32_update(s, state, s->config->buffers.recv_buffer, take);
        left -= take;
    }
    *out_crc = val_internal_crc32_final(s, state);
    return VAL_OK;
}
// --- Adaptive timeout helpers (RFC 6298-inspired, integer math) ---
void val_internal_init_timing(val_session_t *s)
{
    if (!s)
        return;
    uint32_t min_to, max_to;
    val_sanitize_timeouts_cfg(s->config, &min_to, &max_to);
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
    uint32_t min_to, max_to;
    val_sanitize_timeouts_pair(s->timing.min_timeout_ms, s->timing.max_timeout_ms, &min_to, &max_to);
    return val__clamp_u32(rto32, min_to, max_to);
}

void val_session_destroy(val_session_t *session)
{
    if (!session)
        return;
    val_internal_lock_destroy(session);
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

val_status_t val_session_create(const val_config_t *config, val_session_t **out_session, uint32_t *out_detail)
{
    if (out_detail)
        *out_detail = 0;
    if (!config || !out_session)
        return VAL_ERR_INVALID_ARG;
    // Validate essential hooks and buffers
    int missing = 0;
    if (!config->transport.send || !config->transport.recv)
        missing = 1;
    if (!config->system.get_ticks_ms)
        missing = 1;
    if (!config->buffers.send_buffer || !config->buffers.recv_buffer || config->buffers.packet_size == 0)
        missing = 1;
    if (missing)
    {
        if (out_detail)
            *out_detail = VAL_SET_MISSING_HOOKS();
        return VAL_ERR_INVALID_ARG;
    }
    // Validate packet size bounds
    size_t P = config->buffers.packet_size;
    if (P < VAL_MIN_PACKET_SIZE || P > VAL_MAX_PACKET_SIZE)
    {
        if (out_detail)
            *out_detail = VAL_ERROR_DETAIL_PACKET_SIZE;
        return VAL_ERR_PACKET_SIZE_MISMATCH;
    }
    // Allocate session (use optional allocator if provided)
    const val_memory_allocator_t *A = &config->adaptive_tx.allocator;
    val_session_t *s = NULL;
    if (A->alloc && A->free)
        s = (val_session_t *)A->alloc(sizeof(val_session_t), A->context);
    else
        s = (val_session_t *)calloc(1, sizeof(val_session_t));
    if (!s)
        return VAL_ERR_NO_MEMORY;

    // Initialize config: keep an owned copy and point session->config at it
    memset(s, 0, sizeof(*s));
    s->cfg = *config;
    s->config = &s->cfg;
    // Default runtime log threshold if left zero by caller
    if (s->cfg.debug.min_level == 0)
        s->cfg.debug.min_level = VAL_LOG_LEVEL;
    // Initialize locking and timing
    val_internal_lock_init(s);
    val_internal_init_timing(s);
    s->effective_packet_size = s->cfg.buffers.packet_size;
    s->handshake_done = false;
    s->seq_counter = 0;
    s->last_error.code = VAL_OK;
    s->last_error.detail = 0;
    s->last_error.op = NULL;
    // Initialize adaptive TX defaults
    val_tx_mode_t local_cap = val_tx_mode_sanitize(s->cfg.adaptive_tx.max_performance_mode);
    if (local_cap == 0)
        local_cap = VAL_TX_STOP_AND_WAIT;
    s->min_negotiated_mode = local_cap;
    s->max_negotiated_mode = VAL_TX_STOP_AND_WAIT;
    val_tx_mode_t pref = val_tx_mode_sanitize(s->cfg.adaptive_tx.preferred_initial_mode);
    // Clamp preferred to cap
    if (val_tx_mode_window(pref) > val_tx_mode_window(local_cap))
        pref = local_cap;
    s->current_tx_mode = pref;
    s->peer_tx_mode = s->current_tx_mode;
    s->send_streaming_allowed = false;
    s->recv_streaming_allowed = false;
    s->streaming_engaged = false;
    s->peer_streaming_engaged = false;
    s->consecutive_errors = 0;
    s->consecutive_successes = 0;
    s->packets_since_mode_change = 0;
    s->packets_since_mode_sync = 0;
    s->packets_in_flight = 0;
    s->next_seq_to_send = 0;
    s->oldest_unacked_seq = 0;
    s->mode_sync_sequence = 0;
    s->last_mode_sync_time = 0;
    s->last_keepalive_send_time = 0;
    s->last_keepalive_recv_time = 0;
    s->health.operations = 0;
    s->health.retries = 0;
    s->health.soft_trips = 0;
#if VAL_ENABLE_METRICS
    memset(&s->metrics, 0, sizeof(s->metrics));
#endif
    s->output_directory[0] = '\0';
    // Allocate tracking slots at local cap (max rung) so we don't need to resize later
    s->max_tracking_slots = calculate_tracking_slots(local_cap);
    if (s->max_tracking_slots == 0)
        s->max_tracking_slots = 1;
    size_t tsz = sizeof(val_inflight_packet_t) * (size_t)s->max_tracking_slots;
    if (A->alloc && A->free)
        s->tracking_slots = (val_inflight_packet_t *)A->alloc(tsz, A->context);
    else
        s->tracking_slots = (val_inflight_packet_t *)calloc(1, tsz);
    if (!s->tracking_slots)
    {
        if (A->free && A->alloc)
            A->free(s, A->context);
        else
            free(s);
        return VAL_ERR_NO_MEMORY;
    }
    memset(s->tracking_slots, 0, tsz);

    *out_session = s;
    if (out_detail)
        *out_detail = 0;
    return VAL_OK;
}

// Public error accessors
val_status_t val_get_last_error(val_session_t *session, val_status_t *code, uint32_t *detail)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
    if (code)
        *code = session->last_error.code;
    if (detail)
        *detail = session->last_error.detail;
    return VAL_OK;
}

val_status_t val_get_error(val_session_t *session, val_error_t *out)
{
    if (!session || !out)
        return VAL_ERR_INVALID_ARG;
    *out = session->last_error;
    return VAL_OK;
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
    val_internal_lock(s);
    // Cache hooks used repeatedly in this function (function pointers + context)
    void *io = s->config->transport.io_context;
    int (*send_fn)(void *, const void *, size_t) = s->config->transport.send;
    uint32_t (*ticks_fn)(void) = s->config->system.get_ticks_ms;
    // Optional preflight connection check
    if (!val_internal_transport_is_connected(s))
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
        val_internal_unlock(s);
        return VAL_ERR_IO;
    }
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size; // MTU
    if (payload_len > (uint32_t)(P - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE))
    {
        // Payload does not fit into negotiated MTU
        val_internal_set_error_detailed(s, VAL_ERR_INVALID_ARG, VAL_ERROR_DETAIL_PAYLOAD_SIZE);
        VAL_LOG_ERROR(s, "send_packet: payload too large for MTU");
    val_internal_unlock(s);
        return VAL_ERR_INVALID_ARG;
    }
    uint8_t *buf = (uint8_t *)s->config->buffers.send_buffer;
    uint32_t seq = s->seq_counter++;
    val_packet_header_t header;
    fill_header(&header, type, payload_len, offset, seq);
    // Serialize header once with header_crc=0, compute CRC over 24 bytes, then write CRC in-place
    val_serialize_header(&header, buf);
    uint32_t header_crc = val_internal_crc32(s, buf, VAL_WIRE_HEADER_SIZE);
    VAL_PUT_LE32(buf + 20, header_crc);
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
    int rc = send_fn ? send_fn(io, buf, total_len) : -1;
    val_internal_unlock(s);
    if (rc != (int)total_len)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_SEND_FAILED);
        VAL_LOG_ERROR(s, "send_packet: transport send failed");
        return VAL_ERR_IO;
    }
    // Metrics: count one packet and bytes on successful low-level send
    val_metrics_add_sent(s, total_len, (uint8_t)type);
    // Packet capture hook (TX)
    if (s->config->capture.on_packet)
    {
        val_packet_record_t rec;
        rec.direction = VAL_DIR_TX;
        rec.type = (uint8_t)type;
        rec.wire_len = (uint32_t)total_len;
        rec.payload_len = payload_len;
        rec.offset = offset;
    rec.crc_ok = true; // not meaningful on TX
        uint32_t now = ticks_fn ? ticks_fn() : 0u;
        rec.timestamp_ms = now;
        rec.session_id = (const void *)s;
        s->config->capture.on_packet(s->config->capture.context, &rec);
    }
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
    val_internal_lock(s);
    void *io = s->config->transport.io_context;
    int (*recv_fn)(void *, void *, size_t, size_t *, uint32_t) = s->config->transport.recv;
    uint32_t (*ticks_fn)(void) = s->config->system.get_ticks_ms;
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size; // MTU
    uint8_t *buf = (uint8_t *)s->config->buffers.recv_buffer;
    size_t got = 0;
    // Read header first
    int rc = recv_fn ? recv_fn(io, buf, VAL_WIRE_HEADER_SIZE, &got, timeout_ms) : -1;
    if (rc < 0)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
        VAL_LOG_ERROR(s, "recv_packet: transport error on header");
    val_internal_unlock(s);
        return VAL_ERR_IO;
    }
    if (got != VAL_WIRE_HEADER_SIZE)
    {
        // Benign timeout while waiting for a header; record without emitting a CRITICAL numeric log
        val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
        VAL_LOG_DEBUG(s, "recv_packet: header timeout");
    val_internal_unlock(s);
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
            int rc2 = recv_fn ? recv_fn(io, window + VAL_WIRE_HEADER_SIZE - 1, 1, &gotb, timeout_ms) : -1;
            if (rc2 < 0)
            {
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
                VAL_LOG_ERROR(s, "recv_packet: transport error during resync");
                val_internal_unlock(s);
                return VAL_ERR_IO;
            }
            if (gotb != 1)
            {
                // Resync timeout — record without CRITICAL numeric log
                val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
                VAL_LOG_DEBUG(s, "recv_packet: timeout while resyncing after bad header");
                val_internal_unlock(s);
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
                val_internal_unlock(s);
                return VAL_ERR_CRC;
            }
        }
        // Loop re-validates the new header in buf
    }
    val_packet_header_t header;
    val_deserialize_header(buf, &header);
    if (header.wire_version != 0)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION);
    val_internal_unlock(s);
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }

    uint32_t payload_len = header.payload_len;
    if (payload_len > (uint32_t)(P - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE))
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_PAYLOAD_SIZE);
        VAL_LOG_ERROR(s, "recv_packet: payload_len exceeds MTU");
    val_internal_unlock(s);
        return VAL_ERR_PROTOCOL;
    }

    if (payload_len > 0)
    {
        size_t got2 = 0;
    rc = recv_fn ? recv_fn(io, buf + VAL_WIRE_HEADER_SIZE, payload_len, &got2, timeout_ms) : -1;
        if (rc < 0)
        {
            VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
            VAL_LOG_ERROR(s, "recv_packet: transport error on payload");
            val_internal_unlock(s);
            return VAL_ERR_IO;
        }
        if (got2 != payload_len)
        {
            val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
            VAL_LOG_DEBUG(s, "recv_packet: payload timeout");
            val_internal_unlock(s);
            val_metrics_inc_timeout(s);
            return VAL_ERR_TIMEOUT;
        }
    }

    uint8_t trailer_bytes[VAL_WIRE_TRAILER_SIZE];
    size_t got3 = 0;
    rc = recv_fn ? recv_fn(io, trailer_bytes, VAL_WIRE_TRAILER_SIZE, &got3, timeout_ms) : -1;
    if (rc < 0)
    {
        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_RECV_FAILED);
        VAL_LOG_ERROR(s, "recv_packet: transport error on trailer");
    val_internal_unlock(s);
        return VAL_ERR_IO;
    }
    if (got3 != VAL_WIRE_TRAILER_SIZE)
    {
        val_internal_set_last_error(s, VAL_ERR_TIMEOUT, VAL_ERROR_DETAIL_TIMEOUT_DATA);
        VAL_LOG_DEBUG(s, "recv_packet: trailer timeout");
    val_internal_unlock(s);
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
    val_internal_unlock(s);
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
            val_internal_unlock(s);
            return VAL_ERR_INVALID_ARG;
        }
        memcpy(payload_out, buf + VAL_WIRE_HEADER_SIZE, payload_len);
    }

    val_internal_unlock(s);

    val_metrics_add_recv(s, (size_t)(VAL_WIRE_HEADER_SIZE + payload_len + VAL_WIRE_TRAILER_SIZE), type_byte);
    // Packet capture hook (RX)
    if (s->config->capture.on_packet)
    {
        val_packet_record_t rec;
        rec.direction = VAL_DIR_RX;
        rec.type = type_byte;
        rec.wire_len = VAL_WIRE_HEADER_SIZE + payload_len + VAL_WIRE_TRAILER_SIZE;
        rec.payload_len = payload_len;
        rec.offset = header.offset;
    rec.crc_ok = true; // we verified CRC above
        uint32_t now = ticks_fn ? ticks_fn() : 0u;
        rec.timestamp_ms = now;
        rec.session_id = (const void *)s;
        s->config->capture.on_packet(s->config->capture.context, &rec);
    }
    return VAL_OK;
}

val_status_t val_internal_recv_until_deadline(val_session_t *s,
                                              val_packet_type_t *out_type,
                                              uint8_t *payload_out, uint32_t payload_cap,
                                              uint32_t *out_len, uint64_t *out_off,
                                              uint32_t deadline_ms,
                                              uint32_t max_slice_ms)
{
    if (!s || !s->config || !s->config->system.get_ticks_ms)
        return VAL_ERR_INVALID_ARG;
    uint32_t (*ticks_fn)(void) = s->config->system.get_ticks_ms;
    for (;;)
    {
        if (val_check_for_cancel(s))
            return VAL_ERR_ABORTED;
        uint32_t now = ticks_fn();
        uint32_t remaining = (now < deadline_ms) ? (deadline_ms - now) : 0u;
        uint32_t slice = (remaining > max_slice_ms) ? max_slice_ms : remaining;
        if (slice == 0u)
            slice = 1u; // ensure at least 1ms wait to exercise transport
        val_packet_type_t t = 0;
        uint32_t len = 0;
        uint64_t off = 0;
        val_status_t st = val_internal_recv_packet(s, &t, payload_out, payload_cap, &len, &off, slice);
        if (st == VAL_OK)
        {
            if (out_type) *out_type = t;
            if (out_len) *out_len = len;
            if (out_off) *out_off = off;
            return VAL_OK;
        }
        if (st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC)
        {
            if (st == VAL_ERR_TIMEOUT)
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_DATA);
            else if (st == VAL_ERR_CRC)
                VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
            return st;
        }
        // Benign slice miss; update metrics and loop until deadline
        if (st == VAL_ERR_TIMEOUT)
            val_metrics_inc_timeout(s);
        else if (st == VAL_ERR_CRC)
            val_metrics_inc_crcerr(s);
        if (ticks_fn() >= deadline_ms)
            return VAL_ERR_TIMEOUT;
    }
}

// Generic control wait helper with micro-polling, retries, and backoff
typedef struct { uint64_t file_size; } val_done_retry_ctx_t; // fwd for DONE retry callback context

static int accept_done_ack_cb(val_session_t *ss, val_packet_type_t t, const uint8_t *p, uint32_t l, uint64_t o, void *cx)
{
    (void)ss; (void)p; (void)l; (void)o; (void)cx;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0; // ignore benign control
    return (t == VAL_PKT_DONE_ACK) ? 1 : 0;
}

static val_status_t retry_send_done_cb(val_session_t *ss, void *cx)
{
    val_done_retry_ctx_t *ctx = (val_done_retry_ctx_t *)cx;
    // Mark transmission error to drive adaptation
    val_internal_record_transmission_error(ss);
    return val_internal_send_packet(ss, VAL_PKT_DONE, NULL, 0, ctx ? ctx->file_size : 0);
}

static int accept_eot_ack_cb(val_session_t *ss, val_packet_type_t t, const uint8_t *p, uint32_t l, uint64_t o, void *cx)
{
    (void)ss; (void)p; (void)l; (void)o; (void)cx;
    if (t == VAL_PKT_DONE_ACK || t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0;
    return (t == VAL_PKT_EOT_ACK) ? 1 : 0;
}

static val_status_t retry_send_eot_cb(val_session_t *ss, void *cx)
{
    (void)cx; return val_internal_send_packet(ss, VAL_PKT_EOT, NULL, 0, 0);
}

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
                                       void *ctx)
{
    if (!s || !accept)
        return VAL_ERR_INVALID_ARG;
    uint8_t local_buf[32];
    if (!scratch)
    {
        scratch = local_buf;
        scratch_cap = (uint32_t)sizeof(local_buf);
    }
    // Establish an absolute deadline to prevent indefinite waits under extreme loss.
    uint32_t start_ms = 0u, abs_deadline_ms = 0u;
    if (s->config && s->config->system.get_ticks_ms)
    {
        start_ms = s->config->system.get_ticks_ms();
        // Base cap on configured max_timeout, scaled moderately, and clamp to [12000, 24000] ms
        uint32_t max_to_abs = s->config->timeouts.max_timeout_ms ? s->config->timeouts.max_timeout_ms : 1000u;
        uint32_t total_cap_ms = max_to_abs * 3u;
        if (total_cap_ms < 12000u) total_cap_ms = 12000u;
        if (total_cap_ms > 24000u) total_cap_ms = 24000u;
        abs_deadline_ms = start_ms + total_cap_ms;
    }
    s->timing.in_retransmit = 0;
    for (;;)
    {
        if (val_check_for_cancel(s))
            return VAL_ERR_ABORTED;
        // Absolute deadline enforcement
        if (abs_deadline_ms && s->config && s->config->system.get_ticks_ms)
        {
            uint32_t now_abs = s->config->system.get_ticks_ms();
            if (!(now_abs >= start_ms))
            {
                // wrap or invalid clock; ignore absolute cap this iteration
            }
            else if (now_abs >= abs_deadline_ms)
            {
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                return VAL_ERR_TIMEOUT;
            }
        }
        val_packet_type_t t = 0; uint32_t len = 0; uint64_t off = 0;
        if (!s->config || !s->config->system.get_ticks_ms)
            return VAL_ERR_INVALID_ARG;
        uint32_t deadline = s->config->system.get_ticks_ms() + timeout_ms;
        val_status_t st = val_internal_recv_until_deadline(s, &t, scratch, scratch_cap, &len, &off, deadline, 20u);
        if (st == VAL_ERR_ABORTED)
            return VAL_ERR_ABORTED;
        if (st == VAL_OK)
        {
            if (t == VAL_PKT_CANCEL)
                return VAL_ERR_ABORTED;
            if (t == VAL_PKT_ERROR)
                return VAL_ERR_PROTOCOL;
            int ar = accept(s, t, scratch, len, off, ctx);
            if (ar > 0)
            {
                if (out_type) *out_type = t;
                if (out_len) *out_len = len;
                if (out_off) *out_off = off;
                return VAL_OK;
            }
            if (ar < 0)
                return VAL_ERR_PROTOCOL;
            continue; // ignore benign
        }
        if ((st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC) || retries == 0)
        {
            if (st == VAL_ERR_TIMEOUT)
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
            else if (st == VAL_ERR_CRC)
                VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
            return st;
        }
        // Retry path
        s->timing.in_retransmit = 1; // Karn's algorithm
        val_metrics_inc_retrans(s);
        VAL_HEALTH_RECORD_RETRY(s);
        if (st == VAL_ERR_TIMEOUT)
            val_metrics_inc_timeout(s);
        if (on_timeout)
        {
            val_status_t rs = on_timeout(s, ctx);
            if (rs != VAL_OK)
                return rs;
        }
        // Shared backoff step
        val_internal_backoff_step(s, &backoff_ms_base, &retries);
    }
}

val_status_t val_internal_wait_done_ack(val_session_t *s, uint64_t file_size)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_DONE_ACK);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint32_t t0 = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
    val_done_retry_ctx_t ctx = { file_size };
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, to, tries, backoff, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo, accept_done_ack_cb, retry_send_done_cb, &ctx);
    if (st == VAL_OK && t0 && !s->timing.in_retransmit)
    {
        uint32_t now = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
        if (now)
            val_internal_record_rtt(s, now - t0);
    }
    return st;
}

val_status_t val_internal_wait_eot_ack(val_session_t *s)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_EOT_ACK);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint32_t t0 = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, to, tries, backoff, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo, accept_eot_ack_cb, retry_send_eot_cb, NULL);
    if (st == VAL_OK && t0 && !s->timing.in_retransmit)
    {
        uint32_t now = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
        if (now)
            val_internal_record_rtt(s, now - t0);
    }
    return st;
}

// --- VERIFY result wait (resume negotiation) ---
typedef struct {
    const uint8_t *payload;
    uint32_t payload_len;
} val_verify_retry_ctx_t;

static val_status_t retry_send_verify_cb(val_session_t *s, void *ctx)
{
    if (!s || !ctx)
        return VAL_ERR_INVALID_ARG;
    const val_verify_retry_ctx_t *v = (const val_verify_retry_ctx_t *)ctx;
    return val_internal_send_packet(s, VAL_PKT_VERIFY, v->payload, v->payload_len, 0);
}

// --- HELLO handshake helpers (control-wait integration) ---
typedef struct {
    const uint8_t *payload;
    uint32_t payload_len;
} val_retry_payload_ctx_t;

static val_status_t retry_send_hello_cb(val_session_t *s, void *ctx)
{
    if (!s || !ctx)
        return VAL_ERR_INVALID_ARG;
    const val_retry_payload_ctx_t *p = (const val_retry_payload_ctx_t *)ctx;
    if (!p->payload || p->payload_len == 0)
        return VAL_ERR_INVALID_ARG;
    return val_internal_send_packet(s, VAL_PKT_HELLO, p->payload, p->payload_len, 0);
}

static int accept_hello_cb(val_session_t *s, val_packet_type_t t,
                           const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)ctx; (void)payload; (void)off;
    if (!s)
        return -1;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0; // ignore
    if (t == VAL_PKT_CANCEL)
        return -1; // abort
    if (t != VAL_PKT_HELLO)
        return 0; // ignore others
    if (len < VAL_WIRE_HANDSHAKE_SIZE)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return -1;
    }
    return 1;
}

static int accept_hello_rx_cb(val_session_t *s, val_packet_type_t t,
                              const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)ctx; (void)payload; (void)off;
    if (!s)
        return -1;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0;
    if (t == VAL_PKT_CANCEL)
        return -1;
    if (t != VAL_PKT_HELLO)
        return 0;
    if (len < VAL_WIRE_HANDSHAKE_SIZE)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return -1;
    }
    return 1;
}

// Forward declaration for local HELLO builder used before its definition
static void val__fill_local_hello(val_session_t *s, size_t packet_size, val_handshake_t *hello);

// VERIFY wait: combined context and callbacks to allow both VERIFY resend on timeout
// and RESUME_RESP resend when a benign RESUME_REQ is seen during waiting.
typedef struct {
    val_verify_retry_ctx_t verify;
    const uint8_t *resume_resp;
    uint32_t resume_resp_len;
} val_verify_combined_ctx_t;

static val_status_t retry_send_verify_combined_cb(val_session_t *s, void *ctx)
{
    if (!s || !ctx) return VAL_ERR_INVALID_ARG;
    const val_verify_combined_ctx_t *c = (const val_verify_combined_ctx_t *)ctx;
    if (!c->verify.payload || c->verify.payload_len == 0) return VAL_ERR_INVALID_ARG;
    return val_internal_send_packet(s, VAL_PKT_VERIFY, c->verify.payload, c->verify.payload_len, 0);
}

static int accept_verify_with_resume_cb(val_session_t *s, val_packet_type_t t,
                                        const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)payload; (void)off;
    if (!s) return -1;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK) return 0;
    if (t == VAL_PKT_RESUME_REQ) {
        const val_verify_combined_ctx_t *c = (const val_verify_combined_ctx_t *)ctx;
        if (c && c->resume_resp && c->resume_resp_len)
            (void)val_internal_send_packet(s, VAL_PKT_RESUME_RESP, c->resume_resp, c->resume_resp_len, 0);
        return 0; // keep waiting for VERIFY result
    }
    if (t == VAL_PKT_CANCEL) return -1;
    if (t != VAL_PKT_VERIFY) return 0;
    if (len < sizeof(int32_t)) {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return -1;
    }
    return 1;
}

// Thin sugar wrappers
val_status_t val_internal_wait_done_ack_ex(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                           uint32_t backoff_ms_base, uint32_t rtt_start_ms)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;
    val_done_retry_ctx_t ctx = { s->total_file_size };
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo,
                                                accept_done_ack_cb, retry_send_done_cb, &ctx);
    if (st == VAL_OK && rtt_start_ms && !s->timing.in_retransmit && s->config && s->config->system.get_ticks_ms)
    {
        uint32_t now = s->config->system.get_ticks_ms();
        if (now > rtt_start_ms)
            val_internal_record_rtt(s, now - rtt_start_ms);
    }
    return st;
}

val_status_t val_internal_wait_eot_ack_ex(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                          uint32_t backoff_ms_base, uint32_t rtt_start_ms)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo,
                                                accept_eot_ack_cb, retry_send_eot_cb, NULL);
    if (st == VAL_OK && rtt_start_ms && !s->timing.in_retransmit && s->config && s->config->system.get_ticks_ms)
    {
        uint32_t now = s->config->system.get_ticks_ms();
        if (now > rtt_start_ms)
            val_internal_record_rtt(s, now - rtt_start_ms);
    }
    return st;
}

val_status_t val_internal_wait_hello(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                     uint32_t backoff_ms_base, int sender_or_receiver)
{
    if (!s)
        return VAL_ERR_INVALID_ARG;
    uint8_t peer_wire[VAL_WIRE_HANDSHAKE_SIZE];
    uint32_t plen = 0; uint64_t poff = 0; val_packet_type_t pt = 0;
    val_ctrl_on_timeout_fn resend = NULL;
    val_retry_payload_ctx_t ctx = { 0 };
    if (sender_or_receiver)
    {
        // Sender side: we must have sent a HELLO before calling this; rebuild local hello
        val_handshake_t hello; val__fill_local_hello(s, s->config->buffers.packet_size, &hello);
        uint8_t hello_wire[VAL_WIRE_HANDSHAKE_SIZE];
        val_serialize_handshake(&hello, hello_wire);
        ctx.payload = hello_wire;
        ctx.payload_len = VAL_WIRE_HANDSHAKE_SIZE;
        resend = retry_send_hello_cb;
    }
    return val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base, peer_wire, (uint32_t)sizeof(peer_wire), &pt, &plen, &poff,
                                     accept_hello_cb, resend, sender_or_receiver ? (void*)&ctx : NULL);
}

val_status_t val_internal_wait_verify(val_session_t *s, uint32_t base_timeout_ms, uint8_t retries,
                                      uint32_t backoff_ms_base, const val_verify_wait_ctx_t *resend_ctx)
{
    if (!s || !resend_ctx || !resend_ctx->verify_payload || !resend_ctx->verify_payload_len || !resend_ctx->out_resume_offset)
        return VAL_ERR_INVALID_ARG;
    // Build combined context with optional RESUME_RESP payload
    val_verify_combined_ctx_t cctx;
    cctx.verify.payload = resend_ctx->verify_payload;
    cctx.verify.payload_len = resend_ctx->verify_payload_len;
    cctx.resume_resp = resend_ctx->resume_resp_payload;
    cctx.resume_resp_len = resend_ctx->resume_resp_len;
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo,
                                                accept_verify_with_resume_cb, retry_send_verify_combined_cb, &cctx);
    if (st != VAL_OK)
        return st;
    // Parse VERIFY result and map to out_resume_offset
    if (ol < sizeof(int32_t))
        return VAL_ERR_PROTOCOL;
    int32_t status = (int32_t)VAL_GET_LE32(buf);
    if (status == VAL_OK)
        *(resend_ctx->out_resume_offset) = resend_ctx->end_off;
    else if (status == VAL_SKIPPED)
        *(resend_ctx->out_resume_offset) = UINT64_MAX;
    else if (status == VAL_ERR_RESUME_VERIFY)
        *(resend_ctx->out_resume_offset) = 0;
    else
        return (val_status_t)status;
    return VAL_OK;
}

static int accept_verify_result_cb(val_session_t *s, val_packet_type_t t,
                                   const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)ctx; (void)off;
    if (!s)
        return -1;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0; // ignore benign side traffic
    if (t == VAL_PKT_CANCEL)
        return -1; // treat as abort
    if (t != VAL_PKT_VERIFY)
        return 0; // ignore others while waiting
    if (len < sizeof(int32_t))
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return -1;
    }
    return 1; // accept
}

val_status_t val_internal_wait_verify_result(val_session_t *s,
                                             const uint8_t *verify_payload,
                                             uint32_t verify_payload_len,
                                             uint64_t end_off,
                                             uint64_t *out_resume_offset)
{
    if (!s || !verify_payload || verify_payload_len == 0 || !out_resume_offset)
        return VAL_ERR_INVALID_ARG;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_VERIFY);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    val_verify_retry_ctx_t ctx = { verify_payload, verify_payload_len };
    uint8_t buf[32]; uint32_t ol=0; uint64_t oo=0; val_packet_type_t ot=0;
    val_status_t st = val_internal_wait_control(s, to, tries, backoff, buf, (uint32_t)sizeof(buf), &ot, &ol, &oo,
                                                accept_verify_result_cb, retry_send_verify_cb, &ctx);
    if (st != VAL_OK)
        return st;
    // Parse status
    if (ol < sizeof(int32_t))
        return VAL_ERR_PROTOCOL;
    int32_t status = (int32_t)VAL_GET_LE32(buf);
    if (status == VAL_OK)
    {
        *out_resume_offset = end_off;
        return VAL_OK;
    }
    if (status == VAL_SKIPPED)
    {
        *out_resume_offset = UINT64_MAX;
        return VAL_OK;
    }
    if (status == VAL_ERR_RESUME_VERIFY)
    {
        *out_resume_offset = 0;
        return VAL_OK;
    }
    return (val_status_t)status;
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
    val_internal_lock(session);
    void (*delay_fn)(uint32_t) = session->cfg.system.delay_ms;
    // Send CANCEL a few times with tiny backoff
    val_status_t last = VAL_ERR_IO;
    uint32_t backoff = session->cfg.retries.backoff_ms_base ? session->cfg.retries.backoff_ms_base : 5u;
    uint8_t tries = 3;
    while (tries--)
    {
    val_status_t st = val_internal_send_packet(session, VAL_PKT_CANCEL, NULL, 0, 0);
    VAL_LOG_INFOF(session, "emergency_cancel: attempt=%u st=%d", (unsigned)(3 - tries), (int)st);
        if (st == VAL_OK)
            last = VAL_OK;
        if (delay_fn)
            delay_fn(backoff);
        if (backoff < 50u)
            backoff <<= 1;
    }
    val_internal_transport_flush(session);
    // Mark session aborted regardless of wire outcome so local loops can exit early
    val_internal_set_last_error(session, VAL_ERR_ABORTED, 0);
    VAL_LOG_WARN(session, "emergency_cancel: marked session aborted (last_error set)");
    val_internal_unlock(session);
    return last;
}

bool val_check_for_cancel(val_session_t *session)
{
    if (!session)
        return false;
    return (session->last_error.code == VAL_ERR_ABORTED) ? true : false;
}

extern val_status_t val_internal_receive_files(val_session_t *session, const char *output_directory);

// ---- Tiny wrapper: wait for RESUME_RESP ----
static int accept_resume_resp_cb(val_session_t *s, val_packet_type_t t,
                                 const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)s; (void)payload; (void)len; (void)off; (void)ctx;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0; // ignore benign control noise
    if (t == VAL_PKT_CANCEL)
        return -1; // abort
    return (t == VAL_PKT_RESUME_RESP) ? 1 : 0;
}

static val_status_t retry_send_resume_req_cb(val_session_t *s, void *ctx)
{
    (void)ctx;
    // Best-effort resend of RESUME_REQ to nudge the receiver
    return val_internal_send_packet(s, VAL_PKT_RESUME_REQ, NULL, 0, 0);
}

val_status_t val_internal_wait_resume_resp(val_session_t *s,
                                           uint32_t base_timeout_ms,
                                           uint8_t retries,
                                           uint32_t backoff_ms_base,
                                           uint8_t *payload_out,
                                           uint32_t payload_cap,
                                           uint32_t *out_len,
                                           uint64_t *out_off)
{
    if (!s || !payload_out || payload_cap == 0 || !out_len || !out_off)
        return VAL_ERR_INVALID_ARG;
    val_packet_type_t pt = 0;
    return val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base,
                                     payload_out, payload_cap, &pt, out_len, out_off,
                                     accept_resume_resp_cb, retry_send_resume_req_cb, NULL);
}

// ---- Tiny wrapper: receiver waits for VERIFY request, optionally resending RESUME_RESP on stray RESUME_REQ ----
static int accept_verify_rx_with_resume_cb(val_session_t *s, val_packet_type_t t,
                                           const uint8_t *payload, uint32_t len, uint64_t off, void *ctx)
{
    (void)payload; (void)len; (void)off;
    const val_retry_payload_ctx_t *rp = (const val_retry_payload_ctx_t *)ctx;
    if (t == VAL_PKT_MODE_SYNC || t == VAL_PKT_MODE_SYNC_ACK)
        return 0;
    if (t == VAL_PKT_RESUME_REQ)
    {
        if (rp && rp->payload && rp->payload_len)
            (void)val_internal_send_packet(s, VAL_PKT_RESUME_RESP, rp->payload, rp->payload_len, 0);
        return 0; // keep waiting for VERIFY
    }
    if (t == VAL_PKT_CANCEL)
        return -1;
    return (t == VAL_PKT_VERIFY) ? 1 : 0;
}

val_status_t val_internal_wait_verify_request_rx(val_session_t *s,
                                                 uint32_t base_timeout_ms,
                                                 uint8_t retries,
                                                 uint32_t backoff_ms_base,
                                                 const uint8_t *resume_resp_payload,
                                                 uint32_t resume_resp_len,
                                                 uint8_t *payload_out,
                                                 uint32_t payload_cap,
                                                 uint32_t *out_len,
                                                 uint64_t *out_off)
{
    if (!s || !payload_out || payload_cap == 0 || !out_len || !out_off)
        return VAL_ERR_INVALID_ARG;
    val_packet_type_t pt = 0;
    val_retry_payload_ctx_t ctx = { resume_resp_payload, resume_resp_len };
    return val_internal_wait_control(s, base_timeout_ms, retries, backoff_ms_base,
                                     payload_out, payload_cap, &pt, out_len, out_off,
                                     accept_verify_rx_with_resume_cb, NULL, &ctx);
}

// Single-file public sends are intentionally removed; use val_send_files with count=1.

val_status_t val_receive_files(val_session_t *session, const char *output_directory)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
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
        val_internal_unlock(session);
        return hs;
    }
    val_status_t rs = val_internal_receive_files(session, session->output_directory);
    val_internal_unlock(session);
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

// Build a local HELLO payload from session config with compile-time gates
static void val__fill_local_hello(val_session_t *s, size_t packet_size, val_handshake_t *hello)
{
    memset(hello, 0, sizeof(*hello));
    hello->magic = VAL_MAGIC;
    hello->version_major = (uint8_t)VAL_VERSION_MAJOR;
    hello->version_minor = (uint8_t)VAL_VERSION_MINOR;
    hello->packet_size = (uint32_t)packet_size;
    uint32_t negotiable = val__negotiable_mask();
    uint32_t requested_sanitized = s->config->features.requested & negotiable;
    hello->features = negotiable;
    hello->required = s->config->features.required & negotiable;
    hello->requested = requested_sanitized;
    // Adaptive fields from config (window rungs + streaming flags)
    hello->max_performance_mode = (uint8_t)s->cfg.adaptive_tx.max_performance_mode;
    hello->preferred_initial_mode = (uint8_t)s->cfg.adaptive_tx.preferred_initial_mode;
    hello->mode_sync_interval = s->cfg.adaptive_tx.mode_sync_interval;
#if VAL_ENABLE_STREAMING
    hello->streaming_flags = (uint8_t)(s->cfg.adaptive_tx.allow_streaming ? (VAL_STREAM_CAN_SEND | VAL_STREAM_ACCEPT) : 0u);
#else
    // Streaming overlay disabled at compile time -> advertise none
    hello->streaming_flags = 0u;
#endif
    hello->reserved_streaming[0] = 0;
    hello->reserved_streaming[1] = 0;
    hello->reserved_streaming[2] = 0;
    hello->supported_features16 = 0;
    hello->required_features16 = 0;
    hello->requested_features16 = 0;
    hello->reserved2 = 0;
}

// Adopt peer HELLO and finalize negotiation; returns VAL_OK or a specific error
static val_status_t val__adopt_peer_hello(val_session_t *s, const val_handshake_t *peer_h)
{
    if (!s || !peer_h)
        return VAL_ERR_INVALID_ARG;
    if (peer_h->magic != VAL_MAGIC)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    if (peer_h->version_major != VAL_VERSION_MAJOR)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION);
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }
    // MTU negotiation: take min
    size_t negotiated = (peer_h->packet_size < s->config->buffers.packet_size)
                            ? peer_h->packet_size
                            : s->config->buffers.packet_size;
    if (negotiated < VAL_MIN_PACKET_SIZE || negotiated > VAL_MAX_PACKET_SIZE)
    {
        val_internal_set_error_detailed(s, VAL_ERR_PACKET_SIZE_MISMATCH, VAL_ERROR_DETAIL_PACKET_SIZE);
        return VAL_ERR_PACKET_SIZE_MISMATCH;
    }
    s->effective_packet_size = negotiated;

    // Optional features negotiation (required must be subset)
    s->peer_features = peer_h->features;
    uint32_t negotiable = val__negotiable_mask();
    uint32_t local_required = s->config->features.required & negotiable;
    uint32_t missing_on_peer = local_required & ~peer_h->features;
    if (missing_on_peer)
    {
        VAL_SET_FEATURE_ERROR(s, missing_on_peer);
        (void)val_internal_send_error(s, VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE(missing_on_peer));
        return VAL_ERR_FEATURE_NEGOTIATION;
    }

    // Adaptive TX negotiation (window rung + streaming flags)
    val_tx_mode_t negotiated_cap = val_negotiated_tx_cap(&s->cfg, peer_h);
    s->min_negotiated_mode = negotiated_cap;       // largest shared window rung
    s->max_negotiated_mode = VAL_TX_STOP_AND_WAIT; // always supported

    // Streaming permissions
#if VAL_ENABLE_STREAMING
    bool peer_can_stream = (peer_h->streaming_flags & VAL_STREAM_CAN_SEND) ? true : false;
    bool peer_accepts_stream = (peer_h->streaming_flags & VAL_STREAM_ACCEPT) ? true : false;
    bool local_allow = (s->cfg.adaptive_tx.allow_streaming ? true : false);
    s->send_streaming_allowed = (local_allow && peer_accepts_stream);
    s->recv_streaming_allowed = (local_allow && peer_can_stream);
    VAL_LOG_INFOF(s,
                  "handshake: stream_flags=0x%02X local_allow=%u peer_can=%u peer_accept=%u send_allowed=%u recv_allowed=%u",
                  (unsigned)peer_h->streaming_flags, (unsigned)(local_allow?1:0),
                  (unsigned)(peer_can_stream?1:0), (unsigned)(peer_accepts_stream?1:0),
                  (unsigned)(s->send_streaming_allowed?1:0), (unsigned)(s->recv_streaming_allowed?1:0));
#else
    s->send_streaming_allowed = false;
    s->recv_streaming_allowed = false;
#endif

    // Initial mode: conservative selection using shared helper
    val_tx_mode_t init_mode = val_select_initial_mode(s->cfg.adaptive_tx.preferred_initial_mode,
                                                     (val_tx_mode_t)peer_h->preferred_initial_mode,
                                                     negotiated_cap);
    s->current_tx_mode = init_mode;
    s->peer_tx_mode = s->current_tx_mode;
    return VAL_OK;
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
    val__fill_local_hello(s, s->config->buffers.packet_size, &hello);
    uint8_t hello_wire[VAL_WIRE_HANDSHAKE_SIZE];
    val_serialize_handshake(&hello, hello_wire);
    VAL_LOG_TRACE(s, "handshake(sender): sending HELLO");
    val_status_t st = val_internal_send_packet(s, VAL_PKT_HELLO, hello_wire, VAL_WIRE_HANDSHAKE_SIZE, 0);
    if (st != VAL_OK)
        return st;
    // Wait for peer HELLO using centralized control wait with retry-on-timeout
    uint32_t to = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
    uint8_t tries = s->config->retries.handshake_retries ? s->config->retries.handshake_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint8_t peer_wire[VAL_WIRE_HANDSHAKE_SIZE];
    uint32_t plen = 0; uint64_t poff = 0; val_packet_type_t pt = 0;
    val_retry_payload_ctx_t ctx = { hello_wire, VAL_WIRE_HANDSHAKE_SIZE };
    st = val_internal_wait_control(s, to, tries, backoff, peer_wire, (uint32_t)sizeof(peer_wire), &pt, &plen, &poff,
                                   accept_hello_cb, retry_send_hello_cb, &ctx);
    if (st != VAL_OK)
    {
        if (st == VAL_ERR_TIMEOUT)
            VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_HELLO);
        return st;
    }
    val_handshake_t peer_h;
    val_deserialize_handshake(peer_wire, &peer_h);
    // Adopt peer and finalize negotiation
    val_status_t adopt = val__adopt_peer_hello(s, &peer_h);
    if (adopt != VAL_OK)
        return adopt;
    // Optionally adjust behavior based on requested ∧ peer.features later
    s->handshake_done = true;
#if VAL_ENABLE_METRICS
    s->metrics.handshakes++;
#endif
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
    // Receive sender HELLO using centralized control-wait; receiver does not resend on timeout
    uint32_t to = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
    uint8_t tries = s->config->retries.handshake_retries ? s->config->retries.handshake_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint8_t peer_wire[VAL_WIRE_HANDSHAKE_SIZE];
    uint32_t plen = 0; uint64_t poff = 0; val_packet_type_t pt = 0;
    val_status_t st = val_internal_wait_control(s, to, tries, backoff, peer_wire, (uint32_t)sizeof(peer_wire), &pt, &plen, &poff,
                                                accept_hello_rx_cb, NULL, NULL);
    if (st != VAL_OK)
    {
        if (st == VAL_ERR_TIMEOUT)
            VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_HELLO);
        return st;
    }
    val_handshake_t peer_h;
    val_deserialize_handshake(peer_wire, &peer_h);
    if (peer_h.magic != VAL_MAGIC)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    if (peer_h.version_major != VAL_VERSION_MAJOR)
    {
        val_internal_set_error_detailed(s, VAL_ERR_INCOMPATIBLE_VERSION, VAL_ERROR_DETAIL_VERSION);
        return VAL_ERR_INCOMPATIBLE_VERSION;
    }
    // Adopt peer first to set MTU, features, and modes
    val_status_t adopt = val__adopt_peer_hello(s, &peer_h);
    if (adopt != VAL_OK)
        return adopt;

    // Send our hello back
    val_handshake_t hello;
    val__fill_local_hello(s, s->effective_packet_size, &hello);
    s->peer_features = peer_h.features;
    // Enforce peer's required optional features: must be subset of our advertised optional features
    uint32_t missing_local = (peer_h.required & val__negotiable_mask()) & ~hello.features;
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
    s->handshake_done = true;
#if VAL_ENABLE_METRICS
        s->metrics.handshakes++;
#endif
        // Negotiation already applied in adopt step
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
    s->streaming_engaged = false; // drop back to non-streaming pacing on any error

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
    ms.consecutive_successes = s->consecutive_successes;
    // Set streaming flag only when streaming overlay is enabled
#if VAL_ENABLE_STREAMING
    ms.flags = s->streaming_engaged ? 1u : 0u;
#else
    ms.flags = 0u;
#endif
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
            ms.consecutive_successes = s->consecutive_successes;
            ms.flags = s->streaming_engaged ? 1u : 0u;
            uint8_t ms_wire[VAL_WIRE_MODE_SYNC_SIZE];
            val_serialize_mode_sync(&ms, ms_wire);
            (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC, ms_wire, VAL_WIRE_MODE_SYNC_SIZE, 0);
            // Preserve the triggering success so downstream logic (e.g., streaming engage) can act immediately.
            // Previously this was reset to 0 on every mode change, which delayed streaming engagement by one extra ACK.
            s->consecutive_successes = 1;
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
    // Escalate to streaming pacing after sustained successes when allowed.
#if VAL_ENABLE_STREAMING
    if (s->send_streaming_allowed && s->cfg.adaptive_tx.allow_streaming)
    {
        if (!s->streaming_engaged)
        {
            uint16_t stream_threshold = s->cfg.adaptive_tx.recovery_success_threshold ? s->cfg.adaptive_tx.recovery_success_threshold : 10;
            if (s->consecutive_successes >= stream_threshold)
            {
                s->streaming_engaged = true;
                // Notify peer via mode sync with streaming flag set
                val_mode_sync_t ms2 = {0};
                ms2.current_mode = (uint32_t)s->current_tx_mode;
                ms2.sequence = ++s->mode_sync_sequence;
                ms2.consecutive_errors = s->consecutive_errors;
                ms2.consecutive_successes = s->consecutive_successes;
                ms2.flags = 1u; // streaming engaged
                uint8_t ms_wire2[VAL_WIRE_MODE_SYNC_SIZE];
                val_serialize_mode_sync(&ms2, ms_wire2);
                (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC, ms_wire2, VAL_WIRE_MODE_SYNC_SIZE, 0);
                VAL_LOG_INFO(s, "adaptive: engaging streaming pacing after sustained successes");
                s->consecutive_successes = 0; // reset after escalation
            }
        }
    }
#endif
}

#if VAL_ENABLE_METRICS
#include <string.h>
val_status_t val_get_metrics(val_session_t *session, val_metrics_t *out)
{
    if (!session || !out)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    *out = session->metrics;
    val_internal_unlock(session);
    return VAL_OK;
}

val_status_t val_reset_metrics(val_session_t *session)
{
    if (!session)
        return VAL_ERR_INVALID_ARG;
    val_internal_lock(session);
    memset(&session->metrics, 0, sizeof(session->metrics));
    val_internal_unlock(session);
    return VAL_OK;
}
#endif // VAL_ENABLE_METRICS
