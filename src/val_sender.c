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
    long sz = s->config->filesystem.ftell(s->config->filesystem.fs_context, f);
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

static val_status_t send_metadata(val_session_t *s, const char *sender_path, uint64_t file_size, uint32_t file_crc,
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
    meta.file_size = val_htole64(file_size);
    meta.file_crc32 = val_htole32(file_crc);
    return val_internal_send_packet(s, VAL_PKT_SEND_META, &meta, sizeof(meta), 0);
}

static val_status_t compute_crc_region(val_session_t *s, const char *filepath, uint64_t end_offset, uint32_t length,
                                       uint32_t *out_crc)
{
    // Compute CRC over [end_offset - length .. end_offset)
    if (length == 0 || end_offset < length || !out_crc)
    {
        return VAL_ERR_INVALID_ARG;
    }
    void *f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!f)
    {
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    uint64_t start = end_offset - length;
    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, (long)start, SEEK_SET) != 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    uint32_t state = val_internal_crc32_init(s);
    if (!s->config->buffers.recv_buffer)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        return VAL_ERR_INVALID_ARG;
    }
    size_t buf_size = (size_t)(s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size);
    if (buf_size == 0)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        return VAL_ERR_INVALID_ARG;
    }
    uint8_t *buf = (uint8_t *)s->config->buffers.recv_buffer; // reuse buffer
    while (length > 0)
    {
        size_t chunk = (size_t)length;
        if (chunk > buf_size)
            chunk = buf_size;
        int r = s->config->filesystem.fread(s->config->filesystem.fs_context, buf, 1, chunk, f);
        if (r != (int)chunk)
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
            return VAL_ERR_IO;
        }
        state = val_internal_crc32_update(s, state, buf, chunk);
        length -= (uint32_t)chunk;
    }
    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
    *out_crc = val_internal_crc32_final(s, state);
    return VAL_OK;
}

