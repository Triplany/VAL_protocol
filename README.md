# VAL Protocol

A small, robust, blocking-I/O file transfer protocol library in C. Frames are fixed header + variable payload (bounded by a negotiated MTU) + trailer CRC. It supports resume, cumulative ACKs, per-file CRC integrity, and avoids dynamic allocations in steady state.

- Development guide: [DEVELOPMENT.md](./DEVELOPMENT.md)
- Packet flow reference: [PROTOCOL_FLOW.md](./PROTOCOL_FLOW.md)
- Pre‑1.0 policy: backward compatibility isn’t guaranteed until v1.0. The current on‑wire behavior uses cumulative DATA_ACKs, DONE_ACK, and two CRCs (header + trailer). All multi‑byte fields are little‑endian.
  - The packet header includes a reserved `wire_version` byte (after `type`) that is always 0 in the base protocol; receivers validate it and reject non‑zero as incompatible for future evolution.
- Public headers: `include/`
- Sources: `src/`
- Examples: `examples/tcp/` (TCP send/receive)
- Unit tests: `unit_tests/` (CTest executables; artifacts like ut_artifacts live under the build tree)

## Features (from source)

- Feature bits: see `include/val_protocol.h` and `val_get_builtin_features()`
  - Only optional features are negotiated; core behavior is implicit.
  - Optional (negotiated): e.g., `VAL_FEAT_ADVANCED_TX` (bit 0)
- Resume configuration
  - Six modes (see `val_resume_mode_t`): `VAL_RESUME_NEVER`, `VAL_RESUME_SKIP_EXISTING`, `VAL_RESUME_CRC_TAIL`,
    `VAL_RESUME_CRC_TAIL_OR_ZERO`, `VAL_RESUME_CRC_FULL`, `VAL_RESUME_CRC_FULL_OR_ZERO`.
  - Tail modes use a trailing verification window (`crc_verify_bytes`); FULL modes verify a full prefix (with internal caps for large files).
  - Sizes/offsets on wire are 64‑bit LE.
- Adaptive transmitter
  - Window-only rungs: `VAL_TX_WINDOW_64/32/16/8/4/2` and `VAL_TX_STOP_AND_WAIT` (larger enum = larger window; STOP_AND_WAIT (1) is the slowest).
  - Streaming is not a distinct mode; it’s sender pacing. If both sides agree, the sender uses RTT-derived micro-polling between ACK waits to keep the pipe full.
  - Streaming negotiation happens in HELLO via compact `streaming_flags`:
    - bit0: this endpoint can stream when sending
    - bit1: this endpoint accepts an incoming peer that streams to it
    - Effective permissions are directional and can be queried with `val_get_streaming_allowed()`.
  - RTT-derived pacing: poll interval ≈ SRTT/4 clamped to 2–20 ms; falls back to a conservative value when no samples exist.
  - Accessors:
    - `val_get_current_tx_mode(session, &out_mode)` — current window rung
    - `val_get_streaming_allowed(session, &send_ok, &recv_ok)` — negotiated permissions
    - `val_is_streaming_engaged(session, &engaged)` — whether pacing is currently engaged on this side
    - `val_get_peer_tx_mode(session, &out_mode)`, `val_is_peer_streaming_engaged(session, &engaged)` — best-effort peer state
- Diagnostics (optional, compile‑time)
  - Metrics: `VAL_ENABLE_METRICS` — packet/byte counters, timeouts, retransmits, crc errors, etc.
  - Wire audit: `VAL_ENABLE_WIRE_AUDIT` — per‑packet send/recv counters and inflight window snapshot.
  - Emergency cancel: `val_emergency_cancel(session)` sends a best‑effort CANCEL and marks the session aborted.

Limitations
- Very large verification windows (>4 GiB) are not currently supported by the example filesystem adapter during VERIFY CRC computation.
  The core clamps verification windows for responsiveness: tail modes are capped by default (≈2 MiB); FULL verifies full‑prefix up to a cap,
  and falls back to a large‑tail verify beyond that. Application integrations can supply their own filesystem and CRC providers to remove these limits.

## Build options

These CMake options toggle compile-time features; defaults are conservative.

- VAL_ENABLE_ERROR_STRINGS=ON: build host-only string utilities (`val_error_strings`)
- VAL_ENABLE_METRICS=OFF: enable lightweight internal counters/timing
- VAL_ENABLE_ADVANCED_TX=OFF: advertise optional `VAL_FEAT_ADVANCED_TX` during handshake
  - When ON, the core defines `VAL_BUILTIN_FEATURES=VAL_FEAT_ADVANCED_TX` so peers can negotiate the capability.

## Error system and debug logging

Errors
- Numeric `val_status_t` codes (negative for errors) with a 32‑bit detail mask segmented by category (Network/CRC/Protocol/FS/Context) live in `include/val_errors.h`.
- The core library is MCU‑friendly and records only numeric code+detail.
- Optional host‑only utilities (`include/val_error_strings.h`, `src/val_error_strings.c`) provide string formatting and diagnostics; controlled via the `VAL_ENABLE_ERROR_STRINGS` CMake option and linked only into examples/tests.

Logging

Compile-time gated logging with a runtime filter.

