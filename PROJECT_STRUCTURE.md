# VAL Protocol Project Structure

This document summarizes the layout of the VAL protocol repository and the purpose of key files.

## Top-level

- `include/` — Public API headers
	- `val_protocol.h` — Main public API and configuration (`val_config_t`, sessions, send/receive, utilities)
	- `val_errors.h` — Public status codes and 32‑bit error detail mask (shared across host/MCU)
	- `val_error_strings.h` — Optional host‑only error string/report helpers
- `src/` — Implementation sources
	- `val_internal.h` — Internal types (packets, payloads), session struct, helpers, logging macros
	- `val_core.c` — CRC32 (one-shot and streaming), session lifecycle, handshake, packet I/O, error helpers
	- `val_sender.c` — Sender path: metadata, resume negotiation, DATA/DONE/EOT loops with retries/backoff
	- `val_receiver.c` — Receiver path: resume decision, VERIFY exchange, receive/ACK loop, CRC validation
	- `val_error_strings.c` — Host‑only error formatting (compiled when enabled)
- `examples/` — Usage examples and demos
	- `tcp/` — Cross-platform TCP examples: `val_example_send.c`, `val_example_receive.c`, and `common/tcp_util.*`
- `unit_tests/` — Plain C unit tests registered with CTest (no external framework)
	- `support/` — In-memory duplex transport, filesystem/system shims, helpers
	- `send_receive/` — Send/receive suites
	- `recovery/` — Resume/corruption/retry scenarios
	- `CMakeLists.txt` — Adds test executables to CTest
- `docs/` — Additional documentation (if present)
- `CMakeLists.txt`, `CMakePresets.json` — Cross-platform builds for Windows/MSVC and Linux/WSL
- `README.md`, `DEVELOPMENT.md`, `PROTOCOL_FLOW.md` — Overview, API/developer guide, and packet flow reference

Header layout (base protocol)
- Packet header begins with `type` followed by `wire_version` (reserved; always 0), then `payload_len`, `seq`, `offset`, and `header_crc`.

## Notes

- Logging: Compile-time gated logging is available via `VAL_LOG_LEVEL` and an optional runtime filter/sink in `val_config_t.debug`.
- Features: Built-in feature mask is returned by `val_get_builtin_features()` and advertised during handshake.
- Buffers: The library uses caller-provided send/recv buffers sized to the configured MTU (`packet_size`).
- Timeouts/retries: All blocking waits are bounded and configurable; see `DEVELOPMENT.md` for details.

### Test harness and diagnostics
- The Windows test harness installs OS-level guards to suppress crash/assert popups and includes a watchdog to abort hung tests.
- An in-memory duplex transport simulates fragmentation, jitter, reordering, and timeouts deterministically.
- Optional compile-time diagnostics:
	- Metrics (`VAL_ENABLE_METRICS`): wire counters, timeouts, retransmits, CRC errors, file counts, RTT samples.
	- Wire audit (`VAL_ENABLE_WIRE_AUDIT`): per-packet send/recv counters and inflight window auditing.

## Getting oriented

Start with `include/val_protocol.h` for the API surface, then consult `src/val_core.c`, `src/val_sender.c`, and `src/val_receiver.c` for behavior. The example and unit tests show typical configuration and transport integration patterns.