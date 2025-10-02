# VAL Protocol — Development Guide

This document reflects the current implementation in `include/` and `src/` and is kept in sync with the code. It covers architecture, features, API, protocol flows, build/tests, and notes.

## Overview

- Goal: A simple, robust, blocking‑I/O file transfer protocol with a fixed header and variable‑length payload per frame (bounded by a negotiated MTU), CRC integrity, resume support, and zero dynamic allocations in hot paths.
- Language/Build: C99 library with CMake builds for Windows (MSVC) and Linux/WSL (GCC). VS Code presets are provided.
- Philosophy:
  - Block‑oriented with a negotiated MTU per session; frames = fixed header + variable data + trailer CRC (no padding).
  - Minimal state machine; explicit EOT/EOT_ACK to end a multi‑file session.
  - Integrity via IEEE 802.3 CRC32. CRC provider is pluggable.
  - No user‑editable compiled features at runtime; a built‑in feature mask is advertised during handshake.
  - Allocation‑free control/error path: `val_status_t` code + detail mask.
  - Pre‑1.0 compatibility: we may change wire semantics; strict major version check in HELLO prevents cross‑version sessions.
  - A reserved `wire_version` byte exists in every packet header (after `type`). It is always 0 in the base protocol; receivers validate it and reject non‑zero as incompatible.

## Repository layout

- `include/val_protocol.h` — Public API: configuration, session lifecycle, send/receive, utilities.
- `include/val_errors.h` — Public status codes (negative for errors) and a 32‑bit detail mask split into categories.
- `include/val_error_strings.h` — Host‑only helpers to format errors to strings (not compiled into the core unless enabled).
- `src/val_core.c` — CRC implementation and wrappers, handshake, packet send/recv, session setup.
- `src/val_sender.c` — Sender: metadata, resume negotiation, data loop, DONE/EOT.
- `src/val_receiver.c` — Receiver: resume decisions, verify exchange, receive/ACK loop, CRC validate, DONE/EOT.
- `src/val_internal.h` — Internal packet types/headers/payloads, helpers, session struct.
- `src/val_error_strings.c` — Implementation of host‑only error formatting utilities (guarded by a CMake option).
- `examples/tcp/` — Cross‑platform TCP examples: `val_example_send.c`, `val_example_receive.c`, and `common/tcp_util.*`.
- `unit_tests/` — CTest executables (no external framework): send/receive, recovery (resume/corruption), policies, support shims.
- `CMakeLists.txt`, `CMakePresets.json` — Build configuration (Windows/MSVC, Linux/WSL).
- Docs: `README.md`, `DEVELOPMENT.md`, `PROTOCOL_FLOW.md`, `PROJECT_STRUCTURE.md`.

Build options of note (set via CMake cache variables):
- `VAL_ENABLE_ERROR_STRINGS` (ON by default): builds `val_error_strings` and links it only into examples/tests; core library remains numeric‑only.

## Public API (short tour)

Header: `include/val_protocol.h` (source of truth for public constants and types).

- Sessions
  - `val_session_t* val_session_create(const val_config_t* cfg)`
  - `void val_session_destroy(val_session_t* s)`
- Transfer
  - `val_status_t val_send_files(val_session_t*, const char* const* paths, size_t count, const char* sender_path)`
  - `val_status_t val_receive_files(val_session_t*, const char* output_directory)`
- Utilities
  - `uint32_t val_crc32(const void* data, size_t len)`
  - `uint32_t val_get_builtin_features(void)`
  - `val_status_t val_get_last_error(val_session_t*, val_status_t* code_out, uint32_t* detail_mask_out)`
  - `void val_clean_filename(...)`, `void val_clean_path(...)`

### Configuration `val_config_t` (key parts)

- Transport (blocking)
  - `int (*send)(void* io_ctx, const void* data, size_t len)` — Must send exactly `len` bytes.
  - `int (*recv)(void* io_ctx, void* buf, size_t buf_size, size_t* received, uint32_t timeout_ms)` — Reads exactly `buf_size` bytes or times out; returns 0 on success.
  - Optional: `int (*is_connected)(void* io_ctx)` — Return 1 if connected, 0 if disconnected, <0 if unknown. If NULL, the core assumes connected.
  - Optional: `void (*flush)(void* io_ctx)` — Best‑effort flush after control packets. If NULL, treated as no‑op.
  - `void* io_context`
