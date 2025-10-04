#include "val_wire.h"

#include <string.h>

#include "val_protocol.h"

typedef char val_wire_assert_meta_size[(VAL_WIRE_META_SIZE == (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u + 4u) ? 1 : -1];
typedef char val_wire_assert_fname[(VAL_MAX_FILENAME == 127u) ? 1 : -1];
typedef char val_wire_assert_path[(VAL_MAX_PATH == 127u) ? 1 : -1];

void val_serialize_header(const val_packet_header_t *hdr, uint8_t *wire_data)
{
    if (!hdr || !wire_data)
        return;

    wire_data[0] = hdr->type;
    wire_data[1] = hdr->wire_version;
    VAL_PUT_LE16(wire_data + 2, hdr->reserved2);
    VAL_PUT_LE32(wire_data + 4, hdr->payload_len);
    VAL_PUT_LE32(wire_data + 8, hdr->seq);
    VAL_PUT_LE64(wire_data + 12, hdr->offset);
    VAL_PUT_LE32(wire_data + 20, hdr->header_crc);
}

void val_deserialize_header(const uint8_t *wire_data, val_packet_header_t *hdr)
{
    if (!wire_data || !hdr)
        return;

    hdr->type = wire_data[0];
    hdr->wire_version = wire_data[1];
    hdr->reserved2 = VAL_GET_LE16(wire_data + 2);
    hdr->payload_len = VAL_GET_LE32(wire_data + 4);
    hdr->seq = VAL_GET_LE32(wire_data + 8);
    hdr->offset = VAL_GET_LE64(wire_data + 12);
    hdr->header_crc = VAL_GET_LE32(wire_data + 20);
}

void val_serialize_handshake(const val_handshake_t *hs, uint8_t *wire_data)
{
    if (!hs || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, hs->magic);
    wire_data[4] = hs->version_major;
    wire_data[5] = hs->version_minor;
    VAL_PUT_LE16(wire_data + 6, hs->reserved);
    VAL_PUT_LE32(wire_data + 8, hs->packet_size);
    VAL_PUT_LE32(wire_data + 12, hs->features);
    VAL_PUT_LE32(wire_data + 16, hs->required);
    VAL_PUT_LE32(wire_data + 20, hs->requested);
    wire_data[24] = hs->max_performance_mode;
    wire_data[25] = hs->preferred_initial_mode;
    VAL_PUT_LE16(wire_data + 26, hs->mode_sync_interval);
    wire_data[28] = hs->streaming_flags;
    wire_data[29] = hs->reserved_streaming[0];
    wire_data[30] = hs->reserved_streaming[1];
    wire_data[31] = hs->reserved_streaming[2];
    VAL_PUT_LE16(wire_data + 32, hs->supported_features16);
    VAL_PUT_LE16(wire_data + 34, hs->required_features16);
    VAL_PUT_LE16(wire_data + 36, hs->requested_features16);
    wire_data[38] = 0;
    wire_data[39] = 0;
    VAL_PUT_LE32(wire_data + 40, hs->reserved2);
}

void val_deserialize_handshake(const uint8_t *wire_data, val_handshake_t *hs)
{
    if (!wire_data || !hs)
        return;

    hs->magic = VAL_GET_LE32(wire_data + 0);
    hs->version_major = wire_data[4];
    hs->version_minor = wire_data[5];
    hs->reserved = VAL_GET_LE16(wire_data + 6);
    hs->packet_size = VAL_GET_LE32(wire_data + 8);
    hs->features = VAL_GET_LE32(wire_data + 12);
    hs->required = VAL_GET_LE32(wire_data + 16);
    hs->requested = VAL_GET_LE32(wire_data + 20);
    hs->max_performance_mode = wire_data[24];
    hs->preferred_initial_mode = wire_data[25];
    hs->mode_sync_interval = VAL_GET_LE16(wire_data + 26);
    hs->streaming_flags = wire_data[28];
    hs->reserved_streaming[0] = wire_data[29];
    hs->reserved_streaming[1] = wire_data[30];
    hs->reserved_streaming[2] = wire_data[31];
    hs->supported_features16 = VAL_GET_LE16(wire_data + 32);
    hs->required_features16 = VAL_GET_LE16(wire_data + 34);
    hs->requested_features16 = VAL_GET_LE16(wire_data + 36);
    hs->reserved2 = VAL_GET_LE32(wire_data + 40);
}

void val_serialize_meta(const val_meta_payload_t *meta, uint8_t *wire_data)
{
    if (!meta || !wire_data)
        return;

    memcpy(wire_data, meta->filename, VAL_MAX_FILENAME + 1u);
    memcpy(wire_data + (VAL_MAX_FILENAME + 1u), meta->sender_path, VAL_MAX_PATH + 1u);
    VAL_PUT_LE64(wire_data + (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u), meta->file_size);
    VAL_PUT_LE32(wire_data + (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u, meta->file_crc32);
}

void val_deserialize_meta(const uint8_t *wire_data, val_meta_payload_t *meta)
{
    if (!wire_data || !meta)
        return;

    memcpy(meta->filename, wire_data, VAL_MAX_FILENAME + 1u);
    memcpy(meta->sender_path, wire_data + (VAL_MAX_FILENAME + 1u), VAL_MAX_PATH + 1u);
    meta->file_size = VAL_GET_LE64(wire_data + (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u));
    meta->file_crc32 = VAL_GET_LE32(wire_data + (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u);
}

void val_serialize_resume_resp(const val_resume_resp_t *resp, uint8_t *wire_data)
{
    if (!resp || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, resp->action);
    VAL_PUT_LE64(wire_data + 4, resp->resume_offset);
    VAL_PUT_LE32(wire_data + 12, resp->verify_crc);
    VAL_PUT_LE64(wire_data + 16, resp->verify_len);
}

void val_deserialize_resume_resp(const uint8_t *wire_data, val_resume_resp_t *resp)
{
    if (!wire_data || !resp)
        return;

    resp->action = VAL_GET_LE32(wire_data + 0);
    resp->resume_offset = VAL_GET_LE64(wire_data + 4);
    resp->verify_crc = VAL_GET_LE32(wire_data + 12);
    resp->verify_len = VAL_GET_LE64(wire_data + 16);
}

void val_serialize_error_payload(const val_error_payload_t *payload, uint8_t *wire_data)
{
    if (!payload || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, (uint32_t)payload->code);
    VAL_PUT_LE32(wire_data + 4, payload->detail);
}

void val_deserialize_error_payload(const uint8_t *wire_data, val_error_payload_t *payload)
{
    if (!wire_data || !payload)
        return;

    payload->code = (int32_t)VAL_GET_LE32(wire_data + 0);
    payload->detail = VAL_GET_LE32(wire_data + 4);
}

void val_serialize_mode_sync(const val_mode_sync_t *sync, uint8_t *wire_data)
{
    if (!sync || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, sync->current_mode);
    VAL_PUT_LE32(wire_data + 4, sync->sequence);
    VAL_PUT_LE32(wire_data + 8, sync->consecutive_errors);
    VAL_PUT_LE32(wire_data + 12, sync->consecutive_success);
    VAL_PUT_LE32(wire_data + 16, sync->flags);
}

void val_deserialize_mode_sync(const uint8_t *wire_data, val_mode_sync_t *sync)
{
    if (!wire_data || !sync)
        return;

    sync->current_mode = VAL_GET_LE32(wire_data + 0);
    sync->sequence = VAL_GET_LE32(wire_data + 4);
    sync->consecutive_errors = VAL_GET_LE32(wire_data + 8);
    sync->consecutive_success = VAL_GET_LE32(wire_data + 12);
    sync->flags = VAL_GET_LE32(wire_data + 16);
}

void val_serialize_mode_sync_ack(const val_mode_sync_ack_t *ack, uint8_t *wire_data)
{
    if (!ack || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, ack->ack_sequence);
    VAL_PUT_LE32(wire_data + 4, ack->agreed_mode);
    VAL_PUT_LE32(wire_data + 8, ack->receiver_errors);
}

void val_deserialize_mode_sync_ack(const uint8_t *wire_data, val_mode_sync_ack_t *ack)
{
    if (!wire_data || !ack)
        return;

    ack->ack_sequence = VAL_GET_LE32(wire_data + 0);
    ack->agreed_mode = VAL_GET_LE32(wire_data + 4);
    ack->receiver_errors = VAL_GET_LE32(wire_data + 8);
}
