# VAL Protocol Specification

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors. Please verify against source code.

---

**Protocol Name:** VAL (Versatile Adaptive Link Protocol)  
**Version:** 0.7  
**Status:** Pre-1.0 Development  
**Last Updated:** October 2025  
**Authors:** Arthur T Lee

_Dedicated to Valerie Lee - for all her support over the years allowing me to chase my ideas._

---

## 1. Introduction

### 1.1 Purpose

VAL Protocol is a blocking-I/O file transfer protocol designed for reliable file transfers across diverse network conditions, from high-speed LANs to constrained embedded systems. The protocol provides adaptive transmission strategies, comprehensive resume capabilities, and embedded-friendly resource management.

### 1.2 Design Goals

- **Reliability**: Guaranteed file delivery with CRC integrity verification
- **Adaptability**: Dynamic adjustment to network conditions via window-based flow control
- **Resumability**: Multiple resume modes with CRC-verified partial file transfers
- **Embeddability**: Zero dynamic allocations in steady state, configurable memory footprint
- **Transport Agnostic**: Works over any reliable byte stream (TCP, UART, USB, etc.)
- **Simplicity**: Blocking I/O model, no threading requirements

### 1.3 Key Advantages

**Streaming Mode Performance:**
- Continuous transmission (15-20x faster than XMODEM/YMODEM)
- ACKs as heartbeats, not flow control
- High throughput even with small windows (ideal for embedded systems)

**Powerful Abstraction Layer:**
- **Transport**: Any reliable byte stream (TCP, UART, RS-485, CAN, USB CDC, SPI)
- **Filesystem**: Any byte source/sink (files, RAM, flash, network buffers, streaming)
- **System**: Custom clock, allocators, CRC (hardware acceleration support)
- **Enables**: Encryption, compression, custom protocols, in-memory transfers

### 1.4 Non-Goals

- **Unreliable Transports**: VAL requires ordered, reliable delivery (not suitable for UDP without reliability layer)
- **Streaming Media**: Not optimized for real-time live data (designed for file transfer)
- **Built-in Encryption**: Security must be provided by transport layer (TLS) or custom filesystem wrapper

### 1.5 Terminology

- **Sender**: Initiates file transfer, sends file data
- **Receiver**: Accepts files, sends acknowledgments
- **Session**: Single connection lifecycle with handshake and file transfers
- **Batch**: Multiple files transferred in one session
- **MTU**: Maximum Transmission Unit (packet size)
- **Window**: Number of unacknowledged packets allowed in flight
- **Rung**: Discrete window size level (1, 2, 4, 8, 16, 32, 64 packets)

## 2. Protocol Architecture

### 2.1 Layered Model

```
┌─────────────────────────────────────┐
│      Application Layer              │
│  (val_send_files / val_receive_files)│
└─────────────────────────────────────┘
           ↓          ↑
┌─────────────────────────────────────┐
│      Session Layer                  │
│  - Handshake & Negotiation          │
│  - Adaptive Transmission            │
│  - Resume Management                │
│  - Error Recovery                   │
└─────────────────────────────────────┘
           ↓          ↑
┌─────────────────────────────────────┐
│      Packet Layer                   │
│  - Framing (Header + Payload + CRC) │
│  - Sequence Numbers                 │
│  - Packet Types                     │
└─────────────────────────────────────┘
           ↓          ↑
┌─────────────────────────────────────┐
│      Transport Layer                │
│  (TCP, UART, USB - User Provided)   │
└─────────────────────────────────────┘
```

### 2.2 Session State Machine