static val_status_t request_resume_and_get_response(val_session_t *s, const char *filepath, uint64_t *resume_offset_out)
{
    val_status_t st = val_internal_send_packet(s, VAL_PKT_RESUME_REQ, NULL, 0, 0);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "resume_req: send failed %d", (int)st);
        return st;
    }
    VAL_LOG_DEBUG(s, "send: sent RESUME_REQ");
    uint8_t tmp[128];
    uint32_t len = 0;
    uint64_t off = 0;
    val_packet_type_t t = 0;
    {
        uint32_t to = val_internal_get_timeout(s, VAL_OP_VERIFY);
        uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
        uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
        uint32_t t0 = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
        s->timing.in_retransmit = 0;
        for (;;)
        {
            if (!val_internal_transport_is_connected(s))
            {
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                return VAL_ERR_IO;
            }
            st = val_internal_recv_packet(s, &t, tmp, sizeof(tmp), &len, &off, to);
            if (st == VAL_OK)
            {
                if (t == VAL_PKT_RESUME_RESP && t0 && s->config->system.get_ticks_ms && !s->timing.in_retransmit)
                {
                    uint32_t now = s->config->system.get_ticks_ms();
                    val_internal_record_rtt(s, now - t0);
                }
                break;
            }
            if (st != VAL_ERR_TIMEOUT || tries == 0)
            {
                VAL_LOG_ERRORF(s, "resume_req: recv failed %d (tries=%u)", (int)st, (unsigned)tries);
                if (st == VAL_ERR_TIMEOUT && tries == 0)
                    VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                return st;
            }
            // Retransmit RESUME_REQ and backoff
            s->timing.in_retransmit = 1; // Karn's algorithm
            val_status_t rs = val_internal_send_packet(s, VAL_PKT_RESUME_REQ, NULL, 0, 0);
            if (rs != VAL_OK)
            {
                VAL_LOG_ERRORF(s, "resume_req: retransmit send failed %d", (int)rs);
                return rs;
            }
            VAL_LOG_DEBUG(s, "resume_req: timeout, retransmitting");
            if (backoff && s->config->system.delay_ms)
                s->config->system.delay_ms(backoff);
            if (backoff)
                backoff <<= 1;
            --tries;
        }
    }
    if (t == VAL_PKT_CANCEL)
    {
        VAL_LOG_WARN(s, "resume_req: received CANCEL");
        return VAL_ERR_ABORTED;
    }
    if (t != VAL_PKT_RESUME_RESP || len < sizeof(val_resume_resp_t))
    {
        VAL_LOG_ERRORF(s, "resume_req: protocol mismatch t=%d len=%u", (int)t, (unsigned)len);
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    val_resume_resp_t resp;
    memcpy(&resp, tmp, sizeof(resp));
    // Convert from LE
    resp.action = val_letoh32(resp.action);
    resp.resume_offset = val_letoh64(resp.resume_offset);
    resp.verify_crc = val_letoh32(resp.verify_crc);
    resp.verify_len = val_letoh32(resp.verify_len);
    VAL_LOG_DEBUG(s, "send: received RESUME_RESP");
    VAL_LOG_INFOF(s, "resume_resp: action=%u offset=%llu verify_len=%u", (unsigned)resp.action,
                  (unsigned long long)resp.resume_offset, (unsigned)resp.verify_len);
    if (resp.action == VAL_RESUME_ACTION_VERIFY_FIRST)
    {
        // Receiver expects us to send our own CRC for the requested region
        uint32_t crc = 0;
        // Guard against malformed verify_len or resume_offset==0
        if (resp.verify_len == 0 || resp.resume_offset < resp.verify_len)
        {
            // Cannot compute a valid CRC region; treat as mismatch and restart
            VAL_LOG_WARN(s, "verify: invalid region from receiver, restarting at 0");
            *resume_offset_out = 0;
            // Inform receiver we cannot verify: send status directly
            int32_t status_le = (int32_t)val_htole32((uint32_t)VAL_ERR_RESUME_VERIFY);
            val_status_t rs = val_internal_send_packet(s, VAL_PKT_VERIFY, &status_le, sizeof(status_le), 0);
            (void)rs; // best-effort
            return VAL_OK;
        }
        st = compute_crc_region(s, filepath, resp.resume_offset, resp.verify_len, &crc);
        if (st != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "verify: compute crc failed %d", (int)st);
            return st;
        }
        VAL_LOG_DEBUG(s, "send: computed verify CRC");
        val_resume_resp_t verify_payload = resp;
        verify_payload.verify_crc = crc;
        // Encode to LE for wire
        val_resume_resp_t verify_le = verify_payload;
        verify_le.action = val_htole32(verify_le.action);
        verify_le.resume_offset = val_htole64(verify_le.resume_offset);
        verify_le.verify_crc = val_htole32(verify_le.verify_crc);
        verify_le.verify_len = val_htole32(verify_le.verify_len);
        st = val_internal_send_packet(s, VAL_PKT_VERIFY, &verify_le, sizeof(verify_le), 0);
        if (st != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "verify: send VERIFY failed %d", (int)st);
            return st;
        }
        VAL_LOG_DEBUG(s, "send: sent VERIFY");
        // Wait for verification result (ACK via VERIFY packet with action field reused as status)
        {
            uint32_t to = val_internal_get_timeout(s, VAL_OP_VERIFY);
            uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
            uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
            uint32_t t0v = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
            s->timing.in_retransmit = 0;
            for (;;)
            {
                if (!val_internal_transport_is_connected(s))
                {
                    VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                    return VAL_ERR_IO;
                }
                if (val_check_for_cancel(s))
                {
                    VAL_LOG_WARN(s, "verify: local cancel detected");
                    return VAL_ERR_ABORTED;
                }
                st = val_internal_recv_packet(s, &t, tmp, sizeof(tmp), &len, &off, to);
                if (st == VAL_OK)
                {
                    // Accept only VERIFY here; ignore duplicates/unexpected packets that may arrive mid-handshake
                    VAL_LOG_DEBUGF(s, "verify: received packet type=%d len=%u", (int)t, (unsigned)len);
                    if (t == VAL_PKT_CANCEL)
                    {
                        VAL_LOG_WARN(s, "verify: received CANCEL");
                        return VAL_ERR_ABORTED;
                    }
                    if (t == VAL_PKT_VERIFY)
                    {
                        if (t0v && s->config->system.get_ticks_ms && !s->timing.in_retransmit)
                        {
                            uint32_t now = s->config->system.get_ticks_ms();
                            val_internal_record_rtt(s, now - t0v);
                        }
                        break; // proceed below
                    }
                    if (t == VAL_PKT_ERROR)
                    {
                        VAL_LOG_ERROR(s, "verify: received ERROR packet");
                        return VAL_ERR_PROTOCOL;
                    }
                    // Duplicate RESUME_REQ/RESP can show up due to retransmissions; ignore them
                    if (t == VAL_PKT_RESUME_REQ || t == VAL_PKT_RESUME_RESP)
                    {
                        VAL_LOG_DEBUG(s, "verify: ignoring stray RESUME_* during verify wait");
                        continue;
                    }
                    // Ignore any other unexpected packet types during this phase
                    VAL_LOG_DEBUG(s, "verify: ignoring unexpected packet during verify wait");
                    continue;
                }
                if (st != VAL_ERR_TIMEOUT || tries == 0)
                {
                    VAL_LOG_ERRORF(s, "verify: recv failed %d (tries=%u)", (int)st, (unsigned)tries);
                    if (st == VAL_ERR_TIMEOUT && tries == 0)
                        VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                    return st;
                }
                // Retransmit VERIFY on timeout
                if (val_check_for_cancel(s))
                {
                    VAL_LOG_WARN(s, "verify: local cancel during timeout");
                    return VAL_ERR_ABORTED;
                }
                s->timing.in_retransmit = 1; // Karn's algorithm
                val_status_t rs = val_internal_send_packet(s, VAL_PKT_VERIFY, &verify_le, sizeof(verify_le), 0);
                if (rs != VAL_OK)
                {
                    VAL_LOG_ERRORF(s, "verify: retransmit VERIFY failed %d", (int)rs);
                    return rs;
                }
                VAL_LOG_DEBUG(s, "verify: timeout, retransmitting");
                if (backoff && s->config->system.delay_ms)
                    s->config->system.delay_ms(backoff);
                if (backoff)
                    backoff <<= 1;
                --tries;
            }
        }
        // If receiver indicates mismatch, it will have responded with VAL_ERR_RESUME_VERIFY encoded
        int32_t verify_status = 0;
        if (len >= sizeof(int32_t))
        {
            memcpy(&verify_status, tmp, sizeof(int32_t));
            // Convert from LE
            uint32_t vs_bits = (uint32_t)verify_status;
            vs_bits = val_letoh32(vs_bits);
            verify_status = (int32_t)vs_bits;
        }
        VAL_LOG_DEBUGF(s, "send: received VERIFY response status=%d", (int)verify_status);
        if (verify_status == VAL_ERR_RESUME_VERIFY)
        {
            *resume_offset_out = 0;
        }
        else if (verify_status == VAL_SKIPPED)
        {
            *resume_offset_out = UINT64_MAX; // indicate skip to caller
        }
        else if (verify_status == VAL_ERR_ABORTED)
        {
            VAL_LOG_WARN(s, "verify: receiver aborted file");
            return VAL_ERR_ABORTED;
        }
        else if (verify_status != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "verify: status %d", (int)verify_status);
            return (val_status_t)verify_status;
        }
        else
        {
            *resume_offset_out = resp.resume_offset;
        }
    }
    else if (resp.action == VAL_RESUME_ACTION_START_OFFSET)
    {
        *resume_offset_out = resp.resume_offset;
    }
    else if (resp.action == VAL_RESUME_ACTION_SKIP_FILE)
    {
        // Receiver elected to skip; we should jump to DONE.
        // We don't know size here directly; the caller has it. Use a sentinel: caller will treat >=size as skip.
        *resume_offset_out = UINT64_MAX; // signal to caller to bypass sending
    }
    else if (resp.action == VAL_RESUME_ACTION_ABORT_FILE)
    {
        VAL_LOG_WARN(s, "resume_resp: receiver aborted file");
        return VAL_ERR_ABORTED;
    }
    else
    {
        *resume_offset_out = 0;
    }
    VAL_LOG_INFOF(s, "resume: sender will start at offset=%llu", (unsigned long long)(*resume_offset_out));
    return VAL_OK;
}

