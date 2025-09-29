# VAL Protocol — Development Guide

This document explains the current state of the project: architecture, protocol features and flows, public API, internal behaviors, build/test instructions, and next steps. It’s intended to help you quickly pick up where things are.


## Overview

- Goal: A simple, robust, blocking-I/O file transfer protocol with a fixed header and variable-length payload per frame (bounded by a negotiated MTU), CRC integrity, resume support, and zero dynamic allocations in hot paths.
- Language/Build: C99 library with CMake builds for Windows (MSVC) and Linux/WSL (GCC). VS Code presets and variants are included.
- Philosophy:
  - Block-oriented with a negotiated MTU per session; frames consist of a fixed header + variable data + trailer CRC (no padding).
  - Minimal state machine; explicit EOT/EOT_ACK to end a multi-file session.
  - Integrity via IEEE 802.3 CRC32. CRC provider is pluggable to enable MCU acceleration.
  - No user-editable “features”; compile-time feature set is advertised. Users specify required/requested flags to gate behavior.
  - Allocation-free error state: code + detail mask only.
  - Pre-1.0 compatibility: backward compatibility is not guaranteed until v1.0. We may change wire semantics and packet meanings; the strict major version check in HELLO prevents cross-version sessions.


## Repository layout

- `include/val_protocol.h` — Public API: configuration, session lifecycle, send/receive, utilities.
- `src/val_core.c` — CRC implementation and wrappers, handshake, packet send/recv, session setup.
- `src/val_sender.c` — Sender-side file metadata, resume negotiation, data loop, DONE/EOT.
- `src/val_receiver.c` — Receiver-side resume decisions, receive/ack loop, per-file CRC verify, EOT/EOT_ACK.
- `src/val_internal.h` — Internal packet types, headers, payloads, and session struct.
- `tests/test_minimal.c` — Minimal CRC correctness and smoke test.
- `examples/example_loopback.c` — Duplex in-memory transport demo with a receiver thread (actually transfers a file end-to-end).
- `CMakeLists.txt`, `CMakePresets.json`, `.vscode/` — Build configuration and VS Code integration.


## Public API (short tour)

Header: `include/val_protocol.h`.

- Sessions
  - `val_session_t* val_session_create(const val_config_t* cfg)`
  - `void val_session_destroy(val_session_t* s)`
- Transfer
  - `val_status_t val_send_files(val_session_t*, const char* paths[], size_t count, const char* base_dir_opt)` — sends 1..N files; automatically sends EOT at the end.
  - `val_status_t val_receive_files(val_session_t*, const char* output_directory)`
- Utilities
  - `uint32_t val_crc32(const void* data, size_t len)` (one-shot helper)
  - `uint32_t val_get_builtin_features(void)` (compile-time feature mask)
  - `void val_get_last_error(val_session_t*, val_status_t* code_out, uint32_t* detail_mask_out)`
  - `size_t val_clean_filename(...)`, `size_t val_clean_path(...)`

### Configuration `val_config_t` (key parts)

- Transport (blocking)
  - `int (*send)(void* io_ctx, const void* data, size_t len)` — Must send exactly `len` bytes (may be less than MTU for final frames).
  - `int (*recv)(void* io_ctx, void* buf, size_t buf_size, size_t* received, uint32_t timeout_ms)` — Reads exactly `buf_size` bytes unless a timeout occurs; returns 0 on success and sets `*received = buf_size`.
  - `void* io_context`
- Filesystem
  - `fopen/fread/fwrite/fseek/ftell/fclose` — Thin wrappers over stdio or platform-specific FS.
- System
  - `uint32_t (*get_ticks_ms)(void)` — Monotonic tick (can be stubbed for examples).
  - `void (*delay_ms)(uint32_t)`
- Debug logging (optional)
  - Compile-time gated by `VAL_LOG_LEVEL` macro (0=OFF, 1=ERROR, 2=INFO, 3=DEBUG). Defaults: Debug builds=3, Release builds=0.
  - `void (*debug.log)(void* ctx, int level, const char* file, int line, const char* message)` and `void* debug.context`.
  - If no sink is provided, logs are dropped even when enabled.
- Buffers (caller-owned)
  - `void* send_buffer`, `void* recv_buffer`, `size_t packet_size`