```
                    ┌──────────┐
                    │  CREATED │
                    └─────┬────┘
                          │ val_session_create()
                          ↓
                    ┌──────────┐
                    │   IDLE   │
                    └─────┬────┘
                          │ send_files() / receive_files()
                          ↓
                    ┌──────────┐
                    │HANDSHAKE │ ←──────────┐
                    └─────┬────┘            │
                          │ negotiation OK  │
                          ↓                 │
          ┌───────────────────────────┐    │
          │   FILE TRANSFER LOOP      │    │
          │                           │    │
          │  ┌────────────────┐       │    │
          │  │  SEND_META     │       │    │
          │  └───────┬────────┘       │    │
          │          │                │    │
          │          ↓                │    │
          │  ┌────────────────┐       │    │
          │  │  RESUME_REQ    │       │    │
          │  │  RESUME_RESP   │       │    │
          │  └───────┬────────┘       │    │
          │          │                │    │
          │          ↓                │    │
          │  ┌────────────────┐       │    │
          │  │ DATA TRANSFER  │       │    │
          │  │ (windowed)     │       │    │
          │  └───────┬────────┘       │    │
          │          │                │    │
          │          ↓                │    │
          │  ┌────────────────┐       │    │
          │  │  DONE / ACK    │       │    │
          │  └───────┬────────┘       │    │
          │          │                │    │
          │          ├─ more files? ──┘    │
          │          │                     │
          └──────────┼─────────────────────┘
                     │ all files complete
                     ↓
               ┌──────────┐
               │   EOT    │
               └─────┬────┘
                     │ EOT_ACK
                     ↓
               ┌──────────┐
               │   IDLE   │
               └──────────┘
```

## 3. Packet Format

### 3.1 Frame Structure

All packets follow a fixed structure:

```
┌────────────┬──────────────────┬──────────┬──────────┐
│   Header   │     Payload      │   Pad    │ Trailer  │
│  (24 bytes)│  (0..max_payload)│ (0..pad) │ (4 bytes)│
└────────────┴──────────────────┴──────────┴──────────┘
```

- **Header**: Fixed 24-byte structure with CRC
- **Payload**: Variable length (0 to max_payload_size)
- **Padding**: Zero padding to align trailer
- **Trailer**: 4-byte CRC32 of entire packet

### 3.2 Header Format

```c
struct val_packet_header_t {
    uint8_t  type;         // Packet type (val_packet_type_t)
    uint8_t  wire_version; // Wire format version (always 0)
    uint16_t reserved2;    // Reserved (zero)
    uint32_t payload_len;  // Payload length in bytes
    uint32_t seq;          // Sequence number (monotonic per file)
    uint64_t offset;       // File offset or ACK offset
    uint32_t header_crc;   // CRC32 of header (excluding this field and trailer)
};
```

**Field Details:**

- `type`: Packet type enumeration (see Section 3.3)
- `wire_version`: Protocol wire format version; must be 0 in current specification
- `reserved2`: Reserved for future use; must be 0
- `payload_len`: Number of valid payload bytes (0 for header-only packets)
- `seq`: Monotonically increasing sequence number per file (resets for each file)
- `offset`: Context-dependent:
  - **DATA**: File offset of this payload
  - **DATA_ACK**: Next expected offset (cumulative ACK)
  - **RESUME_REQ/RESP**: Resume offset
- `header_crc`: CRC32 of header excluding `header_crc` and trailer CRC

**Endianness:** All multi-byte integers are **little-endian** on wire.

### 3.3 Packet Types

```c
typedef enum {
    VAL_PKT_HELLO       = 1,   // Session/version negotiation
    VAL_PKT_SEND_META   = 2,   // File metadata (name, size, CRC)
    VAL_PKT_RESUME_REQ  = 3,   // Sender requests resume options
    VAL_PKT_RESUME_RESP = 4,   // Receiver responds with action
    VAL_PKT_DATA        = 5,   // File data chunk
    VAL_PKT_DATA_ACK    = 6,   // Cumulative ACK for data
    VAL_PKT_VERIFY      = 7,   // CRC verification request/response
    VAL_PKT_DONE        = 8,   // File transfer complete
    VAL_PKT_ERROR       = 9,   // Error report
    VAL_PKT_EOT         = 10,  // End of transmission (batch)
    VAL_PKT_EOT_ACK     = 11,  // ACK for EOT
    VAL_PKT_DONE_ACK    = 12,  // ACK for DONE
    VAL_PKT_MODE_SYNC   = 13,  // Adaptive mode synchronization
    VAL_PKT_MODE_SYNC_ACK = 14, // ACK for mode sync
    VAL_PKT_DATA_NAK    = 15,  // Negative ACK (Go-Back-N)
    VAL_PKT_CANCEL      = 0x18 // Emergency cancel (ASCII CAN)
} val_packet_type_t;
```

