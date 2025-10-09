#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Verify that the initial TX mode chosen after handshake is the slower (numerically larger enum)
// of the local and peer preferred_initial_mode values, clamped to the negotiated cap.
static int test_conservative_initial_mode(void)
{
    const size_t packet = 1024, depth = 8;
    char basedir[2048], outdir[2048];
    if (ts_build_case_dirs("initial_mode_negotiation", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not build case directories\n");
        return 1;
    }
    char infile[2048], outfile[2048];
    if (ts_path_join(infile, sizeof(infile), basedir, "test.bin") != 0 ||
        ts_path_join(outfile, sizeof(outfile), outdir, "test.bin") != 0)
    {
        printf("FAIL: Could not assemble file paths\n");
        return 1;
    }
    // Tiny file to avoid long runs; upgrades are disabled anyway
    if (ts_write_pattern_file(infile, 4096) != 0)
    {
        printf("FAIL: Could not create test file\n");
        return 1;
    }

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    uint8_t *sb_tx = calloc(1, packet), *rb_tx = calloc(1, packet);
    uint8_t *sb_rx = calloc(1, packet), *rb_rx = calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_tx, rb_tx, packet, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, packet, &end_rx, VAL_RESUME_NEVER, 0);

    // Defaults from ts_make_config are sufficient for this test

    // Keep logs minimal
    ts_set_console_logger_with_level(&cfg_tx, 0);
    ts_set_console_logger_with_level(&cfg_rx, 0);

    // Tight timeouts but not critical here
    cfg_tx.timeouts.min_timeout_ms = 50;
    cfg_tx.timeouts.max_timeout_ms = 500;
    cfg_rx.timeouts.min_timeout_ms = 50;
    cfg_rx.timeouts.max_timeout_ms = 500;

    // Both have full capability, but different initial preferences
    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_16; // faster preference
    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_4; // slower preference

    // Disable upgrades/degrades so the initial rung remains observable throughout transfer
    cfg_tx.adaptive_tx.degrade_error_threshold = 1000;
    cfg_tx.adaptive_tx.recovery_success_threshold = 1000;
    cfg_rx.adaptive_tx.degrade_error_threshold = 1000;
    cfg_rx.adaptive_tx.recovery_success_threshold = 1000;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    if (val_session_create(&cfg_tx, &tx, &dtx) != VAL_OK || val_session_create(&cfg_rx, &rx, &drx) != VAL_OK)
    {
        printf("FAIL: session_create failed (tx_detail=0x%08X rx_detail=0x%08X)\n", dtx, drx);
        goto cleanup;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {infile};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        printf("FAIL: transfer failed: %d\n", st);
        goto cleanup;
    }

    // Confirm integrity
    if (!ts_files_equal(infile, outfile))
    {
        printf("FAIL: file mismatch after transfer\n");
        goto cleanup;
    }

    // Assert initial/active cwnd equals the slower preference (4)
    uint32_t cw = 0;
    if (val_get_cwnd_packets(tx, &cw) != VAL_OK)
    {
        printf("FAIL: could not query cwnd\n");
        goto cleanup;
    }
    if (cw != 4u)
    {
        printf("FAIL: expected initial/active cwnd 4 but got %u\n", (unsigned)cw);
        goto cleanup;
    }

    printf("PASS: conservative initial cwnd selected correctly (4)\n");

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_tx);
    free(rb_tx);
    free(sb_rx);
    free(rb_rx);
    test_duplex_free(&d);
    return 0;

cleanup:
    if (tx)
        val_session_destroy(tx);
    if (rx)
        val_session_destroy(rx);
    free(sb_tx);
    free(rb_tx);
    free(sb_rx);
    free(rb_rx);
    test_duplex_free(&d);
    return 1;
}

int main(void)
{
    return test_conservative_initial_mode();
}