- Resume
  - `val_resume_mode_t mode` — `VAL_RESUME_NONE|VAL_RESUME_APPEND|VAL_RESUME_CRC_VERIFY`
  - `uint32_t verify_bytes` — Window size for CRC verify mode (if used)
- Timeouts (ms)
  - `handshake_ms`, `meta_ms`, `data_ms`, `ack_ms`, `idle_ms` — Zero picks sensible defaults.
- Retries & backoff
  - `handshake_retries`, `meta_retries`, `data_retries`, `ack_retries`, `backoff_ms_base` (exponential: base, 2×base, …)
- Features (policy)
  - `uint32_t required`, `uint32_t requested` — Required must be supported locally; requested are sanitized during handshake.
- CRC provider (optional)
  - Hook functions: `crc_init`, `crc_update`, `crc_final`, `crc32` and `void* crc_context` per-session.
- Callbacks (optional)
  - `on_file_start(const char* filename, const char* sender_path, uint64_t size, uint64_t offset)`
  - `on_file_complete(const char* filename, const char* sender_path, val_status_t result)`
  - `on_progress(uint64_t bytes_sent_or_received, uint64_t total)`


## Features and policy

- Compile-time features (examples):
  - `VAL_FEAT_CRC_RESUME` — CRC-seeded resume verification.
  - `VAL_FEAT_MULTI_FILES` — Multi-file send/receive within a single session.
- `VAL_BUILTIN_FEATURES` is the advertised mask compiled into the library.
- User config sets `required` and `requested`:
  - Required are validated locally before handshake; fail fast if missing.
  - Requested are sanitized against local support and peer advertisement during handshake.
- `val_get_builtin_features()` returns local compile-time features; the session also tracks `peer_features` internally after handshake.


## Protocol: wire format and flow

- Endianness: All multi-byte integers on the wire are little‑endian. Host conversion is handled internally.
- Packet framing: fixed header (type/payload_len/seq/offset with header CRC) + variable-length payload (0..N) + trailer CRC32 (over header+payload). No zero padding.
- Sequence numbers are monotonically increasing per file (informational); 64‑bit offset + cumulative ACKs drive ordering and recovery.

### Packet types (not exhaustive, internal)

- Control/handshake: `HELLO`, `ERROR`
- Metadata/resume: `SEND_META`, `RESUME_REQ`, `RESUME_RESP`, `VERIFY`, `DONE`
- Data path: `DATA`, `DATA_ACK` (cumulative)
- Session end: `EOT`, `EOT_ACK`

### Handshake (once per session)

1. Sides exchange `HELLO` with: protocol version (strict major), `packet_size`, and `features`.
2. Packet size is negotiated to the minimum of both sides.
3. Each side advertises compiled-in features, validates `required` locally, sanitizes `requested`, and stores `peer_features`.
4. If an incompatibility is detected (e.g., missing required), send `ERROR` and fail the session.

### Resume

- Modes:
  - NONE: always start from zero.
  - APPEND: receiver determines current output size and requests resume from that offset.
  - CRC_VERIFY: receiver computes CRC of the last `verify_bytes` region and asks sender to verify/seed CRC to continue safely.
- On resume, receiver seeds its CRC with the verified region and continues. On mismatch, automatically falls back to offset 0.

### Data transfer

- Sender sends `SEND_META` per file (name, size, whole-file CRC).
- Resume negotiation occurs (APPEND or CRC_VERIFY) via `RESUME_REQ/RESP` and optional `VERIFY` exchange.
- Sender streams `DATA` packets; receiver replies with cumulative `DATA_ACK` where `offset` is the next expected byte position.
- Sender `DONE` indicates end-of-file; receiver computes final CRC and compares with metadata, then sends `DONE_ACK` on success.
- After all files, sender sends `EOT`; receiver replies `EOT_ACK` and exits receive loop.

### Integrity

- Header CRC protects control metadata (early reject); trailer CRC protects payload and header against transit errors.
- Per-file CRC32 (IEEE 802.3) validates file integrity at completion.
- CRC implementation is pluggable; default is table-based software in `val_core.c`.

### Errors and retries

