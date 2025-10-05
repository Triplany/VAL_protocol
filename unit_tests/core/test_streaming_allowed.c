#include "test_support.h"
#include "val_protocol.h"
#include <stdio.h>
#include <stdlib.h>

static int run_allowed_case(void)
{
    const size_t pkt = 2048;
    const size_t depth = 16;
    const size_t file_size = 48 * 1024 + 7;

    test_duplex_t d;
    test_duplex_init(&d, pkt, depth);

    char basedir[512], outdir[512];
    if (ts_build_case_dirs("streaming_allowed", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    char *inpath = ts_join_path_dyn(basedir, "in.bin");
    char *outpath = ts_join_path_dyn(outdir, "in.bin");
    if (!inpath || !outpath)
        return 1;
    if (ts_write_pattern_file(inpath, file_size) != 0)
        return 1;

    // Buffers
    uint8_t *sb_tx = (uint8_t *)calloc(1, pkt);
    uint8_t *rb_tx = (uint8_t *)calloc(1, pkt);
    uint8_t *sb_rx = (uint8_t *)calloc(1, pkt);
    uint8_t *rb_rx = (uint8_t *)calloc(1, pkt);

    val_config_t cfg_tx, cfg_rx;
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, sb_tx, rb_tx, pkt, &end_tx, VAL_RESUME_TAIL, 1024);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, pkt, &end_rx, VAL_RESUME_TAIL, 1024);

    // Enable streaming on both sides and allow incoming
    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.allow_streaming = 1;

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.allow_streaming = 1;

    val_session_t *tx = NULL, *rx = NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK)
        return 1;

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t s = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (s != VAL_OK)
        return 1;
    if (!ts_files_equal(inpath, outpath))
        return 1;

    int tx_send_ok = 0, tx_recv_ok = 0, rx_send_ok = 0, rx_recv_ok = 0;
    if (val_get_streaming_allowed(tx, &tx_send_ok, &tx_recv_ok) != VAL_OK)
        return 1;
    if (val_get_streaming_allowed(rx, &rx_send_ok, &rx_recv_ok) != VAL_OK)
        return 1;
    // Sender should be allowed to stream (since receiver accepts incoming)
    if (tx_send_ok != 1 || rx_recv_ok != 1)
        return 1;

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
    ts_cancel_token_t guard = ts_start_timeout_guard(10000, "streaming_allowed");
    int rc = run_allowed_case();
    ts_cancel_timeout_guard(guard);
    if (rc == 0)
        printf("OK\n");
    return rc;
}
