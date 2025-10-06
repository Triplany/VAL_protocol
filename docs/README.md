# VAL Protocol Documentation

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors or inaccuracies. Please verify critical information against the source code and report any issues.

---

## VAL Protocol - Versatile Adaptive Link Protocol

**Version:** 0.7 (Early Development)  
**Status:** Ready for Testing - Not Production Ready  
**License:** MIT License  
**Copyright:** 2025 Arthur T Lee

### Dedication

_For Valerie Lee - whose unwavering support over the years has allowed me to chase my ideas and bring them to life._

---

## Overview

VAL Protocol is a robust, blocking-I/O file transfer protocol library written in C, designed for reliable file transfers across diverse network conditions and embedded systems. The protocol features **continuous streaming mode** (15-20x faster than pure windowing), adaptive transmission that automatically adjusts to network quality, comprehensive resume capabilities with CRC verification, and a powerful abstraction layer that separates protocol from transport/filesystem/system - enabling custom encryption, compression, hardware acceleration, and use with any byte source.

## Documentation Structure

This documentation is organized into the following sections:

### Core Documentation

1. **[Getting Started Guide](getting-started.md)**  
   Quick setup, basic usage, and first implementation tutorial

2. **[Protocol Specification](protocol-specification.md)**  
   Complete technical specification following RFC-style formatting

3. **[API Reference](api-reference.md)**  
   Comprehensive API documentation with function signatures and examples

4. **[Implementation Guide](implementation-guide.md)**  
   Technical implementation details, architecture, and integration patterns

5. **[Message Format Reference](message-formats.md)**  
   Detailed packet structures, field definitions, and encoding specifications

### Additional Resources

6. **[Examples and Tutorials](examples/)**  
   - [Basic Usage](examples/basic-usage.md)
   - [Advanced Features](examples/advanced-features.md)
   - [Integration Examples](examples/integration-examples.md)

7. **[Troubleshooting Guide](troubleshooting.md)**  
   Common issues, error codes, debugging procedures, and FAQ

8. **[Contributing Guidelines](../CONTRIBUTING.md)**  
   Code style, testing requirements, and development setup

9. **[Changelog](CHANGELOG.md)**  
   Version history, release notes, and breaking changes

## Quick Links

- [Source Code Repository](https://github.com/Triplany/VAL_protocol)
- [Issue Tracker](https://github.com/Triplany/VAL_protocol/issues)
- Main README: [Project Root](../README.md)

## Key Features

- **Streaming Mode**: Continuous transmission using ACKs as keepalive heartbeats rather than flow control - enables high throughput with small windows, particularly beneficial for RAM-constrained devices (WINDOW_2 + streaming can approach WINDOW_64 performance with far less memory)
- **Adaptive Transmission**: Dynamic window sizing (1-64 packets) that automatically escalates/de-escalates based on network quality and RTT measurements
- **Powerful Abstraction Layer**: Complete separation of concerns - implement custom encryption, compression, in-memory transfers, hardware CRC acceleration, or use any byte source/sink (files, RAM, flash, network buffers)
- **Resume Support**: Simplified, CRC-verified resume with tail-only verification (configurable cap) and skip-existing policy
- **Embedded-Friendly**: Zero dynamic allocations in steady state, configurable memory footprint, blocking I/O design for bare-metal and RTOS integration
- **Transport Agnostic**: Works seamlessly over TCP, UART, RS-485, CAN, USB CDC, SPI, or any reliable byte stream - no OS networking stack required
- **Robust Error Handling**: Comprehensive error reporting with detailed 32-bit diagnostic masks segmented by category
- **Metrics & Diagnostics**: Optional compile-time metrics collection and a lightweight packet capture hook

## Current Status

VAL Protocol is in **early development** (version 0.7) and is ready for testing and evaluation. The protocol is **not yet production-ready** and backward compatibility is not guaranteed until v1.0.

### What Works

- ✅ Windows and Linux builds via CMake
- ✅ TCP transport examples with full feature support
- ✅ Comprehensive unit test suite
- ✅ Adaptive transmission with window-based flow control
- ✅ Multiple resume modes with CRC verification
- ✅ Metadata validation framework
- ✅ Emergency cancellation mechanism

### Known Limitations

- Pre-1.0: Backward compatibility not guaranteed between versions
- Very large verification windows (>4 GiB) not supported in example filesystem adapter
- Requires blocking I/O transport

## Getting Help

- **Documentation Issues**: Check the [Troubleshooting Guide](troubleshooting.md)
- **Bug Reports**: [GitHub Issues](https://github.com/Triplany/VAL_protocol/issues)
- **Questions**: See [FAQ](troubleshooting.md#frequently-asked-questions)

## License

```
MIT License
Copyright 2025 Arthur T Lee

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
