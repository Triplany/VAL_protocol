# VAL Protocol Message Format Reference

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance. Verify against `include/val_protocol.h` and `src/val_internal.h`.

---

## Overview

This document provides detailed specifications of all packet types and their wire formats in VAL Protocol.

## General Frame Structure

All VAL packets follow the same frame structure:

```
┌──────────────┬────────────────────┬──────────┬────────────┐
│   Header     │      Payload       │ Padding  │  Trailer   │
│  (24 bytes)  │  (0..payload_len)  │ (0..N)   │  (4 bytes) │
└──────────────┴────────────────────┴──────────┴────────────┘

Total Size = 24 + payload_len + padding + 4
```

- **Header**: Fixed 24-byte structure (see below)
- **Payload**: Variable-length data (0 to max_payload)
- **Padding**: Zero bytes to align trailer
- **Trailer**: 4-byte CRC32 of entire packet

**Endianness**: All multi-byte integers are **little-endian** on wire.

---

## Header Format

**Structure** (24 bytes):

```c
struct val_packet_header_t {
    uint8_t  type;          // Offset 0: Packet type
    uint8_t  wire_version;  // Offset 1: Wire format version (must be 0)
    uint16_t reserved2;     // Offset 2: Reserved (must be 0)
    uint32_t payload_len;   // Offset 4: Payload length in bytes
    uint32_t seq;           // Offset 8: Sequence number
    uint64_t offset;        // Offset 12: File offset or ACK offset
    uint32_t header_crc;    // Offset 20: CRC of bytes 0-19
};  // Total: 24 bytes
```

**Field Details:**

- **type**: Packet type from `val_packet_type_t` enum
- **wire_version**: Protocol wire format version; **must be 0** in current spec
- **reserved2**: Reserved for future use; **must be 0**
- **payload_len**: Number of valid payload bytes (0 for header-only packets)
- **seq**: Monotonically increasing per file (resets for each file)
- **offset**: Context-dependent:
  - For DATA: file offset of this chunk
  - For DATA_ACK: next expected byte offset (cumulative)
  - For RESUME_REQ/RESP: resume offset
  - For others: typically 0
- **header_crc**: CRC-32 of header bytes 0-19 (excludes this field and trailer)

**CRC Computation**:
- Polynomial: 0xEDB88320 (CRC-32/IEEE 802.3)
- Initial: 0xFFFFFFFF
- Final XOR: 0xFFFFFFFF
- Computed over header bytes 0-19
- Stored in little-endian format

---

## Packet Types

### HELLO (type=1)

**Purpose**: Session negotiation and capability exchange

**Direction**: Bidirectional (both sender and receiver send HELLO)

**Payload**: `val_handshake_t` (52 bytes)

```c
struct val_handshake_t {
    uint32_t magic;                    // 0x56414C00 ("VAL\0" in LE)
    uint8_t  version_major;            // Protocol major version (0)
    uint8_t  version_minor;            // Protocol minor version (7)
    uint16_t reserved;                 // Reserved (0)
    uint32_t packet_size;              // Requested/supported MTU
    uint32_t features;                 // Supported optional features
    uint32_t required;                 // Required features from peer
    uint32_t requested;                // Requested features from peer
    uint8_t  max_performance_mode;     // Max TX mode (val_tx_mode_t)
    uint8_t  preferred_initial_mode;   // Preferred initial TX mode
    uint16_t mode_sync_interval;       // Reserved (0)
    uint8_t  streaming_flags;          // Bit 0: can stream, Bit 1: accept streaming
    uint8_t  reserved_streaming[3];    // Reserved (0)
    uint16_t supported_features16;     // Reserved (0)
    uint16_t required_features16;      // Reserved (0)
    uint16_t requested_features16;     // Reserved (0)
    uint32_t reserved2;                // Reserved (0)
};  // 52 bytes
```

**Negotiation**:
- Both sides send HELLO with their capabilities
- Effective packet_size = min(sender_size, receiver_size)
- Effective features = intersection of supported features
- Effective TX mode = most conservative of both max_performance_modes
- Streaming allowed only if both sides agree (streaming_flags)

**Wire Format Example**:
```
Header:
  type: 01
  wire_version: 00
  reserved2: 00 00
  payload_len: 34 00 00 00  (52 in LE)
  seq: 00 00 00 00
  offset: 00 00 00 00 00 00 00 00
  header_crc: XX XX XX XX

Payload (52 bytes):
  magic: 00 4C 41 56           ("VAL\0" in LE)
  version_major: 00
  version_minor: 07
  reserved: 00 00
  packet_size: 00 10 00 00     (4096 in LE)
  features: 00 00 00 00        (no optional features)
  required: 00 00 00 00
  requested: 00 00 00 00
  max_performance_mode: 40     (64)
  preferred_initial_mode: 10   (16)
  mode_sync_interval: 00 00
  streaming_flags: 03          (can stream, accept streaming)
  reserved_streaming: 00 00 00
  supported_features16: 00 00
  required_features16: 00 00
  requested_features16: 00 00
  reserved2: 00 00 00 00

Trailer: XX XX XX XX
```

