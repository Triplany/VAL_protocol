# VAL Protocol

A small, robust, blocking-I/O file transfer protocol library in C. Fixed header + variable-length payload frames (bounded by a negotiated MTU), CRC integrity, resume support, and no dynamic allocations in steady-state.

- Development guide: see [DEVELOPMENT.md](./DEVELOPMENT.md) for protocol details, flows, API, and build instructions.
- Packet flow reference: see [PROTOCOL_FLOW.md](./PROTOCOL_FLOW.md) for when each packet is used and the end-to-end sequencing.
 - Pre‑1.0 policy: backward compatibility is not guaranteed until v1.0. Current implementation uses cumulative DATA_ACKs, DONE_ACK, and two CRCs (header CRC + trailer CRC). All multi-byte fields are little‑endian on the wire.
- Public headers: `include/`
- Sources: `src/`
- Example: `examples/example_loopback.c`
- Minimal tests: `tests/`
- Unit tests:  `unit_tests/` (organized by area: core, send/receive, recovery, support)

## Debug logging

Optional, compile-time gated tracing can help while developing or integrating:

- Compile-time level (0..5) is controlled by the macro `VAL_LOG_LEVEL` (lower = higher priority):
	- 0: OFF (no code emitted)
	- 1: CRITICAL
	- 2: WARNING
	- 3: INFO
	- 4: DEBUG
	- 5: TRACE
- Defaults:
	- Debug builds (no `NDEBUG`): level 4 (DEBUG)
	- Release builds (`NDEBUG` defined): level 0 (OFF)
- Routing: if you set `cfg.debug.log` in `val_config_t`, logs are delivered to that sink; otherwise they are dropped even when enabled.
	- Signature: `void (*log)(void* ctx, int level, const char* file, int line, const char* message)` with `cfg.debug.context` passed as `ctx`.
- Runtime threshold: `cfg.debug.min_level` controls which messages are forwarded at runtime. Messages with `level <= min_level` pass through.
	- If you leave it as 0, the session will default it to the compile-time `VAL_LOG_LEVEL` on `val_session_create()`.
	- Set to `VAL_LOG_WARNING` to keep WARN+CRITICAL only, etc.
- Overriding the compile-time level (examples):

```powershell
# Windows PowerShell (build folder already configured)
cmake -DVAL_LOG_LEVEL=2 -S . -B build\windows-debug -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-debug --config Debug -j
```

```bash
# Linux/WSL
cmake -DVAL_LOG_LEVEL=2 --preset linux-debug
cmake --build --preset linux-debug
```

For tests, you can enable a simple stdout logger via `ts_set_console_logger(&cfg);` before `val_session_create()`.

## Try it

Build and run the example. On Windows (PowerShell):

```powershell
cmake -S . -B build\windows-release -G "Visual Studio 17 2022" -A x64
cmake --build build\windows-release --config Release -j
.
\build\windows-release\Release\val_example.exe
```

On Linux/WSL:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/val_example
```

The example transfers a demo file over a duplex in-memory transport and writes artifacts under the executable directory, e.g. `build/.../Release/artifacts/out/`.

To enable example logging (in Debug builds, where logging is enabled by default), add a simple sink before `val_session_create`:

```c
static void example_log(void* ctx, int level, const char* file, int line, const char* msg) {
	(void)ctx; const char* L = level==1?"ERROR": level==2?"INFO":"DEBUG";
	fprintf(stdout, "[%s] %s:%d: %s\n", L, file, line, msg?msg:"");
}
// ...
cfg_a.debug.log = example_log; cfg_a.debug.context = NULL;
cfg_b.debug.log = example_log; cfg_b.debug.context = NULL;
```

## Status

- Windows and WSL builds are configured via `CMakePresets.json` and `.vscode` settings.
- Example transfers a file end-to-end and prints progress.
- On-wire framing: fixed header + variable payload + trailer CRC; header CRC validates early. All multi-byte integers on wire are little‑endian.
- Mid-stream recovery: cumulative DATA_ACK semantics and DONE_ACK implemented.
- Errors on the wire are compact: code + detail (no strings); see `DEVELOPMENT.md` for details and roadmap/TODO.

Resilience and timeouts (pre‑1.0):
- All blocking waits are bounded by configurable timeouts and retries with exponential backoff.
- Handshake uses `timeouts.handshake_ms` with `retries.handshake_retries` (re-sends HELLO on timeout).
- Sender’s waits for DATA_ACK, DONE_ACK, and EOT_ACK are bounded using `timeouts.ack_ms` and `retries.ack_retries` with backoff.
- Resume VERIFY exchange uses the configured ACK timeout; receiver falls back to offset 0 on VERIFY timeout instead of hanging.
- Receiver ignores duplicate/overlap DATA and ACKs the current write position (idempotent).

Diagnostics:
- Logging is compile-time gated and has near-zero overhead when disabled. Provide `cfg.debug.log` to capture logs in your application or use `ts_set_console_logger` in tests.
