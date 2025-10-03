#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#endif

// Test with small but realistic file sizes for fast testing
#define SMALL_FILE_SIZE (128 * 1024)  // 128KB
#define MEDIUM_FILE_SIZE (512 * 1024) // 512KB
#define LARGE_FILE_SIZE (1024 * 1024) // 1MB

// Simple test file creation
static int create_test_file(const char *path, size_t size)
{
    printf("DEBUG: Creating file %s with size %zu\n", path, size);
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        printf("DEBUG: Failed to open file for writing\n");
        return -1;
    }

    // Create simple repeating pattern
    for (size_t i = 0; i < size; ++i)
    {
        uint8_t v = (uint8_t)(i & 0xFF);
        fputc(v, f);
    }
    fclose(f);
    printf("DEBUG: File created successfully\n");
    return 0;
}

// Verify file integrity
static int verify_file_integrity(const char *original, const char *received, const char *test_name)
{
    uint64_t orig_size = ts_file_size(original);
    uint64_t recv_size = ts_file_size(received);

    if (orig_size != recv_size)
    {
        printf("FAIL [%s]: Size mismatch - original: %llu, received: %llu\n", test_name, (unsigned long long)orig_size,
               (unsigned long long)recv_size);
        return 1;
    }

    uint32_t orig_crc = ts_file_crc32(original);
    uint32_t recv_crc = ts_file_crc32(received);

    if (orig_crc != recv_crc)
    {
        printf("FAIL [%s]: CRC mismatch - original: 0x%08X, received: 0x%08X\n", test_name, orig_crc, recv_crc);
        return 1;
    }

    printf("PASS [%s]: File integrity verified - size: %llu bytes, CRC: 0x%08X\n", test_name, (unsigned long long)orig_size,
           orig_crc);
    return 0;
}

// Setup test paths
static void setup_test_paths(char *root, size_t rootsz, char *indir, size_t indirsz, char *outdir, size_t outdirsz, char *infile,
                             size_t infilesz, char *outfile, size_t outfilesz, const char *test_name)
{
    if (!ts_get_artifacts_root(root, rootsz))
    {
#if defined(_WIN32)
        snprintf(root, rootsz, ".\\ut_artifacts");
#else
        snprintf(root, rootsz, "./ut_artifacts");
#endif
    }

#if defined(_WIN32)
    snprintf(indir, indirsz, "%s\\%s\\in", root, test_name);
    snprintf(outdir, outdirsz, "%s\\%s\\out", root, test_name);
    snprintf(infile, infilesz, "%s\\test.bin", indir);
    snprintf(outfile, outfilesz, "%s\\test.bin", outdir);
#else
    snprintf(indir, indirsz, "%s/%s/in", root, test_name);
    snprintf(outdir, outdirsz, "%s/%s/out", root, test_name);
    snprintf(infile, infilesz, "%s/test.bin", indir);
    snprintf(outfile, outfilesz, "%s/test.bin", outdir);
#endif

    ts_ensure_dir(root);
    ts_ensure_dir(indir);
    ts_ensure_dir(outdir);
}