- Filesystem
  - Thin wrappers: `fopen/fread/fwrite/fseek/ftell/fclose`
- CRC provider (optional)
  - One‑shot and streaming hooks; if any pointer is NULL, built‑in software implementation is used.
- System
  - `uint32_t (*get_ticks_ms)(void)`, `void (*delay_ms)(uint32_t)`
  - A monotonic millisecond clock is required in default builds (enforcement ON). When enforcement is explicitly disabled, the clock becomes optional and adaptive timeouts degrade to fixed `max_timeout_ms`.
- Buffers (caller‑owned)
  - `void* send_buffer`, `void* recv_buffer`, `size_t packet_size` (MTU; validated to [`VAL_MIN_PACKET_SIZE`, `VAL_MAX_PACKET_SIZE`])
- Resume
  - Resume modes: six-mode system — `VAL_RESUME_NEVER`, `VAL_RESUME_SKIP_EXISTING`, `VAL_RESUME_CRC_TAIL`, `VAL_RESUME_CRC_TAIL_OR_ZERO`, `VAL_RESUME_CRC_FULL`, `VAL_RESUME_CRC_FULL_OR_ZERO` (+ `crc_verify_bytes` for tail modes)
  - Receiver behavior is solely driven by mode; there are no policy overrides.
- Timeouts (adaptive)
  - Adaptive RTT-based timeouts with Karn’s algorithm. Configure only bounds:
    - `min_timeout_ms` (floor), `max_timeout_ms` (ceiling)
  - A monotonic clock is required in default builds to sample RTT and compute RTO. If enforcement is disabled and no clock is provided, the library uses `max_timeout_ms` as a conservative fixed timeout.
- Retries & backoff
  - `handshake_retries`, `meta_retries`, `data_retries`, `ack_retries`, `backoff_ms_base` (exponential: base, 2×base, …)
- Features (handshake policy)
  - `required`, `requested` — Required validated locally; requested sanitized against built‑in features and peer advertisement.
- Callbacks (optional)
  - `on_file_start`, `on_file_complete`, `on_progress`
- Debug logging (optional)
  - Compile‑time gated by `VAL_LOG_LEVEL` (0..5). Provide `debug.log`/`debug.context` to receive messages.
  - Runtime filter: `debug.min_level` (messages with `level <= min_level` are forwarded). If left 0, a sensible default is applied at session creation.

## Features and policy

Features:
  - Core behavior is implicit and not represented by feature bits.
  - Optional (negotiated via HELLO; included in `val_get_builtin_features()` when compiled in):
    - `VAL_FEAT_ADVANCED_TX` (bit 0) — Advanced transmitter/resume capabilities (placeholder)
- Resume strategies
  - Resume modes: NONE, APPEND, CRC_VERIFY
  - Policies removed: receiver decisions are determined by the selected resume mode.
  - NONE(0) uses resume.mode; SAFE_DEFAULT(1); ALWAYS_START_ZERO(2); ALWAYS_SKIP_IF_EXISTS(3); SKIP_IF_DIFFERENT(4); ALWAYS_SKIP(5); STRICT_RESUME_ONLY(6)
  - Mismatch/anomaly defaults are set during `val_session_create()` based on policy (see `src/val_core.c`).

## Protocol: wire format and flow

- Endianness: all on‑wire multi‑byte integers are little‑endian.
- Framing: fixed header (with header CRC) + variable payload (0..N) + trailer CRC32 over header+payload (no padding).
  - Header layout (base/core): `type`, `wire_version` (=0), `payload_len`, `seq`, `offset`, `header_crc`.
- DATA_ACK semantics: cumulative; ACK.offset is the next expected offset (total bytes durably written).

### Packet types (high‑level)

- Control/handshake: HELLO, ERROR
- Metadata/resume: SEND_META, RESUME_REQ, RESUME_RESP, VERIFY, DONE
- Data path: DATA, DATA_ACK (cumulative)
- Session end: EOT, EOT_ACK

### Handshake (once per session)

1. Sides exchange HELLO: version (strict major), packet_size proposal, features, required/requested.
2. Both adopt `effective_packet_size = min(local, peer)`.
3. Required features are validated (fail fast if missing); requested are sanitized to local and peer.
4. On incompatibility, send ERROR { code=VAL_ERR_FEATURE_NEGOTIATION, detail=missing_mask } and abort.

