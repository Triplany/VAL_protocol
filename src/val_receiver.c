#include "val_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static val_status_t send_resume_response(val_session_t *s, val_resume_action_t action, uint64_t offset, uint32_t crc,
                                         uint64_t verify_len)
{
    val_resume_resp_t resp;
    resp.action = val_htole32((uint32_t)action);
    resp.resume_offset = val_htole64(offset);
    resp.verify_crc = val_htole32(crc);
    resp.verify_len = val_htole64(verify_len);
    return val_internal_send_packet(s, VAL_PKT_RESUME_RESP, &resp, sizeof(resp), offset);
}

// --- Metadata validation helpers ---
// Construct full target path using output_directory and a sanitized filename
static val_status_t val_construct_target_path(val_session_t *session, const char *filename, char *target_path,
                                              size_t target_path_size)
{
    if (!session || !target_path || target_path_size == 0)
        return VAL_ERR_INVALID_ARG;
    char sanitized[VAL_MAX_FILENAME + 1];
    val_clean_filename(filename, sanitized, sizeof(sanitized));
#ifdef _WIN32
    int n = snprintf(target_path, target_path_size, "%s\\%s", session->output_directory, sanitized);
#else
    int n = snprintf(target_path, target_path_size, "%s/%s", session->output_directory, sanitized);
#endif
    if (n < 0 || n >= (int)target_path_size)
        return VAL_ERR_INVALID_ARG;
    return VAL_OK;
}

static val_validation_action_t val_validate_metadata(val_session_t *session, const val_meta_payload_t *meta,
                                                     const char *target_path)
{
    if (!session)
        return VAL_VALIDATION_ABORT;
    if (!session->cfg.metadata_validation.validator)
        return VAL_VALIDATION_ACCEPT; // default
    return session->cfg.metadata_validation.validator(meta, target_path, session->cfg.metadata_validation.validator_context);
}

static val_status_t val_handle_validation_action(val_session_t *session, val_validation_action_t action, const char *filename)
{
    switch (action)
    {
    case VAL_VALIDATION_ACCEPT:
        return VAL_OK;
    case VAL_VALIDATION_SKIP:
        return send_resume_response(session, VAL_RESUME_ACTION_SKIP_FILE, 0, 0, 0);
    case VAL_VALIDATION_ABORT:
        return send_resume_response(session, VAL_RESUME_ACTION_ABORT_FILE, 0, 0, 0);
    default:
        VAL_SET_PROTOCOL_ERROR(session, VAL_ERROR_DETAIL_INVALID_STATE);
        return VAL_ERR_PROTOCOL;
    }
}

