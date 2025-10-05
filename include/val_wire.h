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
#define VAL_WIRE_MODE_SYNC_SIZE 20u
#define VAL_WIRE_MODE_SYNC_ACK_SIZE 12u
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
    uint8_t max_performance_mode;
    uint8_t preferred_initial_mode;
    uint16_t mode_sync_interval;
    uint8_t streaming_flags;
    uint8_t reserved_streaming[3];
    uint16_t supported_features16;
    uint16_t required_features16;
    uint16_t requested_features16;
    uint32_t reserved2;
} val_handshake_t;

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
    uint64_t verify_len;
} val_resume_resp_t;

typedef struct
{
    uint32_t current_mode;
    uint32_t sequence;
    uint32_t consecutive_errors;
    uint32_t consecutive_success;
    uint32_t flags;
} val_mode_sync_t;

typedef struct
{
    uint32_t ack_sequence;
    uint32_t agreed_mode;
    uint32_t receiver_errors;
} val_mode_sync_ack_t;

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

void val_serialize_mode_sync(const val_mode_sync_t *sync, uint8_t *wire_data);
void val_deserialize_mode_sync(const uint8_t *wire_data, val_mode_sync_t *sync);

void val_serialize_mode_sync_ack(const val_mode_sync_ack_t *ack, uint8_t *wire_data);
void val_deserialize_mode_sync_ack(const uint8_t *wire_data, val_mode_sync_ack_t *ack);


#ifdef __cplusplus
}
#endif

#endif // VAL_WIRE_H
