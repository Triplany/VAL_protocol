#include "val_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static val_status_t send_resume_response(val_session_t *s, val_resume_action_t action, uint64_t offset, uint32_t crc,
                                         uint32_t verify_len)
{
    val_resume_resp_t resp;
    resp.action = val_htole32((uint32_t)action);
    resp.resume_offset = val_htole64(offset);
    resp.verify_crc = val_htole32(crc);
    resp.verify_len = val_htole32(verify_len);
    return val_internal_send_packet(s, VAL_PKT_RESUME_RESP, &resp, sizeof(resp), offset);
}

static val_status_t handle_verification_exchange(val_session_t *s, uint64_t resume_offset_expected, uint32_t expected_crc,
                                                 uint32_t verify_len)
{
    VAL_LOG_DEBUG(s, "verify: starting exchange");
    // Wait for sender to echo back the CRC in a VERIFY packet
    uint8_t buf[128];
    uint32_t len = 0;
    uint64_t off = 0;
    val_packet_type_t t = 0;
    uint32_t to = s->config->timeouts.ack_ms ? s->config->timeouts.ack_ms : 10000u;
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    val_status_t st = VAL_OK;
    for (;;)
    {
        st = val_internal_recv_packet(s, &t, buf, sizeof(buf), &len, &off, to);
        if (st != VAL_OK)
        {
            if (st != VAL_ERR_TIMEOUT || tries == 0)
                return st;
            VAL_LOG_DEBUG(s, "verify: waiting for sender CRC");
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
        break;
    }
    VAL_LOG_DEBUG(s, "verify: received VERIFY from sender");
    VAL_LOG_INFO(s, "verify: type ok");
    VAL_LOG_DEBUG(s, "verify: len check");
    VAL_LOG_DEBUG(s, "verify: len ok");
    if (len < sizeof(val_resume_resp_t))
        return VAL_ERR_PROTOCOL;
    VAL_LOG_DEBUGF(s, "verify: extracting crc from payload len=%u", (unsigned)len);
    // Extract verify_crc from payload (offset 12 in struct)
    uint32_t their_verify_crc;
    memcpy(&their_verify_crc, buf + 12, sizeof(their_verify_crc));
    their_verify_crc = val_letoh32(their_verify_crc);
    VAL_LOG_DEBUG(s, "verify: computing result");
    int32_t result;
    if (their_verify_crc == expected_crc)
    {
        result = VAL_OK;
    }
    else
    {
        // Mismatch: derive response based on configured action
        switch (s->config->resume.on_verify_mismatch)
        {
        case VAL_RESUME_MISMATCH_SKIP_FILE:
            result = VAL_SKIPPED;
            break;
        case VAL_RESUME_MISMATCH_ABORT_FILE:
            result = VAL_ERR_ABORTED;
            break;
        case VAL_RESUME_MISMATCH_START_ZERO:
        default:
            result = VAL_ERR_RESUME_VERIFY;
            break;
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
                                                   uint32_t *out_verify_crc, uint32_t *out_verify_length)
{
    (void)sender_path; // not needed for determining resume
    val_resume_mode_t mode = session->config->resume.mode;
    val_resume_policy_t policy = session->config->resume.policy;

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
        if (policy == VAL_RESUME_POLICY_ALWAYS_SKIP || policy == VAL_RESUME_POLICY_ALWAYS_SKIP_IF_EXISTS)
        {
            // No local file exists; skip policies do not apply — proceed to receive fresh
            VAL_LOG_INFO(session, "resume: no existing file; proceeding to receive from 0");
        }
        else
        {
            VAL_LOG_INFO(session, "resume: no existing file, start at 0");
        }
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

    // Preferred: apply policy semantics when specified
    if (policy != VAL_RESUME_POLICY_NONE)
    {
        if (policy == VAL_RESUME_POLICY_ALWAYS_START_ZERO)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "policy: ALWAYS_START_ZERO -> start at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // For other policies, force CRC_VERIFY style logic regardless of legacy mode
        mode = VAL_RESUME_CRC_VERIFY;
    }

    switch (mode)
    {
    case VAL_RESUME_NONE:
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: disabled, start at 0");
        return VAL_RESUME_ACTION_START_ZERO;
    case VAL_RESUME_APPEND:
        if ((uint64_t)existing_size >= incoming_file_size)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: local file >= incoming, restart at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = (uint64_t)existing_size;
        VAL_LOG_INFO(session, "resume: append mode, continue at existing size");
        return VAL_RESUME_ACTION_START_OFFSET;
    case VAL_RESUME_CRC_VERIFY:
    {
        if (existing_size == 0)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: empty existing file, start at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // If the existing file is larger than what the sender will send, do NOT attempt to verify tail
        // against an offset beyond the sender's end-of-file. That would force the sender to seek past EOF
        // during its verify CRC computation and fail. Instead, restart from zero to overwrite and truncate.
        if ((uint64_t)existing_size > incoming_file_size)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: local file larger than incoming, restart at 0");
            return VAL_RESUME_ACTION_START_ZERO;
        }
        // If sizes are exactly equal, we can verify the tail at full size and, on success, skip retransmission.
        // This avoids resending the entire file while still protecting against content mismatch.
        if ((uint64_t)existing_size == incoming_file_size)
        {
            uint32_t verify_bytes = session->config->resume.verify_bytes ? session->config->resume.verify_bytes : 1024u;
            if (verify_bytes > (uint32_t)existing_size)
                verify_bytes = (uint32_t)existing_size;
            long seek_pos = existing_size - (long)verify_bytes;
            session->config->filesystem.fseek(session->config->filesystem.fs_context, file, seek_pos, SEEK_SET);
            if (!session->config->buffers.recv_buffer)
            {
                session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
                *out_resume_offset = 0;
                return VAL_RESUME_ACTION_START_ZERO;
            }
            size_t buf_size =
                session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
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
            *out_resume_offset = (uint64_t)existing_size; // equals incoming_file_size
            *out_verify_crc = val_internal_crc32_final(session, state);
            *out_verify_length = verify_bytes;
            if (policy == VAL_RESUME_POLICY_ALWAYS_SKIP || policy == VAL_RESUME_POLICY_ALWAYS_SKIP_IF_EXISTS)
            {
                VAL_LOG_INFO(session, "policy: SKIP_IF_EXISTS/equal-size -> verify and skip");
                return VAL_RESUME_ACTION_SKIP_FILE;
            }
            VAL_LOG_INFO(session, "resume: sizes match, crc verify requested at full size");
            return VAL_RESUME_ACTION_VERIFY_FIRST;
        }
        uint32_t verify_bytes = session->config->resume.verify_bytes ? session->config->resume.verify_bytes : 1024u;
        if (verify_bytes > (uint32_t)existing_size)
            verify_bytes = (uint32_t)existing_size;
        long seek_pos = existing_size - verify_bytes;
        session->config->filesystem.fseek(session->config->filesystem.fs_context, file, seek_pos, SEEK_SET);
        if (!session->config->buffers.recv_buffer)
        {
            session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
            *out_resume_offset = 0;
            return VAL_RESUME_ACTION_START_ZERO;
        }
        /* Stream the requested verify_bytes into the configured recv_buffer in chunks, allowing verify_bytes > packet_size. */
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
        if (policy == VAL_RESUME_POLICY_STRICT_RESUME_ONLY)
        {
            VAL_LOG_INFO(session, "policy: STRICT -> verify required before resume");
            return VAL_RESUME_ACTION_VERIFY_FIRST;
        }
        VAL_LOG_INFO(session, "resume: crc verify requested");
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
    uint32_t verify_length = 0;
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
        st = handle_verification_exchange(s, resume_offset, verify_crc, verify_length);
        VAL_LOG_INFOF(s, "resume: verify result st=%d", (int)st);
        if (st == VAL_ERR_RESUME_VERIFY)
        {
            // Honor configured mismatch action where possible
            switch (s->config->resume.on_verify_mismatch)
            {
            case VAL_RESUME_MISMATCH_START_ZERO:
                resume_offset = 0;
                VAL_LOG_INFO(s, "resume: verify mismatch, restarting at 0");
                break;
            case VAL_RESUME_MISMATCH_SKIP_FILE:
                // Not representable over current wire; fall back to START_ZERO and log
                resume_offset = 0;
                VAL_LOG_WARN(s, "resume: verify mismatch; SKIP_FILE requested but not supported on wire; restarting at 0");
                break;
            case VAL_RESUME_MISMATCH_ABORT_FILE:
                VAL_LOG_WARN(s, "resume: verify mismatch; aborting file");
                return VAL_ERR_ABORTED;
            default:
                resume_offset = 0;
                VAL_LOG_INFO(s, "resume: verify mismatch, restarting at 0");
                break;
            }
        }
        else if (st == VAL_ERR_TIMEOUT)
        {
            if (s->config->resume.policy == VAL_RESUME_POLICY_STRICT_RESUME_ONLY ||
                s->config->resume.on_verify_mismatch == VAL_RESUME_MISMATCH_ABORT_FILE)
            {
                VAL_LOG_WARN(s, "resume: verify timeout under STRICT; aborting file");
                return VAL_ERR_ABORTED;
            }
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

    // Handshake done upon public API entry; loop to receive files until EOT
    for (;;)
    {
        // Expect metadata from sender
        val_packet_type_t t = 0;
        uint32_t len = 0;
        uint64_t off = 0;
        uint32_t to_meta = s->config->timeouts.meta_ms ? s->config->timeouts.meta_ms : 60000u;
        val_status_t st = VAL_OK;
        {
            uint8_t tries = s->config->retries.meta_retries ? s->config->retries.meta_retries : 0;
            uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
            for (;;)
            {
                st = val_internal_recv_packet(s, &t, tmp, (uint32_t)P, &len, &off, to_meta);
                if (st == VAL_OK)
                    break;
                if (st != VAL_ERR_TIMEOUT || tries == 0)
                    return st;
                VAL_LOG_DEBUG(s, "recv: waiting for metadata");
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
        if (t != VAL_PKT_SEND_META || len < sizeof(val_meta_payload_t))
            return VAL_ERR_PROTOCOL;
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

        // Handle resume negotiation
        uint64_t resume_off = 0;
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

        // If resume negotiation asked to skip, we should wait for DONE and ACK it without writing
        int skipping = (resume_off >= meta.file_size) ? 1 : 0;

        // Open file for write unless we are skipping
        const char *mode = (resume_off == 0) ? "wb" : "ab";
        void *f = NULL;
        if (!skipping)
        {
            f = s->config->filesystem.fopen(s->config->filesystem.fs_context, full_output_path, mode);
            if (!f)
                return VAL_ERR_IO;
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
            uint32_t to_data = s->config->timeouts.data_ms ? s->config->timeouts.data_ms : 60000u;
            {
                uint8_t tries = s->config->retries.data_retries ? s->config->retries.data_retries : 0;
                uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
                for (;;)
                {
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
                        return st;
                    }
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
                    s->config->callbacks.on_progress(written, total);
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
            else if (t == VAL_PKT_DATA_ACK)
            {
                // Receiver shouldn't get DATA_ACK during data receive; log and ignore
                VAL_LOG_DEBUG(s, "data: ignoring unexpected DATA_ACK at receiver");
            }
            else if (t == VAL_PKT_SEND_META)
            {
                // Unexpected new file; for simplicity, treat as protocol error for now
                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
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
        // Loop to wait for next file
    }
}

// Expose internal resume to match design doc naming
val_status_t val_internal_handle_file_resume(val_session_t *session, const char *filename, const char *sender_path,
                                             uint64_t file_size, uint64_t *out_resume_offset)
{
    return handle_file_resume(session, filename, sender_path, file_size, out_resume_offset);
}