static val_status_t handle_verification_exchange(val_session_t *s, uint64_t resume_offset_expected, uint32_t expected_crc,
                                                 uint64_t verify_len, val_status_t on_match_status)
{
    VAL_LOG_DEBUG(s, "verify: starting exchange");
    // Wait for sender to echo back the CRC in a VERIFY packet
    uint8_t buf[128];
    uint32_t len = 0;
    uint64_t off = 0;
    val_packet_type_t t = 0;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_VERIFY);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    val_status_t st = VAL_OK;
    uint32_t t0v = s->config->system.get_ticks_ms();
    s->timing.in_retransmit = 0;
    for (;;)
    {
        if (!val_internal_transport_is_connected(s))
        {
            VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
            return VAL_ERR_IO;
        }
        st = val_internal_recv_packet(s, &t, buf, sizeof(buf), &len, &off, to);
        if (st != VAL_OK)
        {
            if (st != VAL_ERR_TIMEOUT || tries == 0)
            {
                if (st == VAL_ERR_TIMEOUT && tries == 0)
                    VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
                return st;
            }
            VAL_LOG_DEBUG(s, "verify: waiting for sender CRC");
            val_metrics_inc_timeout(s);
            if (backoff && s->config->system.delay_ms)
                s->config->system.delay_ms(backoff);
            if (backoff)
                backoff <<= 1;
            --tries;
            continue;
        }
        // Handle possible duplicate RESUME_REQ caused by sender timeout
        if (t == VAL_PKT_RESUME_REQ)
        {
            VAL_LOG_INFO(s, "verify: got duplicate RESUME_REQ; re-sending RESUME_RESP");
            (void)send_resume_response(s, VAL_RESUME_ACTION_VERIFY_FIRST, resume_offset_expected, expected_crc, verify_len);
            continue; // keep waiting for VERIFY
        }
        if (t != VAL_PKT_VERIFY)
        {
            VAL_LOG_DEBUG(s, "verify: ignoring unexpected packet during verify wait");
            continue;
        }
        if (t0v && !s->timing.in_retransmit)
        {
            uint32_t now = s->config->system.get_ticks_ms();
            val_internal_record_rtt(s, now - t0v);
        }
        break;
    }
    VAL_LOG_DEBUG(s, "verify: received VERIFY from sender");
    VAL_LOG_INFO(s, "verify: type ok");
    VAL_LOG_DEBUG(s, "verify: len check");
    VAL_LOG_DEBUG(s, "verify: len ok");
    if (len < sizeof(val_resume_resp_t))
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    VAL_LOG_DEBUGF(s, "verify: extracting crc from payload len=%u", (unsigned)len);
    // Payload is a val_resume_resp_t encoded in LE; decode to read verify_crc robustly
    val_resume_resp_t vr_le;
    memcpy(&vr_le, buf, sizeof(vr_le));
    val_resume_resp_t vr;
    vr.action = val_letoh32(vr_le.action);
    vr.resume_offset = val_letoh64(vr_le.resume_offset);
    vr.verify_crc = val_letoh32(vr_le.verify_crc);
    vr.verify_len = val_letoh64(vr_le.verify_len);
    uint32_t their_verify_crc = vr.verify_crc;
    // Optional sanity logs to help diagnose mismatches
    VAL_LOG_DEBUGF(s, "verify: expected off=%llu len=%llu, got off=%llu len=%llu", (unsigned long long)resume_offset_expected,
                   (unsigned long long)verify_len, (unsigned long long)vr.resume_offset, (unsigned long long)vr.verify_len);
    VAL_LOG_DEBUG(s, "verify: computing result");
    int32_t result;
    if (their_verify_crc == expected_crc)
    {
        // On match, return the caller-provided status (VAL_OK to resume, or VAL_SKIPPED to skip)
        result = (int32_t)on_match_status;
    }
    else
    {
        // Mismatch: result depends on resume mode policy
        val_resume_mode_t mode = s->config->resume.mode;
        if (mode == VAL_RESUME_CRC_TAIL || mode == VAL_RESUME_CRC_FULL)
        {
            // In CRC_TAIL and CRC_FULL modes, mismatch means skip the file
            result = VAL_SKIPPED;
        }
        else
        {
            // In CRC_TAIL_OR_ZERO and CRC_FULL_OR_ZERO modes, mismatch means restart from zero
            result = VAL_ERR_RESUME_VERIFY;
        }
    }
    // Encode status as LE int32
    int32_t result_le = (int32_t)val_htole32((uint32_t)result);
    VAL_LOG_DEBUG(s, "verify: before sending");
    VAL_LOG_DEBUG(s, "verify: sending status to sender");
    val_status_t send_st = val_internal_send_packet(s, VAL_PKT_VERIFY, &result_le, sizeof(result_le), 0);
    VAL_LOG_DEBUG(s, "verify: send_st");
    VAL_LOG_DEBUG(s, "verify: sent response to sender");
    if (send_st != VAL_OK)
        return send_st;
    // Propagate verification outcome to caller so it can adjust resume offset
    return (val_status_t)result;
}