- `val_internal_send_error` sends a compact `ERROR` packet with code and detail mask only (no message strings on the wire).
- Session stores last error (`val_status_t code`, `uint32_t detail_mask`), retrievable via `val_get_last_error`.
- Data/DONE/EOT ACK waits include bounded retries with exponential backoff; sender retransmits on timeout.

### Mid-stream recovery (pre‑1.0 implemented)

We adopted cumulative ACK semantics, sender retransmission on ACK timeout, and a `DONE_ACK` for tail completion. See `PROTOCOL_FLOW.md` for precise sequences.


## Example: duplex loopback

- File: `examples/example_loopback.c`
- Transport: two in-memory byte FIFOs wired A↔B. On Windows, uses Win32 threads/condition variables; on POSIX, pthreads.
- It spawns a receiver thread, creates a 64 KiB demo file, transfers it, and prints progress and completion.
- Output artifacts are created next to the executable under `artifacts/` (e.g., `artifacts/input_demo.bin` and `artifacts/out/input_demo.bin`).


## Building and running

Presets are defined in `CMakePresets.json` and VS Code integration is present via `.vscode`.

### Windows (Visual Studio 2022, x64)

- Configure and build (preset) — in VS Code, select the preset `windows-release` or `windows-debug`.
- Manual PowerShell commands (optional):

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
.
\build\windows-release\Release\val_test.exe
.
\build\windows-release\Release\val_example.exe
```

Expected example output: progress lines and `sender finished with status 0`.

### Linux / WSL2

- Presets `linux-debug` / `linux-release` are configured to place the binary dir under `$HOME` to avoid `/mnt` permissions.
- Example (bash, optional):

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/val_test
./build/linux-release/val_example
```


## Tests

- `tests/test_minimal.c` checks the CRC32 vector for “123456789” equals `0xCBF43926` and prints `OK` on success.
- More end-to-end and negotiation tests are good future additions (see TODO).


## Internal notes & invariants

- Handshake runs once per session. All subsequent operations assume agreed `packet_size` and feature masks.
- Both header CRC and trailer CRC must validate before a packet is accepted.
- Sender must respect negotiated MTU; frames may be shorter on the wire (final chunk). Transport wrappers are expected to be blocking and may be called with different lengths for header/payload/trailer segments.
- Multi-file send should end with `EOT` and await `EOT_ACK`.
- Resume APPEND uses output file size; CRC_VERIFY uses a trailing window (`verify_bytes`) for safety.
- Error reporting uses `val_status_t` and a compact detail mask; no dynamic message buffers are stored in-session.


## Extensibility

- CRC hooks allow hardware-accelerated or RTOS-specific CRC engines.
- Transports can wrap UART/USB/TCP/etc. as long as they obey blocking fixed-size semantics.
- Filesystem hooks allow embedded storage or platform abstractions.


## Roadmap / TODO

- Public accessor: `val_get_peer_features(session, uint32_t* out_mask)` for user inspection.
- More tests:
  - End-to-end loopback tests that assert file equality and status codes.
  - Handshake failures (version mismatch, required feature missing).
  - Resume scenarios (clean resume, CRC_VERIFY mismatch fallback).
- Logging hooks and optional tracing without allocations.
  - Implemented: compile-time gated logging with optional user sink (see above). Additional formatting variants or categorization can be added if needed.
- Additional examples: serial transport shim, chunked flash FS adapter.
- Documentation: protocol field definitions table, error detail masks reference.


## Contributing & style

- C99, keep portability in mind (Windows MSVC, Linux GCC/WSL).
- Prefer allocation-free control paths; avoid heap use in steady state.
- Validate arguments; fail fast with clear `val_status_t` codes and detail mask.
- Keep public API stable; document changes here and in headers.


## Quick reference

- Built-in features: `val_get_builtin_features()`
- Last error: `val_get_last_error(session, &code, &details)`
- One-shot CRC: `val_crc32(buf, len)`
- Send files: `val_send_files(session, paths, count, base_dir)` (auto EOT)
- Receive loop: `val_receive_files(session, output_dir)`


---

If you’re resuming work, start by running the example (`val_example`) to validate the environment, then add tests for any behavior you plan to modify. For larger changes, update this guide alongside the headers to keep the design and docs in sync.
