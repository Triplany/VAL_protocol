#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include "val_protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Simple byte FIFO with blocking semantics for unit tests
    typedef struct test_fifo_s test_fifo_t;

    test_fifo_t *test_fifo_create(size_t capacity);
    void test_fifo_destroy(test_fifo_t *f);
    void test_fifo_push(test_fifo_t *f, const uint8_t *data, size_t len);
    // Pop exactly 'len' bytes into 'out'. Returns 1 on success, 0 if timed out without enough data.
    int test_fifo_pop_exact(test_fifo_t *f, uint8_t *out, size_t len, uint32_t timeout_ms);

    // Fault injection knobs for the duplex transport
    typedef struct
    {
        // Random bit flip probability per byte in DATA payload (0..1 scaled by 1e6). 0 = no corruption.
        uint32_t bitflip_per_million;
        // Drop entire frame probability (applies to any write) per frame in 1e6.
        uint32_t drop_frame_per_million;
        // Duplicate last frame probability per frame in 1e6.
        uint32_t dup_frame_per_million;
    } fault_injection_t;

    // Duplex in-memory transport context for tests
    typedef struct
    {
        test_fifo_t *a2b; // A sends into a2b, B receives from a2b
        test_fifo_t *b2a; // B sends into b2a, A receives from b2a
        size_t max_packet;
        fault_injection_t faults;
    } test_duplex_t;

    void test_duplex_init(test_duplex_t *d, size_t max_packet, size_t depth_packets);
    void test_duplex_free(test_duplex_t *d);

    int test_tp_send(void *ctx, const void *data, size_t len);
    int test_tp_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms);

    // Minimal stdio wrappers used by tests
    void *ts_fopen(void *ctx, const char *path, const char *mode);
    int ts_fread(void *ctx, void *buffer, size_t size, size_t count, void *file);
    int ts_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file);
    int ts_fseek(void *ctx, void *file, long offset, int whence);
    long ts_ftell(void *ctx, void *file);
    int ts_fclose(void *ctx, void *file);

    // System hooks
    uint32_t ts_ticks(void);
    void ts_delay(uint32_t ms);

    // Helper to build a common config for a session with provided buffers and duplex end
    void ts_make_config(val_config_t *cfg, void *send_buf, void *recv_buf, size_t packet_size, test_duplex_t *end_as_ctx,
                        val_resume_mode_t resume_mode, uint32_t verify_bytes);

    // Optional: register a simple console logger for debugging tests (honors VAL_LOG_LEVEL)
    void ts_set_console_logger(val_config_t *cfg);
    // Same, but let tests choose a runtime threshold (one of val_log_level_t)
    void ts_set_console_logger_with_level(val_config_t *cfg, int min_level);

    // Get a per-build artifacts root under the test executable directory, e.g.,
    //   <build>[/Config]/ut_artifacts
    // Returns 1 on success, 0 on failure. Buffer is always NUL-terminated on success.
    int ts_get_artifacts_root(char *out, size_t out_size);

    // Recursively create directories for the given path. Returns 0 on success, -1 on error.
    int ts_ensure_dir(const char *path);

    // Receiver thread helpers for tests
    typedef void *ts_thread_t; // opaque handle
    ts_thread_t ts_start_receiver(val_session_t *rx, const char *outdir);
    void ts_join_thread(ts_thread_t th);

    // Test utilities: compute file size and CRC32 for verification
    uint64_t ts_file_size(const char *path);
    uint32_t ts_file_crc32(const char *path);

    // Optional: read a size in bytes from an environment variable.
    // Accepts plain numbers or with k/m/g suffix (case-insensitive), e.g., "100k", "2M", "1g".
    // Returns default_value if the variable is unset or invalid. Clamps to at least 1 byte.
    size_t ts_env_size_bytes(const char *env_name, size_t default_value);

#ifdef __cplusplus
}
#endif

#endif // TEST_SUPPORT_H
