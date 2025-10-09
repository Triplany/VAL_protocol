#ifndef VAL_WIRE_H
#define VAL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "val_byte_order.h"
#include "val_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VAL_WIRE_HEADER_SIZE 24u
#define VAL_WIRE_HANDSHAKE_SIZE 44u
#define VAL_WIRE_META_SIZE ((VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u)
#define VAL_WIRE_RESUME_RESP_SIZE 24u
#define VAL_WIRE_ERROR_PAYLOAD_SIZE 8u
#define VAL_WIRE_TRAILER_SIZE 4u

typedef struct
{
    uint8_t type;
    uint8_t wire_version;
    uint16_t reserved2;
    uint32_t payload_len;
    uint32_t seq;
    uint64_t offset;
    uint32_t header_crc;
} val_packet_header_t;

typedef struct
{
    uint32_t magic;
    uint8_t version_major;
    uint8_t version_minor;
    uint16_t reserved;
    uint32_t packet_size;
    uint32_t features;
    uint32_t required;
    uint32_t requested;
    // Repurposed flow-control capability exchange (keeps wire size the same)
    // tx_max_window_packets: maximum in-flight packets this sender can support
    // rx_max_window_packets: maximum in-flight packets this receiver can accept
    // ack_stride_packets: receiver's preferred ACK cadence (0 = once per window)
    uint16_t tx_max_window_packets;
    uint16_t rx_max_window_packets;
    uint8_t ack_stride_packets;
    uint8_t reserved_capabilities[3];
    uint16_t supported_features16;
    uint16_t required_features16;
    uint16_t requested_features16;
    uint32_t reserved2;
} val_handshake_t;

// Note: legacy streaming flags removed from handshake; flow control is bounded-window only.

typedef struct
{
    int32_t code;
    uint32_t detail;
} val_error_payload_t;

typedef struct
{
    uint32_t action;
    uint64_t resume_offset;
    uint32_t verify_crc;
    uint64_t verify_length;
} val_resume_resp_t;

void val_serialize_header(const val_packet_header_t *hdr, uint8_t *wire_data);
void val_deserialize_header(const uint8_t *wire_data, val_packet_header_t *hdr);

void val_serialize_handshake(const val_handshake_t *hs, uint8_t *wire_data);
void val_deserialize_handshake(const uint8_t *wire_data, val_handshake_t *hs);

void val_serialize_meta(const val_meta_payload_t *meta, uint8_t *wire_data);
void val_deserialize_meta(const uint8_t *wire_data, val_meta_payload_t *meta);

void val_serialize_resume_resp(const val_resume_resp_t *resp, uint8_t *wire_data);
void val_deserialize_resume_resp(const uint8_t *wire_data, val_resume_resp_t *resp);

void val_serialize_error_payload(const val_error_payload_t *payload, uint8_t *wire_data);
void val_deserialize_error_payload(const uint8_t *wire_data, val_error_payload_t *payload);

// No MODE_SYNC in bounded-window protocol


#ifdef __cplusplus
}
#endif

#endif // VAL_WIRE_H
