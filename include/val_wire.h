#ifndef VAL_WIRE_H
#define VAL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "val_byte_order.h"
#include "val_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VAL_WIRE_HEADER_SIZE 8u
#define VAL_WIRE_HANDSHAKE_SIZE 44u
#define VAL_WIRE_META_SIZE ((VAL_MAX_FILENAME + 1u) + (VAL_MAX_PATH + 1u) + 8u)
#define VAL_WIRE_RESUME_RESP_SIZE 24u
#define VAL_WIRE_VERIFY_REQ_SIZE 16u
#define VAL_WIRE_VERIFY_RESP_SIZE 8u
#define VAL_WIRE_ERROR_PAYLOAD_SIZE 8u
#define VAL_WIRE_TRAILER_SIZE 4u

// Verify request/response payload sizes (exclude header/trailer)
#define VAL_WIRE_VERIFY_REQ_PAYLOAD_SIZE 16u  // offset(8) + crc(4) + length(4)
#define VAL_WIRE_VERIFY_RESP_PAYLOAD_SIZE 8u  // status(4) + receiver_crc(4)

// Optional flags for RESUME/VERIFY (reserved for future expansion)
#define VAL_RESUMERESP_VERIFY_REQUIRED (1u << 0)
#define VAL_RESUMERESP_HAS_VERIFY_WINDOW (1u << 1)
#define VAL_VERIFY_REQUEST (1u << 0)

// New universal frame header (8 bytes total)
// Layout:
//  byte 0: type (uint8_t)
//  byte 1: flags (uint8_t)
//  byte 2-3: content_len (uint16_t LE)
//  byte 4-7: type_data (uint32_t LE)
// Trailer: 4-byte CRC32 over [header + content]

// Convenience aliases for readability
#define VAL_FRAME_HEADER_SIZE VAL_WIRE_HEADER_SIZE
#define VAL_FRAME_TRAILER_SIZE VAL_WIRE_TRAILER_SIZE

// DATA packet flags
#define VAL_DATA_OFFSET_PRESENT (1u << 0)
#define VAL_DATA_FINAL_CHUNK    (1u << 1)

// ACK packet flags
#define VAL_ACK_FEEDBACK_PRESENT (1u << 0)
#define VAL_ACK_DONE_FILE        (1u << 1)
#define VAL_ACK_EOT              (1u << 2)

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

// Flow control uses bounded-window parameters (tx_max_window_packets, rx_max_window_packets, ack_stride_packets)

typedef struct
{
    int32_t code;
    uint32_t detail;
} val_error_payload_t;

// Universal frame header helpers
void val_serialize_frame_header(uint8_t type, uint8_t flags, uint16_t content_len, uint32_t type_data, uint8_t *wiredata);
void val_deserialize_frame_header(const uint8_t *wiredata, uint8_t *type, uint8_t *flags, uint16_t *content_len, uint32_t *type_data);

void val_serialize_handshake(const val_handshake_t *hs, uint8_t *wire_data);
void val_deserialize_handshake(const uint8_t *wire_data, val_handshake_t *hs);

void val_serialize_meta(const val_meta_payload_t *meta, uint8_t *wire_data);
void val_deserialize_meta(const uint8_t *wire_data, val_meta_payload_t *meta);

void val_serialize_resume_resp(const val_resume_resp_t *resp, uint8_t *wire_data);
void val_deserialize_resume_resp(const uint8_t *wire_data, val_resume_resp_t *resp);

// VERIFY request/response helpers
void val_serialize_verify_request(uint64_t offset, uint32_t crc, uint32_t length, uint8_t *wire_data);
void val_deserialize_verify_request(const uint8_t *wire_data, uint64_t *offset, uint32_t *crc, uint32_t *length);
void val_serialize_verify_response(val_status_t result, uint32_t receiver_crc, uint8_t *wire_data);
void val_deserialize_verify_response(const uint8_t *wire_data, val_status_t *result, uint32_t *receiver_crc);

void val_serialize_error_payload(const val_error_payload_t *payload, uint8_t *wire_data);
void val_deserialize_error_payload(const uint8_t *wire_data, val_error_payload_t *payload);

#ifdef __cplusplus
}
#endif

#endif // VAL_WIRE_H
