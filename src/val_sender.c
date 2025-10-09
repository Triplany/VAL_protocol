#include "val_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static val_status_t get_file_size_and_name(val_session_t *s, const char *filepath, uint64_t *out_size, char *out_filename)
{
    // Open file to determine size

    void *f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!f)
    {
        VAL_LOG_ERROR(s, "fopen failed");
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_FILE_NOT_FOUND);
        return VAL_ERR_IO;
    }
    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, 0, SEEK_END) != 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        VAL_LOG_ERROR(s, "fseek end failed");
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    int64_t sz = s->config->filesystem.ftell(s->config->filesystem.fs_context, f);
    if (sz < 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        VAL_LOG_ERROR(s, "ftell failed");
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, 0, SEEK_SET) != 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        VAL_LOG_ERROR(s, "fseek set failed");
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    *out_size = (uint64_t)sz;
    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);

    // Derive filename from filepath (simple extraction of basename)
    const char *base = filepath;
    for (const char *p = filepath; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    char cleaned[VAL_MAX_FILENAME + 1];
    val_clean_filename(base, cleaned, sizeof(cleaned));
    snprintf(out_filename, VAL_MAX_FILENAME + 1, "%s", cleaned);
    return VAL_OK;
}

static val_status_t send_metadata(val_session_t *s, const char *sender_path, uint64_t file_size,
                                  const char *filename)
{
    val_meta_payload_t meta;
    memset(&meta, 0, sizeof(meta));
    if (filename)
    {
        snprintf(meta.filename, VAL_MAX_FILENAME + 1, "%s", filename);
    }
    if (sender_path)
    {
        char cleaned[VAL_MAX_PATH + 1];
        val_clean_path(sender_path, cleaned, sizeof(cleaned));
        snprintf(meta.sender_path, VAL_MAX_PATH + 1, "%s", cleaned);
    }
    else
    {
        meta.sender_path[0] = '\0';
    }
    meta.file_size = file_size;
    uint8_t meta_wire[VAL_WIRE_META_SIZE];
    val_serialize_meta(&meta, meta_wire);
    return val_internal_send_packet(s, VAL_PKT_SEND_META, meta_wire, VAL_WIRE_META_SIZE, 0);
}
static val_status_t compute_crc_region(val_session_t *s, const char *filepath, uint64_t end_offset, uint64_t length,
                                       uint32_t *out_crc)
{
    if (!s || !filepath || !out_crc || length == 0 || end_offset < length)
        return VAL_ERR_INVALID_ARG;

    void *f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!f)
    {
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }

    uint64_t start = end_offset - length;
    uint32_t crc_tmp = 0;
    val_status_t rst = val_internal_crc32_region(s, f, start, length, &crc_tmp);
    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
    if (rst != VAL_OK)
        return rst;
    *out_crc = crc_tmp;
    return VAL_OK;
}

typedef struct val_send_progress_ctx
{
    uint32_t total_files;
    uint32_t files_sent;
    uint64_t batch_total_bytes;
    uint64_t batch_sent_bytes;
    uint64_t current_file_bytes;
    uint64_t current_file_sent;
    uint32_t start_ms;
    uint32_t last_emit_tick;
} val_send_progress_ctx_t;

typedef struct val_sender_io_ctx_s
{
    val_session_t *session;
    void *file_handle;
    uint8_t *payload_area;
    uint32_t max_payload;
    uint64_t file_size;
    uint64_t file_cursor; // tracked current file position to minimize ftell/fseek
} val_sender_io_ctx_t;

typedef struct val_sender_ack_ctx_s
{
    val_session_t *session;
    void *file_handle;
    const char *filename;
    val_send_progress_ctx_t *progress_ctx;
    uint64_t file_size;
    uint64_t max_payload;
    uint64_t target_ack;
    uint64_t *window_start;
    uint64_t *last_acked;
    uint64_t *next_to_send;
    uint32_t *inflight;
    uint32_t *window_size;
    int *first_ack_grace_flag;
    uint32_t to_ack;
    uint32_t to_ack_base;
    uint8_t tries;
    uint8_t tries_initial;
    uint32_t backoff;
    uint32_t backoff_initial;
    uint32_t wait_deadline;
    uint32_t t0;
    uint64_t *file_cursor_ptr; // keep sender's local cursor in sync on rewinds
} val_sender_ack_ctx_t;