static val_resume_action_t determine_resume_action(val_session_t *session, const char *filename, const char *sender_path,
                                                   uint64_t incoming_file_size, uint64_t *out_resume_offset,
                                                   uint32_t *out_verify_crc, uint64_t *out_verify_length)
{
    (void)sender_path; // not needed for determining resume
    val_resume_mode_t mode = session->config->resume.mode;

    char full_output_path[512];
    if (session->output_directory[0])
    {
#ifdef _WIN32
        snprintf(full_output_path, sizeof(full_output_path), "%s\\%s", session->output_directory, filename);
#else
        snprintf(full_output_path, sizeof(full_output_path), "%s/%s", session->output_directory, filename);
#endif
    }
    else
    {
        snprintf(full_output_path, sizeof(full_output_path), "%s", filename);
    }

    void *file = session->config->filesystem.fopen(session->config->filesystem.fs_context, full_output_path, "rb");
    if (!file)
    {
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: no existing file, start at 0");
        return VAL_RESUME_ACTION_START_ZERO;
    }

    session->config->filesystem.fseek(session->config->filesystem.fs_context, file, 0, SEEK_END);
    long existing_size = session->config->filesystem.ftell(session->config->filesystem.fs_context, file);
    if (existing_size < 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_ACTION_START_ZERO;
    }

    // Implement simplified six-mode behavior
    switch (mode)
    {
    case VAL_RESUME_NEVER:
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: disabled, start at 0");
        return VAL_RESUME_ACTION_START_ZERO;
    case VAL_RESUME_SKIP_EXISTING:
    {
        // If a local file exists at any size, skip it; otherwise start from zero
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        if (existing_size > 0)
        {
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: SKIP_EXISTING -> skipping existing file");
            return VAL_RESUME_ACTION_SKIP_FILE;
        }
        *out_resume_offset = 0;
        return VAL_RESUME_ACTION_START_ZERO;
    }
    case VAL_RESUME_CRC_TAIL:
    case VAL_RESUME_CRC_TAIL_OR_ZERO:
    {
        if (existing_size <= 0)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: no bytes locally, start at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // If the local file is larger than incoming, treat as mismatch per policy
        if ((uint64_t)existing_size > incoming_file_size)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            if (mode == VAL_RESUME_CRC_TAIL)
            {
                VAL_LOG_INFO(session, "resume: local larger than incoming (TAIL) -> skip file");
                *out_resume_offset = 0;
                *out_verify_crc = 0;
                *out_verify_length = 0;
                return VAL_RESUME_ACTION_SKIP_FILE;
            }
            VAL_LOG_INFO(session, "resume: local larger than incoming (TAIL_OR_ZERO) -> start at 0");
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // Determine tail verification window: respect user-configured bytes but cap to a small default for speed.
        // Core default cap: 2 MiB to keep MCU/slow storage responsive.
        uint32_t verify_bytes_cfg = session->config->resume.crc_verify_bytes ? session->config->resume.crc_verify_bytes : 1024u;
        uint64_t verify_bytes = (uint64_t)verify_bytes_cfg;
        if (verify_bytes > (uint64_t)existing_size)
            verify_bytes = (uint64_t)existing_size;
        const uint64_t TAIL_CAP_BYTES = (uint64_t)2 * 1024 * 1024; // 2 MiB
        if (verify_bytes > TAIL_CAP_BYTES)
            verify_bytes = TAIL_CAP_BYTES;
        long seek_pos = (long)((uint64_t)existing_size - verify_bytes);
        session->config->filesystem.fseek(session->config->filesystem.fs_context, file, seek_pos, SEEK_SET);
        if (!session->config->buffers.recv_buffer)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        size_t buf_size = session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
        if (buf_size == 0)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        uint32_t state = val_internal_crc32_init(session);
        uint64_t left = verify_bytes;
        while (left > 0)
        {
            size_t take = (left < buf_size) ? (size_t)left : buf_size;
            int rr = session->config->filesystem.fread(session->config->filesystem.fs_context,
                                                       session->config->buffers.recv_buffer, 1, take, file);
            if (rr != (int)take)
            {
                session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
                *out_resume_offset = 0;
                return VAL_RESUME_ACTION_START_ZERO;
            }
            state = val_internal_crc32_update(session, state, session->config->buffers.recv_buffer, take);
            left -= take;
        }
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = (uint64_t)existing_size;
        *out_verify_crc = val_internal_crc32_final(session, state);
        *out_verify_length = verify_bytes;
        VAL_LOG_INFO(session, "resume: tail crc verify requested");
        return VAL_RESUME_ACTION_VERIFY_FIRST;
    }
    case VAL_RESUME_CRC_FULL:
    case VAL_RESUME_CRC_FULL_OR_ZERO:
    {
        // FULL means: verify all locally available bytes (prefix) when <= incoming size.
        if (existing_size <= 0)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: no local bytes, start at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        if ((uint64_t)existing_size > incoming_file_size)
        {
            // Local larger than incoming cannot match; treat as mismatch per policy
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            if (mode == VAL_RESUME_CRC_FULL)
            {
                VAL_LOG_INFO(session, "resume: local larger than incoming (FULL) -> skip file");
                *out_resume_offset = 0;
                *out_verify_crc = 0;
                *out_verify_length = 0;
                return VAL_RESUME_ACTION_SKIP_FILE;
            }
            VAL_LOG_INFO(session, "resume: local larger than incoming (FULL_OR_ZERO) -> start at 0");
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        if (!session->config->buffers.recv_buffer)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // Compute CRC over either the entire local prefix (if <= cap) or the last 'cap' bytes (large tail fallback)
        const uint64_t FULL_CAP_BYTES = (uint64_t)512 * 1024 * 1024; // 512 MiB
        uint64_t existing_u64 = (uint64_t)existing_size;
        uint64_t verify_len = existing_u64 <= FULL_CAP_BYTES ? existing_u64 : FULL_CAP_BYTES;
        long start_pos = (long)(existing_u64 - verify_len);
        session->config->filesystem.fseek(session->config->filesystem.fs_context, file, start_pos, SEEK_SET);
        size_t buf_size = session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
        if (buf_size == 0)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        uint32_t state = val_internal_crc32_init(session);
        uint64_t left = verify_len;
        while (left > 0)
        {
            size_t take = (left < buf_size) ? (size_t)left : buf_size;
            int rr = session->config->filesystem.fread(session->config->filesystem.fs_context,
                                                       session->config->buffers.recv_buffer, 1, take, file);
            if (rr != (int)take)
            {
                session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
                *out_resume_offset = 0;
                return VAL_RESUME_ACTION_START_ZERO;
            }
            state = val_internal_crc32_update(session, state, session->config->buffers.recv_buffer, take);
            left -= take;
        }
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = (uint64_t)existing_size; // resume from what we have
        *out_verify_crc = val_internal_crc32_final(session, state);
        *out_verify_length = verify_len; // request verify over prefix or large-tail window
        if (verify_len == existing_u64)
            VAL_LOG_INFO(session, "resume: full-prefix crc verify requested");
        else
            VAL_LOG_INFO(session, "resume: FULL exceeded cap -> large-tail crc verify requested");
        return VAL_RESUME_ACTION_VERIFY_FIRST;
    }
    }
    session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
    *out_resume_offset = 0;
    return VAL_RESUME_ACTION_START_ZERO;
}

static val_status_t handle_file_resume(val_session_t *s, const char *filename, const char *sender_path, uint64_t file_size,
                                       uint64_t *resume_offset_out)
{
    uint64_t resume_offset = 0;
    uint32_t verify_crc = 0;
    uint64_t verify_length = 0;
    val_resume_action_t action =
        determine_resume_action(s, filename, sender_path, file_size, &resume_offset, &verify_crc, &verify_length);
    val_status_t st = send_resume_response(s, action, resume_offset, verify_crc, verify_length);
    if (st != VAL_OK)
        return st;
    VAL_LOG_DEBUG(s, "resume: sent RESUME_RESP");
    if (action == VAL_RESUME_ACTION_SKIP_FILE)
    {
        // For SKIP_FILE, we still send a RESUME_RESP with SKIP action and then expect sender to jump to DONE.
        // No verification round-trip is required beyond the provided CRC in the response.
        *resume_offset_out = file_size; // treat as already complete on receiver side
        VAL_LOG_INFO(s, "resume: receiver chose SKIP_FILE; awaiting DONE");
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, sender_path, file_size, file_size);
        return VAL_OK;
    }
    if (action == VAL_RESUME_ACTION_ABORT_FILE)
    {
        VAL_LOG_WARN(s, "resume: receiver chose ABORT_FILE");
        return VAL_ERR_ABORTED;
    }
    if (action == VAL_RESUME_ACTION_VERIFY_FIRST)
    {
        // Map verification success to either resume (OK) or skip (SKIPPED) depending on mode
        val_resume_mode_t mode = s->config->resume.mode;
        val_status_t on_match = VAL_OK;
        if (mode == VAL_RESUME_CRC_FULL || mode == VAL_RESUME_CRC_FULL_OR_ZERO)
        {
            // In FULL modes, if verify covered the entire file (existing_size == incoming size), we can skip.
            // Otherwise, we should resume from the verified offset.
            on_match = (verify_length == file_size) ? VAL_SKIPPED : VAL_OK;
        }
        st = handle_verification_exchange(s, resume_offset, verify_crc, verify_length, on_match);
        VAL_LOG_INFOF(s, "resume: verify result st=%d", (int)st);
        if (st == VAL_ERR_RESUME_VERIFY)
        {
            // Sender will restart from zero; receiver resets resume offset
            resume_offset = 0;
            VAL_LOG_INFO(s, "resume: verify mismatch -> restarting at 0");
        }
        else if (st == VAL_SKIPPED)
        {
            // File will be skipped; set resume offset to file size
            resume_offset = file_size;
            VAL_LOG_WARN(s, "resume: verify mismatch -> skipping file");
            if (s->config->callbacks.on_file_start)
                s->config->callbacks.on_file_start(filename, sender_path, file_size, file_size);
        }
        else if (st == VAL_ERR_TIMEOUT)
        {
            // Otherwise treat as failure to verify; restart from zero rather than aborting session
            resume_offset = 0;
            VAL_LOG_INFO(s, "resume: verify timeout, restarting at 0");
        }
        else if (st == VAL_SKIPPED)
        {
            // Receiver-side policy elected to skip after successful equality check; proceed in skip mode.
            // We must not return early here; continue so the receiver can wait for DONE and reply with DONE_ACK.
            resume_offset = file_size;
            VAL_LOG_INFO(s, "resume: verify indicated SKIPPED; proceeding to wait for DONE and ACK");
        }
        else if (st != VAL_OK)
        {
            return st;
        }
    }
    *resume_offset_out = resume_offset;
    VAL_LOG_INFOF(s, "resume: receiver will start at offset=%llu", (unsigned long long)resume_offset);
    if (s->config->callbacks.on_file_start)
        s->config->callbacks.on_file_start(filename, sender_path, file_size, resume_offset);
    return VAL_OK;
}

