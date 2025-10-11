#include "val_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Receiver data path for bounded-window protocol (no streaming overlay, no mode sync).

// In the redesigned protocol, we communicate resume intent via a cumulative ACK
// carrying the receiver's starting offset (or file_size to skip, or 0 to restart).
static val_status_t send_resume_ack(val_session_t *s, uint64_t resume_offset)
{
    // DATA_ACK with offset in header fields
    return val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, resume_offset);
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
    int n = val_internal_join_path(target_path, target_path_size, session->output_directory, sanitized);
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
    (void)filename; // currently unused; reserved for future diagnostics
    switch (action)
    {
    case VAL_VALIDATION_ACCEPT:
        return VAL_OK;
    case VAL_VALIDATION_SKIP:
        // No wire I/O here; caller will send appropriate ACK with file_size
        return VAL_OK;
    case VAL_VALIDATION_ABORT:
        // Caller will handle signaling; just return aborted status
        return VAL_ERR_ABORTED;
    default:
        VAL_SET_PROTOCOL_ERROR(session, VAL_ERROR_DETAIL_INVALID_STATE);
        return VAL_ERR_PROTOCOL;
    }
}

// Verification round-trip removed in redesign; receiver decides policy locally and communicates via ACK offset

static inline uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi)
{
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static val_resume_action_t determine_resume_action(val_session_t *session, const char *filename, const char *sender_path,
                                                   uint64_t incoming_file_size, uint64_t *out_resume_offset,
                                                   uint32_t *out_verify_crc, uint64_t *out_verify_length)
{
    (void)sender_path; // not needed for determining resume
    val_resume_mode_t mode = session->config->resume.mode;

    char full_output_path[512];
    if (session->output_directory[0])
        val_internal_join_path(full_output_path, sizeof(full_output_path), session->output_directory, filename);
    else
        snprintf(full_output_path, sizeof(full_output_path), "%s", filename);

    void *file = session->config->filesystem.fopen(session->config->filesystem.fs_context, full_output_path, "rb");
    if (!file)
    {
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: no existing file, start at 0");
        return VAL_RESUME_START_ZERO;
    }

    session->config->filesystem.fseek(session->config->filesystem.fs_context, file, 0, SEEK_END);
    int64_t existing_size_l = session->config->filesystem.ftell(session->config->filesystem.fs_context, file);
    if (existing_size_l < 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }
    uint64_t existing_size = (uint64_t)existing_size_l;

    // Policy evaluation by mode
    if (mode == VAL_RESUME_NEVER)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: disabled, start at 0");
        return VAL_RESUME_START_ZERO;
    }
    if (mode == VAL_RESUME_SKIP_EXISTING)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        if (existing_size > 0)
        {
            *out_resume_offset = 0;
            VAL_LOG_INFO(session, "resume: SKIP_EXISTING -> skipping existing file");
            return VAL_RESUME_SKIP_FILE;
        }
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }

    // TAIL mode
    if (existing_size == 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: no local bytes, start at 0");
        return VAL_RESUME_START_ZERO;
    }
    // Local larger than incoming => treat as mismatch per policy
    if (existing_size > incoming_file_size)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        if (session->config->resume.mismatch_skip)
        {
            VAL_LOG_INFO(session, "resume: local > incoming -> skip file (policy)");
            *out_resume_offset = 0;
            *out_verify_crc = 0;
            *out_verify_length = 0;
            return VAL_RESUME_SKIP_FILE;
        }
        VAL_LOG_INFO(session, "resume: local > incoming -> start at 0 (policy)");
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }

    if (!session->config->buffers.recv_buffer)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }
    size_t buf_size = session->effective_packet_size ? session->effective_packet_size : session->config->buffers.packet_size;
    if (buf_size == 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }

    // Compute tail verification window
    uint64_t default_cap = 8ull * 1024ull * 1024ull; // 8 MiB default (single profile for now)
    uint64_t req_cap = session->config->resume.tail_cap_bytes ? (uint64_t)session->config->resume.tail_cap_bytes : default_cap;
    const uint64_t ABS_MAX = 256ull * 1024ull * 1024ull; // 256 MiB hard clamp
    uint64_t cap = clamp_u64(req_cap, 1ull, ABS_MAX);
    uint64_t min_verify = session->config->resume.min_verify_bytes ? (uint64_t)session->config->resume.min_verify_bytes : 0ull;
    uint64_t verify_len = existing_size < cap ? existing_size : cap;
    if (min_verify > 0 && verify_len < min_verify)
        verify_len = (existing_size < min_verify) ? existing_size : min_verify;

    uint32_t tail_crc = 0;
    long start_pos = (long)(existing_size - verify_len);
    if (val_internal_crc32_region(session, file, (uint64_t)start_pos, verify_len, &tail_crc) != VAL_OK)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_START_ZERO;
    }
    session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
    *out_resume_offset = existing_size;
    *out_verify_crc = tail_crc;
    *out_verify_length = verify_len;
    VAL_LOG_INFO(session, "resume: tail crc verify requested");
    return VAL_RESUME_VERIFY_FIRST;
}