static val_status_t send_data_packet(val_sender_io_ctx_t *io_ctx, uint64_t *next_to_send, uint32_t *inflight);
static int handle_nak_retransmit(val_session_t *s, void *file_handle, uint64_t file_size, uint64_t *last_acked,
                                 uint64_t *next_to_send, uint32_t *inflight, const uint8_t *payload,
                                 uint32_t payload_len);
static val_status_t wait_for_window_ack(val_sender_ack_ctx_t *ack_ctx, int *restart_window);
static void val_emit_progress_sender(val_session_t *s, val_send_progress_ctx_t *ctx, const char *filename,
                                     uint64_t bytes_sent, int force_emit);
static val_status_t request_resume_and_get_response(val_session_t *s, const char *filepath,
                                                    uint64_t *resume_offset_out);

static val_status_t send_data_packet(val_sender_io_ctx_t *io_ctx, uint64_t *next_to_send, uint32_t *inflight)
{
    if (!io_ctx || !io_ctx->session || !io_ctx->file_handle || !io_ctx->payload_area || !next_to_send || !inflight)
        return VAL_ERR_INVALID_ARG;
    val_session_t *s = io_ctx->session;
    if (!s->config || !s->config->filesystem.fread)
        return VAL_ERR_INVALID_ARG;
    if (io_ctx->max_payload == 0)
        return VAL_ERR_INVALID_ARG;
    if (*next_to_send >= io_ctx->file_size)
        return VAL_OK;

    uint64_t remaining = io_ctx->file_size - *next_to_send;
    size_t to_read = (size_t)((remaining < (uint64_t)io_ctx->max_payload) ? remaining : (uint64_t)io_ctx->max_payload);
    if (to_read == 0)
        return VAL_OK;

    // Assume sequential IO; only seek if our tracked position differs
    // Use tracked cursor to avoid redundant ftell/fseek; only seek if needed
    if (io_ctx->file_cursor != *next_to_send)
    {
        (void)s->config->filesystem.fseek(s->config->filesystem.fs_context, io_ctx->file_handle, (int64_t)(*next_to_send), SEEK_SET);
        io_ctx->file_cursor = *next_to_send;
    }

    size_t have = 0;
    while (have < to_read)
    {
        size_t r = s->config->filesystem.fread(s->config->filesystem.fs_context, io_ctx->payload_area + have, 1,
                                               to_read - have, io_ctx->file_handle);
        if (r == 0)
            break;
        have += r;
    }
    if (have != to_read)
        return VAL_ERR_IO;

    val_status_t st = val_internal_send_packet(s, VAL_PKT_DATA, io_ctx->payload_area, (uint32_t)to_read, *next_to_send);
    if (st != VAL_OK)
        return st;

    *next_to_send += to_read;
    io_ctx->file_cursor += to_read;
    ++(*inflight);
    // Wire audit removed
    return VAL_OK;
}

static int handle_nak_retransmit(val_session_t *s, void *file_handle, uint64_t file_size, uint64_t *last_acked,
                                 uint64_t *next_to_send, uint32_t *inflight, const uint8_t *payload,
                                 uint32_t payload_len)
{
    if (!s || !file_handle || !last_acked || !next_to_send || !inflight)
        return 0;

    if (payload && payload_len >= 12)
    {
    uint64_t nak_next = VAL_GET_LE64(payload);
    uint32_t reason = VAL_GET_LE32(payload + 8);
        VAL_LOG_DEBUGF(s, "data(win): got DATA_NAK next=%llu reason=0x%08X (last_acked=%llu next_to_send=%llu)",
                       (unsigned long long)nak_next, (unsigned)reason, (unsigned long long)(*last_acked),
                       (unsigned long long)(*next_to_send));
        if (nak_next > *last_acked && nak_next <= file_size)
        {
            uint64_t prev = *last_acked;
            *last_acked = nak_next;
            VAL_LOG_DEBUGF(s, "data(win): advance on NAK prev=%llu -> last_acked=%llu", (unsigned long long)prev,
                           (unsigned long long)(*last_acked));
        }
    }

    val_internal_record_transmission_error(s);
    val_metrics_inc_retrans(s);
    s->timing.in_retransmit = 1;

    long target_l = (long)(*last_acked);
    int64_t curpos = s->config->filesystem.ftell(s->config->filesystem.fs_context, file_handle);
    if (curpos < 0 || (uint64_t)curpos != *last_acked)
        (void)s->config->filesystem.fseek(s->config->filesystem.fs_context, file_handle, (int64_t)target_l, SEEK_SET);

    *inflight = 0;
    *next_to_send = *last_acked;
    return 1;
}

