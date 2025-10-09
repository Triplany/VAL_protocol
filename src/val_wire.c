#include "val_wire.h"

#include <string.h>

#include "val_protocol.h"
#include <assert.h>

// Portable compile-time assertions: prefer C11 _Static_assert, fall back if unavailable
#ifndef VAL_STATIC_ASSERT
#  if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#    define VAL_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#  elif defined(static_assert)
#    define VAL_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#  else
#    define VAL_STATIC_ASSERT_JOIN(a, b) a##b
#    define VAL_STATIC_ASSERT_XJOIN(a, b) VAL_STATIC_ASSERT_JOIN(a, b)
#    define VAL_STATIC_ASSERT(cond, msg) typedef char VAL_STATIC_ASSERT_XJOIN(val_static_assert_, __LINE__)[(cond) ? 1 : -1]
#  endif
#endif

// Compile-time validation of wire sizes and constants
VAL_STATIC_ASSERT(VAL_WIRE_META_SIZE == (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u,
                  "VAL_WIRE_META_SIZE must equal filename+path+size");
VAL_STATIC_ASSERT(VAL_MAX_FILENAME == 127u, "VAL_MAX_FILENAME must be 127");
VAL_STATIC_ASSERT(VAL_MAX_PATH == 127u, "VAL_MAX_PATH must be 127");

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
    // Flow-control capabilities (repurposed from legacy mode/streaming fields)
    VAL_PUT_LE16(wire_data + 24, hs->tx_max_window_packets);
    VAL_PUT_LE16(wire_data + 26, hs->rx_max_window_packets);
    wire_data[28] = hs->ack_stride_packets;
    wire_data[29] = hs->reserved_capabilities[0];
    wire_data[30] = hs->reserved_capabilities[1];
    wire_data[31] = hs->reserved_capabilities[2];
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
    hs->tx_max_window_packets = VAL_GET_LE16(wire_data + 24);
    hs->rx_max_window_packets = VAL_GET_LE16(wire_data + 26);
    hs->ack_stride_packets = wire_data[28];
    hs->reserved_capabilities[0] = wire_data[29];
    hs->reserved_capabilities[1] = wire_data[30];
    hs->reserved_capabilities[2] = wire_data[31];
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
}

void val_deserialize_meta(const uint8_t *wire_data, val_meta_payload_t *meta)
{
    if (!wire_data || !meta)
        return;

    memcpy(meta->filename, wire_data, VAL_MAX_FILENAME + 1u);
    memcpy(meta->sender_path, wire_data + (VAL_MAX_FILENAME + 1u), VAL_MAX_PATH + 1u);
    meta->file_size = VAL_GET_LE64(wire_data + (VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u));
}

void val_serialize_resume_resp(const val_resume_resp_t *resp, uint8_t *wire_data)
{
    if (!resp || !wire_data)
        return;

    VAL_PUT_LE32(wire_data + 0, resp->action);
    VAL_PUT_LE64(wire_data + 4, resp->resume_offset);
    VAL_PUT_LE32(wire_data + 12, resp->verify_crc);
    VAL_PUT_LE64(wire_data + 16, resp->verify_length);
}

void val_deserialize_resume_resp(const uint8_t *wire_data, val_resume_resp_t *resp)
{
    if (!wire_data || !resp)
        return;

    resp->action = VAL_GET_LE32(wire_data + 0);
    resp->resume_offset = VAL_GET_LE64(wire_data + 4);
    resp->verify_crc = VAL_GET_LE32(wire_data + 12);
    resp->verify_length = VAL_GET_LE64(wire_data + 16);
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

// MODE_SYNC payloads have been removed in bounded-window protocol.