typedef struct
{
    // Accumulated across batch
    uint64_t batch_total_bytes; // sum of all file sizes in batch
    uint64_t batch_transferred; // cumulative bytes acknowledged
    uint32_t total_files;       // number of files in batch
    uint32_t files_completed;   // completed files
    uint32_t start_ms;          // tick at batch start (if system.get_ticks_ms present)
} val_send_progress_ctx_t;

static void val_emit_progress_sender(val_session_t *s, val_send_progress_ctx_t *ctx, const char *filename,
                                     uint64_t current_file_bytes, int include_current)
{
    if (!s || !s->config || !s->config->callbacks.on_progress)
        return;
    val_progress_info_t info;
    if (ctx)
        info.bytes_transferred = ctx->batch_transferred + (include_current ? current_file_bytes : 0);
    else
        info.bytes_transferred = current_file_bytes;
    info.total_bytes = ctx ? ctx->batch_total_bytes : 0;
    info.current_file_bytes = current_file_bytes;
    info.files_completed = ctx ? ctx->files_completed : 0;
    info.total_files = ctx ? ctx->total_files : 0;
    // Compute rate/ETA if possible
    uint32_t now = (s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0);
    uint32_t elapsed_ms = (ctx && ctx->start_ms && now >= ctx->start_ms) ? (now - ctx->start_ms) : 0;
    if (elapsed_ms > 0)
    {
        // bytes per second
        uint64_t bps = (info.bytes_transferred * 1000ull) / (uint64_t)elapsed_ms;
        info.transfer_rate_bps = (uint32_t)(bps > 0xFFFFFFFFu ? 0xFFFFFFFFu : bps);
        if (info.total_bytes > info.bytes_transferred)
        {
            uint64_t remaining = info.total_bytes - info.bytes_transferred;
            info.eta_seconds = (uint32_t)((bps == 0) ? 0 : (remaining / (bps ? bps : 1)));
        }
        else
        {
            info.eta_seconds = 0;
        }
    }
    else
    {
        info.transfer_rate_bps = 0;
        info.eta_seconds = 0;
    }
    info.current_filename = filename;
    s->config->callbacks.on_progress(&info);
}

