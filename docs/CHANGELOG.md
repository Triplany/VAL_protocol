# Changelog

All notable changes to VAL Protocol will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

**Note**: Pre-1.0 versions may introduce breaking changes without major version bump.

---

## [Unreleased]

### Planned
- Full protocol specification freeze for v1.0
- Performance benchmarking suite
- Additional platform examples (Raspberry Pi, Arduino)
- Python bindings

---

## [0.7.0] - 2025-10-04

### Added
- **Streaming Mode**: Continuous non-blocking transmission using ACKs as heartbeats (not flow control) - provides significant performance improvement, especially beneficial for memory-constrained devices (enables high throughput with small windows like WINDOW_2/4)
- **Adaptive Transmission System**: Dynamic window-based flow control with discrete rungs (1, 2, 4, 8, 16, 32, 64 packets) that automatically adjusts based on network conditions
- **Powerful Abstraction Layer**: Complete separation of protocol from transport/filesystem/system - enables custom encryption, compression, hardware CRC, in-memory transfers, any byte source
- **Six Resume Modes**: NEVER, SKIP_EXISTING, CRC_TAIL, CRC_TAIL_OR_ZERO, CRC_FULL, CRC_FULL_OR_ZERO
- **Metadata Validation Framework**: Application callbacks to accept/skip/abort files based on metadata
- **Connection Health Monitoring**: Graceful failure on excessive retry rates (>50%)
- **Emergency Cancellation**: Best-effort CANCEL packet (ASCII CAN 0x18) with session abort
- **Wire Audit**: Optional compile-time packet counters and inflight window tracking (VAL_ENABLE_WIRE_AUDIT)
- **Metrics Collection**: Optional compile-time statistics (VAL_ENABLE_METRICS)
- **Comprehensive Logging**: Five log levels (CRITICAL, WARNING, INFO, DEBUG, TRACE) with compile-time gating
- **RFC 6298 Adaptive Timeouts**: RTT-based timeout computation with Karn's algorithm
- **Transport Optional Hooks**: `is_connected()` and `flush()` for enhanced control
- **Complete TCP Examples**: Full-featured sender/receiver with all options
- **Extensive Unit Tests**: 80+ tests covering core, integration, recovery, and transport

### Changed
- **Breaking**: Simplified resume configuration (single `val_resume_config_t` struct)
- **Breaking**: Adaptive TX uses discrete window rungs instead of mixed modes
- **Breaking**: Streaming is pacing behavior, not a separate transmission mode
- **Breaking**: Transport `recv()` signature changed to support timeout indication
- **Improved**: Resume verification now supports tail and full-prefix modes with configurable windows
- **Improved**: Error detail masks reorganized with clear category segmentation
- **Improved**: Progress callbacks now include enhanced `val_progress_info_t` with ETA and rate

### Fixed
- Resume verification now correctly handles files larger than incoming size
- CRC verification window capping for responsive embedded systems (2 MB tail default)
- Mode synchronization handles directional streaming permissions correctly
- Header CRC computation excludes reserved fields properly
- Transport flush called after control packets for better reliability

### Security
- Path sanitization prevents directory traversal attacks
- Receiver never uses sender_path directly for output paths
- Metadata validation can reject files before any disk I/O

---

## [0.6.0] - 2025-09-15 _(Historical - Not Released)_

### Added
- Initial adaptive transmission scaffolding
- Basic resume support with CRC verification
- Cumulative DATA_ACK semantics
- DONE_ACK and EOT_ACK packets for reliable completion
- Error detail mask system
- Cross-platform builds (Windows, Linux, WSL)

### Changed
- Protocol version to 0.6
- On-wire format uses little-endian exclusively

---

## [0.5.0] - 2025-08-20 _(Historical - Not Released)_

### Added
- Core protocol implementation
- Handshake negotiation
- Basic file transfer (sender/receiver)
- CRC integrity checking (header + trailer)
- Configurable MTU support
- Standard C file I/O abstraction

### Known Issues
- No resume support
- Fixed timeouts (no RTT adaptation)
- Stop-and-wait transmission only
- Limited error recovery

---

## Version History Notes

### Pre-1.0 Development Status

**Current Version**: 0.7.0  
**Status**: Early Development - Ready for Testing  
**Production Ready**: NO

**Compatibility Promise**:
- **Pre-1.0**: Breaking changes may occur between minor versions
- **Post-1.0**: Semantic versioning applies; breaking changes only on major version bump

**What's Stable**:
- Core packet framing (header + payload + CRC)
- Little-endian wire format
- CRC-32 (IEEE 802.3) algorithm
- Basic handshake/metadata/data flow

**What May Change Before 1.0**:
- Resume mode semantics (may add new modes or refine existing)
- Adaptive TX thresholds and tuning parameters
- Feature negotiation flags (bit assignments)
- Error detail mask layout
- Metrics structure fields

**Testing Recommendations**:
- Use in development/test environments
- Expect protocol updates between pre-1.0 versions
- Provide feedback via GitHub issues
- Do not deploy in production systems yet

---

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md) for guidelines on:
- Reporting bugs
- Suggesting features
- Submitting pull requests
- Code style and testing requirements

---

## License

MIT License - Copyright 2025 Arthur T Lee

See [LICENSE](../LICENSE) for full text.

---

_Dedicated to Valerie Lee - for all her support over the years allowing me to chase my ideas._