- Compile-time level macro `VAL_LOG_LEVEL` (0..5; lower = higher priority)
  - 0: OFF, 1: CRITICAL, 2: WARNING, 3: INFO, 4: DEBUG, 5: TRACE
- Routing: set `cfg.debug.log` and `cfg.debug.context` to receive messages; otherwise they’re dropped.
- Runtime threshold: `cfg.debug.min_level`. Messages with `level <= min_level` are forwarded.
  - If left 0, the session sets a sensible default on create (based on build-time level).

Example (Windows PowerShell) to build with a different compile-time level:

```powershell
cmake -DVAL_LOG_LEVEL=2 -S . -B build\windows-debug -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-debug --config Debug -j
```

## Try the TCP example

Two executables live under `examples/tcp/` with optional flags for MTU and resume mode.

- Sender usage: `val_example_send.exe [--mtu N] [--resume MODE] [--tail-bytes N] [--log-level L] [--log-file PATH] <host> <port> <file1> [file2 ...]`
- Receiver usage: `val_example_receive.exe [--mtu N] [--resume MODE] [--tail-bytes N] [--log-level L] [--log-file PATH] <port> <outdir>`

Resume mode names (case-insensitive) map to `val_resume_mode_t`:

- `never`, `skip`/`skip_existing`, `tail`, `tail_or_zero`, `full`, `full_or_zero`

Build (Windows/Visual Studio):

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
```

Run (PowerShell):

```powershell
# Receiver (port 9000, output to D:\\out, MTU 8192, resume tail-or-zero with 16 KiB)
.\build\windows-release\bin\val_example_receive.exe --mtu 8192 --resume tail_or_zero --tail-bytes 16384 9000 D:\\out

# Sender (connect to localhost, same resume settings)
.\build\windows-release\bin\val_example_send.exe --mtu 8192 --resume tail_or_zero --tail-bytes 16384 127.0.0.1 9000 D:\\files\a.bin D:\\files\b.bin
```

On Linux/WSL, use the provided `linux-*` presets in `CMakePresets.json` and run the corresponding binaries.

Notes
- The repository root should stay clean. All build outputs and test artifacts go under `build/<preset>/...`.
- Executables are emitted to `build/<preset>/bin` and libraries to `build/<preset>/lib` (no extra `Debug/` level for Visual Studio generators).
- If you previously had stray `Testing/` or `out/` folders at the root, they were from older ad-hoc runs; they are ignored by `.gitignore` and no longer created by the current CMake setup.

## Status

- Windows and WSL builds via `CMakePresets.json`
- Examples: cross‑platform TCP transport using a minimal helper
- On‑wire framing: fixed header + variable payload + trailer CRC; header CRC validates early. All multi‑byte integers are little‑endian.
  - Header layout (base/pre‑1.0): type, wire_version(=0), payload_len, seq, offset, header CRC.
- Mid‑stream recovery: cumulative DATA_ACK semantics and DONE_ACK implemented.
- Errors on the wire are compact: `val_status_t code` + `detail mask` (both little‑endian in ERROR payload).

Transport contract (recv)
- Transports must be blocking.
- For each header/payload/trailer read, the core passes an exact length to `recv(ctx, buf, len, &received, timeout_ms)`.
- On success, return 0 and set `*received == len`.
- On timeout, return 0 and set `*received == 0` (the core will treat this as a timeout and retry per policy).
- On fatal I/O error, return <0 (the core will abort the operation).

Resilience and timeouts:
- All blocking waits use adaptive timeouts derived from round-trip time (RFC 6298 style) with Karn’s algorithm, clamped between `timeouts.min_timeout_ms` and `timeouts.max_timeout_ms`.
- A monotonic clock via `cfg.system.get_ticks_ms` is required in default builds (clock enforcement is ON). When explicitly disabled at build time, the library falls back to using `max_timeout_ms` as a fixed timeout and will not sample RTTs.
- Retries use exponential backoff (`retries.*` + `backoff_ms_base`).

Diagnostics:
- Logging is compile-time gated. Provide `cfg.debug.log` to capture logs (unit tests use a simple console logger).

### Choosing adaptive timeout bounds

As a starting point, consider these conservative values for `timeouts.{min,max}_timeout_ms`:

- Localhost/LAN or USB/UART (low jitter): min=50–100 ms, max=2,000–5,000 ms
- Typical Wi‑Fi/LAN across switches: min=100–200 ms, max=5,000–10,000 ms
- Internet/WAN or cellular: min=200–500 ms, max=10,000–30,000 ms
- High‑latency links (satellite, long‑haul): min=500–1,000 ms, max=30,000–60,000 ms

Pick a floor that’s above your median RTT to avoid spurious timeouts, and a ceiling that tolerates occasional pauses. If clock enforcement is disabled and no clock is provided, the library will use `max_timeout_ms` for all waits.

### Transport hooks (optional)

The transport interface in `val_config_t` supports two optional hooks:

- `int (*is_connected)(void* ctx)`: returns 1 if connected/usable, 0 if definitively disconnected, <0 if unknown. When absent, the core assumes connected.
- `void (*flush)(void* ctx)`: best‑effort flush after control packets (HELLO, DONE, EOT, ERROR). When absent, treated as a no‑op.

The TCP examples wire these to `tcp_is_connected()` and `tcp_flush()` from `examples/tcp/common/tcp_util.*`.
