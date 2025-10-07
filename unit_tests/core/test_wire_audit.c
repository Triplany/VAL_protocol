#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to write a temporary file of given size
static int write_tmp_file(const char *path, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    const size_t K = 1024;
    uint8_t buf[1024];
    for (size_t i = 0; i < K; ++i)
        buf[i] = (uint8_t)(i & 0xFF);
    size_t left = bytes;
    while (left > 0)
    {
        size_t take = left < K ? left : K;
        if (fwrite(buf, 1, take, f) != take)
        {
            fclose(f);
            return -1;
        }
        left -= take;
    }
    fclose(f);
    return 0;
}

static int run_mode_and_check(val_tx_mode_t mode, unsigned expected_cap)
{
    // Arrange duplex and configs
    const size_t P = 4096;
    test_duplex_t d = {0};
    test_duplex_init(&d, P, 128);
    // TX endpoint uses d as-is; RX must see directions reversed
    test_duplex_t end_tx = d;
    test_duplex_t end_rx;
    end_rx.a2b = d.b2a;
    end_rx.b2a = d.a2b;
    end_rx.max_packet = d.max_packet;
    end_rx.faults = d.faults;

    uint8_t *tx_sbuf = (uint8_t *)malloc(P);
    uint8_t *tx_rbuf = (uint8_t *)malloc(P);
    uint8_t *rx_sbuf = (uint8_t *)malloc(P);
    uint8_t *rx_rbuf = (uint8_t *)malloc(P);
    if (!tx_sbuf || !tx_rbuf || !rx_sbuf || !rx_rbuf)
    {
        fprintf(stderr, "alloc failure for buffers\n");
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_sbuf, tx_rbuf, P, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&rx_cfg, rx_sbuf, rx_rbuf, P, &end_rx, VAL_RESUME_NEVER, 0);

    // Defaults from ts_make_config

    // Keep logs quiet unless debugging
    ts_set_console_logger_with_level(&tx_cfg, VAL_LOG_WARNING);
    ts_set_console_logger_with_level(&rx_cfg, VAL_LOG_WARNING);
    // Adaptive caps: force fixed mode for this test to keep invariants deterministic
    tx_cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    tx_cfg.adaptive_tx.preferred_initial_mode = mode;
    tx_cfg.adaptive_tx.allow_streaming = 1;
    tx_cfg.adaptive_tx.degrade_error_threshold = 1000; // avoid automatic changes
    tx_cfg.adaptive_tx.recovery_success_threshold = 1000;
    rx_cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    rx_cfg.adaptive_tx.preferred_initial_mode = mode;
    rx_cfg.adaptive_tx.allow_streaming = 1;

    val_session_t *tx = NULL, *rx = NULL;
    if (val_session_create(&tx_cfg, &tx, NULL) != VAL_OK || val_session_create(&rx_cfg, &rx, NULL) != VAL_OK)
    {
        fprintf(stderr, "val_session_create failed\n");
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }

    // File IO: prepare tiny file in a per-test artifacts dir
    char root[512];
    if (ts_get_artifacts_root(root, sizeof(root)) != 1)
    {
        fprintf(stderr, "artifacts root not found\n");
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
#if defined(_WIN32)
    char basedir[1024];
    ts_str_copy(basedir, sizeof(basedir), root);
    ts_str_append(basedir, sizeof(basedir), "\\wire_audit");
    char modedir[1024];
    ts_str_copy(modedir, sizeof(modedir), basedir);
    ts_str_append(modedir, sizeof(modedir), "\\mode_");
    {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)mode);
    ts_str_append(modedir, sizeof(modedir), tmp);
    }
    char outdir[1024];
    ts_str_copy(outdir, sizeof(outdir), modedir);
    ts_str_append(outdir, sizeof(outdir), "\\out");
    char inpath[1024];
    ts_str_copy(inpath, sizeof(inpath), modedir);
    ts_str_append(inpath, sizeof(inpath), "\\in.bin");
#else
    char basedir[1024];
    ts_str_copy(basedir, sizeof(basedir), root);
    ts_str_append(basedir, sizeof(basedir), "/wire_audit");
    char modedir[1024];
    ts_str_copy(modedir, sizeof(modedir), basedir);
    ts_str_append(modedir, sizeof(modedir), "/mode_");
    {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)mode);
    ts_str_append(modedir, sizeof(modedir), tmp);
    }
    char outdir[1024];
    ts_str_copy(outdir, sizeof(outdir), modedir);
    ts_str_append(outdir, sizeof(outdir), "/out");
    char inpath[1024];
    ts_str_copy(inpath, sizeof(inpath), modedir);
    ts_str_append(inpath, sizeof(inpath), "/in.bin");