### 3.4 CRC Computation

**Algorithm:** CRC-32 (IEEE 802.3 polynomial)

- **Polynomial:** 0xEDB88320 (reflected)
- **Initial Value:** 0xFFFFFFFF
- **Final XOR:** 0xFFFFFFFF
- **Reflection:** Input and output reflected

**Header CRC:**
- Computed over header bytes excluding `header_crc` field and trailer
- Offset 0 through 19 (20 bytes total)

**Trailer CRC:**
- Computed over entire packet: header + payload + padding
- Stored after padding in little-endian format

## 4. Handshake and Negotiation

### 4.1 Handshake Flow

```
Sender                                  Receiver
  │                                        │
  │───────── HELLO (capabilities) ────────>│
  │                                        │
  │<──────── HELLO (capabilities) ─────────│
  │                                        │
  │        [negotiate parameters]          │
  │                                        │
  │───────────── Ready ───────────────────>│
```

### 4.2 HELLO Payload

```c
struct val_handshake_t {
    uint32_t magic;                    // 0x56414C00 ("VAL\0")
    uint8_t  version_major;            // Protocol major version (0)
    uint8_t  version_minor;            // Protocol minor version (7)
    uint16_t reserved;                 // Reserved (0)
    uint32_t packet_size;              // Requested MTU
    uint32_t features;                 // Supported optional features
    uint32_t required;                 // Required features from peer
    uint32_t requested;                // Requested features from peer
    uint8_t  max_performance_mode;     // Max window rung (val_tx_mode_t)
    uint8_t  preferred_initial_mode;   // Initial window rung
    uint16_t mode_sync_interval;       // Reserved (0)
    uint8_t  streaming_flags;          // Streaming capabilities
    uint8_t  reserved_streaming[3];    // Reserved (0)
    uint16_t supported_features16;     // Reserved (0)
    uint16_t required_features16;      // Reserved (0)
    uint16_t requested_features16;     // Reserved (0)
    uint32_t reserved2;                // Reserved (0)
};
```

**Negotiation Rules:**

1. **Version Compatibility**: Both sides must have same `version_major`
2. **Packet Size**: Use minimum of both sides' `packet_size`
3. **Features**: Intersection of supported features
4. **TX Mode**: Use most conservative `max_performance_mode`
5. **Streaming**: Allowed only if both sides agree

### 4.3 Feature Negotiation

**Feature Bits:**
- Currently no optional features are defined; all core features are implicit and always available
- Bits 0-31: Reserved for future use

**Streaming Flags:**
- Bit 0: Can stream when sending
- Bit 1: Accept peer streaming
- Bits 2-7: Reserved (0)

## 5. File Transfer Protocol

### 5.1 Metadata Exchange

**Sender → Receiver: SEND_META**

```c
struct val_meta_payload_t {
    char     filename[128];      // Sanitized basename (UTF-8)
    char     sender_path[128];   // Original path hint (UTF-8)
    uint64_t file_size;          // File size in bytes
    uint32_t file_crc32;         // Whole-file CRC32
};
```

**Security Note:** Receiver must sanitize `sender_path` and never use it directly for output paths to prevent path traversal attacks.

### 5.2 Resume Protocol

#### 5.2.1 Resume Request

**Sender → Receiver: RESUME_REQ**

Header-only packet requesting resume options. The receiver evaluates local file state and responds.

**Receiver → Sender: RESUME_RESP**

```c
struct val_resume_resp_t {
    uint32_t action;          // val_resume_action_t
    uint64_t resume_offset;   // Resume from this offset
    uint32_t verify_crc;      // Expected CRC for verification
    uint64_t verify_len;      // Length of verification region
};
```

**Resume Actions:**

```c
typedef enum {
    VAL_RESUME_ACTION_START_ZERO   = 0,  // Start from beginning
    VAL_RESUME_ACTION_START_OFFSET = 1,  // Resume from offset
    VAL_RESUME_ACTION_VERIFY_FIRST = 2,  // CRC verification required
    VAL_RESUME_ACTION_SKIP_FILE    = 3,  // Skip this file
    VAL_RESUME_ACTION_ABORT_FILE   = 4,  // Abort transfer
} val_resume_action_t;
```

