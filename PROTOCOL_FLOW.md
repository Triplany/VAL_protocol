# VAL Protocol — Packet Flow Reference

This document maps the end-to-end control flow and clarifies when each packet type is used, in what order, and with what expectations. It reflects the current implementation in `src/`.

- Audience: developers integrating or modifying the protocol.
- Scope: packet purposes, sequencing, timeouts/retries, and notable edge cases.


## Compatibility policy (pre‑1.0)

- Until v1.0, we do not guarantee backward compatibility. We may change packet semantics and framing to improve the design.
- Handshake enforces strict major version equality; for any breaking wire change we will bump `VAL_VERSION_MAJOR` (even while pre‑1.0), so mixed versions will refuse to talk.
- This file always reflects the current implementation; proposed changes below may be adopted without compatibility shims until v1.0.


## Packet glossary

- HELLO: Handshake and negotiation (version, packet_size, features).
- SEND_META: Per-file metadata: sanitized filename, sender_path hint, file_size, file_crc32.
- RESUME_REQ: Sender asks receiver how to resume this file.
- RESUME_RESP: Receiver tells sender to start at zero, at an offset, perform CRC verify-first, skip the file, or abort the file (policy-driven).
- VERIFY: Two-stage CRC verify exchange used only when RESUME_RESP requested verify-first.
- DATA: A chunk of file bytes; header.offset is the file offset for the first byte in this chunk.
- DATA_ACK: Cumulative acknowledgement; header.offset is the next expected offset (total bytes durably written).
- DONE: Marks end-of-file from sender to receiver; explicitly acknowledged by DONE_ACK.
- ERROR: Unrecoverable error notification (compact payload: code + detail only).
- EOT: End-of-transmission (after the final file).
- EOT_ACK: Acknowledgement for EOT.

Notes
- Endianness: All multi-byte integers on wire are little‑endian. Implementations must convert to/from host order.
- Packet framing: fixed-length header (type, wire_version, payload_len, seq, offset + header CRC) + variable-length data (0..N) + trailer CRC32. No zero padding; the last packet of a file can be shorter. CRC32 covers Header+Data.
- wire_version: a reserved byte immediately after type. It must be set to 0 by senders and validated as 0 by receivers. Non‑zero causes an incompatible version error (VAL_ERR_INCOMPATIBLE_VERSION with VAL_ERROR_DETAIL_VERSION_MAJOR). The field is reserved for future wire format changes and is not used for feature negotiation.
- seq is monotonically increasing per file (currently informational; offset is normative for ordering/ACKing).
- Strings (filename, sender_path) are UTF‑8 on the wire. Receivers should treat them as UTF‑8; sanitization preserves bytes but may truncate to fit size limits.


## Session lifecycle overview

1) Handshake (once per session)
- Sender → HELLO: { VAL_MAGIC, version, packet_size, features, required, requested }
- Receiver → HELLO: { VAL_MAGIC, version, packet_size=min(local,peer), features, required, requested }
- Both sides set effective_packet_size = min(local, peer).
- Feature policy:
  - Each side advertises its compiled-in features.
  - Each side validates its own required flags locally (fail fast if unsupported).
  - Each side ensures the peer’s required ⊆ our features. If not:
    - Send ERROR { code=VAL_ERR_FEATURE_NEGOTIATION, detail=missing_mask } and abort.
- On version-major mismatch, fail handshake (sender/receiver may send ERROR then abort).

2) Per-file transfer (repeat for N files)
- Sender → SEND_META: { filename (basename only), sender_path (hint), file_size, file_crc32 }
- Resume negotiation (see below)
- Sender streams DATA chunks; Receiver replies with DATA_ACK per chunk.
- Sender → DONE when all bytes sent for the file.
- Receiver validates final CRC32 == file_crc32 from metadata. If mismatch, treat as error.

3) Session end
- Sender → EOT after the last file.
- Receiver → EOT_ACK and exits receive loop.


## Resume negotiation and VERIFY exchange

Immediately after SEND_META, the sender issues a resume query:

- Sender → RESUME_REQ
- Receiver decides based on configured resume mode/policy and any existing partial file:
  - START_ZERO: start from offset 0.
  - START_OFFSET: resume from existing_size (append mode).
  - VERIFY_FIRST: verify trailing window before resuming.
  - SKIP_FILE: receiver will not write this file; sender should skip sending data and proceed to DONE (receiver will ACK DONE).
  - ABORT_FILE: receiver will not accept this file; sender should abort this file and continue with the next.
- Receiver → RESUME_RESP: { action, resume_offset, verify_crc, verify_len }