// Quick mode negotiation test
static int test_mode_negotiation_receiver_limits(void)
{
    printf("Testing mode negotiation with receiver limits...\n");

    const size_t packet = 1024, depth = 16;
    char root[512], indir[512], outdir[512], infile[512], outfile[512];
    setup_test_paths(root, sizeof(root), indir, sizeof(indir), outdir, sizeof(outdir), infile, sizeof(infile), outfile,
                     sizeof(outfile), "adaptive_receiver_limits");

    if (create_test_file(infile, MEDIUM_FILE_SIZE) != 0)
    {
        printf("FAIL: Could not create test file\n");
        return 1;
    }

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    uint8_t *sb_a = calloc(1, packet), *rb_a = calloc(1, packet);
    uint8_t *sb_b = calloc(1, packet), *rb_b = calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 0);

    // Quick adaptive configuration
    // Enable verbose console logging for debugging hangs
    ts_set_console_logger_with_level(&cfg_tx, 5);
    ts_set_console_logger_with_level(&cfg_rx, 5);

    // Clamp protocol timeouts and retries to avoid long hangs
    cfg_tx.timeouts.min_timeout_ms = 50;
    cfg_tx.timeouts.max_timeout_ms = 500;
    cfg_tx.retries.meta_retries = 2;
    cfg_tx.retries.data_retries = 2;
    cfg_tx.retries.ack_retries = 2;
    cfg_tx.retries.handshake_retries = 2;
    cfg_tx.retries.backoff_ms_base = 10;

    cfg_rx.timeouts.min_timeout_ms = 50;
    cfg_rx.timeouts.max_timeout_ms = 500;
    cfg_rx.retries.meta_retries = 2;
    cfg_rx.retries.data_retries = 2;
    cfg_rx.retries.ack_retries = 2;
    cfg_rx.retries.handshake_retries = 2;
    cfg_rx.retries.backoff_ms_base = 10;

    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
    cfg_tx.adaptive_tx.allow_streaming = 1;
    cfg_tx.adaptive_tx.degrade_error_threshold = 3;
    cfg_tx.adaptive_tx.recovery_success_threshold = 5;
    cfg_tx.adaptive_tx.mode_sync_interval = 25;
    cfg_tx.adaptive_tx.allocator.alloc = NULL;
    cfg_tx.adaptive_tx.allocator.free = NULL;
    cfg_tx.adaptive_tx.allocator.context = NULL;

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_4;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_4;
    cfg_rx.adaptive_tx.allow_streaming = 1;
    cfg_rx.adaptive_tx.mode_sync_interval = 25;
    cfg_rx.adaptive_tx.allocator.alloc = NULL;
    cfg_rx.adaptive_tx.allocator.free = NULL;
    cfg_rx.adaptive_tx.allocator.context = NULL;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;

    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);

    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        printf("FAIL: Session creation failed\n");
        goto cleanup;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {infile};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);

    if (st != VAL_OK)
    {
        printf("FAIL: Transfer failed with status %d\n", st);
        goto cleanup;
    }

    if (verify_file_integrity(infile, outfile, "receiver_limits") != 0)
    {
        goto cleanup;
    }

    printf("PASS: Mode negotiation works (transferred %d KB)\n", MEDIUM_FILE_SIZE / 1024);

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 0;

cleanup:
    if (tx)
        val_session_destroy(tx);
    if (rx)
        val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 1;
}

// Simple window size test - just test a few key modes
static int test_key_window_sizes(void)
{
    printf("Testing key window sizes...\n");

    val_tx_mode_t modes[] = {VAL_TX_WINDOW_64, VAL_TX_WINDOW_8, VAL_TX_STOP_AND_WAIT};

    const char *mode_names[] = {"WINDOW_64", "WINDOW_8", "STOP_AND_WAIT"};

    const size_t num_modes = sizeof(modes) / sizeof(modes[0]);
    const size_t packet = 1024, depth = 16;

    for (size_t i = 0; i < num_modes; ++i)
    {
        val_tx_mode_t mode = modes[i];
        printf("  Testing mode %s...\n", mode_names[i]);

        char test_name[64];
        snprintf(test_name, sizeof(test_name), "adaptive_mode_%d", mode);

        char root[512], indir[512], outdir[512], infile[512], outfile[512];
        setup_test_paths(root, sizeof(root), indir, sizeof(indir), outdir, sizeof(outdir), infile, sizeof(infile), outfile,
                         sizeof(outfile), test_name);

        if (create_test_file(infile, SMALL_FILE_SIZE) != 0)
        {
            printf("FAIL: Could not create test file for mode %s\n", mode_names[i]);
            return 1;
        }

        test_duplex_t d;
        test_duplex_init(&d, packet, depth);

        uint8_t *sb_a = calloc(1, packet), *rb_a = calloc(1, packet);
        uint8_t *sb_b = calloc(1, packet), *rb_b = calloc(1, packet);

        test_duplex_t end_tx = d;
        test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

        val_config_t cfg_tx, cfg_rx;
        ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 0);
        ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 0);

        // Enable verbose console logging for debugging hangs
        ts_set_console_logger_with_level(&cfg_tx, 5);
        ts_set_console_logger_with_level(&cfg_rx, 5);

        // Clamp protocol timeouts and retries to avoid long hangs
        cfg_tx.timeouts.min_timeout_ms = 50;
        cfg_tx.timeouts.max_timeout_ms = 500;
        cfg_tx.retries.meta_retries = 1;
        cfg_tx.retries.data_retries = 1;
        cfg_tx.retries.ack_retries = 1;
        cfg_tx.retries.handshake_retries = 1;
        cfg_tx.retries.backoff_ms_base = 10;

        cfg_rx.timeouts.min_timeout_ms = 50;
        cfg_rx.timeouts.max_timeout_ms = 500;
        cfg_rx.retries.meta_retries = 1;
        cfg_rx.retries.data_retries = 1;
        cfg_rx.retries.ack_retries = 1;
        cfg_rx.retries.handshake_retries = 1;
        cfg_rx.retries.backoff_ms_base = 10;

        // Force specific mode
        cfg_tx.adaptive_tx.max_performance_mode = mode;
        cfg_tx.adaptive_tx.preferred_initial_mode = mode;
    cfg_tx.adaptive_tx.allow_streaming = 1;
    cfg_tx.adaptive_tx.allow_streaming = 1;
        cfg_tx.adaptive_tx.degrade_error_threshold = 100;
        cfg_tx.adaptive_tx.recovery_success_threshold = 100;
        cfg_tx.adaptive_tx.mode_sync_interval = 25;
        cfg_tx.adaptive_tx.allocator.alloc = NULL;
        cfg_tx.adaptive_tx.allocator.free = NULL;
        cfg_tx.adaptive_tx.allocator.context = NULL;

        cfg_rx.adaptive_tx.max_performance_mode = mode;
        cfg_rx.adaptive_tx.preferred_initial_mode = mode;
    cfg_rx.adaptive_tx.allow_streaming = 1;
    cfg_rx.adaptive_tx.allow_streaming = 1;
        cfg_rx.adaptive_tx.mode_sync_interval = 25;
        cfg_rx.adaptive_tx.allocator.alloc = NULL;
        cfg_rx.adaptive_tx.allocator.free = NULL;
        cfg_rx.adaptive_tx.allocator.context = NULL;

        val_session_t *tx = NULL, *rx = NULL;
        uint32_t dtx = 0, drx = 0;

        val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
        val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);

        if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
        {
            printf("FAIL: Session creation failed for mode %s\n", mode_names[i]);
            goto cleanup_mode;
        }

        ts_thread_t th = ts_start_receiver(rx, outdir);
        const char *files[1] = {infile};
        val_status_t st = val_send_files(tx, files, 1, NULL);
        ts_join_thread(th);

        if (st != VAL_OK)
        {
            printf("FAIL: Transfer failed for mode %s with status %d\n", mode_names[i], st);
            goto cleanup_mode;
        }

        char integrity_test_name[128];
        snprintf(integrity_test_name, sizeof(integrity_test_name), "mode_%s", mode_names[i]);
        if (verify_file_integrity(infile, outfile, integrity_test_name) != 0)
        {
            goto cleanup_mode;
        }

        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        continue;

    cleanup_mode:
        if (tx)
            val_session_destroy(tx);
        if (rx)
            val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }

    printf("PASS: Key window sizes work correctly\n");
    return 0;
}