static void val_emit_progress_sender(val_session_t *s, val_send_progress_ctx_t *ctx, const char *filename,
                                     uint64_t bytes_sent, int force_emit)
{
    if (!s || !s->config || !s->config->callbacks.on_progress)
        return;

    val_progress_info_t info;
    memset(&info, 0, sizeof(info));
    info.current_filename = filename ? filename : "";

    uint32_t now = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;

    if (ctx)
    {
        if (ctx->start_ms == 0 && now)
            ctx->start_ms = now;
        ctx->current_file_sent = bytes_sent;
        info.current_file_bytes = ctx->current_file_sent;
        info.files_completed = ctx->files_sent;
        info.total_files = ctx->total_files;
        info.total_bytes = ctx->batch_total_bytes;
        info.bytes_transferred = ctx->batch_sent_bytes + ctx->current_file_sent;

        uint32_t elapsed_ms = (ctx->start_ms && now >= ctx->start_ms) ? (now - ctx->start_ms) : 0;
        if (elapsed_ms > 0)
        {
            uint64_t bps = (info.bytes_transferred * 1000ull) / (uint64_t)elapsed_ms;
            if (bps > 0xFFFFFFFFull)
                bps = 0xFFFFFFFFull;
            info.transfer_rate_bps = (uint32_t)bps;
            if (info.total_bytes > info.bytes_transferred && info.transfer_rate_bps > 0)
            {
                uint64_t remaining = info.total_bytes - info.bytes_transferred;
                info.eta_seconds = (uint32_t)(remaining / info.transfer_rate_bps);
            }
        }

        if (!force_emit && ctx->last_emit_tick && now && (now - ctx->last_emit_tick) < 50u)
            return;
        ctx->last_emit_tick = now;
    }
    else
    {
        info.current_file_bytes = bytes_sent;
        info.bytes_transferred = bytes_sent;
        info.files_completed = 0;
        info.total_files = 0;
        info.total_bytes = 0;
        info.transfer_rate_bps = 0;
        info.eta_seconds = 0;
    }

    s->config->callbacks.on_progress(&info);
}