#endif
    if (ts_ensure_dir(basedir) != 0 || ts_ensure_dir(modedir) != 0 || ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "failed to create artifacts dirs (base='%s' out='%s')\n", basedir, outdir);
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    // Remove any stale outputs from the same mode to ensure a clean run
    {
        // Construct out file path to remove; receiver writes the same filename as input by default
    char outpath[1024];
#if defined(_WIN32)
    ts_str_copy(outpath, sizeof(outpath), outdir);
    ts_str_append(outpath, sizeof(outpath), "\\in.bin");
        DeleteFileA(outpath);
#else
    ts_str_copy(outpath, sizeof(outpath), outdir);
    ts_str_append(outpath, sizeof(outpath), "/in.bin");
        (void)remove(outpath);
#endif
        // Also remove input if present to avoid partial leftovers
#if defined(_WIN32)
        DeleteFileA(inpath);
#else
        (void)remove(inpath);
#endif
    }
    if (write_tmp_file(inpath, 128 * 1024) != 0)
    {
        fprintf(stderr, "failed to write tmp file '%s'\n", inpath);
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }

    // Pre-verify the input file is readable and the expected size
    uint64_t fsz = ts_file_size(inpath);
    if (fsz != 128ull * 1024ull)
    {
        fprintf(stderr, "precheck: input file wrong size: got=%llu expected=%u\n", (unsigned long long)fsz, 128u * 1024u);
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    uint32_t fcrc = ts_file_crc32(inpath);
    (void)fcrc; // not strictly used but exercises read path

    // Start receiver thread
    ts_thread_t th = ts_start_receiver(rx, outdir);
    // Give receiver a small head start to listen
    if (tx_cfg.system.delay_ms)
        tx_cfg.system.delay_ms(15);

    const char *files[1] = {inpath};
    val_status_t send_rc = val_send_files(tx, files, 1, "");
    ts_join_thread(th);
    if (send_rc != VAL_OK)
    {
        fprintf(stderr, "val_send_files failed (mode=%u) rc=%d\n", (unsigned)mode, (int)send_rc);
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }

#if VAL_ENABLE_WIRE_AUDIT
    // Verify on-wire hygiene and window behavior
    val_wire_audit_t a_tx = {0}, a_rx = {0};
    if (val_get_wire_audit(tx, &a_tx) != VAL_OK || val_get_wire_audit(rx, &a_rx) != VAL_OK)
    {
        fprintf(stderr, "val_get_wire_audit failed\n");
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    // Receiver should not send DATA
    if (a_rx.sent_data != 0)
    {
        fprintf(stderr, "wire_audit hygiene: receiver sent DATA unexpectedly (count=%llu)\n", (unsigned long long)a_rx.sent_data);
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    // Sender should not send DONE_ACK/EOT_ACK
    if (a_tx.sent_done_ack != 0 || a_tx.sent_eot_ack != 0)
    {
        fprintf(stderr, "wire_audit hygiene: sender sent *_ACK unexpectedly (done_ack=%llu eot_ack=%llu)\n",
                (unsigned long long)a_tx.sent_done_ack, (unsigned long long)a_tx.sent_eot_ack);
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    // Mode-specific inflight bounds: expected_cap is the theoretical max
    if (a_tx.max_inflight_observed > expected_cap)
    {
        fprintf(stderr, "wire_audit window: observed inflight=%u exceeds cap=%u for mode=%u\n", a_tx.max_inflight_observed,
                expected_cap, (unsigned)mode);
        val_session_destroy(tx);
        val_session_destroy(rx);
        test_duplex_free(&d);
        free(tx_sbuf);
        free(tx_rbuf);
        free(rx_sbuf);
        free(rx_rbuf);
        return 1;
    }
    // No special case for streaming: window rung governs inflight bound.
#else
    (void)expected_cap;
    (void)mode;
#endif

    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&d);
    free(tx_sbuf);
    free(tx_rbuf);
    free(rx_sbuf);
    free(rx_rbuf);
    return 0;
}

int main(void)
{
#if !VAL_ENABLE_WIRE_AUDIT
    // If auditing is not compiled in, this test becomes a no-op quick pass
    return 0;
#else
    int rc = 0;
    rc |= run_mode_and_check(VAL_TX_STOP_AND_WAIT, 1);
    rc |= run_mode_and_check(VAL_TX_WINDOW_2, 2);
    rc |= run_mode_and_check(VAL_TX_WINDOW_8, 8);
    rc |= run_mode_and_check(VAL_TX_WINDOW_16, 16);
    return rc;
#endif
}