If action == VERIFY_FIRST:
- Sender computes CRC over [resume_offset - verify_len, resume_offset) of its local file.
- Sender → VERIFY: echo the RESUME_RESP with verify_crc replaced by sender’s computed CRC (same struct on the wire).
- Receiver compares CRCs and replies with a status:
  - Receiver → VERIFY: payload is int32 status (VAL_OK or VAL_ERR_RESUME_VERIFY)
- Outcomes:
  - If VAL_OK: Sender starts sending from resume_offset; Receiver seeds its CRC state with existing bytes.
  - If VAL_ERR_RESUME_VERIFY: Both fall back to offset 0 (start fresh).

If action == START_OFFSET: Sender seeks to resume_offset and starts streaming DATA from there.
If action == START_ZERO: Sender starts streaming DATA from offset 0.
If action == SKIP_FILE: Sender skips data and proceeds to DONE; receiver will ACK DONE.
If action == ABORT_FILE: Sender aborts this file.


## Data path

- Sender loops reading up to max_payload bytes per packet:
  - max_payload is bounded by an implementation target frame size (MTU). Actual frame size varies: sizeof(header) + payload_len + sizeof(trailer_crc).
  - Sender → DATA(payload, header.offset=current_file_offset)
  - Receiver validates CRCs (header CRC and trailer CRC over Header+Data), writes payload, updates CRC state, increments written by payload_len.
  - Receiver → DATA_ACK (no payload), header.offset is the next expected offset (cumulative).
- Sender waits for DATA_ACK for each DATA; if timeout:
  - Retries with exponential backoff up to configured attempts.
  - On failure after retries, the transfer aborts.

DONE handling
- Sender → DONE (offset == file_size, no payload)
- Receiver computes file CRC32 and compares to meta.file_crc32.
  - Equal: file complete OK; Receiver → DONE_ACK.
  - Mismatch: return VAL_ERR_CRC to caller (no DONE_ACK is sent; sender will error on timeout/retries).


## EOT and multi-file semantics

- After the last file completes, the sender sends EOT automatically (public EOT is not exposed).
- Receiver responds with EOT_ACK and exits the receive loop.
- Sender waits for EOT_ACK with bounded retries and retransmits EOT on timeout.


## ERROR packet usage

- Payload: { int32 code, uint32 detail } — no strings on the wire.
- Primary use today: handshake failures (feature negotiation issues, version mismatch scenarios).
- Current receive loop for file transfer treats incoming ERROR as protocol error and aborts; future versions may extract code/detail and store via val_internal_set_last_error.
- Sessions track last error internally and expose it via val_get_last_error(session, &code, &detail).


## Timeouts and retries (high-level)

- Timeouts are adaptive and derived from measured RTT using RFC 6298 (SRTT/RTTVAR) with Karn’s rule, then clamped to [`min_timeout_ms`, `max_timeout_ms`].
- On timeout, operations retry with exponential backoff (base doubles each attempt) up to the configured retry count for that operation.
- A monotonic clock is required in default builds. If enforcement is disabled and no clock is provided, a conservative fixed timeout of `max_timeout_ms` is used for all waits.


## Edge cases and protocol invariants

- Sanitization: filename is sanitized basename only; receiver constructs output path using its own output_directory + filename. sender_path is informational only.
- Unexpected SEND_META while a file is in progress: protocol error (receiver aborts current file).
- DATA offsets must match the receiver’s current write position; mismatches are protocol errors.
- Both header CRC and trailer CRC must verify; otherwise the packet is rejected.
- Header size is fixed; data length varies up to a negotiated/configured maximum frame size. Frames are not padded; final file packet may be smaller.
- seq is incremented per-packet per-file but not used for reordering; offset is authoritative.


## Minimal sequence examples

Happy path (one file, no resume):
1. S → HELLO
2. R → HELLO
3. S → SEND_META
4. S → RESUME_REQ
5. R → RESUME_RESP { START_ZERO }
6. [loop] S → DATA(0), R → DATA_ACK(0)
7. ...
8. S → DONE
9. R validates CRC OK, R → DONE_ACK
10. S → EOT
11. R → EOT_ACK

Resume with CRC_VERIFY accepted:
1–3 as above
4. S → RESUME_REQ
5. R → RESUME_RESP { VERIFY_FIRST, resume_offset, verify_crc=crc_tail_R, verify_len }
6. S computes crc_tail_S over [resume_offset-verify_len, resume_offset)
7. S → VERIFY { resume_offset, verify_len, verify_crc=crc_tail_S } (same struct as RESUME_RESP)
8. R compares crc_tail_S vs crc_tail_R and responds
9. R → VERIFY { int32 status=VAL_OK }
10. S seeks resume_offset
11. [loop] S → DATA(resume_offset), R → DATA_ACK(resume_offset)
12. S → DONE; R validates CRC OK
13. S → EOT; R → EOT_ACK