static val_status_t wait_for_window_ack(val_sender_ack_ctx_t *ack_ctx, int *restart_window)
{
    if (!ack_ctx || !ack_ctx->session || !restart_window || !ack_ctx->window_start || !ack_ctx->last_acked ||
        !ack_ctx->next_to_send || !ack_ctx->inflight || !ack_ctx->window_size)
        return VAL_ERR_INVALID_ARG;

    val_session_t *s = ack_ctx->session;
    val_packet_type_t t = 0;
    uint32_t len = 0;
    uint64_t off = 0;
    uint8_t ctrl_buf[32];

    *restart_window = 0;

    for (;;)
    {
        if (val_check_for_cancel(s))
            return VAL_ERR_ABORTED;
        // Wait up to the ACK timeout; no protocol-level absolute/no-progress caps
        uint32_t wait_ms = ack_ctx->to_ack;
        uint32_t now_wait = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
        uint32_t deadline = now_wait + (wait_ms ? wait_ms : 1u);
        val_status_t st = val_internal_recv_until_deadline(s, &t, ctrl_buf, (uint32_t)sizeof(ctrl_buf), &len, &off, deadline, 5u);
        if (st == VAL_OK)
        {
            if (t == VAL_PKT_CANCEL)
                return VAL_ERR_ABORTED;

            if (t == VAL_PKT_DATA_NAK)
            {
                if (handle_nak_retransmit(s, ack_ctx->file_handle, ack_ctx->file_size, ack_ctx->last_acked,
                                          ack_ctx->next_to_send, ack_ctx->inflight, ctrl_buf, len))
                {
                    if (ack_ctx->file_cursor_ptr)
                        *ack_ctx->file_cursor_ptr = *ack_ctx->last_acked;
                    // Update window size from session's current setting
                    *ack_ctx->window_size = s->current_window_packets ? s->current_window_packets : 1u;
                    if (*ack_ctx->window_size == 0u)
                        *ack_ctx->window_size = 1u;
                    *restart_window = 1;
                    return VAL_OK;
                }
                continue;
            }

            if (t == VAL_PKT_DATA_ACK)
            {
                // Treat any DATA_ACK (even stale) as a keepalive to extend streaming deadlines
                if (s->config->system.get_ticks_ms)
                    s->last_keepalive_recv_time = s->config->system.get_ticks_ms();
                if (ack_ctx->t0 && !s->timing.in_retransmit)
                {
                    uint32_t now = s->config->system.get_ticks_ms();
                    val_internal_record_rtt(s, now - ack_ctx->t0);
                }
                if (ack_ctx->first_ack_grace_flag && *ack_ctx->first_ack_grace_flag)
                    *ack_ctx->first_ack_grace_flag = 0;

                if (off <= *ack_ctx->last_acked)
                {
                    VAL_LOG_DEBUGF(s, "data(win): ignoring stale DATA_ACK off=%llu (<= last_acked=%llu)",
                                   (unsigned long long)off, (unsigned long long)(*ack_ctx->last_acked));
                    continue;
                }

                // Record success only when ACK advances our high-water mark (off > last_acked)
                if (!s->timing.in_retransmit)
                    val_internal_record_transmission_success(s);
                *ack_ctx->last_acked = off;
                // Reset soft trip counter on real forward progress
                s->health.soft_trips = 0;
                if (ack_ctx->progress_ctx)
                    val_emit_progress_sender(s, ack_ctx->progress_ctx, ack_ctx->filename, *ack_ctx->last_acked, 1);
                else
                    val_emit_progress_sender(s, NULL, ack_ctx->filename, *ack_ctx->last_acked, 1);

                if (val_check_for_cancel(s))
                    return VAL_ERR_ABORTED;

                uint64_t outstanding = (*ack_ctx->next_to_send > *ack_ctx->last_acked)
                                           ? (*ack_ctx->next_to_send - *ack_ctx->last_acked)
                                           : 0;
                *ack_ctx->inflight = (uint32_t)((outstanding + ack_ctx->max_payload - 1) / ack_ctx->max_payload);
                // Wire audit removed

                ack_ctx->wait_deadline = s->config->system.get_ticks_ms() + ack_ctx->to_ack_base;
                ack_ctx->tries = ack_ctx->tries_initial;
                ack_ctx->backoff = ack_ctx->backoff_initial;
                s->timing.in_retransmit = 0;
                // In bounded-window mode, we always gate on window; no streaming fast-path

                if (*ack_ctx->last_acked < ack_ctx->target_ack)
                    continue;

                return VAL_OK;
            }

            // No MODE_SYNC traffic in bounded-window protocol

            if (t == VAL_PKT_ERROR)
                return VAL_ERR_PROTOCOL;

            continue;
        }

        if ((st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC))
        {
            if (st == VAL_ERR_TIMEOUT)
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
            else if (st == VAL_ERR_CRC)
                VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
            return st;
        }
        // Timeout/CRC path: if we've exhausted retries, return timeout/CRC; otherwise, trigger a bounded-window restart
        if (ack_ctx->tries == 0)
        {
            if (st == VAL_ERR_TIMEOUT)
            {
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                // Terminal ACK wait timeout
                val_metrics_inc_timeout_hard(s);
            }
            else if (st == VAL_ERR_CRC)
                VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
            return st;
        }
        // Interim TIMEOUT/CRC: backoff one try and request a retransmit by restarting the current window
        VAL_HEALTH_RECORD_RETRY(s);
        if (st == VAL_ERR_TIMEOUT)
            val_metrics_inc_timeout_soft(s);
        else if (st == VAL_ERR_CRC)
            val_metrics_inc_crcerr(s);
        // Mark retransmit path and rewind sender state to last_acked
        val_internal_record_transmission_error(s);
        val_metrics_inc_retrans(s);
        s->timing.in_retransmit = 1;
        // Ensure underlying file position is rewound to last_acked so resend reads correct bytes
        if (ack_ctx->file_handle && s->config && s->config->filesystem.ftell && s->config->filesystem.fseek)
        {
            int64_t pos = s->config->filesystem.ftell(s->config->filesystem.fs_context, ack_ctx->file_handle);
            if (pos < 0 || (uint64_t)pos != *ack_ctx->last_acked)
            {
                (void)s->config->filesystem.fseek(s->config->filesystem.fs_context, ack_ctx->file_handle,
                                                  (int64_t)(*ack_ctx->last_acked), SEEK_SET);
            }
        }
        if (ack_ctx->file_cursor_ptr)
            *ack_ctx->file_cursor_ptr = *ack_ctx->last_acked;
        *ack_ctx->inflight = 0;
        *ack_ctx->next_to_send = *ack_ctx->last_acked;
        // Update window size from session (may have been adapted)
        *ack_ctx->window_size = s->current_window_packets ? s->current_window_packets : 1u;
        if (*ack_ctx->window_size == 0u)
            *ack_ctx->window_size = 1u;
        // One try consumed; reset backoff boundedly and request restart
        if (ack_ctx->tries)
            --ack_ctx->tries;
        ack_ctx->backoff = 0;
        *restart_window = 1;
        return VAL_OK;
    }
}

