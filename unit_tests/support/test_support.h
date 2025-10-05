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

    // Network simulation controls (disabled by default)
    typedef struct
    {
        // Partial IO on send: when >0, the transport may accept fewer bytes than requested.
        int enable_partial_send;
        size_t min_send_chunk; // >=1
        size_t max_send_chunk; // <= requested len

        // Partial IO on recv: when >0, the transport may deliver fewer bytes than requested.
        int enable_partial_recv;
        size_t min_recv_chunk; // >=1
        size_t max_recv_chunk; // <= buffer_size

        // Jitter and spikes (ms)
        int enable_jitter;
        uint32_t jitter_min_ms;
        uint32_t jitter_max_ms;
        uint32_t spike_per_million; // additional spike probability
        uint32_t spike_ms;          // added delay on spike

        // Frame reordering: hold back frames and release out of order
        int enable_reorder;
        uint32_t reorder_per_million; // probability to enqueue frame instead of immediate send
        size_t reorder_queue_max;     // cap queued frames; flush oldest when exceeded

        // RNG seed for determinism
        uint64_t rng_seed;
        // Keep the first N bytes in-order and unmodified to allow clean handshake. 0 = disabled. Default tests use ~128.
        uint32_t handshake_grace_bytes;
    } ts_net_sim_t;

    // Configure global network simulation (applies to all test transports)
    void ts_net_sim_set(const ts_net_sim_t *cfg);
    void ts_net_sim_reset(void);
    void ts_rand_seed_set(uint64_t seed);

    // Minimal stdio wrappers used by tests
    void *ts_fopen(void *ctx, const char *path, const char *mode);
    size_t ts_fread(void *ctx, void *buffer, size_t size, size_t count, void *file);
    size_t ts_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file);
    int ts_fseek(void *ctx, void *file, long offset, int whence);
    long ts_ftell(void *ctx, void *file);
    int ts_fclose(void *ctx, void *file);

    // Filesystem fault injection (disabled by default)
    typedef enum
    {
        TS_FS_FAIL_NONE = 0,
        TS_FS_FAIL_SHORT_WRITE,
        TS_FS_FAIL_DISK_FULL,
        TS_FS_FAIL_EACCES
    } ts_fs_fail_mode_t;
    typedef struct
    {
        ts_fs_fail_mode_t mode;
        uint64_t fail_after_total_bytes; // after this many bytes written across a file, subsequent writes fail
        // Optional path prefix to deny writes (simulates EACCES/locked). If non-NULL, any fopen for write under this prefix
        // fails.
        const char *deny_write_prefix;
    } ts_fs_faults_t;
    void ts_fs_faults_set(const ts_fs_faults_t *f);
    void ts_fs_faults_reset(void);

    // System hooks
    //
    // Test clock policy:
    // - ts_make_config() can install a real, monotonic millisecond clock and delay function if
    //   the caller leaves cfg->system.get_ticks_ms or cfg->system.delay_ms NULL (tests only).
    //   In production, a clock is always required and must be provided by the application.
    // - Platform implementations:
    //     * Windows: GetTickCount64() for ticks; Sleep(ms) for delay
    //     * POSIX: clock_gettime(CLOCK_MONOTONIC) for ticks; nanosleep() for delay
    //       (with a coarse time() fallback if CLOCK_MONOTONIC is unavailable)
    // - Deterministic tests (e.g., adaptive timeout unit tests) can override the
    //   defaults by assigning cfg->system.get_ticks_ms (and optionally delay_ms)
    //   before calling val_session_create(). ts_make_config() only fills these
    //   hooks when they are NULL, so explicit assignments are preserved.
    uint32_t ts_ticks(void);
    void ts_delay(uint32_t ms);

    // Helper to build a common config for a session with provided buffers and duplex end
    // resume_mode: VAL_RESUME_NEVER, VAL_RESUME_SKIP_EXISTING, or VAL_RESUME_TAIL
    // verify_bytes: for VAL_RESUME_TAIL, this sets tail_cap_bytes (cap on verification window)
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

    // Extra cross-platform helpers for tests to reduce duplication
    // Safe string copy/append into fixed-size buffers. Always NUL-terminates.
    void ts_str_copy(char *dst, size_t dst_size, const char *src);
    void ts_str_append(char *dst, size_t dst_size, const char *src);

    int ts_remove_file(const char *path);                           // ignore if not present, returns 0 on success
    int ts_write_pattern_file(const char *path, size_t size_bytes); // writes 0..255 repeating
    int ts_files_equal(const char *a, const char *b);               // returns 1 if identical, 0 otherwise
    // Fixed-buffer path join using platform separator; returns 0 on success
    int ts_path_join(char *dst, size_t dst_size, const char *a, const char *b);
    // Dynamic formatting and path-join (caller must free)
    char *ts_dyn_sprintf(const char *fmt, ...);
    char *ts_join_path_dyn(const char *a, const char *b);
    // Locate example binaries (val_example_receive/send) built by this tree.
    // Fills absolute paths into rx_out/tx_out and returns 1 on success, 0 on failure.
    int ts_find_example_bins(char *rx_out, size_t rx_out_size, char *tx_out, size_t tx_out_size);
    // Convenience: create standard artifacts directories for a test case
    // Produces <artifacts_root>/<case_name> and <...>/out
    int ts_build_case_dirs(const char *case_name, char *basedir, size_t basedir_sz, char *outdir, size_t outdir_sz);
    // Optional: small receiver warm-up without open-coding delay checks
    void ts_receiver_warmup(const val_config_t *cfg, uint32_t ms);

    // Fake clock support for deterministic time tests
    typedef struct
    {
        uint32_t now_ms;     // current time
        uint32_t wrap_after; // 0 to disable; when non-zero, modulo arithmetic simulates wrap
        int enable_wrap;     // whether to wrap at 2^32 (or wrap_after)
    } ts_fake_clock_t;
    // Returns a get_ticks_ms function compatible with val_config_t that reads from the provided fake clock
    uint32_t ts_fake_get_ticks_ms(void);
    void ts_fake_delay_ms(uint32_t ms);
    // Manage a process-global fake clock instance (disabled by default)
    void ts_fake_clock_install(const ts_fake_clock_t *init);
    void ts_fake_clock_uninstall(void);
    void ts_fake_clock_advance(uint32_t delta_ms);
    void ts_fake_clock_set(uint32_t now_ms);

    // Watchdog: abort process if not cancelled within the timeout (helps catch hangs in CI)
    typedef void *ts_cancel_token_t; // opaque
    ts_cancel_token_t ts_start_timeout_guard(uint32_t timeout_ms, const char *name);
    void ts_cancel_timeout_guard(ts_cancel_token_t token);

#ifdef __cplusplus
}
#endif

#endif // TEST_SUPPORT_H