val_status_t val_internal_send_file(val_session_t *s, const char *filepath, const char *sender_path, void *progress_ctx)
{
    // Gather meta
    uint64_t size = 0;
    char filename[VAL_MAX_FILENAME + 1];
    VAL_LOG_INFOF(s, "send_file: begin filepath='%s'", filepath ? filepath : "<null>");
    val_status_t st = get_file_size_and_name(s, filepath, &size, filename);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "get_file_size_and_name failed %d", (int)st);
        return st;
    }
    // Determine reported/original path hint to include in metadata for receiver information
    const char *reported_path = (sender_path && sender_path[0]) ? sender_path : filepath;

    // Compute file CRC32 (whole file). For large files, this traverses file once upfront.
    void *fcrc = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!fcrc)
    {
        VAL_LOG_ERROR(s, "fopen for CRC failed");
        return VAL_ERR_IO;
    }
    uint32_t crc_state = val_internal_crc32_init(s);
    if (!s->config->buffers.recv_buffer)
    {
        VAL_LOG_ERROR(s, "recv_buffer null during CRC");
        return VAL_ERR_INVALID_ARG;
    }
    uint8_t *buf = (uint8_t *)s->config->buffers.recv_buffer;
    size_t chunk = (s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size);
    /* Ensure chunk does not exceed the actual recv buffer size to avoid overruns. */
    if (chunk > s->config->buffers.packet_size)
        chunk = s->config->buffers.packet_size;
    for (;;)
    {
        if (val_check_for_cancel(s))
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, fcrc);
            VAL_LOG_WARN(s, "crc: local cancel detected");
            return VAL_ERR_ABORTED;
        }
        int r = s->config->filesystem.fread(s->config->filesystem.fs_context, buf, 1, chunk, fcrc);
        if (r <= 0)
            break;
        crc_state = val_internal_crc32_update(s, crc_state, buf, (size_t)r);
    }
    s->config->filesystem.fclose(s->config->filesystem.fs_context, fcrc);
    uint32_t file_crc = val_internal_crc32_final(s, crc_state);

    // Send metadata
    st = send_metadata(s, reported_path, size, file_crc, filename);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "send_metadata failed %d", (int)st);
        fprintf(stdout, "[VAL][TX] send_metadata failed st=%d\n", (int)st);
        fflush(stdout);
        return st;
    }
    VAL_LOG_INFO(s, "send: sent SEND_META");

    // Ask for resume and honor receiver's decision
    uint64_t resume_off = 0;
    st = request_resume_and_get_response(s, filepath, &resume_off);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "request_resume_and_get_response failed %d", (int)st);
        fprintf(stdout, "[VAL][TX] resume exchange failed st=%d\n", (int)st);
        fflush(stdout);
        return st;
    }

    // If receiver asked to skip, bypass sending and go straight to DONE
    if (resume_off == UINT64_MAX)
    {
        VAL_LOG_INFO(s, "data: receiver elected to skip file; jumping to DONE");
        // Notify start with full size resume (optional UI)
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, reported_path ? reported_path : "", size, size);
        goto send_done_no_file;
    }

    // Open file and seek to resume_off
    void *f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
    if (!f)
    {
        VAL_LOG_ERROR(s, "fopen for data failed");
        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
        return VAL_ERR_IO;
    }
    if (val_check_for_cancel(s))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        VAL_LOG_WARN(s, "data: local cancel before sending");
        return VAL_ERR_ABORTED;
    }
    if (resume_off != 0)
    {
        if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, (long)resume_off, SEEK_SET) != 0)
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            VAL_LOG_ERROR(s, "fseek to resume_off failed");
            val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
            return VAL_ERR_IO;
        }
    }

    // Notify start
    if (s->config->callbacks.on_file_start)
        s->config->callbacks.on_file_start(filename, reported_path ? reported_path : "", size, resume_off);

    // Emit an initial progress snapshot at the resume offset so UIs/tests can react (e.g., trigger cancel)
    if (progress_ctx)
        val_emit_progress_sender(s, (val_send_progress_ctx_t *)progress_ctx, filename, resume_off, 1);
    else
        val_emit_progress_sender(s, NULL, filename, resume_off, 1);
    // If cancel was requested by the progress callback, abort before sending any further data
    if (val_check_for_cancel(s))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
        VAL_LOG_WARN(s, "data: local cancel immediately after initial progress emission");
        return VAL_ERR_ABORTED;
    }

    // Send data packets
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size;
    // Stage file reads directly into the send buffer payload area to avoid clobbering the recv buffer
    uint8_t *send_buf_bytes = (uint8_t *)s->config->buffers.send_buffer;
    uint8_t *payload_area = send_buf_bytes ? (send_buf_bytes + sizeof(val_packet_header_t)) : NULL;
    uint64_t sent = resume_off;
    if (resume_off >= size)
    {
        VAL_LOG_INFO(s, "data: nothing to send after successful resume verify; skipping to DONE");
        goto send_done;
    }
    if (P <= (sizeof(val_packet_header_t) + sizeof(uint32_t)))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        VAL_LOG_ERROR(s, "packet size too small for payload");
        return VAL_ERR_INVALID_ARG;
    }
    size_t max_payload = (size_t)(P - sizeof(val_packet_header_t) - sizeof(uint32_t));
    while (sent < size)
    {
        size_t to_read = (size_t)((size - sent) < (uint64_t)max_payload ? (size - sent) : (uint64_t)max_payload);
        if (!payload_area)
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            VAL_LOG_ERROR(s, "send buffer is null");
            return VAL_ERR_INVALID_ARG;
        }
        // Ensure file position matches expected 'sent'
        long curpos = s->config->filesystem.ftell(s->config->filesystem.fs_context, f);
        if (curpos < 0 || (uint64_t)curpos != sent)
        {
            (void)s->config->filesystem.fseek(s->config->filesystem.fs_context, f, (long)sent, SEEK_SET);
        }
        // Robust read: loop until we fill the payload or hit EOF/error
        size_t have = 0;
        int reopen_retry = 1; // one-time reopen+seek retry in case of a transient 0-byte read mid-file
        for (;;)
        {
            while (have < to_read)
            {
                if (val_check_for_cancel(s))
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                    VAL_LOG_WARN(s, "data: local cancel during read");
                    return VAL_ERR_ABORTED;
                }
                size_t need = to_read - have;
                int r = s->config->filesystem.fread(s->config->filesystem.fs_context, payload_area + have, 1, need, f);
                if (r <= 0)
                    break;
                have += (size_t)r;
            }
            if (have == to_read)
                break; // filled
            if (!reopen_retry)
            {
                // Log detailed diagnostics before failing
                long pos_after = s->config->filesystem.ftell(s->config->filesystem.fs_context, f);
                VAL_LOG_ERRORF(s, "fread data failed (have=%u need=%u sent=%llu size=%llu ftell=%ld)", (unsigned)have,
                               (unsigned)to_read, (unsigned long long)sent, (unsigned long long)size, pos_after);
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return VAL_ERR_IO;
            }
            // Attempt one-time recovery: reopen file and seek back to 'sent', then retry filling this chunk
            reopen_retry = 0;
            VAL_LOG_DEBUGF(s, "data: reopen+seek retry at sent=%llu have=%u need=%u", (unsigned long long)sent, (unsigned)have,
                           (unsigned)to_read);
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            f = s->config->filesystem.fopen(s->config->filesystem.fs_context, filepath, "rb");
            if (!f)
            {
                VAL_LOG_ERROR(s, "fopen retry for data failed");
                return VAL_ERR_IO;
            }
            if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, (long)sent, SEEK_SET) != 0)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                VAL_LOG_ERROR(s, "fseek retry to sent failed");
                return VAL_ERR_IO;
            }
            have = 0; // restart filling the chunk
        }
        VAL_LOG_DEBUGF(s, "data: sending chunk off=%llu len=%u", (unsigned long long)sent, (unsigned)to_read);
        st = val_internal_send_packet(s, VAL_PKT_DATA, payload_area, (uint32_t)to_read, sent);
        if (st != VAL_OK)
        {
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
            VAL_LOG_ERRORF(s, "send DATA failed %d", (int)st);
            return st;
        }
        // Wait for cumulative ACK (offset == next expected on receiver)
        uint32_t len = 0;
        uint64_t off = 0;
        val_packet_type_t t = 0;
        uint32_t to_ack = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
        uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
        uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
        uint32_t t0 = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
        s->timing.in_retransmit = 0;
        for (;;)
        {
            if (!val_internal_transport_is_connected(s))
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                fprintf(stdout, "[VAL][TX] DATA_ACK wait: transport disconnected -> IO error\n");
                fflush(stdout);
                return VAL_ERR_IO;
            }
            if (val_check_for_cancel(s))
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                VAL_LOG_WARN(s, "recv DATA_ACK: local cancel");
                return VAL_ERR_ABORTED;
            }
            st = val_internal_recv_packet(s, &t, NULL, 0, &len, &off, to_ack);
            if (st == VAL_OK)
            {
                if (t == VAL_PKT_CANCEL)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                    VAL_LOG_WARN(s, "recv DATA_ACK: received CANCEL");
                    fprintf(stdout, "[VAL][TX] DATA_ACK wait: received CANCEL -> ABORT\n");
                    fflush(stdout);
                    return VAL_ERR_ABORTED;
                }
                if (t == VAL_PKT_ERROR)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    VAL_LOG_ERROR(s, "recv DATA_ACK: ERROR packet");
                    return VAL_ERR_PROTOCOL;
                }
                if (t != VAL_PKT_DATA_ACK)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    VAL_LOG_ERRORF(s, "recv: expected DATA_ACK got %d", (int)t);
                    VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_UNKNOWN_TYPE);
                    return VAL_ERR_PROTOCOL;
                }
                VAL_LOG_DEBUGF(s, "data: got DATA_ACK off=%llu", (unsigned long long)off);
                if (t0 && s->config->system.get_ticks_ms && !s->timing.in_retransmit)
                {
                    uint32_t now = s->config->system.get_ticks_ms();
                    val_internal_record_rtt(s, now - t0);
                }
                // Cumulative semantics: off == receiver's next expected offset
                if (off > sent + to_read)
                {
                    // Receiver is ahead; seek forward
                    uint64_t target = off;
                    long target_l = (long)target;
                    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, target_l, SEEK_SET) != 0)
                    {
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        VAL_LOG_ERROR(s, "fseek forward failed");
                        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
                        return VAL_ERR_IO;
                    }
                    sent = off;
                }
                else if (off < sent)
                {
                    // Receiver expects earlier data; seek back and resume
                    long target_l = (long)off;
                    if (s->config->filesystem.fseek(s->config->filesystem.fs_context, f, target_l, SEEK_SET) != 0)
                    {
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        VAL_LOG_ERROR(s, "fseek backward failed");
                        val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
                        return VAL_ERR_IO;
                    }
                    sent = off;
                }
                else
                {
                    // Normal advance: receiver accepted this chunk
                    sent = off; // equals sent + to_read in happy path
                }
                break;
            }
            if ((st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC) || tries == 0)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                VAL_LOG_ERRORF(s, "recv DATA_ACK failed %d", (int)st);
                if (st == VAL_ERR_TIMEOUT && tries == 0)
                    VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                fprintf(stdout, "[VAL][TX] DATA_ACK wait: giving up st=%d (tries=%u)\n", (int)st, (unsigned)tries);
                fflush(stdout);
                return st;
            }
            // Timeout: retransmit the same DATA chunk and backoff
            if (val_check_for_cancel(s))
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                VAL_LOG_WARN(s, "data: local cancel before retransmit");
                return VAL_ERR_ABORTED;
            }
            s->timing.in_retransmit = 1; // Karn's algorithm
            val_status_t rs = val_internal_send_packet(s, VAL_PKT_DATA, payload_area, (uint32_t)to_read, sent);
            if (rs != VAL_OK)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return rs;
            }
            VAL_LOG_DEBUG(s, "data: ack timeout, retransmitting");
            if (backoff && s->config->system.delay_ms)
                s->config->system.delay_ms(backoff);
            if (backoff)
                backoff <<= 1;
            --tries;
        }
        // Progress callback reflects receiver-accepted position
        if (progress_ctx)
            val_emit_progress_sender(s, (val_send_progress_ctx_t *)progress_ctx, filename, sent, 1);
        else
            val_emit_progress_sender(s, NULL, filename, sent, 1);
    }

    // If a local cancel was requested right after the final DATA_ACK, honor it before sending DONE
    if (val_check_for_cancel(s))
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
        VAL_LOG_WARN(s, "data: local cancel after last DATA_ACK, before DONE");
        fprintf(stdout, "[VAL][TX] local CANCEL detected after last DATA_ACK before DONE -> ABORT\n");
        fflush(stdout);
        return VAL_ERR_ABORTED;
    }

    // Signal done and wait for DONE_ACK