// Sender-side: request resume policy and finalize resume offset
static val_status_t request_resume_and_get_response(val_session_t *s, const char *filepath,
                                                    uint64_t *resume_offset_out)
{
    if (!s || !filepath || !resume_offset_out)
        return VAL_ERR_INVALID_ARG;

    // Best-effort send RESUME_REQ immediately (receiver may also proactively send RESP after META)
    (void)val_internal_send_packet(s, VAL_PKT_RESUME_REQ, NULL, 0, 0);

    uint32_t to = val_internal_get_timeout(s, VAL_OP_META);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    val_status_t st = VAL_OK;
    for (;;)
    {
        VAL_HEALTH_RECORD_OPERATION(s);
        val_status_t health = val_internal_check_health(s);
        if (health != VAL_OK)
            return health;
            
    uint8_t buf[128];
    uint32_t len = 0;
    uint64_t off = 0;
        uint32_t wait_ms = to;
        // Use centralized RESUME_RESP wait helper which ignores benign traffic and resends on timeout
        st = val_internal_wait_resume_resp(s, wait_ms, tries, backoff, buf, (uint32_t)sizeof(buf), &len, &off);
        if (st == VAL_OK)
        {

            if (len < VAL_WIRE_RESUME_RESP_SIZE)
            {
                VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
                return VAL_ERR_PROTOCOL;
            }

            val_resume_resp_t rr;
            val_deserialize_resume_resp(buf, &rr);

            uint32_t action = rr.action;
            if (action == VAL_RESUME_ACTION_SKIP_FILE)
            {
                *resume_offset_out = UINT64_MAX; // sentinel to skip
                return VAL_OK;
            }
            if (action == VAL_RESUME_ACTION_ABORT_FILE)
            {
                return VAL_ERR_ABORTED;
            }
            if (action == VAL_RESUME_ACTION_START_ZERO)
            {
                *resume_offset_out = 0;
                return VAL_OK;
            }
            if (action == VAL_RESUME_ACTION_START_OFFSET)
            {
                *resume_offset_out = rr.resume_offset;
                return VAL_OK;
            }
            if (action == VAL_RESUME_ACTION_VERIFY_FIRST)
            {
                // Compute CRC over [resume_offset - verify_length, resume_offset)
                uint64_t end_off = rr.resume_offset;
                uint64_t vlen = rr.verify_length;
                uint32_t my_crc = 0;
                val_status_t cst = compute_crc_region(s, filepath, end_off, vlen, &my_crc);
                if (cst != VAL_OK)
                    return cst;
                // Reply with VERIFY containing our CRC; reuse val_resume_resp_t payload format (receiver parses verify_crc)
                val_resume_resp_t v;
                v.action = 0;
                v.resume_offset = end_off;
                v.verify_crc = my_crc;
                v.verify_length = vlen;
                uint8_t verify_wire[VAL_WIRE_RESUME_RESP_SIZE];
                val_serialize_resume_resp(&v, verify_wire);
                st = val_internal_send_packet(s, VAL_PKT_VERIFY, verify_wire, VAL_WIRE_RESUME_RESP_SIZE, 0);
                if (st != VAL_OK)
                    return st;

                // Wait for receiver's VERIFY result using centralized helper
                st = val_internal_wait_verify_result(s, verify_wire, VAL_WIRE_RESUME_RESP_SIZE, end_off, resume_offset_out);
                return st;
            }

            // Unknown action
            VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_INVALID_STATE);
            return VAL_ERR_PROTOCOL;
        }

        // TIMEOUT/CRC and retry behavior handled within the resume wait wrapper; on non-OK return, just propagate
        return st;
    }
}

