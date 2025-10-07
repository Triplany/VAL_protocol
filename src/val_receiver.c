#include "val_internal.h"
#include "val_scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Streaming: send sparse heartbeat DATA_ACK (liveness) only when idle.
// Conditions:
//  - streaming is allowed locally and peer is engaged
//  - no DATA received recently (>= interval_ms)
//  - no ACK sent recently (>= interval_ms)
//  - last heartbeat was >= interval_ms ago
// Heartbeat moved to scheduler module (val_rx_maybe_send_heartbeat)

static val_status_t send_resume_response(val_session_t *s, val_resume_action_t action, uint64_t offset, uint32_t crc,
                                         uint64_t verify_length)
{
    val_resume_resp_t resp;
    resp.action = (uint32_t)action;
    resp.resume_offset = offset;
    resp.verify_crc = crc;
    resp.verify_length = verify_length;
    uint8_t resp_wire[VAL_WIRE_RESUME_RESP_SIZE];
    val_serialize_resume_resp(&resp, resp_wire);
    return val_internal_send_packet(s, VAL_PKT_RESUME_RESP, resp_wire, VAL_WIRE_RESUME_RESP_SIZE, offset);
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
        return send_resume_response(session, VAL_RESUME_ACTION_SKIP_FILE, 0, 0, 0);
    case VAL_VALIDATION_ABORT:
        return send_resume_response(session, VAL_RESUME_ACTION_ABORT_FILE, 0, 0, 0);
    default:
        VAL_SET_PROTOCOL_ERROR(session, VAL_ERROR_DETAIL_INVALID_STATE);
        return VAL_ERR_PROTOCOL;
    }
}