val_status_t val_internal_receive_files(val_session_t *s, const char *output_directory)
{
    (void)output_directory;
    size_t P = s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size;
    uint8_t *tmp = (uint8_t *)s->config->buffers.recv_buffer;

    // Local batch progress context (no persistent RAM)
    uint64_t batch_transferred = 0; // sum of completed file sizes
    uint32_t files_completed = 0;
    uint32_t start_ms = s->config->system.get_ticks_ms();

    // Handshake done upon public API entry; loop to receive files until EOT
    for (;;)
    {
        // Expect metadata from sender
        val_packet_type_t t = 0;
        uint32_t len = 0;
        uint64_t off = 0;
        uint32_t to_meta = val_internal_get_timeout(s, VAL_OP_META);
        val_status_t st = VAL_OK;
        {
            uint8_t tries = s->config->retries.meta_retries ? s->config->retries.meta_retries : 0;
            uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
            for (;;)
            {
                if (!val_internal_transport_is_connected(s))
                {
                    VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                    return VAL_ERR_IO;
                }
                if (val_check_for_cancel(s))
                {
                    VAL_LOG_WARN(s, "recv: local cancel before metadata");
                    return VAL_ERR_ABORTED;
                }
                st = val_internal_recv_packet(s, &t, tmp, (uint32_t)P, &len, &off, to_meta);
                if (st == VAL_OK)
                    break;
                if (st != VAL_ERR_TIMEOUT || tries == 0)
                {
                    if (st == VAL_ERR_TIMEOUT && tries == 0)
                        VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_META);
                    return st;
                }
                VAL_LOG_DEBUG(s, "recv: waiting for metadata");
                val_metrics_inc_timeout(s);
                if (backoff && s->config->system.delay_ms)
                    s->config->system.delay_ms(backoff);
                if (backoff)
                    backoff <<= 1; // exponential
                --tries;
            }
        }
        if (t == VAL_PKT_EOT)
        {
            (void)val_internal_send_packet(s, VAL_PKT_EOT_ACK, NULL, 0, 0);
            return VAL_OK;
        }
        if (t == VAL_PKT_CANCEL)
        {
            VAL_LOG_WARN(s, "recv: received CANCEL while waiting for metadata");
            val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
            fprintf(stdout, "[VAL][RX] CANCEL observed before metadata, setting last_error ABORTED\n");
            fflush(stdout);
            return VAL_ERR_ABORTED;
        }
        if (t != VAL_PKT_SEND_META || len < sizeof(val_meta_payload_t))
        {
            VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
            return VAL_ERR_PROTOCOL;
        }
        val_meta_payload_t meta;
        memcpy(&meta, tmp, sizeof(meta));
        // Convert numeric fields from LE
        meta.file_size = val_letoh64(meta.file_size);
        meta.file_crc32 = val_letoh32(meta.file_crc32);
        VAL_LOG_INFO(s, "recv: received SEND_META");

        // Determine output path (receiver controls)
        char clean_name[VAL_MAX_FILENAME + 1];
        val_clean_filename(meta.filename, clean_name, sizeof(clean_name));
        char full_output_path[512];
        if (s->output_directory[0])
        {
#ifdef _WIN32
            snprintf(full_output_path, sizeof(full_output_path), "%s\\%s", s->output_directory, clean_name);
#else
            snprintf(full_output_path, sizeof(full_output_path), "%s/%s", s->output_directory, clean_name);
#endif
        }
        else
        {
            snprintf(full_output_path, sizeof(full_output_path), "%s", clean_name);
        }

        // Perform optional metadata validation (ACCEPT/SKIP/ABORT)
        int validation_skipped = 0;
        uint64_t resume_off = 0; // may be set by validation skip path
        {
            char target_path[VAL_MAX_PATH * 2 + 8];
            val_status_t pr = val_construct_target_path(s, clean_name, target_path, sizeof(target_path));
            if (pr != VAL_OK)
            {
                // Treat as validation failure -> abort this file
                (void)val_handle_validation_action(s, VAL_VALIDATION_ABORT, clean_name);
                return VAL_ERR_INVALID_ARG;
            }
            val_validation_action_t act = val_validate_metadata(s, &meta, target_path);
            val_status_t hr = val_handle_validation_action(s, act, clean_name);
            if (act == VAL_VALIDATION_ABORT)
            {
                // We informed sender to abort this file; stop processing
                (void)hr; // hr is send status; even if send fails, abort
                return VAL_ERR_ABORTED;
            }
            if (act == VAL_VALIDATION_SKIP)
            {
                // We informed sender to skip; set skip state and emulate resume skip behavior
                validation_skipped = 1;
                resume_off = meta.file_size; // treat as complete locally
                if (s->config->callbacks.on_file_start)
                    s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
            }
        }
        int skipping = 0;
        if (!validation_skipped)
        {
            // Handle resume negotiation if not overridden by validation
            st = handle_file_resume(s, clean_name, meta.sender_path, meta.file_size, &resume_off);
            if (st != VAL_OK)
            {
                if (st == VAL_ERR_ABORTED)
                {
                    // Drain until DONE for protocol hygiene, then NAK with ERROR and continue (simple: send ERROR now)
                    (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                }
                return st;
            }
            VAL_LOG_INFO(s, "recv: resume handled");
            skipping = (resume_off >= meta.file_size) ? 1 : 0;
        }
        else
        {
            skipping = 1;
        }

        // Open file for write unless we are skipping
        const char *mode = (resume_off == 0) ? "wb" : "ab";
        void *f = NULL;
        if (!skipping)
        {
            f = s->config->filesystem.fopen(s->config->filesystem.fs_context, full_output_path, mode);
            if (!f)
            {
                val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_PERMISSION);
                return VAL_ERR_IO;
            }
        }

        uint64_t total = meta.file_size;
        uint64_t written = resume_off;
        VAL_LOG_DEBUGF(s, "data: starting receive loop (written=%llu,total=%llu)", (unsigned long long)written,
                       (unsigned long long)total);
        // Compute CRC across the final file contents: seed if resuming
        uint32_t crc_state = val_internal_crc32_init(s);
        if (!skipping && resume_off > 0)
        {
            // Re-read existing bytes to seed CRC (could be optimized by caching)
            void *fr = s->config->filesystem.fopen(s->config->filesystem.fs_context, full_output_path, "rb");
            if (!fr)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return VAL_ERR_IO;
            }
            if (!s->config->buffers.recv_buffer)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, fr);
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return VAL_ERR_INVALID_ARG;
            }
            uint8_t *buf_crc = (uint8_t *)s->config->buffers.recv_buffer;
            size_t step = (size_t)(s->effective_packet_size ? s->effective_packet_size : s->config->buffers.packet_size);
            if (step == 0)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, fr);
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return VAL_ERR_INVALID_ARG;
            }
            uint64_t left = resume_off;
            while (left > 0)
            {
                size_t take = (left < step) ? (size_t)left : step;
                /* Ensure take does not exceed the configured recv buffer */
                if (take > s->config->buffers.packet_size)
                    take = s->config->buffers.packet_size;
                int rr = s->config->filesystem.fread(s->config->filesystem.fs_context, buf_crc, 1, take, fr);
                if (rr != (int)take)
                {
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, fr);
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    return VAL_ERR_IO;
                }
                crc_state = val_internal_crc32_update(s, crc_state, buf_crc, take);
                left -= take;
            }
            s->config->filesystem.fclose(s->config->filesystem.fs_context, fr);
        }
        for (;;)
        {
            t = 0;
            len = 0;
            off = 0;
            uint32_t to_data = val_internal_get_timeout(s, VAL_OP_DATA_RECV);
            {
                uint8_t tries = s->config->retries.data_retries ? s->config->retries.data_retries : 0;
                uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
                for (;;)
                {
                    if (!val_internal_transport_is_connected(s))
                    {
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        VAL_SET_NETWORK_ERROR(s, VAL_ERROR_DETAIL_CONNECTION);
                        return VAL_ERR_IO;
                    }
                    if (val_check_for_cancel(s))
                    {
                        VAL_LOG_WARN(s, "data: local cancel at receiver");
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        if (s->config->callbacks.on_file_complete)
                            s->config->callbacks.on_file_complete(clean_name, meta.sender_path, VAL_ERR_ABORTED);
                        val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
                        fprintf(stdout, "[VAL][RX] Local cancel detected in data loop, setting last_error ABORTED\n");
                        fflush(stdout);
                        return VAL_ERR_ABORTED;
                    }
                    st = val_internal_recv_packet(s, &t, tmp, (uint32_t)P, &len, &off, to_data);
                    if (st == VAL_OK)
                    {
                        VAL_LOG_DEBUGF(s, "data: got packet type=%d len=%u off=%llu", (int)t, (unsigned)len,
                                       (unsigned long long)off);
                        break;
                    }
                    if ((st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC) || tries == 0)
                    {
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        if (st == VAL_ERR_TIMEOUT)
                        {
                            VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_DATA);
                        }
                        else if (st == VAL_ERR_CRC)
                        {
                            VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
                        }
                        return st;
                    }
                    if (st == VAL_ERR_TIMEOUT)
                        val_metrics_inc_timeout(s);
                    if (backoff && s->config->system.delay_ms)
                        s->config->system.delay_ms(backoff);
                    if (backoff)
                        backoff <<= 1;
                    --tries;
                }
            }
            if (t == VAL_PKT_DATA)
            {
                // Cumulative ACK semantics
                if (off == written)
                {
                    // Normal in-order chunk
                    if (!skipping && len)
                    {
                        int w = s->config->filesystem.fwrite(s->config->filesystem.fs_context, tmp, 1, len, f);
                        if (w != (int)len)
                        {
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                            val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_DISK_FULL);
                            return VAL_ERR_IO;
                        }
                        crc_state = val_internal_crc32_update(s, crc_state, tmp, len);
                    }
                    written += len;
                }
                else if (off < written)
                {
                    // Duplicate or overlap: ignore write; do not roll back CRC; still ACK current 'written'
                    // No-op, fall through to ACK written
                    VAL_LOG_DEBUG(s, "data: duplicate/overlap ignored");
                }
                else /* off > written */
                {
                    // Sender is ahead; do not write, just re-ACK what we have
                    // No-op
                    VAL_LOG_DEBUG(s, "data: sender ahead, re-ACK current position");
                }
                if (s->config->callbacks.on_progress)
                {
                    val_progress_info_t info;
                    // Cumulative across batch = completed bytes + current file bytes
                    info.bytes_transferred = batch_transferred + written;
                    info.total_bytes = 0; // unknown on receiver; protocol doesn't pre-announce batch size
                    info.current_file_bytes = written;
                    info.files_completed = files_completed;
                    info.total_files = 0; // unknown on receiver
                    // Rate/ETA
                    uint32_t now = s->config->system.get_ticks_ms();
                    uint32_t elapsed_ms = (start_ms && now >= start_ms) ? (now - start_ms) : 0;
                    if (elapsed_ms > 0)
                    {
                        uint64_t bps = (info.bytes_transferred * 1000ull) / (uint64_t)elapsed_ms;
                        info.transfer_rate_bps = (uint32_t)(bps > 0xFFFFFFFFu ? 0xFFFFFFFFu : bps);
                    }
                    else
                    {
                        info.transfer_rate_bps = 0;
                    }
                    info.eta_seconds = 0; // unknown without total_bytes
                    info.current_filename = clean_name;
                    s->config->callbacks.on_progress(&info);
                    // If progress callback triggered a local cancel (e.g., tests call val_emergency_cancel), abort now
                    if (val_check_for_cancel(s))
                    {
                        VAL_LOG_WARN(s, "data: local cancel after progress, before ACK");
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        if (s->config->callbacks.on_file_complete)
                            s->config->callbacks.on_file_complete(clean_name, meta.sender_path, VAL_ERR_ABORTED);
                        val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
                        fprintf(stdout, "[VAL][RX] Local cancel after progress, not sending DATA_ACK, aborting\n");
                        fflush(stdout);
                        return VAL_ERR_ABORTED;
                    }
                }
                // Ack cumulative next expected offset (written)
                VAL_LOG_DEBUGF(s, "data: sending DATA_ACK off=%llu", (unsigned long long)written);
                val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                if (st2 != VAL_OK)
                {
                    if (!skipping && f)
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    return st2;
                }
            }
            else if (t == VAL_PKT_DONE)
            {
                // Validate whole-file CRC
                uint32_t crc_final = val_internal_crc32_final(s, crc_state);
                if (!skipping && crc_final != meta.file_crc32)
                {
                    VAL_LOG_ERROR(s, "done: crc mismatch");
                    if (!skipping && f)
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_CRC_FILE);
                    return VAL_ERR_CRC;
                }
                // Acknowledge DONE explicitly
                val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DONE_ACK, NULL, 0, written);
                if (st2 != VAL_OK)
                {
                    if (!skipping && f)
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                    return st2;
                }
                break; // file complete
            }
            else if (t == VAL_PKT_ERROR)
            {
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                return VAL_ERR_PROTOCOL;
            }
            else if (t == VAL_PKT_CANCEL)
            {
                VAL_LOG_WARN(s, "data: received CANCEL");
                if (!skipping && f)
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                if (s->config->callbacks.on_file_complete)
                    s->config->callbacks.on_file_complete(clean_name, meta.sender_path, VAL_ERR_ABORTED);
                val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
                fprintf(stdout, "[VAL][RX] CANCEL received during data, setting last_error ABORTED\n");
                fflush(stdout);
                return VAL_ERR_ABORTED;
            }
            else if (t == VAL_PKT_DATA_ACK)
            {
                // Receiver shouldn't get DATA_ACK during data receive; log and ignore
                VAL_LOG_DEBUG(s, "data: ignoring unexpected DATA_ACK at receiver");
            }
            else if (t == VAL_PKT_SEND_META)
            {
                // Unexpected new file; for simplicity, treat as protocol error for now
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_INVALID_STATE);
                return VAL_ERR_PROTOCOL;
            }
            else
            {
                VAL_LOG_DEBUGF(s, "data: ignoring unexpected packet type=%d", (int)t);
            }
        }
        if (!skipping && f)
            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
        if (s->config->callbacks.on_file_complete)
            s->config->callbacks.on_file_complete(clean_name, meta.sender_path, skipping ? VAL_SKIPPED : VAL_OK);
        val_metrics_inc_files_recv(s);
        // Update batch context after each file is fully handled
        files_completed += 1;
        batch_transferred += total;
        // Loop to wait for next file
    }
}

// Expose internal resume to match design doc naming
val_status_t val_internal_handle_file_resume(val_session_t *session, const char *filename, const char *sender_path,
                                             uint64_t file_size, uint64_t *out_resume_offset)
{
    return handle_file_resume(session, filename, sender_path, file_size, out_resume_offset);
}