// Adaptive controller entrypoint - routes to appropriate sender based on negotiated mode
static val_status_t send_file_data_adaptive(val_session_t *s, const char *filepath, const char *sender_path, void *progress_ctx)
{
    // Start with current bounded window; fallback to negotiated or 1
    int mode_used_dummy = 0; // legacy placeholder removed
    uint32_t win = s->current_window_packets ? s->current_window_packets : (s->negotiated_window_packets ? s->negotiated_window_packets : 1u);
    // Use a simplified Go-Back-N cumulative ACK approach
    // Gather meta
    uint64_t size = 0;
    char filename[VAL_MAX_FILENAME + 1];
    VAL_LOG_INFOF(s, "send_file(win=%u): begin filepath='%s'", (unsigned)win, filepath ? filepath : "<null>");
    val_status_t st = get_file_size_and_name(s, filepath, &size, filename);
    if (st != VAL_OK)
        return st;
    const char *reported_path = (sender_path && sender_path[0]) ? sender_path : filepath;
    // Send metadata
    st = send_metadata(s, reported_path, size, filename);
    if (st != VAL_OK)
        return st;
    // Resume negotiation
    uint64_t resume_off = 0;
    st = request_resume_and_get_response(s, filepath, &resume_off);
    if (st != VAL_OK)
        return st;
    // If receiver requested to skip this file entirely, resume_off is a sentinel (UINT64_MAX)
    if (resume_off == UINT64_MAX)
    {
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, reported_path ? reported_path : "", size, size);
        // Send DONE and wait for DONE_ACK without sending any data
        st = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
        if (st != VAL_OK)
            return st;
        st = val_internal_wait_done_ack(s, size);
        if (st != VAL_OK)
        {
            if (s->config->callbacks.on_file_complete)
                s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
            return st;
        }
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_SKIPPED);
        val_metrics_inc_files_sent(s);
        return VAL_OK;
    }
    // Open file
    void *f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!f)
        return VAL_ERR_IO;
    if (resume_off && s->config->filesystem.fseek(s->config->filesystem.fs_context, f, (int64_t)resume_off, SEEK_SET) != 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        return VAL_ERR_IO;
    }
    if (s->config->callbacks.on_file_start)
        s->config->callbacks.on_file_start(filename, reported_path ? reported_path : "", size, resume_off);
    if (progress_ctx)
        val_emit_progress_sender(s, (val_send_progress_ctx_t *)progress_ctx, filename, resume_off, 1);
    else
        val_emit_progress_sender(s, NULL, filename, resume_off, 1);
    if (val_check_for_cancel(s))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
        return VAL_ERR_ABORTED;
    }
    // Prepare windowed send
    size_t mtu_bytes = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size;
    if (mtu_bytes <= (VAL_WIRE_HEADER_SIZE + VAL_WIRE_TRAILER_SIZE))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        return VAL_ERR_INVALID_ARG;
    }
    uint8_t *send_buf_bytes = (uint8_t *)s->config->buffers.send_buffer;
    uint8_t *payload_area = send_buf_bytes ? (send_buf_bytes + VAL_WIRE_HEADER_SIZE) : NULL;
    size_t max_payload = (size_t)(mtu_bytes - VAL_WIRE_HEADER_SIZE - VAL_WIRE_TRAILER_SIZE);
    uint64_t last_acked = resume_off;
    uint64_t next_to_send = resume_off;
    uint32_t inflight = 0;
    // If we resumed mid-file, the receiver may need to re-read existing bytes to seed its CRC before
    // it starts pulling network data. Give the very first DATA_ACK after resume extra time.
    int first_ack_grace = (resume_off > 0) ? 1 : 0;
    // Compute additional retries budget for the very first DATA_ACK on large resumes.
    // Heuristic: +1 retry per ~256 MiB of resume offset, capped at +6.
    uint8_t first_ack_extra_tries = 0;
    if (resume_off > 0)
    {
        uint64_t blocks = resume_off / (256ull * 1024ull * 1024ull);
        if (blocks > 6) blocks = 6;
        first_ack_extra_tries = (uint8_t)blocks;
    }
    if (!payload_area)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        return VAL_ERR_INVALID_ARG;
    }
    val_sender_io_ctx_t io_ctx = {s, f, payload_area, max_payload, size, resume_off};
    while (last_acked < size)
    {
        if (val_check_for_cancel(s))
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            if (s->config->callbacks.on_file_complete)
                s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
            VAL_LOG_WARN(s, "data(win): local cancel detected at loop top");
            return VAL_ERR_ABORTED;
        }
        
        // Check connection health (graceful failure on extreme conditions)
        VAL_HEALTH_RECORD_OPERATION(s);
        val_status_t health = val_internal_check_health(s);
        if (health != VAL_OK)
        {
            if (health == VAL_ERR_PERFORMANCE)
            {
                // Treat health performance trip as a soft error during active data send:
                // trigger adaptive de-escalation and keep going instead of aborting the session.
                VAL_LOG_WARN(s, "health: soft performance trip during data send -> de-escalate and continue");
                val_internal_record_transmission_error(s);
                // Track soft trip and, if too many occur without progress, escalate to hard failure
                s->health.soft_trips++;
                if (s->health.soft_trips >= 2)
                {
                    VAL_LOG_CRIT(s, "health: repeated soft performance trips -> escalating to hard failure");
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_PERFORMANCE);
                    VAL_SET_PERFORMANCE_ERROR(s, VAL_ERROR_DETAIL_EXCESSIVE_RETRIES);
                    return VAL_ERR_PERFORMANCE;
                }
                // Continue without closing the file; allow outer loop to refill at a smaller window.
            }
            else
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", health);
                return health;
            }
        }
        
        // Establish a window and persist retry/backoff state across restarts until target ACK is reached
        uint64_t window_start = last_acked;
    uint32_t to_ack_base = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
    uint32_t ack_timeout_ms = to_ack_base;
        if (first_ack_grace && last_acked == resume_off)
        {
            uint32_t ext = s->config->timeouts.max_timeout_ms ? s->config->timeouts.max_timeout_ms : to_ack_base;
            if (ext > ack_timeout_ms)
                ack_timeout_ms = ext;
            VAL_LOG_INFO(s, "data: extended first DATA_ACK wait after resume");
        }
        uint8_t tries_rem = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
        if (first_ack_grace && last_acked == resume_off)
            tries_rem = (uint8_t)(tries_rem + first_ack_extra_tries);
    uint32_t backoff_ms = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
        // Loop until we advance to target ACK without exhausting retries
        for (;;)
        {
            // Absolute and no-progress watchdogs inside inner loop as well
            // Fill window bounded by current window size
            while (inflight < win && next_to_send < size)
            {
                if (val_check_for_cancel(s))
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                    VAL_LOG_WARN(s, "data(win): local cancel during fill-window");
                    return VAL_ERR_ABORTED;
                }
                val_status_t send_status = send_data_packet(&io_ctx, &next_to_send, &inflight);
                if (send_status != VAL_OK)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    return send_status;
                }
            }

            // No streaming fast-path; always proceed to ACK wait

            const uint64_t target_ack = next_to_send;
            uint32_t t0 = s->config->system.get_ticks_ms();
            uint32_t wait_deadline = s->config->system.get_ticks_ms() + ack_timeout_ms;
            s->timing.in_retransmit = 0;

            val_sender_ack_ctx_t ack_ctx = {
                .session = s,
                .file_handle = f,
                .filename = filename,
                .progress_ctx = progress_ctx ? (val_send_progress_ctx_t *)progress_ctx : NULL,
                .file_size = size,
                .max_payload = max_payload,
                .target_ack = target_ack,
                .window_start = &window_start,
                .last_acked = &last_acked,
                .next_to_send = &next_to_send,
                .inflight = &inflight,
                .window_size = &win,
                .first_ack_grace_flag = &first_ack_grace,
                .to_ack = ack_timeout_ms,
                .to_ack_base = to_ack_base,
                .tries = tries_rem,
                .tries_initial = (uint8_t)(s->config->retries.ack_retries ? s->config->retries.ack_retries : 0),
                .backoff = backoff_ms,
                .backoff_initial = (s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0),
                .wait_deadline = wait_deadline,
                .t0 = t0,
                .file_cursor_ptr = &io_ctx.file_cursor
            };

            int restart_window = 0;
            uint64_t prev_last_acked = last_acked;

            val_status_t ack_status = wait_for_window_ack(&ack_ctx, &restart_window);
            if (ack_status != VAL_OK)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (ack_status == VAL_ERR_ABORTED)
                {
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                }
                else if (s->config->callbacks.on_file_complete)
                {
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", ack_status);
                }
                return ack_status;
            }

            // Reset no-progress window on real forward progress
            (void)prev_last_acked; // progress tracking retained via last_acked and callbacks

            // Persist remaining retries/backoff across restarts in this window
            tries_rem = ack_ctx.tries;
            backoff_ms = ack_ctx.backoff;

            if (restart_window)
            {
                // Refresh win from session and continue
                win = s->current_window_packets ? s->current_window_packets : 1u;
                // Refill and continue waiting within this window using remaining tries
                continue;
            }
            // Window progressed to or past target_ack; break to outer while if file not done
            break;
        }
    }
    // DONE/DONE_ACK
    st = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
    if (st != VAL_OK)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
        return st;
    }
    {
        // Use centralized DONE_ACK wait helper
        st = val_internal_wait_done_ack(s, size);
        if (st != VAL_OK)
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            if (s->config->callbacks.on_file_complete)
                s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
            return st;
        }
    }
    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
    if (s->config->callbacks.on_file_complete)
        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_OK);
    val_metrics_inc_files_sent(s);
    // Successful file completion counts as a success for adaptive mode tracking
    val_internal_record_transmission_success(s);
    return VAL_OK;
}