send_done:
    st = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
    if (st != VAL_OK)
    {
        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
        VAL_LOG_ERRORF(s, "send DONE failed %d", (int)st);
        fprintf(stdout, "[VAL][TX] send DONE failed st=%d\n", (int)st);
        fflush(stdout);
        return st;
    }
    {
        val_packet_type_t t = 0;
        uint32_t len = 0;
        uint64_t off = 0;
        uint32_t to = val_internal_get_timeout(s, VAL_OP_DONE_ACK);
        uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
        uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
        uint32_t t0done = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
        s->timing.in_retransmit = 0;
        for (;;)
        {
            if (!val_internal_transport_is_connected(s))
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                return VAL_ERR_IO;
            }
            if (val_check_for_cancel(s))
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                VAL_LOG_WARN(s, "recv DONE_ACK: local cancel while waiting");
                return VAL_ERR_ABORTED;
            }
            st = val_internal_recv_packet(s, &t, NULL, 0, &len, &off, to);
            if (st == VAL_OK)
            {
                fprintf(stdout, "[VAL][TX] DONE_ACK wait: got pkt t=%d len=%u off=%llu\n", (int)t, (unsigned)len,
                        (unsigned long long)off);
                fflush(stdout);
                if (t == VAL_PKT_ERROR)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_PROTOCOL);
                    VAL_LOG_ERROR(s, "recv DONE_ACK: ERROR packet");
                    return VAL_ERR_PROTOCOL;
                }
                if (t == VAL_PKT_DONE_ACK)
                {
                    if (t0done && s->config->system.get_ticks_ms && !s->timing.in_retransmit)
                    {
                        uint32_t now = s->config->system.get_ticks_ms();
                        val_internal_record_rtt(s, now - t0done);
                    }
                    break;
                }
                if (t == VAL_PKT_CANCEL)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                    VAL_LOG_WARN(s, "recv DONE_ACK: received CANCEL");
                    fprintf(stdout, "[VAL][TX] DONE_ACK wait: received CANCEL -> ABORT\n");
                    fflush(stdout);
                    return VAL_ERR_ABORTED;
                }
            }
            if (st != VAL_OK && st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
                VAL_LOG_ERRORF(s, "recv DONE_ACK failed %d", (int)st);
                fprintf(stdout, "[VAL][TX] DONE_ACK wait: error st=%d -> returning\n", (int)st);
                fflush(stdout);
                return st;
            }
            if (tries == 0)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_TIMEOUT);
                VAL_LOG_ERROR(s, "recv DONE_ACK: retries exhausted");
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                fprintf(stdout, "[VAL][TX] DONE_ACK wait: retries exhausted -> TIMEOUT\n");
                fflush(stdout);
                return VAL_ERR_TIMEOUT;
            }
            // Retransmit DONE on timeout or wrong packet
            s->timing.in_retransmit = 1; // Karn's algorithm
            val_status_t rs = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
            if (rs != VAL_OK)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", rs);
                VAL_LOG_ERRORF(s, "retransmit DONE failed %d", (int)rs);
                fprintf(stdout, "[VAL][TX] DONE retransmit failed rs=%d\n", (int)rs);
                fflush(stdout);
                return rs;
            }
            VAL_LOG_DEBUG(s, "done: ack timeout, retransmitting");
            if (backoff && s->config->system.delay_ms)
                s->config->system.delay_ms(backoff);
            if (backoff)
                backoff <<= 1;
            --tries;
        }
    }
    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
    if (s->config->callbacks.on_file_complete)
        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
    fprintf(stdout, "[VAL][TX] file complete callback with st=%d\n", (int)st);
    fflush(stdout);
    // Update batch context on completion
    if (progress_ctx)
    {
        val_send_progress_ctx_t *ctx = (val_send_progress_ctx_t *)progress_ctx;
        if (st == VAL_OK)
        {
            ctx->files_completed += 1;
            // Add full file size to batch transferred
            ctx->batch_transferred += size;
            // Emit a final batch progress snapshot post-completion
            val_emit_progress_sender(s, ctx, filename, size, 0);
        }
    }
    return st;