#### 5.2.2 Resume Modes

**Six Resume Modes:**

1. **VAL_RESUME_NEVER**: Always overwrite from zero
2. **VAL_RESUME_SKIP_EXISTING**: Skip any existing file (no verification)
3. **VAL_RESUME_CRC_TAIL**: Verify tail, resume on match, skip on mismatch
4. **VAL_RESUME_CRC_TAIL_OR_ZERO**: Verify tail, resume on match, restart on mismatch
5. **VAL_RESUME_CRC_FULL**: Verify full prefix, resume/skip on match, skip on mismatch
6. **VAL_RESUME_CRC_FULL_OR_ZERO**: Verify full prefix, resume/skip on match, restart on mismatch

**Tail Verification:**
- Verifies last N bytes of local file (configurable, default 1 KB, capped at 2 MB)
- If local_size > incoming_size, treat as mismatch

**Full Verification:**
- Verifies entire local file when local_size ≤ incoming_size
- If local_size == incoming_size and CRC matches, skip file (already complete)
- Large files use "large tail" verification (last CAP bytes) to maintain responsiveness

#### 5.2.3 CRC Verification Exchange

When `VERIFY_FIRST` action is requested:

```
Sender                                  Receiver
  │                                        │
  │<────── RESUME_RESP (VERIFY) ───────────│
  │        (with verify_crc/len)           │
  │                                        │
  │──────── VERIFY (echo CRC) ────────────>│
  │                                        │
  │<─────── VERIFY (result) ───────────────│
  │        (VAL_OK, VAL_SKIPPED, etc.)     │
```

### 5.3 Data Transfer

#### 5.3.1 Windowed Transmission

Data packets are sent with a sliding window:

```
Sender                                  Receiver
  │                                        │
  │───────── DATA (seq=0, off=0) ─────────>│
  │───────── DATA (seq=1, off=N) ─────────>│
  │───────── DATA (seq=2, off=2N) ────────>│
  │  ...                                   │
  │                                        │
  │<─────── DATA_ACK (off=3N) ─────────────│
  │        (cumulative ACK)                │
  │                                        │
  │───────── DATA (seq=3, off=3N) ────────>│
  │  ...                                   │
```

**Cumulative ACK Semantics:**
- `DATA_ACK.offset` = next expected byte offset
- All data before `offset` is acknowledged
- Sender may free/reuse buffer space for acked data

#### 5.3.2 Adaptive Window Sizing

**Window Rungs (Discrete Levels):**

| Mode | Value | Window Size | Description |
|------|-------|-------------|-------------|
| VAL_TX_STOP_AND_WAIT | 1 | 1 packet | Most reliable |
| VAL_TX_WINDOW_2 | 2 | 2 packets | |
| VAL_TX_WINDOW_4 | 4 | 4 packets | |
| VAL_TX_WINDOW_8 | 8 | 8 packets | |
| VAL_TX_WINDOW_16 | 16 | 16 packets | Balanced |
| VAL_TX_WINDOW_32 | 32 | 32 packets | |
| VAL_TX_WINDOW_64 | 64 | 64 packets | Maximum performance |

**Adaptation Algorithm:**

```
consecutive_errors++
if consecutive_errors >= degrade_threshold:
    degrade to next lower rung
    consecutive_errors = 0
    consecutive_successes = 0

on successful ACK:
    consecutive_successes++
    consecutive_errors = 0
    if consecutive_successes >= recovery_threshold:
        upgrade to next higher rung
        consecutive_successes = 0
```

Default thresholds:
- `degrade_error_threshold`: 3 errors
- `recovery_success_threshold`: 10 successes

#### 5.3.3 Streaming Mode - Continuous Transmission

**Streaming is VAL's high-performance mode** that transforms the protocol from windowed to continuous transmission.

**Key Behavior Changes When Streaming Engaged:**