static val_status_t handle_file_resume(val_session_t *s, const char *filename, const char *sender_path, uint64_t file_size,
                                       uint64_t *resume_offset_out)
{
    uint64_t resume_offset = 0;
    uint32_t verify_crc = 0;
    uint64_t verify_length = 0;
    val_resume_action_t action =
        determine_resume_action(s, filename, sender_path, file_size, &resume_offset, &verify_crc, &verify_length);
    // Map legacy actions to immediate ACK offset signaling
    val_status_t st = VAL_OK;
    if (action == VAL_RESUME_SKIP_FILE)
    {
        // Communicate skip by ACKing file_size
        st = send_resume_ack(s, file_size);
        if (st != VAL_OK) return st;
        *resume_offset_out = file_size;
        VAL_LOG_INFO(s, "resume: receiver chose SKIP_FILE; awaiting DONE");
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, sender_path, file_size, file_size);
        return VAL_OK;
    }
    if (action == VAL_RESUME_ABORT_FILE)
    {
        // Communicate abort by ACKing 0 and then expect CANCEL/ERROR at higher layer
        st = send_resume_ack(s, 0);
        if (st != VAL_OK) return st;
        return VAL_ERR_ABORTED;
    }
    if (action == VAL_RESUME_START_ZERO)
    {
        // Special-case: when resume is explicitly disabled, do NOT send a pre-data DATA_ACK.
        // This lets the first DATA_ACK during the actual data loop represent real progress
        // and ensures tests that drop the first ACK affect a data ACK.
        if (s->config->resume.mode == VAL_RESUME_NEVER)
        {
            *resume_offset_out = 0;
            VAL_LOG_INFO(s, "resume: disabled (NEVER), no pre-data ACK; start at 0");
            if (s->config->callbacks.on_file_start)
                s->config->callbacks.on_file_start(filename, sender_path, file_size, 0);
            return VAL_OK;
        }
        // Default START_ZERO behavior (non-NEVER modes): inform sender explicitly via DATA_ACK(0)
        st = send_resume_ack(s, 0);
        if (st != VAL_OK) return st;
        *resume_offset_out = 0;
        VAL_LOG_INFO(s, "resume: start at 0");
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, sender_path, file_size, 0);
        return VAL_OK;
    }
    if (action == VAL_RESUME_VERIFY_FIRST)
    {
        // Simplify: resume from local existing_size without extra VERIFY round-trip.
        // If policy prefers skip on mismatch for larger-than-incoming, it was handled earlier.
        st = send_resume_ack(s, resume_offset);
        if (st != VAL_OK) return st;
        *resume_offset_out = resume_offset;
        VAL_LOG_INFOF(s, "resume: tail policy -> offset=%llu", (unsigned long long)resume_offset);
        if (s->config->callbacks.on_file_start)
            s->config->callbacks.on_file_start(filename, sender_path, file_size, resume_offset);
        return VAL_OK;
    }
    // Should not reach here
    return VAL_ERR_PROTOCOL;
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
                VAL_HEALTH_RECORD_OPERATION(s);
                val_status_t health = val_internal_check_health(s);
                if (health != VAL_OK)
                    return health;
                    
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
                {
                    // Break to evaluate packet type (SEND_META/EOT/CANCEL/…) below
                    break;
                }
                if (st != VAL_ERR_TIMEOUT || tries == 0)
                {
                    if (st == VAL_ERR_TIMEOUT && tries == 0)
                    {
                        VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_META);
                        // Terminal metadata wait timeout
                        val_metrics_inc_timeout_hard(s);
                    }
                    return st;
                }
                VAL_LOG_DEBUG(s, "recv: waiting for metadata");
                VAL_HEALTH_RECORD_RETRY(s);
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
            return VAL_ERR_ABORTED;
        }
        if (t != VAL_PKT_SEND_META || len < VAL_WIRE_META_SIZE)
        {
            VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
            return VAL_ERR_PROTOCOL;
        }
        val_meta_payload_t meta;
        val_deserialize_meta(tmp, &meta);
        VAL_LOG_INFO(s, "recv: received SEND_META");

        // Determine output path (receiver controls)
        char clean_name[VAL_MAX_FILENAME + 1];
        val_clean_filename(meta.filename, clean_name, sizeof(clean_name));
        char full_output_path[512];
        if (s->output_directory[0])
            val_internal_join_path(full_output_path, sizeof(full_output_path), s->output_directory, clean_name);
        else
            snprintf(full_output_path, sizeof(full_output_path), "%s", clean_name);

        int validation_skipped = 0;
        uint64_t resume_off = 0; // may be set by validation skip path
        int predecide_skip = 0; // if validator says SKIP, we still must participate in RESUME negotiation
        int skipping = 0;
        if (s->config->resume.mode == VAL_RESUME_NEVER)
        {
            /* If the peer (sender) actually issues a RESUME_REQ despite our local
             * NEVER setting (sender may be configured to negotiate), we must honor
             * that control path and reply with RESUME_RESP so the sender doesn't
             * block waiting for a response. Try a single recv for RESUME_REQ using
             * the META timeout; if none arrives, fall back to the legacy pre-data
             * DATA_ACK behaviour.
             */
            // Peek once for a RESUME_REQ. Do not copy payload into a tiny buffer; use header-only receive
            // to avoid spurious INVALID_ARG(PAYLOAD_SIZE) if a larger control/DATA frame arrives here.
            uint32_t rlen = 0; uint64_t roff = 0; val_packet_type_t rt = 0;
            uint32_t to_req = val_internal_get_timeout(s, VAL_OP_META);
            val_status_t rcs = val_internal_recv_packet(s, &rt, NULL, 0, &rlen, &roff, to_req);
            if (rcs == VAL_OK && rt == VAL_PKT_RESUME_REQ)
            {
                VAL_LOG_INFO(s, "receiver(NEVER): observed RESUME_REQ, replying with RESUME_RESP");
                uint64_t rec_resume_off = 0; uint32_t verify_crc = 0; uint64_t verify_len = 0;
                val_resume_action_t action = determine_resume_action(s, clean_name, meta.sender_path, meta.file_size,
                                                                     &rec_resume_off, &verify_crc, &verify_len);
                VAL_LOG_INFOF(s, "receiver: decided resume action=%d offset=%llu", (int)action, (unsigned long long)rec_resume_off);
                val_resume_resp_t rr;
                rr.action = (uint32_t)action;
                rr.resume_offset = rec_resume_off;
                rr.verify_crc = verify_crc;
                rr.verify_length = verify_len;
                uint8_t rr_wire[VAL_WIRE_RESUME_RESP_SIZE];
                val_serialize_resume_resp(&rr, rr_wire);
                    VAL_LOG_INFO(s, "receiver: sending RESUME_RESP (NEVER path)");
                    /* TRACE: expose resume response fields for diagnosis */
                    VAL_LOG_TRACEF(s, "resume_resp: action=%u resume_off=%llu verify_crc=0x%08x verify_len=%llu",
                                   (unsigned)rr.action, (unsigned long long)rr.resume_offset,
                                   (unsigned)rr.verify_crc, (unsigned long long)rr.verify_length);
                    /* Also append a small diagnostic record to disk so test harness can inspect it */
                    {
                        FILE *tf = fopen("resume_trace.log", "a");
                        if (tf)
                        {
                            fprintf(tf, "RECV_RESUME_RESP action=%u resume_off=%llu verify_crc=0x%08x verify_len=%llu\n",
                                    (unsigned)rr.action, (unsigned long long)rr.resume_offset,
                                    (unsigned)rr.verify_crc, (unsigned long long)rr.verify_length);
                            fclose(tf);
                        }
                    }
                val_status_t send_st = val_internal_send_packet(s, VAL_PKT_RESUME_RESP, rr_wire, (uint32_t)sizeof(rr_wire), 0);
                if (send_st != VAL_OK)
                {
                    VAL_LOG_ERRORF(s, "receiver: failed to send RESUME_RESP st=%d", (int)send_st);
                    return send_st;
                }
                if (action == VAL_RESUME_SKIP_FILE)
                {
                    validation_skipped = 1;
                    skipping = 1;
                    resume_off = meta.file_size;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
                }
                else if (action == VAL_RESUME_START_ZERO)
                {
                    resume_off = 0;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, 0);
                }
                else if (action == VAL_RESUME_START_OFFSET)
                {
                    resume_off = rec_resume_off;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
                }
                else if (action == VAL_RESUME_VERIFY_FIRST)
                {
                    // For VERIFY_FIRST we'll follow the existing VERIFY request handling path
                    uint8_t vbuf[VAL_WIRE_VERIFY_REQ_PAYLOAD_SIZE]; uint32_t vlen = 0; uint64_t voff = 0; val_packet_type_t vt = 0;
                    uint32_t tov = val_internal_get_timeout(s, VAL_OP_VERIFY);
                    uint8_t vtries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
                    uint32_t vback = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
                    val_status_t vw = val_internal_wait_verify_request_rx(s, tov, vtries, vback,
                                                                           rr_wire, (uint32_t)sizeof(rr_wire),
                                                                           vbuf, (uint32_t)sizeof(vbuf), &vlen, &voff);
                    if (vw != VAL_OK) return vw;
                    uint64_t verify_offset = 0; uint32_t sender_crc = 0; uint32_t verify_length = 0;
                    val_deserialize_verify_request(vbuf, &verify_offset, &sender_crc, &verify_length);
                    void *lf = s->config->filesystem.fopen(s->config->filesystem.fs_context, full_output_path, "rb");
                    if (!lf)
                    {
                        uint8_t resp[VAL_WIRE_VERIFY_RESP_PAYLOAD_SIZE];
                        val_serialize_verify_response(VAL_ERR_RESUME_VERIFY, 0, resp);
                        (void)val_internal_send_packet(s, VAL_PKT_VERIFY, resp, (uint32_t)sizeof(resp), (uint64_t)VAL_ERR_RESUME_VERIFY);
                        return VAL_ERR_IO;
                    }
                    uint32_t local_crc = 0; val_status_t crcst = val_internal_crc32_region(s, lf, verify_offset, verify_length, &local_crc);
                    s->config->filesystem.fclose(s->config->filesystem.fs_context, lf);
                    val_status_t result = (crcst == VAL_OK && local_crc == sender_crc) ? VAL_OK : VAL_ERR_RESUME_VERIFY;
                    uint8_t resp[VAL_WIRE_VERIFY_RESP_PAYLOAD_SIZE];
                    val_serialize_verify_response(result, local_crc, resp);
                    (void)val_internal_send_packet(s, VAL_PKT_VERIFY, resp, (uint32_t)sizeof(resp), (uint64_t)result);
                    if (result == VAL_OK)
                    {
                        resume_off = rec_resume_off;
                        if (s->config->callbacks.on_file_start)
                            s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
                    }
                    else
                    {
                        resume_off = 0;
                        if (s->config->callbacks.on_file_start)
                            s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, 0);
                    }
                }
                else if (action == VAL_RESUME_ABORT_FILE)
                {
                    (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                    return VAL_ERR_ABORTED;
                }
                VAL_LOG_INFO(s, "recv: resume handled (NEVER path)");
                // --- Metadata validation after negotiation ---
                char target_path[VAL_MAX_PATH * 2 + 8];
                val_status_t pr = val_construct_target_path(s, clean_name, target_path, sizeof(target_path));
                if (pr != VAL_OK)
                {
                    (void)val_handle_validation_action(s, VAL_VALIDATION_ABORT, clean_name);
                    return VAL_ERR_INVALID_ARG;
                }
                val_validation_action_t act = val_validate_metadata(s, &meta, target_path);
                val_status_t hr = val_handle_validation_action(s, act, clean_name);
                if (act == VAL_VALIDATION_ABORT)
                {
                    (void)hr;
                    (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                    return VAL_ERR_ABORTED;
                }
                if (act == VAL_VALIDATION_SKIP)
                {
                    // Honor skip: send ACK with file_size to tell sender to skip
                    val_status_t ack_st = send_resume_ack(s, meta.file_size);
                    if (ack_st != VAL_OK)
                        return ack_st;
                    skipping = 1;
                    resume_off = meta.file_size;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
                }
            }
            else if (rcs == VAL_ERR_TIMEOUT)
            {
                /* No RESUME_REQ observed from sender: fall back to legacy behaviour
                 * where receiver pre-ACKs a starting offset (0 or file_size for skip).
                 * Do not increment soft-timeout metrics here; treat this as an
                 * opportunistic wait. */
                val_status_t st = VAL_OK;
                if (predecide_skip)
                {
                    st = send_resume_ack(s, meta.file_size);
                    resume_off = meta.file_size;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
                }
                else
                {
                    st = send_resume_ack(s, 0);
                    resume_off = 0;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
                }
                if (st != VAL_OK)
                    return st;
                // --- Metadata validation after negotiation ---
                char target_path[VAL_MAX_PATH * 2 + 8];
                val_status_t pr = val_construct_target_path(s, clean_name, target_path, sizeof(target_path));
                if (pr != VAL_OK)
                {
                    (void)val_handle_validation_action(s, VAL_VALIDATION_ABORT, clean_name);
                    return VAL_ERR_INVALID_ARG;
                }
                val_validation_action_t act = val_validate_metadata(s, &meta, target_path);
                val_status_t hr = val_handle_validation_action(s, act, clean_name);
                if (act == VAL_VALIDATION_ABORT)
                {
                    (void)hr;
                    (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                    return VAL_ERR_ABORTED;
                }
                if (act == VAL_VALIDATION_SKIP)
                {
                    // Skip already signaled via resume_ack above; set flags
                    skipping = 1;
                    resume_off = meta.file_size;
                }
            }
            else
            {
                /* Any other receive error or unexpected packet: propagate (CANCEL/ERROR)
                 * or fall back to legacy ACK behaviour conservatively. */
                if (rcs == VAL_ERR_CRC) return rcs;
                if (rcs == VAL_ERR_IO) return rcs;
                if (rcs == VAL_ERR_ABORTED) return VAL_ERR_ABORTED;
                if (rcs == VAL_ERR_PROTOCOL) return VAL_ERR_PROTOCOL;
                /* For any other case, fall back to pre-data ACK to avoid deadlock */
                if (predecide_skip)
                {
                    val_status_t st = send_resume_ack(s, meta.file_size);
                    if (st != VAL_OK) return st;
                    resume_off = meta.file_size;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
                }
                else
                {
                    val_status_t st = send_resume_ack(s, 0);
                    if (st != VAL_OK) return st;
                    resume_off = 0;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, 0);
                }
                // --- Metadata validation after negotiation ---
                char target_path[VAL_MAX_PATH * 2 + 8];
                val_status_t pr = val_construct_target_path(s, clean_name, target_path, sizeof(target_path));
                if (pr != VAL_OK)
                {
                    (void)val_handle_validation_action(s, VAL_VALIDATION_ABORT, clean_name);
                    return VAL_ERR_INVALID_ARG;
                }
                val_validation_action_t act = val_validate_metadata(s, &meta, target_path);
                val_status_t hr = val_handle_validation_action(s, act, clean_name);
                if (act == VAL_VALIDATION_ABORT)
                {
                    (void)hr;
                    (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                    return VAL_ERR_ABORTED;
                }
                if (act == VAL_VALIDATION_SKIP)
                {
                    // Skip already signaled via resume_ack above; set flags
                    skipping = 1;
                    resume_off = meta.file_size;
                }
            }
        }
        else
        {
            // Wait for RESUME_REQ from sender, then decide action and reply with RESUME_RESP
            // Do not copy payload into a tiny buffer here; use header-only receive to avoid
            // spurious INVALID_ARG(PAYLOAD_SIZE) if a larger control/DATA frame arrives while
            // we're waiting for RESUME_REQ.
            uint32_t rlen=0; uint64_t roff=0; val_packet_type_t rt=0;
            uint32_t to_req = val_internal_get_timeout(s, VAL_OP_META);
            uint8_t rtries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
            uint32_t rback = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
            for (;;)
            {
                val_status_t rcs = val_internal_recv_packet(s, &rt, NULL, 0, &rlen, &roff, to_req);
                if (rcs == VAL_OK)
                {
                    VAL_LOG_DEBUGF(s, "receiver: got packet type=%d while waiting for RESUME_REQ", (int)rt);
                    if (rt == VAL_PKT_RESUME_REQ)
                    {
                        VAL_LOG_INFO(s, "receiver: received RESUME_REQ, processing");
                        break;
                    }
                    if (rt == VAL_PKT_CANCEL) return VAL_ERR_ABORTED;
                    if (rt == VAL_PKT_ERROR) return VAL_ERR_PROTOCOL;
                    // Ignore benign
                    continue;
                }
                if (rcs != VAL_ERR_TIMEOUT || rtries == 0)
                {
                    if (rcs == VAL_ERR_TIMEOUT) VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_META);
                    return rcs;
                }
                if (rback && s->config->system.delay_ms) s->config->system.delay_ms(rback);
                if (rback) rback <<= 1; --rtries;
            }
            // Decide resume action based on local filesystem
            uint64_t rec_resume_off = 0; uint32_t verify_crc = 0; uint64_t verify_len = 0;
            val_resume_action_t action = determine_resume_action(s, clean_name, meta.sender_path, meta.file_size,
                                                                 &rec_resume_off, &verify_crc, &verify_len);
            VAL_LOG_INFOF(s, "receiver: decided resume action=%d offset=%llu", (int)action, (unsigned long long)rec_resume_off);
            // Build RESUME_RESP payload
            val_resume_resp_t rr;
            rr.action = (uint32_t)action;
            rr.resume_offset = rec_resume_off;
            rr.verify_crc = verify_crc;
            rr.verify_length = verify_len;
            uint8_t rr_wire[VAL_WIRE_RESUME_RESP_SIZE];
            val_serialize_resume_resp(&rr, rr_wire);
            VAL_LOG_INFO(s, "receiver: sending RESUME_RESP");
            val_status_t send_st = val_internal_send_packet(s, VAL_PKT_RESUME_RESP, rr_wire, (uint32_t)sizeof(rr_wire), 0);
            if (send_st != VAL_OK)
            {
                VAL_LOG_ERRORF(s, "receiver: failed to send RESUME_RESP st=%d", (int)send_st);
                return send_st;
            }
            VAL_LOG_INFO(s, "receiver: RESUME_RESP sent successfully");
            if (action == VAL_RESUME_SKIP_FILE)
            {
                // Resume policy says skip file - preserve existing file
                skipping = 1;
                resume_off = meta.file_size;
                if (s->config->callbacks.on_file_start)
                    s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
            }
            else if (action == VAL_RESUME_START_ZERO)
            {
                resume_off = 0;
                if (s->config->callbacks.on_file_start)
                    s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, 0);
            }
            else if (action == VAL_RESUME_START_OFFSET)
            {
                resume_off = rec_resume_off;
                if (s->config->callbacks.on_file_start)
                    s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
            }
            else if (action == VAL_RESUME_VERIFY_FIRST)
            {
                // Wait for VERIFY request and respond with our CRC result
                uint8_t vbuf[VAL_WIRE_VERIFY_REQ_PAYLOAD_SIZE]; uint32_t vlen=0; uint64_t voff=0; val_packet_type_t vt=0;
                uint32_t tov = val_internal_get_timeout(s, VAL_OP_VERIFY);
                uint8_t vtries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
                uint32_t vback = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
                // Use helper that also resends RESUME_RESP on stray RESUME_REQ
                val_status_t vw = val_internal_wait_verify_request_rx(s, tov, vtries, vback,
                                                                       rr_wire, (uint32_t)sizeof(rr_wire),
                                                                       vbuf, (uint32_t)sizeof(vbuf), &vlen, &voff);
                if (vw != VAL_OK)
                    return vw;
                uint64_t verify_offset = 0; uint32_t sender_crc = 0; uint32_t verify_length = 0;
                val_deserialize_verify_request(vbuf, &verify_offset, &sender_crc, &verify_length);
                // Compute local CRC over requested window
                // Open file for read - use full_output_path which includes directory
                void *lf = s->config->filesystem.fopen(s->config->filesystem.fs_context, full_output_path, "rb");
                if (!lf)
                {
                    uint8_t resp[VAL_WIRE_VERIFY_RESP_PAYLOAD_SIZE];
                    val_serialize_verify_response(VAL_ERR_RESUME_VERIFY, 0, resp);
                    (void)val_internal_send_packet(s, VAL_PKT_VERIFY, resp, (uint32_t)sizeof(resp), (uint64_t)VAL_ERR_RESUME_VERIFY);
                    return VAL_ERR_IO;
                }
                uint32_t local_crc = 0; val_status_t crcst = val_internal_crc32_region(s, lf, verify_offset, verify_length, &local_crc);
                s->config->filesystem.fclose(s->config->filesystem.fs_context, lf);
                val_status_t result = (crcst == VAL_OK && local_crc == sender_crc) ? VAL_OK : VAL_ERR_RESUME_VERIFY;
                // If verification failed but policy requests skipping on mismatch, advertise SKIPPED
                if (result != VAL_OK && s->config->resume.mismatch_skip)
                {
                    VAL_LOG_INFO(s, "verify: mismatch and mismatch_skip enabled -> reporting SKIPPED");
                    result = VAL_SKIPPED;
                }
                uint8_t resp[VAL_WIRE_VERIFY_RESP_PAYLOAD_SIZE];
                val_serialize_verify_response(result, local_crc, resp);
                (void)val_internal_send_packet(s, VAL_PKT_VERIFY, resp, (uint32_t)sizeof(resp), (uint64_t)result);
                if (result == VAL_OK)
                {
                    resume_off = rec_resume_off;
                    if (s->config->callbacks.on_file_start)
                        s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, resume_off);
                }
                else
                {
                    if (result == VAL_SKIPPED)
                    {
                        resume_off = meta.file_size;
                        if (s->config->callbacks.on_file_start)
                            s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, meta.file_size);
                        validation_skipped = 1;
                        skipping = 1;
                    }
                    else
                    {
                        resume_off = 0;
                        if (s->config->callbacks.on_file_start)
                            s->config->callbacks.on_file_start(clean_name, meta.sender_path, meta.file_size, 0);
                    }
                }
            }
            else if (action == VAL_RESUME_ABORT_FILE)
            {
                (void)val_internal_send_error(s, VAL_ERR_ABORTED, 0);
                return VAL_ERR_ABORTED;
            }
            VAL_LOG_INFO(s, "recv: resume handled");
            skipping = (resume_off >= meta.file_size) ? 1 : 0;
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
        if (skipping)
        {
            // If skipping, don't enter data loop; just wait for DONE and ACK it.
            for (;;)
            {
                t = 0; len = 0; off = 0;
                uint32_t to_done = val_internal_get_timeout(s, VAL_OP_DONE_ACK);
                uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
                uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
                val_status_t stw;
                // No separate else: negotiation always runs; 'skipping' set if SKIP_FILE
                for (;;)
                {
                    stw = val_internal_recv_packet(s, &t, tmp, (uint32_t)P, &len, &off, to_done);
                    if (stw == VAL_OK) break;
                    if (stw != VAL_ERR_TIMEOUT || tries == 0)
                        return stw;
                    if (backoff && s->config->system.delay_ms) s->config->system.delay_ms(backoff);
                    if (backoff) backoff <<= 1; --tries;
                }
                if (t == VAL_PKT_DONE)
                {
                    (void)val_internal_send_packet(s, VAL_PKT_DONE_ACK, NULL, 0, total);
                    break;
                }
                if (t == VAL_PKT_EOT)
                {
                    (void)val_internal_send_packet(s, VAL_PKT_EOT_ACK, NULL, 0, 0);
                    return VAL_OK;
                }
                if (t == VAL_PKT_CANCEL)
                {
                    val_internal_set_last_error(s, VAL_ERR_ABORTED, 0);
                    return VAL_ERR_ABORTED;
                }
                // Ignore any incoming DATA/DATA_ACK/NAK while skipping
            }
            if (s->config->callbacks.on_file_complete)
                s->config->callbacks.on_file_complete(clean_name, meta.sender_path, VAL_SKIPPED);
            val_metrics_inc_files_recv(s);
            files_completed += 1;
            batch_transferred += total;
            continue; // next file
        }
        // Compute CRC across only the newly received bytes; no re-CRC of existing bytes.
        uint32_t crc_state = val_crc32_init_state();
    // ACK coalescing state (per-file)
        uint32_t pkts_since_ack = 0;
    // Heartbeat removed: ACKs are emitted based on stride and progress only
    // Conservative ACK cadence: ACK every packet to guarantee progress under jitter/reordering.
    // We can make this adaptive later; for now, keep it simple and robust.
    uint32_t ack_stride = 1u;
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
                    VAL_HEALTH_RECORD_OPERATION(s);
                    val_status_t health = val_internal_check_health(s);
                    if (health != VAL_OK)
                    {
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        return health;
                    }
                        
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
                        VAL_LOG_WARN(s, "[RX] Local cancel detected in data loop, last_error=ABORTED");
                        return VAL_ERR_ABORTED;
                    }
                    st = val_internal_recv_packet(s, &t, tmp, (uint32_t)P, &len, &off, to_data);
                    if (st == VAL_OK)
                    {
                        // Per-packet trace: keep at TRACE to avoid slowing tests under DEBUG builds
                        VAL_LOG_TRACEF(s, "data: got packet type=%d len=%u off=%llu", (int)t, (unsigned)len,
                                       (unsigned long long)off);
                        break;
                    }
                    if ((st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC) || tries == 0)
                    {
                        s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        if (st == VAL_ERR_TIMEOUT)
                        {
                            VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_DATA);
                            // Terminal data receive timeout
                            val_metrics_inc_timeout_hard(s);
                        }
                        else if (st == VAL_ERR_CRC)
                        {
                            VAL_SET_CRC_ERROR(s, VAL_ERROR_DETAIL_PACKET_CORRUPT);
                        }
                        return st;
                    }
                    // Soft timeout tracking removed - only hard timeouts are meaningful
                    VAL_HEALTH_RECORD_RETRY(s);
                    if (backoff && s->config->system.delay_ms)
                        s->config->system.delay_ms(backoff);
                    if (backoff)
                        backoff <<= 1;
                    --tries;
                }
            }
            if (t == VAL_PKT_DATA)
            {
                // Determine effective offset: UINT64_MAX indicates implied offset (current 'written')
                uint64_t eff_off = (off == UINT64_MAX) ? written : off;
                // Determine ordering before mutating 'written'
                int in_order = (eff_off == written) ? 1 : 0;
                int dup_or_overlap = (eff_off < written) ? 1 : 0;
                // Cumulative ACK semantics
                if (in_order)
                {
                    // Normal in-order chunk
                    if (!skipping && len)
                    {
                        size_t w = s->config->filesystem.fwrite(s->config->filesystem.fs_context, tmp, 1, len, f);
                        if (w != len)
                        {
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                            val_internal_set_error_detailed(s, VAL_ERR_IO, VAL_ERROR_DETAIL_DISK_FULL);
                            return VAL_ERR_IO;
                        }
                        crc_state = val_crc32_update_state(crc_state, tmp, len);
                    }
                    // If this completes the file exactly, force an ACK immediately regardless of stride
                    uint8_t completes_file = (written + len >= total) ? 1u : 0u;
                    written += len;
                    if (completes_file)
                    {
                        VAL_LOG_TRACEF(s, "data: final chunk received, forcing DATA_ACK off=%llu",
                                       (unsigned long long)written);
                        val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                        if (st2 != VAL_OK)
                        {
                            if (!skipping && f)
                                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                            return st2;
                        }
                        pkts_since_ack = 0;
                        // Continue to next iteration to process DONE
                        continue;
                    }
                }
                else if (dup_or_overlap)
                {
                    // Duplicate or overlap: ignore write; simply reaffirm current position with DATA_ACK
                    // This avoids NAK/ACK oscillation when sender has already advanced.
                    VAL_LOG_TRACEF(s, "data: duplicate/overlap -> reaffirm DATA_ACK off=%llu",
                                   (unsigned long long)written);
                    (void)val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                }
                else /* sender_ahead */
                {
                    // Sender is ahead; immediately NAK with next expected offset
                    VAL_LOG_TRACEF(s, "data: sender ahead -> sending DATA_NAK (next_expected=%llu)",
                                   (unsigned long long)written);
                    uint32_t reason = 0x1u; // GAP
                    // Use new NAK format: low32 in header (via offset param), content carries [high32, reason, reserved]
                    uint8_t payload[4]; // pass reason only; core will build full 12-byte content
                    VAL_PUT_LE32(payload, reason);
                    (void)val_internal_send_packet_ex(s, VAL_PKT_DATA_NAK, payload, sizeof(payload), written, 0);
                    // Also send an ACK at our current high-water to help the sender resync
                    (void)val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
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
                        VAL_LOG_WARN(s, "[RX] Local cancel after progress, aborting before DATA_ACK");
                        return VAL_ERR_ABORTED;
                    }
                }
                // ACK policy:
                // - Immediate ACK on duplicate/overlap or sender-ahead (reaffirm position).
                // - For in-order chunks, ACK once per receiver-preferred stride (ack_stride).
                // - Additionally, if we reached the end of file (written >= total), force an ACK so sender can proceed to DONE.
                int force_ack = 0; // NAK covers negative feedback; reserve ACK for cadence/EOF
                if (in_order)
                    pkts_since_ack++;
                if (in_order && written >= total)
                    force_ack = 1;
                int streaming = 0; // streaming removed in bounded-window flow control
                // Log state for first packet and every 5000 packets for debugging
                static uint32_t debug_pkt_count = 0;
                debug_pkt_count++;
                if (debug_pkt_count == 1 || debug_pkt_count % 5000 == 0) {
                    VAL_LOG_INFOF(s, "ACK policy (#%u): streaming=%d, pkts_since_ack=%u ack_stride=%u",
                                  debug_pkt_count, streaming,
                                  pkts_since_ack, (unsigned)ack_stride);
                }
                if (force_ack || pkts_since_ack >= ack_stride)
                {
                    // ACK cadence trace moved to TRACE to reduce console overhead during tests
                    VAL_LOG_TRACEF(s, "data: sending DATA_ACK off=%llu", (unsigned long long)written);
                    val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                    if (st2 != VAL_OK)
                    {
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        return st2;
                    }
                    pkts_since_ack = 0;
                }
            }
            else if (t == VAL_PKT_DONE)
            {
                // Protocol no longer validates whole-file CRC at DONE; rely on packet-level integrity and resume verify
                (void)crc_state; // kept for potential metrics/debug, not used for validation
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
