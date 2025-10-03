#include "test_support.h"
#include "val_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_veto_case(void)
{
    const size_t pkt = 2048;
    const size_t depth = 16;
    const size_t file_size = 64 * 1024;

    test_duplex_t d;
    test_duplex_init(&d, pkt, depth);

    char basedir[512], outdir[512];
    if (ts_build_case_dirs("streaming_veto", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
    {
        fprintf(stderr, "case dirs failed\n");
        return 1;
    }
    char *inpath = ts_join_path_dyn(basedir, "in.bin");
    char *outpath = ts_join_path_dyn(outdir, "in.bin");
    if (!inpath || !outpath)
    {
        fprintf(stderr, "path alloc failed\n");
        free(inpath);
        free(outpath);
        return 1;
    }
    if (ts_write_pattern_file(inpath, file_size) != 0)
    {
        fprintf(stderr, "write pattern failed\n");
        free(inpath);
        free(outpath);
        return 1;
    }

    // Allocate buffers
    uint8_t *sb_tx = (uint8_t *)calloc(1, pkt);
    uint8_t *rb_tx = (uint8_t *)calloc(1, pkt);
    uint8_t *sb_rx = (uint8_t *)calloc(1, pkt);
    uint8_t *rb_rx = (uint8_t *)calloc(1, pkt);

    val_config_t cfg_tx, cfg_rx;
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, sb_tx, rb_tx, pkt, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, pkt, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);

    // Configure adaptive TX/window and streaming flags.
    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.allow_streaming = 1;         // sender would like to stream and accept peer

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.allow_streaming = 0;         // VETO: do not allow peer to stream to me

    // Optional: keep logs lower noise in CI
    // ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_WARNING);
    // ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_WARNING);

    val_session_t *tx = NULL, *rx = NULL;
    val_status_t stx = val_session_create(&cfg_tx, &tx, NULL);
    val_status_t srx = val_session_create(&cfg_rx, &rx, NULL);
    if (stx != VAL_OK || srx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx=%d rx=%d)\n", (int)stx, (int)srx);
        free(inpath);
        free(outpath);
        return 1;
    }

    // Start receiver and perform a single transfer
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t s = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (s != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)s);
        free(inpath);
        free(outpath);
        return 1;
    }

    // Validate file integrity
    if (!ts_files_equal(inpath, outpath))
    {
        fprintf(stderr, "file mismatch after transfer\n");
        free(inpath);
        free(outpath);
        return 1;
    }

    // Query negotiated streaming permissions on both sessions after handshake/transfer
    int tx_send_allowed = -1, tx_recv_allowed = -1;
    int rx_send_allowed = -1, rx_recv_allowed = -1;
    if (val_get_streaming_allowed(tx, &tx_send_allowed, &tx_recv_allowed) != VAL_OK ||
        val_get_streaming_allowed(rx, &rx_send_allowed, &rx_recv_allowed) != VAL_OK)
    {
        fprintf(stderr, "val_get_streaming_allowed failed\n");
        free(inpath);
        free(outpath);
        return 1;
    }

    // Expectation: sender wanted to stream, but receiver vetoed -> sender may NOT stream
    if (tx_send_allowed != 0)
    {
        fprintf(stderr, "expected sender streaming vetoed, but got allowed=%d\n", tx_send_allowed);
        free(inpath);
        free(outpath);
        return 1;
    }
    // And receiver reports it does NOT accept peer streaming
    if (rx_recv_allowed != 0)
    {
        fprintf(stderr, "expected receiver not accepting streaming, but got recv_allowed=%d\n", rx_recv_allowed);
        free(inpath);
        free(outpath);
        return 1;
    }

    // Cleanup
    val_session_destroy(tx);
    val_session_destroy(rx);
    free(inpath);
    free(outpath);
    test_duplex_free(&d);
    free(sb_tx);
    free(rb_tx);
    free(sb_rx);
    free(rb_rx);
    return 0;
}

int main(void)
{
    ts_cancel_token_t guard = ts_start_timeout_guard(15000, "streaming_veto");
    int rc = run_veto_case();
    ts_cancel_timeout_guard(guard);
    if (rc == 0)
        printf("OK\n");
    return rc;
}