1. **Window Constraint Removed**: Sender ignores window size and sends continuously until NAK or EOF
2. **ACKs Become Heartbeats**: ACKs no longer gate transmission - they only prove receiver liveness
3. **Non-Blocking Operation**: Sender uses short polling (SRTT/4, clamped 2-20ms) instead of full timeout waits
4. **No Timeout Rewind**: Timeouts don't trigger window rewind - sender keeps pushing forward
5. **NAK-Triggered Recovery**: Only NAKs (corruption/out-of-order) cause retransmission
6. **Coalesced ACKs**: Receiver sends ACKs once per window + periodic heartbeats (~3 seconds)

**Activation Requirements:**
- Both sides advertise streaming capability in HELLO
- Sender reaches maximum negotiated window mode
- Sustained clean transmission (10+ consecutive successes)
- Protocol sends MODE_SYNC with streaming flag set

**Disengagement Triggers:**
- Any NAK received (corruption detected)
- Transmission error or timeout exceeds threshold
- Automatic fallback to conservative windowed mode

**Performance Impact:**

**Window Size vs Streaming Trade-offs:**
- **WINDOW_64 (pure windowing)**: High throughput, requires most RAM
- **WINDOW_64 + streaming**: Slightly faster, same RAM
- **WINDOW_4 + streaming**: Moderate throughput, far less RAM than WINDOW_64
- **WINDOW_2 + streaming**: Good throughput, minimal RAM (ideal for MCUs)
- **Streaming advantage increases** as window size decreases

**Memory vs Performance:**
- Larger windows require more RAM but enable better escalation/de-escalation
- **Recommended minimum: WINDOW_4** (allows effective adaptation to errors)
- WINDOW_2/SINGLE + streaming: saves RAM but limits error adaptation
- WINDOW_64: maximum performance and adaptation range, highest RAM usage

**Implementation:** Streaming is not a separate mode - it's a behavioral overlay that changes how the sender manages window constraints and ACK handling at the highest performance rung.

#### 5.3.4 Error Recovery (Go-Back-N)

On timeout or DATA_NAK:

```
Sender                                  Receiver
  │                                        │
  │───────── DATA (seq=5) ─────────────────> (lost)
  │───────── DATA (seq=6) ─────────────────>│
  │                                        │ (seq=6 unexpected)
  │<──────── DATA_NAK (off=N, reason) ─────│
  │                                        │
  │  [rewind file to offset N]             │
  │  [clear window]                        │
  │                                        │
  │───────── DATA (seq=5, off=N) ─────────>│
  │  ...                                   │
```

**DATA_NAK Payload:**

```c
struct {
    uint64_t next_expected_offset;  // Resume from here
    uint32_t reason;                // Diagnostic flags
};
```

### 5.4 Completion and Batch Handling

#### 5.4.1 Single File Completion

```
Sender                                  Receiver
  │                                        │
  │───────── DONE ─────────────────────────>│
  │                                        │
  │<──────── DONE_ACK ─────────────────────│
```

After DONE_ACK, sender proceeds to next file or sends EOT.

#### 5.4.2 Batch Completion

```
Sender                                  Receiver
  │                                        │
  │───────── EOT ──────────────────────────>│
  │                                        │
  │<──────── EOT_ACK ──────────────────────│
  │                                        │
  │        [session can be reused]         │
```

## 6. Adaptive Timeout Management

### 6.1 RTT Estimation (RFC 6298)

```c
// On first RTT sample:
SRTT = R
RTTVAR = R / 2

// On subsequent samples:
RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R|
SRTT = (1 - alpha) * SRTT + alpha * R

// RTO calculation:
RTO = SRTT + max(G, 4 * RTTVAR)
RTO = clamp(RTO, min_timeout_ms, max_timeout_ms)
```

Where:
- `alpha = 1/8`
- `beta = 1/4`
- `G = clock granularity` (typically 0 for ms clocks)

### 6.2 Karn's Algorithm

**Do not sample RTT during retransmissions:**
- Set `in_retransmit` flag on error/timeout
- Clear flag after successful new transmission
- Only sample RTT when flag is clear

### 6.3 Timeout Selection by Operation

| Operation | Base Timeout | Typical Range |
|-----------|--------------|---------------|
| Handshake | Adaptive RTO | 100-10000 ms |
| Metadata | Adaptive RTO | 100-10000 ms |
| Data ACK | Adaptive RTO | 100-10000 ms |
| Verify | Adaptive RTO | 100-10000 ms |
| DONE_ACK | Adaptive RTO | 100-10000 ms |
| EOT_ACK | Adaptive RTO | 100-10000 ms |