send_done_no_file:
    // Send DONE without having opened the file
    st = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
    if (st != VAL_OK)
    {
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
        VAL_LOG_ERRORF(s, "send DONE (skip) failed %d", (int)st);
        return st;
    }
    {
        val_packet_type_t t = 0;
        uint32_t len = 0;
        uint64_t off = 0;
        uint32_t to = val_internal_get_timeout(s, VAL_OP_DONE_ACK);
        uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
        uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
        for (;;)
        {
            if (!val_internal_transport_is_connected(s))
            {
                VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                return VAL_ERR_IO;
            }
            st = val_internal_recv_packet(s, &t, NULL, 0, &len, &off, to);
            if (st == VAL_OK)
            {
                if (t == VAL_PKT_ERROR)
                {
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_PROTOCOL);
                    VAL_LOG_ERROR(s, "recv DONE_ACK (skip): ERROR packet");
                    return VAL_ERR_PROTOCOL;
                }
                if (t == VAL_PKT_DONE_ACK)
                    break;
                if (t == VAL_PKT_CANCEL)
                {
                    if (s->config->callbacks.on_file_complete)
                        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_ABORTED);
                    VAL_LOG_WARN(s, "recv DONE_ACK (skip): received CANCEL");
                    return VAL_ERR_ABORTED;
                }
            }
            if (st != VAL_OK && st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC)
            {
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", st);
                VAL_LOG_ERRORF(s, "recv DONE_ACK (skip) failed %d", (int)st);
                return st;
            }
            if (tries == 0)
            {
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_ERR_TIMEOUT);
                VAL_LOG_ERROR(s, "recv DONE_ACK (skip): retries exhausted");
                VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                return VAL_ERR_TIMEOUT;
            }
            // Retransmit DONE on timeout or wrong packet
            val_status_t rs = val_internal_send_packet(s, VAL_PKT_DONE, NULL, 0, size);
            if (rs != VAL_OK)
            {
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", rs);
                VAL_LOG_ERRORF(s, "retransmit DONE (skip) failed %d", (int)rs);
                return rs;
            }
            VAL_LOG_DEBUG(s, "done: ack timeout (skip), retransmitting");
            if (backoff && s->config->system.delay_ms)
                s->config->system.delay_ms(backoff);
            if (backoff)
                backoff <<= 1;
            --tries;
        }
    }
    if (s->config->callbacks.on_file_complete)
        s->config->callbacks.on_file_complete(filename, reported_path ? reported_path : "", VAL_SKIPPED);
    if (progress_ctx)
    {
        val_send_progress_ctx_t *ctx = (val_send_progress_ctx_t *)progress_ctx;
        ctx->files_completed += 1;
        ctx->batch_transferred += size;
        val_emit_progress_sender(s, ctx, filename, size, 0);
    }
    return VAL_OK;
}