---

### SEND_META (type=2)

**Purpose**: Send file metadata (name, size, path)

**Direction**: Sender → Receiver

**Payload**: `val_meta_payload_t` (264 bytes)

```c
struct val_meta_payload_t {
    char     filename[128];    // Sanitized basename (UTF-8, null-terminated)
    char     sender_path[128]; // Original path hint (UTF-8, null-terminated)
    uint64_t file_size;        // File size in bytes (LE)
    // Removed file_crc32: not used by protocol
};  // 264 bytes
```

**Field Details**:
- **filename**: Sanitized basename only (no directories)
- **sender_path**: Original path hint (informational; receiver must not use for output path)
- **file_size**: Total file size in bytes
// Note: Whole-file CRC is not part of SEND_META. Integrity is ensured via packet CRCs and optional resume tail verify.

**Security Note**: Receiver must sanitize `filename` and never use `sender_path` directly to prevent path traversal.

---

### RESUME_REQ (type=3)

**Purpose**: Sender requests resume options

**Direction**: Sender → Receiver

**Payload**: None (header-only packet)

**Semantics**:
- Sender asks receiver to evaluate resume possibilities
- Receiver checks local file state and responds with RESUME_RESP

---

### RESUME_RESP (type=4)

**Purpose**: Receiver responds with resume decision

**Direction**: Receiver → Sender

**Payload**: `val_resume_resp_t` (24 bytes)

```c
struct val_resume_resp_t {
    uint32_t action;         // val_resume_action_t (LE)
    uint64_t resume_offset;  // Resume from this offset (LE)
    uint32_t verify_crc;     // Expected CRC for verification (LE)
    uint64_t verify_len;     // Length of verification region (LE)
};  // 24 bytes
```

**Resume Actions**:
```c
typedef enum {
    VAL_RESUME_ACTION_START_ZERO   = 0,  // Start from beginning
    VAL_RESUME_ACTION_START_OFFSET = 1,  // Resume from resume_offset
    VAL_RESUME_ACTION_VERIFY_FIRST = 2,  // CRC verification required
    VAL_RESUME_ACTION_SKIP_FILE    = 3,  // Skip this file entirely
    VAL_RESUME_ACTION_ABORT_FILE   = 4,  // Abort transfer
} val_resume_action_t;
```

**Action Details**:

| Action | resume_offset | verify_crc | verify_len | Meaning |
|--------|---------------|------------|------------|---------|
| START_ZERO | 0 | 0 | 0 | Start fresh from beginning |
| START_OFFSET | N | 0 | 0 | Resume from offset N (no verification) |
| VERIFY_FIRST | N | CRC | LEN | Verify CRC of LEN bytes at offset N |
| SKIP_FILE | 0 | 0 | 0 | Skip this file (already complete) |
| ABORT_FILE | 0 | 0 | 0 | Abort transfer (validation failure) |

---

### DATA (type=5)

**Purpose**: File data chunk

**Direction**: Sender → Receiver

**Payload**: File data bytes (1 to max_payload_size)

**Header Fields**:
- **seq**: Monotonic sequence number (starts at 0 for each file)
- **offset**: File offset of this chunk
- **payload_len**: Number of data bytes in this packet

**Wire Format Example** (1024-byte chunk at offset 4096):
```
Header:
  type: 05
  wire_version: 00
  reserved2: 00 00
  payload_len: 00 04 00 00  (1024 in LE)
  seq: 05 00 00 00          (sequence 5)
  offset: 00 10 00 00 00 00 00 00  (4096 in LE)
  header_crc: XX XX XX XX

Payload (1024 bytes):
  [file data bytes]

Padding: (if needed to align trailer)

Trailer: XX XX XX XX
```

---

### DATA_ACK (type=6)

**Purpose**: Cumulative acknowledgment

**Direction**: Receiver → Sender

**Payload**: Optional (typically header-only)

**Header Fields**:
- **seq**: Sequence number of last received DATA packet
- **offset**: Next expected byte offset (cumulative ACK)

**Cumulative Semantics**:
- All bytes before `offset` are acknowledged
- Sender can advance window and free buffer space

**Optional Payload** (future use):
```c
struct {
    uint8_t flags;  // VAL_ACK_FLAG_HEARTBEAT, VAL_ACK_FLAG_EOF, etc.
};
```

