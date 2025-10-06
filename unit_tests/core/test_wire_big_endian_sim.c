#include "val_wire.h"
#include "val_protocol.h"
#include <stdio.h>
#include <string.h>

static int roundtrip_all(void) {
    int fails = 0;
    // Header
    {
        uint8_t buf[VAL_WIRE_HEADER_SIZE];
        val_packet_header_t in = {0}, out = {0};
        in.type = VAL_PKT_HELLO;
        in.wire_version = 0;
        in.reserved2 = 0x2211;
        in.payload_len = 0x11223344u;
        in.seq = 0x90ABCDEFu;
        in.offset = 0x0A0B0C0D0E0F1011ULL;
        in.header_crc = 0x55667788u;
        val_serialize_header(&in, buf);
        val_deserialize_header(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    // Handshake
    {
        uint8_t buf[VAL_WIRE_HANDSHAKE_SIZE];
        val_handshake_t in = {0}, out = {0};
        in.magic = VAL_MAGIC;
        in.version_major = VAL_VERSION_MAJOR;
        in.version_minor = VAL_VERSION_MINOR;
        in.packet_size = 2048;
        in.features = 0x01020304;
        in.required = 0xA5A5A5A5;
        in.requested = 0x5A5A5A5A;
        in.max_performance_mode = VAL_TX_WINDOW_32;
        in.preferred_initial_mode = VAL_TX_WINDOW_8;
        in.mode_sync_interval = 250;
        in.streaming_flags = 0x3C;
        in.supported_features16 = 0xBEEF;
        in.required_features16 = 0xFEED;
        in.requested_features16 = 0xAA55;
        in.reserved2 = 0x01020304;
        val_serialize_handshake(&in, buf);
        val_deserialize_handshake(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    // Meta
    {
        uint8_t buf[VAL_WIRE_META_SIZE];
        val_meta_payload_t in; memset(&in, 0, sizeof(in));
        strcpy(in.filename, "bebe.bin");
        strcpy(in.sender_path, "be/path");
        in.file_size = 0x0102030405060708ULL;
        val_serialize_meta(&in, buf);
        val_meta_payload_t out; memset(&out, 0, sizeof(out));
        val_deserialize_meta(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    // Resume resp
    {
        uint8_t buf[VAL_WIRE_RESUME_RESP_SIZE];
        val_resume_resp_t in = {0}, out = {0};
        in.action = 2;
        in.resume_offset = 0x1122334455667788ULL;
        in.verify_crc = 0x01020304;
    in.verify_length = 0x0A0B0C0D0E0F1011ULL;
        val_serialize_resume_resp(&in, buf);
        val_deserialize_resume_resp(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    // Error payload
    {
        uint8_t buf[VAL_WIRE_ERROR_PAYLOAD_SIZE];
        val_error_payload_t in = {0}, out = {0};
        in.code = -7;
        in.detail = 0xABCDEF12;
        val_serialize_error_payload(&in, buf);
        val_deserialize_error_payload(buf, &out);
        if (!(in.code == out.code && in.detail == out.detail)) fails++;
    }
    // Mode sync
    {
        uint8_t buf[VAL_WIRE_MODE_SYNC_SIZE];
        val_mode_sync_t in = {0}, out = {0};
        in.current_mode = VAL_TX_WINDOW_4;
        in.sequence = 7;
        in.consecutive_errors = 1;
    in.consecutive_successes = 9;
        in.flags = 0xCAFEBABE;
        val_serialize_mode_sync(&in, buf);
        val_deserialize_mode_sync(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    // Mode sync ack
    {
        uint8_t buf[VAL_WIRE_MODE_SYNC_ACK_SIZE];
        val_mode_sync_ack_t in = {0}, out = {0};
        in.ack_sequence = 1234;
        in.agreed_mode = VAL_TX_WINDOW_2;
        in.receiver_errors = 5;
        val_serialize_mode_sync_ack(&in, buf);
        val_deserialize_mode_sync_ack(buf, &out);
        if (memcmp(&in, &out, sizeof(in)) != 0) fails++;
    }
    return fails;
}

int main(void) {
    int fails = roundtrip_all();
    if (fails == 0) {
        printf("wire_be_sim: PASS\n");
        return 0;
    } else {
        printf("wire_be_sim: FAIL (%d)\n", fails);
        return 1;
    }
}
