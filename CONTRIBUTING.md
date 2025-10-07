# Contributing to VAL Protocol

**⚠️ AI-ASSISTED DOCUMENTATION NOTICE**  
This documentation was created with AI assistance and may contain errors.

---

Thank you for your interest in contributing to VAL Protocol!

_Dedicated to Valerie Lee - for all her support over the years allowing me to chase my ideas._

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Development Workflow](#development-workflow)
4. [Code Style](#code-style)
5. [Testing Requirements](#testing-requirements)
6. [Pull Request Process](#pull-request-process)
7. [Issue Reporting](#issue-reporting)

---

## Code of Conduct

### Our Pledge

We are committed to providing a welcoming and inclusive environment for all contributors.

### Expected Behavior

- Be respectful and constructive
- Welcome newcomers and help them get started
- Focus on what's best for the project
- Show empathy towards other community members

### Unacceptable Behavior

- Harassment, discrimination, or derogatory comments
- Personal attacks or trolling
- Publishing others' private information
- Any conduct that would be inappropriate in a professional setting

---

## Getting Started

### Prerequisites

- **C Compiler**: GCC 7+, Clang 10+, or MSVC 2017+
- **CMake**: 3.15 or higher
- **Git**: For version control
- **Platform**: Windows, Linux, or macOS

### Fork and Clone

```bash
# Fork the repository on GitHub, then:
git clone https://github.com/YOUR_USERNAME/VAL_protocol.git
cd VAL_protocol

# Add upstream remote
git remote add upstream https://github.com/Triplany/VAL_protocol.git
```

### Build and Test

```bash
# Windows (PowerShell)
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug

# Linux
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

---

## Development Workflow

### 1. Create a Branch

```bash
# Sync with upstream
git fetch upstream
git checkout main
git merge upstream/main

# Create feature branch
git checkout -b feature/my-new-feature
```

**Branch Naming**:
- `feature/description` - New features
- `fix/description` - Bug fixes
- `docs/description` - Documentation only
- `refactor/description` - Code refactoring
- `test/description` - Test additions/fixes

### 2. Make Changes

- Keep commits focused and atomic
- Write clear commit messages (see below)
- Test your changes locally

### 3. Commit

**Commit Message Format**:
```
<type>: <subject>

<body>

<footer>
```

**Types**:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style (formatting, no logic change)
- `refactor`: Code refactoring
- `test`: Test changes
- `chore`: Build process, tooling, etc.

**Example**:
```
feat: add hardware CRC acceleration support

Implement optional hardware CRC provider interface to allow
platforms with CRC accelerators (e.g., STM32) to plug in their
own implementations.

- Add cfg.crc callbacks structure
- Fallback to software CRC if NULL
- Update documentation

Closes #42
```

### 4. Push and Create Pull Request

```bash
git push origin feature/my-new-feature
```

Then create a Pull Request on GitHub.

---

## Code Style

### General Principles

- **Clarity over cleverness**: Write code that's easy to understand
- **Consistency**: Follow existing patterns in the codebase
- **Embedded-friendly**: Avoid dynamic allocations, keep footprint small
- **Documentation**: Comment complex algorithms and non-obvious logic

### C Style Guidelines

**Indentation**: 4 spaces (no tabs)

**Braces**: K&R style with braces on same line for functions
```c
void my_function(int arg) {
    if (condition) {
        // code
    } else {
        // code
    }
}
```

**Naming Conventions**:
- Functions: `val_snake_case()` (prefix with `val_`)
- Internal functions: `val_internal_snake_case()`
- Types: `val_type_name_t` (suffix with `_t`)
- Enums: `VAL_ENUM_VALUE` (uppercase, prefix with `VAL_`)
- Macros: `VAL_MACRO_NAME` (uppercase, prefix with `VAL_`)
- Static functions: `snake_case()` (no prefix)

**Example**:
```c
// Public API
val_status_t val_send_files(val_session_t *session, ...);

// Internal helper
void val_internal_record_rtt(val_session_t *s, uint32_t rtt_ms);

// Static helper
static int parse_header(const uint8_t *data, val_packet_header_t *hdr);

// Type
typedef struct {
    uint32_t field;
} val_my_struct_t;

// Enum
typedef enum {
    VAL_MODE_NORMAL = 0,
    VAL_MODE_FAST = 1
} val_mode_t;

// Macro
// Example constants in tests/docs should reflect current limits; as of v0.7, max packet size is 2 MiB.
#define VAL_MAX_SIZE (2u * 1024u * 1024u)
```

**Comments**:
```c
// Single-line comment for brief explanations

/* Multi-line comment
 * for longer explanations,
 * API documentation, etc.
 */
```

**Include Order**:
1. Corresponding header (if .c file)
2. Standard C headers (`<stdint.h>`, `<string.h>`, etc.)
3. System headers (`<windows.h>`, `<pthread.h>`, etc.)
4. Project headers (`"val_internal.h"`, `"val_protocol.h"`, etc.)

**Example**:
```c
#include "val_internal.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
```

**Error Handling**:
```c
// Check and return immediately on error
val_status_t my_function(val_session_t *s) {
    if (!s) {
        return VAL_ERR_INVALID_ARG;
    }
    
    val_status_t status = val_internal_do_something(s);
    if (status != VAL_OK) {
        return status;
    }
    
    return VAL_OK;
}
```

---

## Testing Requirements

### Unit Tests

All new features and bug fixes must include unit tests.

**Test Location**: `unit_tests/`

**Test Structure**:
```c
#include "test_support.h"
#include "val_protocol.h"

void test_my_feature(void) {
    // Arrange
    val_config_t cfg = {0};
    setup_test_config(&cfg);
    
    // Act
    val_status_t status = val_my_function(&cfg);
    
    // Assert
    TEST_ASSERT(status == VAL_OK, "Function should succeed");
    
    // Cleanup
    cleanup_test_config(&cfg);
}

int main(void) {
    RUN_TEST(test_my_feature);
    return TEST_SUMMARY();
}
```

**Running Tests**:
```bash
# All tests
ctest --preset windows-debug

# Specific test
ctest --preset windows-debug -R ut_my_feature -VV

# With logging
set VAL_LOG_LEVEL=5
ctest --preset windows-debug -R ut_my_feature -VV
```

### Test Coverage

- **Core Logic**: All branches must be tested
- **Error Paths**: Test error conditions and edge cases
- **Platform-Specific**: Test on Windows, Linux, and embedded if possible

### Integration Tests

For features involving file transfer, add integration tests:
- `unit_tests/integration/` - End-to-end transfer scenarios
- `unit_tests/recovery/` - Error recovery and resume scenarios

---

## Pull Request Process

### Before Submitting

1. **Sync with upstream**:
   ```bash
   git fetch upstream
   git rebase upstream/main
   ```

2. **Run all tests**:
   ```bash
   cmake --build build/windows-debug
   ctest --test-dir build/windows-debug --output-on-failure
   ```

3. **Check code style**: Ensure your code follows style guidelines

4. **Update documentation**: If adding features, update relevant docs

5. **Update CHANGELOG**: Add entry to `docs/CHANGELOG.md` under `[Unreleased]`

### PR Description Template

```markdown
## Description

Brief description of the change and motivation.

## Type of Change

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Documentation update

## Testing

- [ ] All existing tests pass
- [ ] New tests added for this change
- [ ] Tested on: [Windows / Linux / Both]

## Checklist

- [ ] Code follows project style guidelines
- [ ] Self-reviewed the code
- [ ] Commented complex/non-obvious code
- [ ] Updated documentation (if applicable)
- [ ] Added tests that prove the fix/feature works
- [ ] Updated CHANGELOG.md

## Related Issues

Closes #(issue number)
```

### Review Process

1. **Automated Checks**: CI will run tests and linters
2. **Maintainer Review**: A maintainer will review your code
3. **Feedback**: Address any requested changes
4. **Approval**: Once approved, your PR will be merged

### What We Look For

- **Correctness**: Does the code do what it claims?
- **Tests**: Are there adequate tests?
- **Style**: Does it follow project conventions?
- **Documentation**: Is it properly documented?
- **Performance**: Are there any performance regressions?
- **Compatibility**: Does it work on all platforms?

---

## Issue Reporting

### Before Opening an Issue

1. **Search existing issues**: Your issue may already be reported
2. **Check documentation**: Is this covered in the docs?
3. **Try latest version**: Is the issue fixed in the latest code?

### Bug Report Template

```markdown
**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Configure with '...'
2. Call function '...'
3. See error

**Expected behavior**
What you expected to happen.

**Actual behavior**
What actually happened.

**Environment:**
- OS: [Windows 10 / Ubuntu 22.04 / etc.]
- Compiler: [MSVC 2022 / GCC 11.3 / etc.]
- VAL Version: [0.7.0]
- Build flags: [e.g., VAL_ENABLE_METRICS=ON]

**Logs:**
```
Paste relevant logs here (with VAL_LOG_LEVEL=5 if possible)
```

**Additional context**
Any other context about the problem.
```

### Feature Request Template

```markdown
**Is your feature request related to a problem?**
A clear and concise description of the problem.

**Describe the solution you'd like**
What you want to happen.

**Describe alternatives you've considered**
Other approaches you've thought about.

**Additional context**
Any other context or examples.

**Would you be willing to implement this?**
Yes / No / Maybe with guidance
```

---

## Development Setup

### Recommended Tools

- **IDE/Editor**: VS Code, CLion, Visual Studio, Vim, Emacs
- **Debugger**: GDB, LLDB, MSVC Debugger
- **Linter**: clang-format (optional)
- **Git GUI**: GitKraken, SourceTree, or built-in IDE tools

### VS Code Setup

Recommended extensions:
- C/C++ (Microsoft)
- CMake Tools
- GitLens
- Markdown All in One

**`.vscode/settings.json`** example:
```json
{
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
  "cmake.buildDirectory": "${workspaceFolder}/build/${buildKitVendor}-${buildKitVersion}-${buildType}",
  "files.associations": {
    "val_protocol.h": "c",
    "val_internal.h": "c"
  }
}
```

### Build with Sanitizers (Linux)

```bash
cmake -S . -B build/linux-asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -fsanitize=undefined"

cmake --build build/linux-asan
ctest --test-dir build/linux-asan
```

### Build with Coverage (Linux)

```bash
cmake -S . -B build/linux-coverage \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="--coverage"

cmake --build build/linux-coverage
ctest --test-dir build/linux-coverage

# Generate coverage report
lcov --capture --directory build/linux-coverage --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

---

## Questions?

- **Documentation**: Check `docs/` directory
- **Examples**: See `examples/tcp/` for reference implementations
- **Issues**: Open a GitHub issue with the "question" label
- **Email**: [Contact maintainer] (TBD)

---

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

---

**Thank you for contributing to VAL Protocol!**

_Your contributions help make reliable file transfer accessible to everyone, from embedded systems to high-speed networks._
