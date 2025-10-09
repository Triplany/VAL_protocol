#include "val_wire.h"
#include "val_protocol.h"
#include <stdio.h>
#include <string.h>

static int test_header(void) {
    uint8_t buf[VAL_WIRE_HEADER_SIZE];
    val_packet_header_t in = {0};
    in.type = VAL_PKT_HELLO;
    in.wire_version = 0;
    in.reserved2 = 0x1234;
    in.payload_len = 0x89ABCDEFu;
    in.seq = 0x01020304u;
    in.offset = 0x1122334455667788ULL;
    in.header_crc = 0xA1B2C3D4u;

    val_serialize_header(&in, buf);
    val_packet_header_t out = {0};
    val_deserialize_header(buf, &out);

    return (memcmp(&in, &out, sizeof(in)) == 0) ? 0 : 1;
}

static int test_handshake(void) {
    uint8_t buf[VAL_WIRE_HANDSHAKE_SIZE];
    val_handshake_t in = {0};
    in.magic = VAL_MAGIC;
    in.version_major = VAL_VERSION_MAJOR;
    in.version_minor = VAL_VERSION_MINOR;
    in.packet_size = 1024;
    in.features = 0x11;
    in.required = 0x22;
    in.requested = 0x33;
    // New bounded-window capability exchange fields
    in.tx_max_window_packets = 16; // sender capability
    in.rx_max_window_packets = 48; // receiver capability
    in.ack_stride_packets = 100;   // preferred ACK cadence
    in.reserved_capabilities[0] = 0x5A; // exercise reserved bytes to ensure roundtrip
    in.reserved_capabilities[1] = 0xC3;
    in.reserved_capabilities[2] = 0x3C;
    in.supported_features16 = 0xAA55;
    in.required_features16 = 0x55AA;
    in.requested_features16 = 0x0F0F;
    in.reserved2 = 0xCAFEBABE;

    val_serialize_handshake(&in, buf);
    val_handshake_t out = {0};
    val_deserialize_handshake(buf, &out);

    return (memcmp(&in, &out, sizeof(in)) == 0) ? 0 : 1;
}

static int test_meta(void) {
    uint8_t buf[VAL_WIRE_META_SIZE];
    val_meta_payload_t in;
    memset(&in, 0, sizeof(in));
    strcpy(in.filename, "file.bin");
    strcpy(in.sender_path, "path/to");
    in.file_size = 0x0123456789ABCDEFULL;

    val_serialize_meta(&in, buf);
    val_meta_payload_t out;
    memset(&out, 0, sizeof(out));
    val_deserialize_meta(buf, &out);

    return (memcmp(&in, &out, sizeof(in)) == 0) ? 0 : 1;
}

static int test_resume_resp(void) {
    uint8_t buf[VAL_WIRE_RESUME_RESP_SIZE];
    val_resume_resp_t in = {0};
    in.action = 3;
    in.resume_offset = 0x1020304050607080ULL;
    in.verify_crc = 0xAABBCCDD;
    in.verify_length = 0xFEEDBEEFULL;

    val_serialize_resume_resp(&in, buf);
    val_resume_resp_t out = {0};
    val_deserialize_resume_resp(buf, &out);

    return (memcmp(&in, &out, sizeof(in)) == 0) ? 0 : 1;
}

static int test_error_payload(void) {
    uint8_t buf[VAL_WIRE_ERROR_PAYLOAD_SIZE];
    val_error_payload_t in = {0};
    in.code = -12345;
    in.detail = 0x12345678;

    val_serialize_error_payload(&in, buf);
    val_error_payload_t out = {0};
    val_deserialize_error_payload(buf, &out);

    return (in.code == out.code && in.detail == out.detail) ? 0 : 1;
}


int main(void) {
    int fails = 0;
    fails += test_header();
    fails += test_handshake();
    fails += test_meta();
    fails += test_resume_resp();
    fails += test_error_payload();
    // Legacy MODE_SYNC removed in the bounded-window protocol

    if (fails == 0) {
        printf("wire_roundtrip: PASS\n");
        return 0;
    } else {
        printf("wire_roundtrip: FAIL (%d)\n", fails);
        return 1;
    }
}