## 7. Error Handling

### 7.1 Error Packet Format

```c
struct val_error_payload_t {
    int32_t  code;      // val_status_t (negative)
    uint32_t detail;    // Detail mask (see Section 7.2)
};
```

### 7.2 Error Detail Mask

32-bit detail mask with category segmentation:

```
Bits 0-7:   Network/Transport
Bits 8-15:  CRC/Integrity
Bits 16-23: Protocol/Feature
Bits 24-27: Filesystem
Bits 28-31: Context (payload selector)
```

**Network Details (0-7):**
- 0x01: Network reset
- 0x02: Timeout ACK
- 0x04: Timeout DATA
- 0x08: Timeout META
- 0x10: Timeout HELLO
- 0x20: Send failed
- 0x40: Recv failed
- 0x80: Connection

**CRC Details (8-15):**
- 0x0100: Header CRC
- 0x0200: Trailer CRC
- 0x0400: File CRC
- 0x0800: Resume CRC
- 0x1000: Size mismatch
- 0x2000: Packet corrupt
- 0x4000: Sequence error
- 0x8000: Offset error

**Protocol Details (16-23):**
- 0x010000: Version major
- 0x020000: Version minor
- 0x040000: Packet size
- 0x080000: Feature missing
- 0x100000: Invalid state
- 0x200000: Malformed packet
- 0x400000: Unknown type
- 0x800000: Payload size

**Filesystem Details (24-27):**
- 0x01000000: File not found
- 0x02000000: File locked
- 0x04000000: Disk full
- 0x08000000: Permission denied

### 7.3 Emergency Cancellation

**CANCEL packet (0x18 - ASCII CAN):**
- Best-effort abort notification
- Sent 3 times with no ACK required
- Marks session as `VAL_ERR_ABORTED`
- Application can query with `val_check_for_cancel()`

## 8. Security Considerations

### 8.1 Path Traversal Prevention

**Receiver Responsibilities:**
- Sanitize `filename` from SEND_META (strip directory separators)
- Never trust `sender_path` for output directory
- Construct output paths using receiver-controlled directory + sanitized filename

**Sender Responsibilities:**
- Clean filenames before sending (remove dangerous characters)
- Provide `sender_path` as informational hint only

### 8.2 Transport Security

VAL Protocol does not provide:
- Encryption
- Authentication
- Integrity protection beyond CRC

**Recommendations:**
- Use TLS for network transports
- Implement application-level authentication
- Use authenticated encryption (AES-GCM, ChaCha20-Poly1305) if needed

### 8.3 Resource Limits

**Denial of Service Protection:**
- Enforce maximum file sizes at application level
- Use metadata validation callbacks to reject unwanted files
- Implement rate limiting in transport layer
- Monitor connection health (excessive retries trigger abort)

## 9. Wire Format Examples

### 9.1 HELLO Packet

```
Offset  Size  Field              Value (hex)
------  ----  -----              -----------
0       1     type               01
1       1     wire_version       00
2       2     reserved2          00 00
4       4     payload_len        34 00 00 00  (52 bytes)
8       4     seq                00 00 00 00
12      8     offset             00 00 00 00 00 00 00 00
20      4     header_crc         XX XX XX XX

24      4     magic              00 4C 41 56  ("VAL\0" LE)
28      1     version_major      00
29      1     version_minor      07
30      2     reserved           00 00
32      4     packet_size        00 10 00 00  (4096)
36      4     features           01 00 00 00
40      4     required           00 00 00 00
44      4     requested          01 00 00 00
48      1     max_perf_mode      40  (VAL_TX_WINDOW_64)
49      1     pref_init_mode     10  (VAL_TX_WINDOW_16)
50      2     mode_sync_interval 00 00
52      1     streaming_flags    03  (send=1, recv=1)
53      3     reserved_streaming 00 00 00
56      2     supported_feat16   00 00
58      2     required_feat16    00 00
60      2     requested_feat16   00 00
62      4     reserved2          00 00 00 00

76      4     trailer_crc        XX XX XX XX
```

### 9.2 DATA Packet (1024-byte payload)