val_status_t val_send_files(val_session_t *s, const char *const *filepaths, size_t file_count, const char *sender_path)
{
    if (!s || !filepaths || file_count == 0)
    {
        VAL_LOG_ERROR(s, "val_send_files: invalid args");
        return VAL_ERR_INVALID_ARG;
    }
    // Coarse-grained session lock to keep send operations thread-safe per session
#if defined(_WIN32)
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
    // Validate local features before any handshake; requested gets sanitized, required must be supported locally
    extern val_status_t val_internal_do_handshake_sender(val_session_t * s);
    val_status_t hs = val_internal_do_handshake_sender(s);
    if (hs != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "handshake (sender) failed %d", (int)hs);
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
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
    prog.start_ms = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
    for (size_t i = 0; i < file_count; ++i)
    {
        VAL_LOG_INFOF(s, "send_files: sending [%u/%u] '%s'", (unsigned)(i + 1), (unsigned)file_count,
                      filepaths[i] ? filepaths[i] : "<null>");
        val_status_t st = val_internal_send_file(s, filepaths[i], sender_path, &prog);
        VAL_LOG_INFOF(s, "send_files: result for '%s' = %d", filepaths[i] ? filepaths[i] : "<null>", (int)st);
        if (st != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "send_file failed %d", (int)st);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return st;
        }
    }
    // Send EOT and wait for ACK automatically
    val_status_t st = val_internal_send_packet(s, VAL_PKT_EOT, NULL, 0, 0);
    if (st != VAL_OK)
    {
        VAL_LOG_ERRORF(s, "send EOT failed %d", (int)st);
#if defined(_WIN32)
        LeaveCriticalSection(&s->lock);
#else
        pthread_mutex_unlock(&s->lock);
#endif
        return st;
    }
    val_packet_type_t t = 0;
    uint32_t len = 0;
    uint64_t off = 0;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_EOT_ACK);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint32_t t0eot = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0;
    s->timing.in_retransmit = 0;
    for (;;)
    {
        if (val_check_for_cancel(s))
        {
            VAL_LOG_WARN(s, "wait EOT_ACK: local cancel while waiting");
            fprintf(stdout, "[VAL][TX] EOT_ACK wait: local CANCEL -> ABORT\n");
            fflush(stdout);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return VAL_ERR_ABORTED;
        }
        st = val_internal_recv_packet(s, &t, NULL, 0, &len, &off, to);
        if (st == VAL_OK)
        {
            fprintf(stdout, "[VAL][TX] EOT_ACK wait: got pkt t=%d len=%u off=%llu\n", (int)t, (unsigned)len,
                    (unsigned long long)off);
            fflush(stdout);
            if (t == VAL_PKT_ERROR)
            {
#if defined(_WIN32)
                LeaveCriticalSection(&s->lock);
#else
                pthread_mutex_unlock(&s->lock);
#endif
                return VAL_ERR_PROTOCOL;
            }
            if (t == VAL_PKT_EOT_ACK)
            {
                if (t0eot && s->config->system.get_ticks_ms && !s->timing.in_retransmit)
                {
                    uint32_t now = s->config->system.get_ticks_ms();
                    val_internal_record_rtt(s, now - t0eot);
                }
                break;
            }
            if (t == VAL_PKT_CANCEL)
            {
                VAL_LOG_WARN(s, "wait EOT_ACK: received CANCEL");
                fprintf(stdout, "[VAL][TX] EOT_ACK wait: received CANCEL -> ABORT\n");
                fflush(stdout);
#if defined(_WIN32)
                LeaveCriticalSection(&s->lock);
#else
                pthread_mutex_unlock(&s->lock);
#endif
                return VAL_ERR_ABORTED;
            }
        }
        if (st != VAL_OK && st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC)
        {
            VAL_LOG_ERRORF(s, "wait EOT_ACK failed %d", (int)st);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return st;
        }
        if (tries == 0)
        {
            VAL_LOG_ERROR(s, "wait EOT_ACK: retries exhausted");
            fprintf(stdout, "[VAL][TX] EOT_ACK wait: retries exhausted -> TIMEOUT\n");
            fflush(stdout);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return VAL_ERR_TIMEOUT;
        }
        // Retransmit EOT and backoff
        s->timing.in_retransmit = 1; // Karn's algorithm
        val_status_t rs = val_internal_send_packet(s, VAL_PKT_EOT, NULL, 0, 0);
        if (rs != VAL_OK)
        {
            VAL_LOG_ERRORF(s, "retransmit EOT failed %d", (int)rs);
            fprintf(stdout, "[VAL][TX] EOT retransmit failed rs=%d\n", (int)rs);
            fflush(stdout);
#if defined(_WIN32)
            LeaveCriticalSection(&s->lock);
#else
            pthread_mutex_unlock(&s->lock);
#endif
            return rs;
        }
        VAL_LOG_DEBUG(s, "eot: ack timeout, retransmitting");
        if (backoff && s->config->system.delay_ms)
            s->config->system.delay_ms(backoff);
        if (backoff)
            backoff <<= 1;
        --tries;
    }
    val_status_t out = VAL_OK;
#if defined(_WIN32)
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
    return out;
}
