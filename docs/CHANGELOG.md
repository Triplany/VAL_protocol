## 0.7.0

Breaking changes

- Whole-file CRC32 is not part of SEND_META or the public API.
	- Wire: VAL_WIRE_META_SIZE reduced by 4 bytes (filename + path + size only).
	- API: val_meta_payload_t no longer contains file_crc32.
	- Integrity is provided by per-packet CRCs and optional tail verification during resume.
- Simplified resume modes to a tail-only scheme:
Other removals


	- Modes: VAL_RESUME_NEVER, VAL_RESUME_SKIP_EXISTING, VAL_RESUME_TAIL.
	- New resume config fields: tail_cap_bytes, min_verify_bytes, mismatch_skip.
	- FULL/TAIL variants are consolidated under a simplified policy; use mismatch_skip=1 for skip-on-mismatch behavior.

Migration notes

- If your code referenced meta.file_crc32, remove it. No replacement is needed; keep using per-packet CRCs and (optionally) tail verify.
- If you previously used VAL_RESUME_CRC_TAIL[_OR_ZERO] or VAL_RESUME_CRC_FULL[_OR_ZERO]:
	- Use VAL_RESUME_TAIL and set mismatch_skip=0 (restart) or 1 (skip) to choose the mismatch policy.
	- Use tail_cap_bytes to cap the verification window; the core clamps to a safe maximum.

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
Removed: Streaming pacing overlay. Protocol now uses a single bounded-window model with cwnd-based adaptation.
- **Adaptive Transmission System**: Dynamic window-based flow control with discrete rungs (1, 2, 4, 8, 16, 32, 64 packets) that automatically adjusts based on network conditions
- **Powerful Abstraction Layer**: Complete separation of protocol from transport/filesystem/system - enables custom encryption, compression, hardware CRC, in-memory transfers, any byte source
- **Simplified Resume Modes**: NEVER, SKIP_EXISTING, TAIL (tail verification with configurable cap and unified mismatch policy)
- **Metadata Validation Framework**: Application callbacks to accept/skip/abort files based on metadata
- **Connection Health Monitoring**: Graceful failure on excessive retry rates (>50%)
- **Emergency Cancellation**: Best-effort CANCEL packet (ASCII CAN 0x18) with session abort
- **Metrics Collection**: Optional compile-time statistics (VAL_ENABLE_METRICS)
- **Comprehensive Logging**: Five log levels (CRITICAL, WARNING, INFO, DEBUG, TRACE) with compile-time gating
- **RFC 6298 Adaptive Timeouts**: RTT-based timeout computation with Karn's algorithm
- **Transport Optional Hooks**: `is_connected()` and `flush()` for enhanced control
- **Complete TCP Examples**: Full-featured sender/receiver with all options
- **Extensive Unit Tests**: 80+ tests covering core, integration, recovery, and transport

### Changed
- **Breaking**: Simplified resume configuration (single `val_resume_config_t` struct)
- **Breaking**: Adaptive TX uses discrete window rungs instead of mixed modes
- **Breaking**: Removed streaming mode and mode ladder; replaced with bounded-window cwnd engine
- **Breaking**: Transport `recv()` signature changed to support timeout indication
- **Improved**: Resume verification now supports tail and full-prefix modes with configurable windows
- **Improved**: Error detail masks reorganized with clear category segmentation
- **Improved**: Progress callbacks now include enhanced `val_progress_info_t` with ETA and rate

### Fixed
- Resume verification now correctly handles files larger than incoming size
- CRC verification window capping for responsive embedded systems (2 MB tail default)
Removed: Mode synchronization and streaming permissions
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