Append-mode resume accepted:
1–4 as above
5. R → RESUME_RESP { START_OFFSET, resume_offset=existing_size }
6. S seeks existing_size
7. Continue with DATA/DATA_ACK loop

Error on feature negotiation:
1. S → HELLO { required has bit X }
2. R determines X not supported locally
3. R → ERROR { code=VAL_ERR_FEATURE_NEGOTIATION, detail=missing_mask }
4. R aborts; S records last_error and aborts


---

For field definitions and API details, see `DEVELOPMENT.md` and `include/val_protocol.h`. For a working demonstration, build and run the TCP examples under `examples/tcp/`.

Developer diagnostics
- The library has compile-time gated logging to help analyze sequences like resume verify and ACK retries. Enable it by setting `VAL_LOG_LEVEL` at build time and provide a sink via `cfg.debug.log`.
 - Optional compile-time diagnostics:
   - Metrics (`VAL_ENABLE_METRICS`): packet/byte counters, timeouts, retransmits, CRC errors, files sent/recv, RTT samples.
   - Wire audit (`VAL_ENABLE_WIRE_AUDIT`): per-packet send/recv counters and inflight window snapshots.

Transport integration notes
- The library can consult an optional `transport.is_connected` hook before blocking waits/retransmits to fail fast on disconnects. If the hook is absent, it assumes connected.
- After control packets (HELLO, DONE, EOT, ERROR), the library may call an optional `transport.flush` hook to reduce latency. Absent the hook, flush is a no‑op.
 - Timeout semantics for `transport.recv`: return 0 with `*received == 0` to indicate a timeout (recoverable). Return <0 only for fatal I/O errors. On success, return 0 with `*received == exact_len`.


## Implemented pre‑1.0 changes (mid‑stream recovery)

We simplified and hardened recovery without preserving previous on‑wire semantics (pre‑1.0 policy):

1) Cumulative ACK semantics for DATA_ACK
- DATA_ACK.header.offset now means "next expected offset" (total bytes durably written).
- Receiver behavior:
  - On valid DATA at offset == written: write, advance, ACK with offset = new written.
  - On duplicate DATA at offset < written: do not write; re‑ACK with offset = written (idempotent).
  - On offset > written: do not write; ACK with current written (soft resync signal).
  - On CRC failure: drop packet silently (no ACK); sender will retransmit on timeout.
- Sender behavior:
  - If ACK.offset > current send pointer: seek/advance to ACK.offset and continue.
  - If ACK.offset == current send pointer: retransmit last DATA.
  - If ACK.offset < current send pointer: seek back to ACK.offset and resume.

2) Sender retransmission on ACK timeout
- On ACK timeout, the sender resends the last DATA (bounded retries with exponential backoff).

3) DONE acknowledgement
- DONE_ACK added. Receiver replies after final CRC verification. Sender retransmits DONE on timeout until DONE_ACK is received.

These keep the fixed‑size framing and stop‑and‑wait simplicity while enabling mid‑stream resync.

## Adaptive TX and streaming pacing

- Adaptive transmitter uses window-only rungs: 64, 32, 16, 8, 4, 2, and stop-and-wait (1 in flight). Larger enum value = larger window = higher throughput potential. STOP_AND_WAIT (1) is the slowest rung.
- Streaming is sender pacing, not a separate mode. When negotiated, the sender interleaves short ACK polls derived from RTT during DATA_ACK waits to keep the window full without long blocking sleeps.
- Negotiation: HELLO carries a compact `streaming_flags` byte.
  - bit0: this endpoint can stream when sending to the peer
  - bit1: this endpoint accepts an incoming peer that streams to it
  - Effective permissions are directional and can be queried at runtime via `val_get_streaming_allowed()`.
- Pacing policy: poll interval ≈ SRTT/4 clamped to [2, 20] ms (falls back to a conservative value before enough samples). Each ACK wait has a fixed deadline; if the deadline elapses, the code escalates to a real timeout and retries with exponential backoff.

Windowed send/ACK behavior
- Sender maintains up to `window` in-flight DATA packets and advances the send pointer as DATA_ACKs arrive.
- DATA_ACK is cumulative: ACK.offset is the next expected byte offset; sender may drop any already-ACKed inflight and continue filling the window.
- On gaps, duplicates, or soft resync, the receiver re-ACKs its current `written` offset without writing out-of-order data.
- On timeout, the sender retries with exponential backoff; a DATA_NAK may prompt a targeted rewind to the indicated offset.