```
Offset  Size  Field              Value (hex)
------  ----  -----              -----------
0       1     type               05  (VAL_PKT_DATA)
1       1     wire_version       00
2       2     reserved2          00 00
4       4     payload_len        00 04 00 00  (1024)
8       4     seq                0A 00 00 00  (seq 10)
12      8     offset             00 20 00 00 00 00 00 00  (8192)
20      4     header_crc         XX XX XX XX

24      1024  payload            [file data]

1048    4     trailer_crc        XX XX XX XX
```

### 9.3 DATA_ACK (Cumulative)

```
Offset  Size  Field              Value (hex)
------  ----  -----              -----------
0       1     type               06  (VAL_PKT_DATA_ACK)
1       1     wire_version       00
2       2     reserved2          00 00
4       4     payload_len        00 00 00 00  (0)
8       4     seq                0B 00 00 00  (seq 11)
12      8     offset             00 28 00 00 00 00 00 00  (10240 = next expected)
20      4     header_crc         XX XX XX XX

24      4     trailer_crc        XX XX XX XX
```

## 10. Implementation Requirements

### 10.1 Mandatory Features

- Little-endian wire format encoding/decoding
- CRC-32 (IEEE 802.3) computation
- Monotonic millisecond clock
- Blocking I/O transport interface
- File I/O interface (seek, read, write, tell)

### 10.2 Optional Features

- Hardware CRC acceleration
- Custom memory allocators
- Metrics collection
- Wire audit trails
- Debug logging

### 10.3 Conformance Levels

**Level 1 (Minimal):**
- Stop-and-wait transmission only
- RESUME_NEVER mode
- Fixed timeouts (no RTT adaptation)
- No logging

**Level 2 (Standard):**
- Window-based transmission (up to 16 packets)
- All resume modes
- Adaptive timeouts (RFC 6298)
- Basic logging

**Level 3 (Full):**
- All window rungs (1-64 packets)
- Streaming pacing overlay
- Metrics and diagnostics
- Full logging support

## 11. Future Extensions

### 11.1 Reserved Fields

The following fields are reserved for future protocol versions:

- `wire_version` in header (currently 0)
- `reserved2` in header
- `supported_features16`, `required_features16`, `requested_features16` in handshake
- `mode_sync_interval` in handshake

### 11.2 Potential Features

- Compression (payload-level)
- Multiple file metadata pre-declaration
- Bidirectional transfers in single session
- Selective ACK (SACK) extensions
- Forward error correction (FEC)

## 12. References

- **RFC 6298**: Computing TCP's Retransmission Timer
- **RFC 1122**: Requirements for Internet Hosts - Communication Layers
- **ISO 3309**: HDLC frame structure (CRC-32 polynomial)

---

## Appendix A: Complete Packet Type Reference

| Type | Value | Direction | Payload | Description |
|------|-------|-----------|---------|-------------|
| HELLO | 1 | Both | val_handshake_t | Session negotiation |
| SEND_META | 2 | S→R | val_meta_payload_t | File metadata |
| RESUME_REQ | 3 | S→R | None | Request resume options |
| RESUME_RESP | 4 | R→S | val_resume_resp_t | Resume decision |
| DATA | 5 | S→R | File bytes | File data chunk |
| DATA_ACK | 6 | R→S | Optional | Cumulative ACK |
| VERIFY | 7 | Both | val_resume_resp_t or int32_t | CRC verification |
| DONE | 8 | S→R | None | File complete |
| ERROR | 9 | Both | val_error_payload_t | Error notification |
| EOT | 10 | S→R | None | End of batch |
| EOT_ACK | 11 | R→S | None | ACK for EOT |
| DONE_ACK | 12 | R→S | None | ACK for DONE |
| MODE_SYNC | 13 | S→R | val_mode_sync_t | Mode synchronization |
| MODE_SYNC_ACK | 14 | R→S | val_mode_sync_ack_t | Mode sync ACK |
| DATA_NAK | 15 | R→S | offset+reason | Negative ACK |
| CANCEL | 0x18 | Both | None | Emergency abort |

S→R: Sender to Receiver  
R→S: Receiver to Sender

---

**End of Specification**