**Wire Format Example** (ACK up to offset 8192):
```
Header:
  type: 06
  wire_version: 00
  reserved2: 00 00
  payload_len: 00 00 00 00  (0 = header-only)
  seq: 07 00 00 00          (last received seq)
  offset: 00 20 00 00 00 00 00 00  (8192 in LE)
  header_crc: XX XX XX XX

Trailer: XX XX XX XX
```

---

### VERIFY (type=7)

**Purpose**: CRC verification exchange

**Direction**: Bidirectional

**Payload (Sender → Receiver)**: `val_resume_resp_t` (echo of receiver's request)

**Payload (Receiver → Sender)**: `int32_t` result (LE)

**Flow**:
1. Receiver sends RESUME_RESP with VERIFY_FIRST action
2. Sender computes CRC over specified region
3. Sender sends VERIFY with computed CRC (echoes val_resume_resp_t)
4. Receiver compares CRCs
5. Receiver sends VERIFY with result:
   - `VAL_OK` (0): CRCs match, resume
   - `VAL_SKIPPED` (1): CRCs match and file complete, skip
   - `VAL_ERR_RESUME_VERIFY` (-7): CRC mismatch

---

### DONE (type=8)

**Purpose**: Sender indicates file transfer complete

**Direction**: Sender → Receiver

**Payload**: None (header-only packet)

**Semantics**:
- Sender has sent all file data
- Receiver should verify and send DONE_ACK

---

### ERROR (type=9)

**Purpose**: Error notification

**Direction**: Bidirectional

**Payload**: `val_error_payload_t` (8 bytes)

```c
struct val_error_payload_t {
    int32_t  code;    // val_status_t (negative for errors, LE)
    uint32_t detail;  // Error detail mask (LE)
};  // 8 bytes
```

**Error Codes** (see `val_errors.h`):
- VAL_ERR_IO (-3)
- VAL_ERR_TIMEOUT (-4)
- VAL_ERR_PROTOCOL (-5)
- VAL_ERR_CRC (-6)
- etc.

**Detail Mask**: 32-bit segmented by category (see [Troubleshooting](troubleshooting.md#error-code-reference))

---

### EOT (type=10)

**Purpose**: End of transmission (batch complete)

**Direction**: Sender → Receiver

**Payload**: None (header-only packet)

**Semantics**:
- Sender has finished sending all files in batch
- Session can be reused for another batch

---

### EOT_ACK (type=11)

**Purpose**: Acknowledge end of transmission

**Direction**: Receiver → Sender

**Payload**: None (header-only packet)

---

### DONE_ACK (type=12)

**Purpose**: Acknowledge file completion

**Direction**: Receiver → Sender

**Payload**: None (header-only packet)

---

### MODE_SYNC (type=13)

**Purpose**: Adaptive mode synchronization

**Direction**: Sender → Receiver

**Payload**: `val_mode_sync_t` (20 bytes)

```c
struct val_mode_sync_t {
    uint32_t current_mode;         // Current val_tx_mode_t (LE)
    uint32_t sequence;             // Sync sequence number (LE)
    uint32_t consecutive_errors;   // Recent error count (LE)
    uint32_t consecutive_success;  // Recent success count (LE)
    uint32_t flags;                // Bit 0: streaming_engaged (LE)
};  // 20 bytes
```

**Purpose**: Inform receiver of sender's current adaptive state

---

### MODE_SYNC_ACK (type=14)

**Purpose**: Acknowledge mode sync

**Direction**: Receiver → Sender

**Payload**: `val_mode_sync_ack_t` (12 bytes)

```c
struct val_mode_sync_ack_t {
    uint32_t ack_sequence;     // Sequence from MODE_SYNC (LE)
    uint32_t agreed_mode;      // Agreed val_tx_mode_t (LE)
    uint32_t receiver_errors;  // Receiver's error count (LE)
};  // 12 bytes
```

---

### DATA_NAK (type=15)

**Purpose**: Negative acknowledgment (Go-Back-N trigger)

**Direction**: Receiver → Sender

**Payload**: (12 bytes)

```c
struct {
    uint64_t next_expected_offset;  // Resume from this offset (LE)
    uint32_t reason;                // Diagnostic flags (LE)
};  // 12 bytes
```

**Semantics**:
- Receiver detected gap/error in data sequence
- Sender should rewind to `next_expected_offset` and retransmit

**Reason Flags** (diagnostic):
- Bit 0: Sequence gap
- Bit 1: Offset mismatch
- Bit 2: CRC error
- etc.

---

### CANCEL (type=0x18)

**Purpose**: Emergency cancellation

**Direction**: Bidirectional

**Payload**: None (header-only packet)

**Special**: Uses ASCII CAN character (0x18) as type

**Semantics**:
- Best-effort abort notification
- Sent 3 times with no ACK expected
- Marks session as `VAL_ERR_ABORTED`

---

## CRC Computation Details

### Algorithm

VAL Protocol uses **CRC-32** (IEEE 802.3 / polynomial 0x04C11DB7).

**Reflected Form**:
- Polynomial: 0xEDB88320
- Initial value: 0xFFFFFFFF
- Final XOR: 0xFFFFFFFF
- Reflect input: Yes
- Reflect output: Yes

**Reference Implementation** (see `src/val_core.c`):
```c
uint32_t val_crc32(const void *data, size_t length) {
    static uint32_t table[256];
    // ... table initialization ...
    
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
```

**Test Vector**:
```c
const char *test = "123456789";
uint32_t crc = val_crc32(test, 9);
assert(crc == 0xCBF43926);  // Expected CRC-32
```

### Header CRC

**Computation**:
1. Zero out `header_crc` field (bytes 20-23)
2. Compute CRC-32 over bytes 0-19
3. Store result in `header_crc` field (little-endian)

**Verification**:
1. Read header
2. Save `header_crc` value
3. Zero out `header_crc` field
4. Compute CRC-32 over bytes 0-19
5. Compare with saved value

### Trailer CRC

**Computation**:
1. Compute CRC-32 over entire packet: header + payload + padding
2. Append CRC as 4-byte trailer (little-endian)

**Verification**:
1. Read entire packet (header + payload + padding + trailer)
2. Extract trailer CRC
3. Compute CRC-32 over: header + payload + padding
4. Compare with trailer value

---

## Byte Order Examples

### uint16_t to Wire (Little-Endian)

```c
uint16_t value = 0x1234;
uint8_t wire[2];

// Little-endian encoding:
wire[0] = value & 0xFF;         // 0x34
wire[1] = (value >> 8) & 0xFF;  // 0x12

// Wire bytes: [34 12]
```

### uint32_t to Wire

```c
uint32_t value = 0x12345678;
uint8_t wire[4];

wire[0] = value & 0xFF;         // 0x78
wire[1] = (value >> 8) & 0xFF;  // 0x56
wire[2] = (value >> 16) & 0xFF; // 0x34
wire[3] = (value >> 24) & 0xFF; // 0x12

// Wire bytes: [78 56 34 12]
```

### uint64_t to Wire

```c
uint64_t value = 0x123456789ABCDEF0;
uint8_t wire[8];

for (int i = 0; i < 8; i++) {
    wire[i] = (value >> (i * 8)) & 0xFF;
}

// Wire bytes: [F0 DE BC 9A 78 56 34 12]
```

**VAL provides built-in helpers**:
```c
uint16_t val_htole16(uint16_t v);  // Host to little-endian
uint32_t val_htole32(uint32_t v);
uint64_t val_htole64(uint64_t v);

uint16_t val_letoh16(uint16_t v);  // Little-endian to host
uint32_t val_letoh32(uint32_t v);
uint64_t val_letoh64(uint64_t v);
```

---

## Packet Size Constraints

| Packet Type | Min Payload | Max Payload | Typical Size |
|-------------|-------------|-------------|--------------|
| HELLO | 52 | 52 | 52 bytes |
| SEND_META | 264 | 264 | 264 bytes |
| RESUME_REQ | 0 | 0 | Header-only |
| RESUME_RESP | 24 | 24 | 24 bytes |
| DATA | 1 | MTU - overhead | Variable |
| DATA_ACK | 0 | 0-8 | Header-only or 1 byte |
| VERIFY | 4-24 | 24 | 4 or 24 bytes |
| DONE | 0 | 0 | Header-only |
| ERROR | 8 | 8 | 8 bytes |
| EOT | 0 | 0 | Header-only |
| EOT_ACK | 0 | 0 | Header-only |
| DONE_ACK | 0 | 0 | Header-only |
| MODE_SYNC | 20 | 20 | 20 bytes |
| MODE_SYNC_ACK | 12 | 12 | 12 bytes |
| DATA_NAK | 12 | 12 | 12 bytes |
| CANCEL | 0 | 0 | Header-only |

**Total Packet Size** = 24 (header) + payload_len + padding + 4 (trailer)

**MTU Overhead**:
- Header: 24 bytes
- Trailer: 4 bytes
- Total overhead: 28 bytes
- Max data per packet: MTU - 28 bytes

**Example**: With 4096-byte MTU, max data = 4068 bytes per DATA packet.

---

## See Also

- [Protocol Specification](protocol-specification.md) - High-level protocol flows
- [API Reference](api-reference.md) - C API structures
- [Implementation Guide](implementation-guide.md) - Integration examples