Wire version enforcement
- Independently of HELLO’s version check, every packet header carries `wire_version`. Receivers must verify it is 0 in the base protocol. A non‑zero value results in `VAL_ERR_INCOMPATIBLE_VERSION` with `VAL_ERROR_DETAIL_VERSION_MAJOR` and aborting the operation.

### Resume negotiation and VERIFY exchange

Immediately after SEND_META the sender issues RESUME_REQ. The receiver responds based on policy and local file state:

- RESUME_RESP actions: START_ZERO, START_OFFSET, VERIFY_FIRST, SKIP_FILE, ABORT_FILE.
  - START_ZERO: sender starts at offset 0.
  - START_OFFSET: sender seeks to `resume_offset` and continues from there.
  - VERIFY_FIRST: receiver provides `{resume_offset, verify_len, verify_crc_R}`; sender computes its tail CRC and replies VERIFY with `{verify_crc_S}`. Receiver replies VERIFY status (VAL_OK or VAL_ERR_RESUME_VERIFY). On OK, both resume from `resume_offset` with seeded CRC; on mismatch or timeout, policy default applies (often START_ZERO).
  - SKIP_FILE: receiver will not write this file; sender should skip sending data for it and proceed to DONE (receiver will ACK DONE).
  - ABORT_FILE: receiver will not accept this file; sender should abort this file and continue to the next (session continues).

### Data transfer

- Sender loops sending DATA up to MTU payload; receiver validates CRCs and writes, then replies DATA_ACK with the next expected offset.
- On ACK timeout, sender retries with exponential backoff up to configured attempts; on failure, transfer aborts with last error recorded.
- DONE: sender marks end‑of‑file; receiver validates final CRC32 against metadata and replies DONE_ACK on success.

### EOT and multi‑file semantics

- After the last file, sender sends EOT; receiver replies EOT_ACK and exits receive loop.

### ERROR packet usage

- Payload: `{ int32 code, uint32 detail }` — compact, no strings. Primarily used during handshake failures today.
- Sessions track last error (`val_get_last_error`). On wire, ERROR uses compact payload: `{ int32 code, uint32 detail }` (LE). The 32‑bit detail mask is segmented into categories (Network, CRC, Protocol, FS, Context).

Error system (host vs MCU)
- The core library records numeric `val_status_t` and a 32‑bit detail mask (see `include/val_errors.h`).
- Optional host‑only helpers in `val_error_strings.*` can format human‑readable strings and reports; enabled by `VAL_BUILD_HOST_UTILS`.
- Examples/tests print rich error messages when host utils are linked; otherwise they print numeric code/detail.

## Examples: TCP send/receive

- Files: `examples/tcp/val_example_send.c`, `examples/tcp/val_example_receive.c`, `examples/tcp/common/tcp_util.*`
- Transport: minimal TCP helper (Winsock on Windows, BSD sockets on POSIX).
- Command line flags (optional): `--mtu N`, `--resume MODE` and `--tail-bytes N` to configure packet size and resume behavior.

## Building and running

Presets in `CMakePresets.json` cover Windows and Linux/WSL.

### Windows (Visual Studio 2022, x64)

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
.\build\windows-release\Release\val_example_receive.exe --help
.\build\windows-release\Release\val_example_send.exe --help
```

### Linux / WSL2

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/val_example_receive --help
./build/linux-release/val_example_send --help
```

## Tests

- Unit tests live under `unit_tests/` and are registered with CTest. Suites cover send/receive, policy behaviors, and corruption/recovery scenarios. Artifacts are placed under the build tree.

## Internal notes & invariants

- Handshake runs once per session; subsequent operations assume agreed `packet_size` and feature masks.
- Header CRC and trailer CRC must both validate for a packet to be accepted.
- Transports must be blocking and may be invoked with different lengths for header/payload/trailer segments.
- Multi‑file sessions end with EOT/EOT_ACK.
- Resume never truncates files; policies choose between START_ZERO, START_OFFSET, VERIFY_FIRST, SKIP_FILE, ABORT_FILE.

## Roadmap / TODO

- Optional accessor for peer features (if apps require it).
- More examples (serial/USB) and filesystem adapters.
- Expand docs with protocol field tables and error detail masks.