static val_status_t handle_verification_exchange(val_session_t *s, uint64_t resume_offset_expected, uint32_t expected_crc,
                                                 uint64_t verify_length, val_status_t on_match_status)
{
    VAL_LOG_DEBUG(s, "verify: starting exchange");
    // Wait for sender to echo back the CRC in a VERIFY packet using centralized helper
    uint8_t buf[128];
    uint32_t len = 0; uint64_t off = 0;
    uint32_t to = val_internal_get_timeout(s, VAL_OP_VERIFY);
    uint8_t tries = s->config->retries.ack_retries ? s->config->retries.ack_retries : 0;
    uint32_t backoff = s->config->retries.backoff_ms_base ? s->config->retries.backoff_ms_base : 0;
    uint32_t t0v = s->config->system.get_ticks_ms();
    s->timing.in_retransmit = 0;
    val_status_t st = val_internal_wait_verify_request_rx(s, to, tries, backoff,
                                                          /*resume_resp_payload*/ NULL, /*resume_resp_len*/ 0,
                                                          buf, (uint32_t)sizeof(buf), &len, &off);
    if (st != VAL_OK)
    {
        if (st == VAL_ERR_TIMEOUT)
            VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_ACK);
        return st;
    }
    if (t0v && !s->timing.in_retransmit)
    {
        uint32_t now = s->config->system.get_ticks_ms();
        val_internal_record_rtt(s, now - t0v);
    }
    VAL_LOG_DEBUG(s, "verify: received VERIFY from sender");
    VAL_LOG_INFO(s, "verify: type ok");
    VAL_LOG_DEBUG(s, "verify: len check");
    VAL_LOG_DEBUG(s, "verify: len ok");
    if (len < VAL_WIRE_RESUME_RESP_SIZE)
    {
        VAL_SET_PROTOCOL_ERROR(s, VAL_ERROR_DETAIL_MALFORMED_PKT);
        return VAL_ERR_PROTOCOL;
    }
    VAL_LOG_DEBUGF(s, "verify: extracting crc from payload len=%u", (unsigned)len);
    val_resume_resp_t vr;
    val_deserialize_resume_resp(buf, &vr);
    uint32_t their_verify_crc = vr.verify_crc;
    // Optional sanity logs to help diagnose mismatches
    VAL_LOG_DEBUGF(s, "verify: expected off=%llu len=%llu, got off=%llu len=%llu", (unsigned long long)resume_offset_expected,
                   (unsigned long long)verify_length, (unsigned long long)vr.resume_offset, (unsigned long long)vr.verify_length);
    VAL_LOG_DEBUG(s, "verify: computing result");
    int32_t result;
    if (their_verify_crc == expected_crc)
    {
        // On match, return the caller-provided status (VAL_OK to resume, or VAL_SKIPPED to skip)
        result = (int32_t)on_match_status;
    }
    else
    {
        // Mismatch: result depends on new policy flag mismatch_skip (1=skip, 0=restart)
        result = s->config->resume.mismatch_skip ? VAL_SKIPPED : VAL_ERR_RESUME_VERIFY;
    }
    uint8_t status_wire[sizeof(uint32_t)];
    VAL_PUT_LE32(status_wire, (uint32_t)result);
    VAL_LOG_DEBUG(s, "verify: before sending");
    VAL_LOG_DEBUG(s, "verify: sending status to sender");
    val_status_t send_st = val_internal_send_packet(s, VAL_PKT_VERIFY, status_wire, sizeof(status_wire), 0);
    VAL_LOG_DEBUG(s, "verify: send_st");
    VAL_LOG_DEBUG(s, "verify: sent response to sender");
    if (send_st != VAL_OK)
        return send_st;
    // Propagate verification outcome to caller so it can adjust resume offset
    return (val_status_t)result;
}

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
        return VAL_RESUME_ACTION_START_ZERO;
    }

    session->config->filesystem.fseek(session->config->filesystem.fs_context, file, 0, SEEK_END);
    long existing_size_l = session->config->filesystem.ftell(session->config->filesystem.fs_context, file);
    if (existing_size_l < 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        return VAL_RESUME_ACTION_START_ZERO;
    }
    uint64_t existing_size = (uint64_t)existing_size_l;

    // Policy evaluation by mode
    if (mode == VAL_RESUME_NEVER)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: disabled, start at 0");
        return VAL_RESUME_ACTION_START_ZERO;
    }
    if (mode == VAL_RESUME_SKIP_EXISTING)
    {
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

    // TAIL mode
    if (existing_size == 0)
    {
        session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
        *out_resume_offset = 0;
        VAL_LOG_INFO(session, "resume: no local bytes, start at 0");
        return VAL_RESUME_ACTION_START_ZERO;
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
            return VAL_RESUME_ACTION_SKIP_FILE;
        }
        VAL_LOG_INFO(session, "resume: local > incoming -> start at 0 (policy)");
        *out_resume_offset = 0;
        return VAL_RESUME_ACTION_START_ZERO;
    }

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
        return VAL_RESUME_ACTION_START_ZERO;
    }
    session->config->filesystem.fclose(session->config->filesystem.fs_context, file);
    *out_resume_offset = existing_size;
    *out_verify_crc = tail_crc;
    *out_verify_length = verify_len;
    VAL_LOG_INFO(session, "resume: tail crc verify requested");
    return VAL_RESUME_ACTION_VERIFY_FIRST;
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
        // On match: if verify covered entire file length, skip; else resume from offset.
        val_status_t on_match = (verify_length == file_size) ? VAL_SKIPPED : VAL_OK;
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
            if (s->config->resume.mismatch_skip)
            {
                resume_offset = file_size; // treat as skip
                VAL_LOG_INFO(s, "resume: verify timeout -> skipping file (policy)");
                if (s->config->callbacks.on_file_start)
                    s->config->callbacks.on_file_start(filename, sender_path, file_size, file_size);
            }
            else
            {
                resume_offset = 0;
                VAL_LOG_INFO(s, "resume: verify timeout -> restarting at 0 (policy)");
            }
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
                    // While waiting for SEND_META, tolerate and process benign control packets like MODE_SYNC.
                    if (t == VAL_PKT_MODE_SYNC)
                    {
                        if (len >= VAL_WIRE_MODE_SYNC_SIZE)
                        {
                            val_mode_sync_t ms;
                            val_deserialize_mode_sync(tmp, &ms);
                            s->peer_tx_mode = (val_tx_mode_t)ms.current_mode;
                            s->peer_streaming_engaged = (ms.flags & 1u) ? 1 : 0;
                            // Best-effort ACK so peer can track health
                            val_mode_sync_ack_t ack;
                            memset(&ack, 0, sizeof(ack));
                            ack.ack_sequence = ms.sequence;
                            ack.agreed_mode = ms.current_mode;
                            ack.receiver_errors = s->consecutive_errors;
                            uint8_t ack_wire[VAL_WIRE_MODE_SYNC_ACK_SIZE];
                            val_serialize_mode_sync_ack(&ack, ack_wire);
                            (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC_ACK, ack_wire, VAL_WIRE_MODE_SYNC_ACK_SIZE, 0);
                        }
                        // Keep waiting for metadata/EOT/CANCEL
                        continue;
                    }
                    if (t == VAL_PKT_MODE_SYNC_ACK)
                    {
                        // Benign; simply continue waiting for metadata
                        continue;
                    }
                    // Break to evaluate packet type (SEND_META/EOT/CANCEL/…) below
                    break;
                }
                if (st != VAL_ERR_TIMEOUT || tries == 0)
                {
                    if (st == VAL_ERR_TIMEOUT && tries == 0)
                        VAL_SET_TIMEOUT_ERROR(s, VAL_ERROR_DETAIL_TIMEOUT_META);
                    return st;
                }
                VAL_LOG_DEBUG(s, "recv: waiting for metadata");
                val_metrics_inc_timeout(s);
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
        // Compute CRC across only the newly received bytes; no re-CRC of existing bytes.
        uint32_t crc_state = val_internal_crc32_init(s);
    // ACK coalescing state (per-file)
        uint32_t pkts_since_ack = 0;
    // Streaming heartbeat state (per-file): send sparse ACKs only when idle
    uint32_t heartbeat_last_ms = 0;
    uint32_t last_ack_sent_ms = 0;   // timestamp of last DATA_ACK we sent
    uint32_t last_data_rx_ms = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0; // last DATA receipt
        // Map peer's current TX mode (window rung) to a window size used as the ACK stride
        // We cannot ACK less frequently than once per window, otherwise sender will stall at in-flight cap.
        uint32_t ack_stride = val_rx_ack_stride_from_mode(s->peer_tx_mode);
        // In streaming mode, keep ACKs coalesced once per peer window to avoid sender stalling; heartbeats continue.
        if (s->peer_streaming_engaged)
        {
            VAL_LOG_INFOF(s, "streaming: peer engaged; ACK cadence coalesced to once per %u packets",
                          (unsigned)ack_stride);
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
                    VAL_HEALTH_RECORD_OPERATION(s);
                    val_status_t health = val_internal_check_health(s);
                    if (health != VAL_OK)
                    {
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        return health;
                    }
                        
                    // Streaming: sparse heartbeat DATA_ACK (liveness) only when idle
                    val_rx_maybe_send_heartbeat(s, written, 3000u, &heartbeat_last_ms, &last_ack_sent_ms, last_data_rx_ms);
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
                if (s->config->system.get_ticks_ms)
                    last_data_rx_ms = s->config->system.get_ticks_ms();
                // Determine ordering before mutating 'written'
                int in_order = (off == written) ? 1 : 0;
                int dup_or_overlap = (off < written) ? 1 : 0;
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
                        crc_state = val_internal_crc32_update(s, crc_state, tmp, len);
                    }
                    // If this completes the file exactly, force an ACK immediately regardless of stride
                    uint8_t completes_file = (written + len >= total) ? 1u : 0u;
                    written += len;
                    if (completes_file)
                    {
                        VAL_LOG_DEBUGF(s, "data: final chunk received, forcing DATA_ACK off=%llu",
                                       (unsigned long long)written);
                        val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                        if (st2 != VAL_OK)
                        {
                            if (!skipping && f)
                                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                            return st2;
                        }
                        if (s->config->system.get_ticks_ms)
                            last_ack_sent_ms = s->config->system.get_ticks_ms();
                        pkts_since_ack = 0;
                        // Continue to next iteration to process DONE
                        continue;
                    }
                }
                else if (dup_or_overlap)
                {
                    // Duplicate or overlap: ignore write; simply reaffirm current position with DATA_ACK
                    // This avoids NAK/ACK oscillation when sender has already advanced.
                    VAL_LOG_DEBUGF(s, "data: duplicate/overlap -> reaffirm DATA_ACK off=%llu",
                                   (unsigned long long)written);
                    (void)val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                    if (s->config->system.get_ticks_ms)
                        last_ack_sent_ms = s->config->system.get_ticks_ms();
                }
                else /* sender_ahead */
                {
                    // Sender is ahead; immediately NAK with next expected offset
                    VAL_LOG_DEBUGF(s, "data: sender ahead -> sending DATA_NAK (next_expected=%llu)",
                                   (unsigned long long)written);
                    uint32_t reason = 0x1u; // GAP
                    uint8_t payload[8 + 4];
                    VAL_PUT_LE64(payload, (uint64_t)written);
                    VAL_PUT_LE32(payload + 8, reason);
                    (void)val_internal_send_packet(s, VAL_PKT_DATA_NAK, payload, sizeof(payload), 0);
                    // Also send an ACK at our current high-water to help the sender resync
                    (void)val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                    if (s->config->system.get_ticks_ms)
                        last_ack_sent_ms = s->config->system.get_ticks_ms();
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
                // - For in-order chunks, ACK once per peer window (ack_stride).
                // - Additionally, if we reached the end of file (written >= total), force an ACK so sender can proceed to DONE.
                int force_ack = 0; // NAK covers negative feedback; reserve ACK for cadence/EOF
                if (in_order)
                    pkts_since_ack++;
                if (in_order && written >= total)
                    force_ack = 1;
                int streaming = (s->recv_streaming_allowed && s->peer_streaming_engaged) ? 1 : 0;
                // ACK policy when streaming:
                // - Still ACK at least once per peer window (ack_stride) to keep sender's pipeline moving.
                // - Also force an ACK at EOF.
                if (streaming)
                {
                    // In streaming mode, do NOT ACK per window; only ACK on EOF (force_ack)
                    if (force_ack)
                    {
                        VAL_LOG_DEBUGF(s, "data: streaming DATA_ACK off=%llu", (unsigned long long)written);
                        val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                        if (st2 != VAL_OK)
                        {
                            if (!skipping && f)
                                s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                            return st2;
                        }
                        if (s->config->system.get_ticks_ms)
                            last_ack_sent_ms = s->config->system.get_ticks_ms();
                        pkts_since_ack = 0;
                    }
                }
                else if (force_ack || pkts_since_ack >= ack_stride)
                {
                    VAL_LOG_DEBUGF(s, "data: sending DATA_ACK off=%llu", (unsigned long long)written);
                    val_status_t st2 = val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, written);
                    if (st2 != VAL_OK)
                    {
                        if (!skipping && f)
                            s->config->filesystem.fclose(s->config->filesystem.fs_context, f);
                        return st2;
                    }
                    if (s->config->system.get_ticks_ms)
                        last_ack_sent_ms = s->config->system.get_ticks_ms();
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
            else if (t == VAL_PKT_MODE_SYNC)
            {
                // Peer informs us of its current TX mode; update and ACK
                if (len >= VAL_WIRE_MODE_SYNC_SIZE)
                {
                    val_mode_sync_t ms;
                    val_deserialize_mode_sync(tmp, &ms);
                    // current_mode field is native enum value in payload
                    s->peer_tx_mode = (val_tx_mode_t)ms.current_mode;
                    // bit0 indicates peer engaged streaming
                    uint8_t prev_streaming = s->peer_streaming_engaged;
                    s->peer_streaming_engaged = (ms.flags & 1u) ? 1 : 0;
                    // Adapt ACK coalescing stride dynamically to peer's reported window rung
                    ack_stride = val_rx_ack_stride_from_mode(s->peer_tx_mode);
                    // If peer is in streaming mode, we still ACK once per window (no suppression) to keep pipeline moving.
                    if (s->peer_streaming_engaged)
                    {
                        // no change to ack_stride; it already reflects the peer's window rung
                    }
                    // Emit a one-line log on transition for visibility
                    if (!prev_streaming && s->peer_streaming_engaged)
                    {
                        VAL_LOG_INFOF(s, "streaming: peer engaged; ACK cadence coalesced to once per %u packets",
                                      (unsigned)ack_stride);
                    }
                    else if (prev_streaming && !s->peer_streaming_engaged)
                    {
                        VAL_LOG_INFOF(s, "streaming: peer disengaged; restoring regular ACK cadence once per %u packets",
                                      (unsigned)ack_stride);
                    }
                    pkts_since_ack = 0; // start a fresh stride after mode change
                    // Reply with MODE_SYNC_ACK (best-effort); not used as heartbeat
                    val_mode_sync_ack_t ack;
                    memset(&ack, 0, sizeof(ack));
                    ack.ack_sequence = ms.sequence;
                    ack.agreed_mode = ms.current_mode;
                    ack.receiver_errors = s->consecutive_errors;
                    uint8_t ack_wire[VAL_WIRE_MODE_SYNC_ACK_SIZE];
                    val_serialize_mode_sync_ack(&ack, ack_wire);
                    (void)val_internal_send_packet(s, VAL_PKT_MODE_SYNC_ACK, ack_wire, VAL_WIRE_MODE_SYNC_ACK_SIZE, 0);
                }
                // Continue receiving data
                continue;
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