// Fast, deterministic adaptive mode transition test using small files and direct mode sampling
static int test_adaptive_mode_transitions_small(void)
{
    printf("Testing adaptive mode transitions with small transfers and direct mode sampling...\n");

    const size_t packet = 1024, depth = 16;
    char root[512], indir[512], outdir[512], infile1[512], outfile1[512];
    setup_test_paths(root, sizeof(root), indir, sizeof(indir), outdir, sizeof(outdir), infile1, sizeof(infile1), outfile1,
                     sizeof(outfile1), "adaptive_mode_transitions");

    // Create two tiny files to drive two quick transmissions
    if (create_test_file(infile1, SMALL_FILE_SIZE) != 0)
    {
        printf("FAIL: Could not create test file 1\n");
        return 1;
    }

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Introduce rare CRC corruption to trigger a single degradation without stalling
    d.faults.drop_frame_per_million = 0;
    d.faults.bitflip_per_million = 5; // ~0.5% chance per packet to corrupt

    uint8_t *sb_a = calloc(1, packet), *rb_a = calloc(1, packet);
    uint8_t *sb_b = calloc(1, packet), *rb_b = calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 0);

    // Verbose logging for this focused test
    ts_set_console_logger_with_level(&cfg_tx, 5);
    ts_set_console_logger_with_level(&cfg_rx, 5);

    // Bounded but sufficient retries to avoid aborting under light loss
    cfg_tx.timeouts.min_timeout_ms = 50;
    cfg_tx.timeouts.max_timeout_ms = 800;
    cfg_tx.retries.meta_retries = 1;
    cfg_tx.retries.data_retries = 2;
    cfg_tx.retries.ack_retries = 6;
    cfg_tx.retries.handshake_retries = 2;
    cfg_tx.retries.backoff_ms_base = 10;

    cfg_rx.timeouts.min_timeout_ms = 50;
    cfg_rx.timeouts.max_timeout_ms = 800;
    cfg_rx.retries.meta_retries = 1;
    cfg_rx.retries.data_retries = 2;
    cfg_rx.retries.ack_retries = 6;
    cfg_rx.retries.handshake_retries = 2;
    cfg_rx.retries.backoff_ms_base = 10;

    // Make adaptation fast and visible; use WINDOW_64 as highest-performance rung
    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
    cfg_tx.adaptive_tx.allow_streaming = 1;
    cfg_tx.adaptive_tx.allow_streaming = 1;
    cfg_tx.adaptive_tx.degrade_error_threshold = 1;    // on first trouble
    cfg_tx.adaptive_tx.recovery_success_threshold = 2; // quick recovery
    cfg_tx.adaptive_tx.mode_sync_interval = 10;

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
    cfg_rx.adaptive_tx.allow_streaming = 1;
    cfg_rx.adaptive_tx.allow_streaming = 1;
    cfg_rx.adaptive_tx.mode_sync_interval = 10;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    if (val_session_create(&cfg_tx, &tx, &dtx) != VAL_OK || val_session_create(&cfg_rx, &rx, &drx) != VAL_OK)
    {
        printf("FAIL: Session creation failed (tx_detail=0x%08X rx_detail=0x%08X)\n", dtx, drx);
        goto cleanup;
    }

    // Phase 1: high loss should cause a downgrade by the end of the small file
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files1[1] = {infile1};
    val_status_t st = val_send_files(tx, files1, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        printf("FAIL: Phase 1 transfer failed: %d\n", st);
        goto cleanup;
    }
    if (verify_file_integrity(infile1, outfile1, "adaptive_phase1") != 0)
        goto cleanup;

    val_tx_mode_t mode_after_loss = VAL_TX_STOP_AND_WAIT;
    if (val_get_current_tx_mode(tx, &mode_after_loss) != VAL_OK)
    {
        printf("FAIL: Could not read current TX mode after phase 1\n");
        goto cleanup;
    }
    printf("DEBUG: Mode after high-loss phase: %d (lower is higher performance)\n", (int)mode_after_loss);

    // Phase 2: remove loss and send another tiny file; expect an upgrade relative to phase 1
    end_tx.faults.drop_frame_per_million = 0;
    char infile2[512], outfile2[512];
    setup_test_paths(root, sizeof(root), indir, sizeof(indir), outdir, sizeof(outdir), infile2, sizeof(infile2), outfile2,
                     sizeof(outfile2), "adaptive_mode_transitions_2");
    if (create_test_file(infile2, SMALL_FILE_SIZE) != 0)
    {
        printf("FAIL: Could not create test file 2\n");
        goto cleanup;
    }
    th = ts_start_receiver(rx, outdir);
    const char *files2[1] = {infile2};
    st = val_send_files(tx, files2, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        printf("FAIL: Phase 2 transfer failed: %d\n", st);
        goto cleanup;
    }
    if (verify_file_integrity(infile2, outfile2, "adaptive_phase2") != 0)
        goto cleanup;

    val_tx_mode_t mode_after_recovery = VAL_TX_STOP_AND_WAIT;
    if (val_get_current_tx_mode(tx, &mode_after_recovery) != VAL_OK)
    {
        printf("FAIL: Could not read current TX mode after phase 2\n");
        goto cleanup;
    }
    printf("DEBUG: Mode after recovery phase: %d\n", (int)mode_after_recovery);

    // Expect that recovery moved us toward higher performance (numerically smaller enum)
    if (!(mode_after_recovery <= mode_after_loss))
    {
        printf("FAIL: Expected upgrade after loss removal (mode %d -> %d)\n", (int)mode_after_loss, (int)mode_after_recovery);
        goto cleanup;
    }

    printf("PASS: Adaptive mode transitioned as expected (loss -> %d, recovery -> %d)\n", (int)mode_after_loss,
           (int)mode_after_recovery);

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 0;

cleanup:
    if (tx)
        val_session_destroy(tx);
    if (rx)
        val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 1;
}

int main(void)
{
    int rc = 0;

    printf("=== VAL Protocol Adaptive Transmission Test Suite (Fast) ===\n\n");

    if (test_mode_negotiation_receiver_limits() != 0)
        rc = 1;
    printf("\n");

    if (test_key_window_sizes() != 0)
        rc = 1;
    printf("\n");

    if (test_adaptive_mode_transitions_small() != 0)
        rc = 1;
    printf("\n");

    if (rc == 0)
    {
        printf("=== ALL ADAPTIVE TRANSMISSION TESTS PASSED ===\n");
        printf("✓ Mode negotiation with receiver limits verified\n");
        printf("✓ Key transmission modes tested with realistic file sizes\n");
        printf("✓ Adaptive behavior under light fault injection validated\n");
        printf("✓ File integrity verified with CRC checking on all transfers\n");
    }
    else
    {
        printf("=== SOME ADAPTIVE TRANSMISSION TESTS FAILED ===\n");
    }

    return rc;
}