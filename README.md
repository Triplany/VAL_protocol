# VAL Protocol

A small, robust, blocking-I/O file transfer protocol library in C. Frames are fixed header + variable payload (bounded by a negotiated MTU) + trailer CRC. It supports resume, cumulative ACKs, per-file CRC integrity, and avoids dynamic allocations in steady state.

- Development guide: [DEVELOPMENT.md](./DEVELOPMENT.md)
- Packet flow reference: [PROTOCOL_FLOW.md](./PROTOCOL_FLOW.md)
- Pre‑1.0 policy: backward compatibility isn’t guaranteed until v1.0. The current on‑wire behavior uses cumulative DATA_ACKs, DONE_ACK, and two CRCs (header + trailer). All multi‑byte fields are little‑endian.
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

## Debug logging

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
- Mid‑stream recovery: cumulative DATA_ACK semantics and DONE_ACK implemented.
- Errors on the wire are compact: `val_status_t code` + `detail mask`.

Resilience and timeouts:
- All blocking waits are bounded with configurable timeouts and retries (exponential backoff).
- Handshake uses `timeouts.handshake_ms` with `retries.handshake_retries`.
- Sender waits for DATA_ACK, DONE_ACK, EOT_ACK with `ack_ms` and `ack_retries`.
- VERIFY timeouts are handled robustly; receiver won’t hang.

Diagnostics:
- Logging is compile-time gated. Provide `cfg.debug.log` to capture logs (unit tests use a simple console logger).
