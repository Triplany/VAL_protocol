📚 **[Complete Documentation](docs/README.md)** | [Getting Started](docs/getting-started.md) | [API Reference](docs/api-reference.md) | [Protocol Spec](docs/protocol-specification.md)

- **Quick Start**: [Getting Started Guide](docs/getting-started.md)
- **API Documentation**: [Complete API Reference](docs/api-reference.md)
- **Protocol Details**: [Protocol Specification](docs/protocol-specification.md)
- **Implementation**: [Implementation Guide](docs/implementation-guide.md)
- **Wire Formats**: [Message Format Reference](docs/message-formats.md)
- **Help**: [Troubleshooting Guide](docs/troubleshooting.md)
- **History**: [Changelog](docs/CHANGELOG.md)

### Repository Structure

- **Public headers**: `include/` - Public API (`val_protocol.h`, `val_errors.h`)
- **Sources**: `src/` - Core implementation
- **Examples**: `examples/tcp/` - Complete TCP send/receive examples
- **Unit tests**: `unit_tests/` - Comprehensive test suite
- **Documentation**: `docs/` - Complete protocol and API documentation

### Protocol Notes

- **Pre-1.0 Policy**: Backward compatibility isn't guaranteed until v1.0
- **Wire Format**: Fixed header + variable payload + trailer CRC; all multi-byte fields are little-endian
- **Packet Header**: Includes reserved `wire_version` byte (always 0); receivers validate and reject non-zero for future compatibility
- **Flow Control**: Bounded-window with cumulative DATA_ACKs, DONE_ACK, and two CRCs (header + trailer)

**⚠️ EARLY DEVELOPMENT NOTICE**
VAL Protocol v0.7 is ready for testing and evaluation but not production-ready. Backward compatibility is not guaranteed until v1.0.

---

**MIT License** - Copyright 2025 Arthur T Lee

_Dedicated to Valerie Lee - for all her support over the years allowing me to chase my ideas._

---

## Overview

VAL (Versatile Adaptive Link) Protocol v0.7 is a robust, blocking-I/O file transfer protocol library written in C, designed for reliable file transfers across diverse network conditions and embedded systems. The protocol uses fixed header + variable payload + trailer CRC format with bounded congestion window, AIMD flow control, and zero dynamic allocations in steady state.

### Key Features

- **Bounded Window Flow Control**: Congestion window negotiated at handshake (packet-count based) with AIMD adaptation based on network conditions
- **Powerful Abstraction Layer**: Complete separation of protocol from transport, filesystem, and system - enables custom encryption, compression, in-memory transfers, hardware acceleration
- **Resume Support**: Simplified tail-verification resume with configurable window cap; supports never/skip-existing/tail modes  
- **Embedded-Friendly**: Zero dynamic allocations in steady state, configurable memory footprint, works on bare-metal MCUs
- **Transport Agnostic**: Works over TCP, UART, RS-485, CAN, USB CDC, or any reliable byte stream
- **Robust Error Handling**: Comprehensive error codes with detailed 32-bit diagnostic masks
- **Optional Diagnostics**: Compile-time metrics collection and lightweight packet capture hook
- Pre‑1.0 policy: backward compatibility isn’t guaranteed until v1.0. The current on‑wire behavior uses cumulative DATA_ACKs, DONE_ACK, and two CRCs (header + trailer). All multi‑byte fields are little‑endian.
  - The packet header includes a reserved `wire_version` byte (after `type`) that is always 0 in the base protocol; receivers validate it and reject non‑zero as incompatible for future evolution.
- Public headers: `include/`
- Sources: `src/`
- Examples: `examples/tcp/` (TCP send/receive)
- Unit tests: `unit_tests/` (CTest executables; artifacts like ut_artifacts live under the build tree)

## Protocol Overview

**Wire Format:**
- Fixed 24-byte header: type(1) + wire_version(1) + reserved(2) + payload_len(4) + seq(4) + offset(8) + header_crc(4)
- Variable payload (0 to MTU-24 bytes)
- Trailer CRC-32 over header+payload
- All multi-byte fields are little-endian

**Core Features:**
- Protocol version 0.7 with bounded-window transmission
- Packet-based flow control with negotiated window caps (1-65535 packets)
- AIMD congestion control with configurable thresholds
- Adaptive timeouts using RFC 6298-like RTT estimation
- CRC-32 integrity verification on all packets
- Emergency cancel capability (ASCII CAN 0x18)

**Resume Modes:**
- `VAL_RESUME_NEVER`: Always overwrite from zero
- `VAL_RESUME_SKIP_EXISTING`: Skip any existing file (no verification)
- `VAL_RESUME_TAIL`: Verify trailing window of local file; resume on match

**Flow Control Configuration:**
- `window_cap_packets`: Max in-flight packets (negotiated with peer)
- `initial_cwnd_packets`: Initial congestion window size
- `degrade_error_threshold`: Errors before halving cwnd (AIMD)
- `recovery_success_threshold`: Successes before cwnd += 1

**API Functions:**
- `val_send_files()` / `val_receive_files()`: Main transfer functions
- `val_get_cwnd_packets()`: Current congestion window size
- `val_get_peer_tx_cap_packets()`: Peer's advertised TX capability
- `val_get_effective_packet_size()`: Negotiated MTU
- `val_emergency_cancel()`: Best-effort session abort

**Optional Diagnostics:**
- Metrics: packet/byte counters, timeouts (soft/hard), retransmits, CRC errors
- Packet capture hook: observe TX/RX packets with minimal overhead
- Debug logging with runtime filtering

## Build options

These CMake options toggle compile-time features; defaults are conservative.

- **VAL_ENABLE_ERROR_STRINGS=ON**: Build host-only string utilities (`val_error_strings`) for human-readable error messages
- **VAL_ENABLE_METRICS=OFF**: Enable lightweight internal counters/timing (packets, bytes, RTT, errors, retransmits)
- Packet capture is a runtime callback (no build flag). Wire audit has been removed.
- **VAL_LOG_LEVEL** (0-5): Compile-time log level (0=OFF, 1=CRITICAL, 2=WARNING, 3=INFO, 4=DEBUG, 5=TRACE)

**Example:**
```bash
cmake -B build -DVAL_ENABLE_METRICS=ON -DVAL_LOG_LEVEL=4
```

See [Implementation Guide](docs/implementation-guide.md#building-from-source) and [Advanced Features](docs/examples/advanced-features.md) for detailed usage.

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

 - Sender usage: `val_example_send_tcp.exe [--mtu N] [--resume MODE] [--tail-bytes N] [--log-level L] [--log-file PATH] <host> <port> <file1> [file2 ...]`
 - Receiver usage: `val_example_receive_tcp.exe [--mtu N] [--resume MODE] [--tail-bytes N] [--log-level L] [--log-file PATH] <port> <outdir>`

Resume mode names (case-insensitive) map to `val_resume_mode_t`:

- `never`, `skip`/`skip_existing`, `tail`

Build (Windows/Visual Studio):

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
```

Run (PowerShell):

```powershell
# Receiver (port 9000, output to D:\out, MTU 8192, resume tail with 16 KiB cap)
.\build\windows-release\bin\val_example_receive_tcp.exe --mtu 8192 --resume tail --tail-bytes 16384 9000 D:\\out

# Sender (connect to localhost, same resume settings)
.\build\windows-release\bin\val_example_send_tcp.exe --mtu 8192 --resume tail --tail-bytes 16384 127.0.0.1 9000 D:\\files\a.bin D:\\files\b.bin
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