val_status_t val_internal_send_file(val_session_t *s, const char *filepath, const char *sender_path, void *progress_ctx)
{
    // Delegate to adaptive controller (which currently uses stop-and-wait path)
    return send_file_data_adaptive(s, filepath, sender_path, progress_ctx);
}

// --- Legacy stop-and-wait implementation moved behind a helper ---
val_status_t val_send_files(val_session_t *s, const char *const *filepaths, size_t file_count, const char *sender_path)
{
    if (!s || !filepaths || file_count == 0)
    {
        VAL_LOG_ERROR(s, "val_send_files: invalid args");
        return VAL_ERR_INVALID_ARG;
    }
    // Coarse-grained session lock to keep send operations thread-safe per session
    val_internal_lock(s);
    // Validate local features before any handshake; requested gets sanitized, required must be supported locally
    extern val_status_t val_internal_do_handshake_sender(val_session_t * s);
    val_status_t hs = val_internal_do_handshake_sender(s);
    if (hs != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "handshake (sender) failed %d", (int)hs);
    val_internal_unlock(s);
        return hs;
    }
    // Prepare batch progress context locally (no additional persistent RAM in session)
    val_send_progress_ctx_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.total_files = (uint32_t)file_count;
    // Pre-scan sizes to compute total_bytes; if any file missing size, we best-effort compute
    for (size_t i = 0; i < file_count; ++i)
    {
        uint64_t sz = 0;
        char tmpname[VAL_MAX_FILENAME + 1];
        if (get_file_size_and_name(s, filepaths[i], &sz, tmpname) == VAL_OK)
            prog.batch_total_bytes += sz;
    }
    prog.start_ms = s->config->system.get_ticks_ms();
    for (size_t i = 0; i < file_count; ++i)
    {
        VAL_LOG_INFOF(s, "send_files: sending [%u/%u] '%s'", (unsigned)(i + 1), (unsigned)file_count,
                      filepaths[i] ? filepaths[i] : "<null>");
        val_status_t st = val_internal_send_file(s, filepaths[i], sender_path, &prog);
        VAL_LOG_INFOF(s, "send_files: result for '%s' = %d", filepaths[i] ? filepaths[i] : "<null>", (int)st);
        if (st != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "send_file failed %d", (int)st);
            val_internal_unlock(s);
            return st;
        }
    }
    // Send EOT and wait for ACK automatically
    val_status_t st = val_internal_send_packet(s, VAL_PKT_EOT, NULL, 0, 0);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "send EOT failed %d", (int)st);
    val_internal_unlock(s);
        return st;
    }
    // Use centralized EOT_ACK wait helper
    st = val_internal_wait_eot_ack(s);
    if (st != VAL_OK)
    {
        val_internal_unlock(s);
        return st;
    }
    val_status_t out = VAL_OK;
    val_internal_unlock(s);
    return out;
}
