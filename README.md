# VAL Protocol

A small, robust, blocking-I/O file transfer protocol library in C. Frames are fixed header + variable payload (bounded by a negotiated MTU) + trailer CRC. It supports resume, cumulative ACKs, per-file CRC integrity, and avoids dynamic allocations in steady state.

- Development guide: [DEVELOPMENT.md](./DEVELOPMENT.md)
- Packet flow reference: [PROTOCOL_FLOW.md](./PROTOCOL_FLOW.md)
- Pre‑1.0 policy: backward compatibility isn’t guaranteed until v1.0. The current on‑wire behavior uses cumulative DATA_ACKs, DONE_ACK, and two CRCs (header + trailer). All multi‑byte fields are little‑endian.
  - The packet header includes a reserved `wire_version` byte (after `type`) that is always 0 in the base protocol; receivers validate it and reject non‑zero as incompatible for future evolution.
- Public headers: `include/`
- Sources: `src/`
- Examples: `examples/tcp/` (TCP send/receive)
- Unit tests: `unit_tests/` (CTest executables)

## Features (from source)

- Built-in feature bits: see `include/val_protocol.h` and `val_get_builtin_features()`
  - `VAL_FEAT_CRC_RESUME` — CRC-seeded resume verification
  - `VAL_FEAT_MULTI_FILES` — Multi-file send/receive per session
- Resume modes and policies
  - Legacy modes: `VAL_RESUME_NONE`, `VAL_RESUME_APPEND`, `VAL_RESUME_CRC_VERIFY` (+ `verify_bytes`)
  - Receiver-driven policies (preferred): `val_resume_policy_t`
    - NONE(0) keeps legacy behavior; SAFE_DEFAULT(1); ALWAYS_START_ZERO(2); ALWAYS_SKIP_IF_EXISTS(3);
      SKIP_IF_DIFFERENT(4); ALWAYS_SKIP(5); STRICT_RESUME_ONLY(6)
  - On mismatch/anomaly defaults come from the session (see `val_session_create()` in `src/val_core.c`)

## Error system and debug logging

Errors
- Numeric `val_status_t` codes (negative for errors) with a 32‑bit detail mask segmented by category (Network/CRC/Protocol/FS/Context) live in `include/val_errors.h`.
- The core library is MCU‑friendly and records only numeric code+detail.
- Optional host‑only utilities (`include/val_error_strings.h`, `src/val_error_strings.c`) provide string formatting and diagnostics; controlled via the `VAL_BUILD_HOST_UTILS` CMake option and linked only into examples/tests.

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

Two executables live under `examples/tcp/` with optional flags for MTU and resume policy.

- Sender usage: `val_example_send.exe [--mtu N] [--policy NAME|ID] <host> <port> <file1> [file2 ...]`
- Receiver usage: `val_example_receive.exe [--mtu N] [--policy NAME|ID] <port> <outdir>`

Policy names (case-insensitive) map to `val_resume_policy_t`:

- `none(0)`, `safe(1)`, `start_zero(2)`, `skip_if_exists(3)`, `skip_if_different(4)`, `always_skip(5)`, `strict_only(6)`

Build (Windows/Visual Studio):

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
```

Run (PowerShell):

```powershell
# Receiver (port 9000, output to D:\out)
.\build\windows-release\Release\val_example_receive.exe --policy safe --mtu 8192 9000 D:\out

# Sender (connect to localhost, send files)
.\build\windows-release\Release\val_example_send.exe --policy strict_only 127.0.0.1 9000 D:\files\a.bin D:\files\b.bin
```

On Linux/WSL, use the provided `linux-*` presets in `CMakePresets.json` and run the corresponding binaries.

## Status

- Windows and WSL builds via `CMakePresets.json`
- Examples: cross‑platform TCP transport using a minimal helper
- On‑wire framing: fixed header + variable payload + trailer CRC; header CRC validates early. All multi‑byte integers are little‑endian.
  - Header layout (base/pre‑1.0): type, wire_version(=0), payload_len, seq, offset, header CRC.
- Mid‑stream recovery: cumulative DATA_ACK semantics and DONE_ACK implemented.
- Errors on the wire are compact: `val_status_t code` + `detail mask` (both little‑endian in ERROR payload).

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
