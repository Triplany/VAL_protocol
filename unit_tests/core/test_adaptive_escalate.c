#include "test_support.h"
#include "val_protocol.h"
#include <stdio.h>
#include <stdlib.h>

static int write_file(const char *path, size_t bytes) { return ts_write_pattern_file(path, bytes); }

static int run_escalate(void)
{
    const size_t pkt = 2048, depth = 64;
    const size_t FILESZ = 128 * 1024; // clean run to upgrade and engage streaming

    test_duplex_t d; test_duplex_init(&d, pkt, depth);

    char basedir[512], outdir[512];
    if (ts_build_case_dirs("adaptive_escalate", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0) return 1;
    char *in1 = ts_join_path_dyn(basedir, "in1.bin");
    char *out1 = ts_join_path_dyn(outdir, "in1.bin");
    if (!in1 || !out1) return 1;
    if (write_file(in1, FILESZ) != 0) return 1;

    uint8_t *sb_tx = (uint8_t*)calloc(1, pkt), *rb_tx = (uint8_t*)calloc(1, pkt);
    uint8_t *sb_rx = (uint8_t*)calloc(1, pkt), *rb_rx = (uint8_t*)calloc(1, pkt);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){ .a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet };

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_tx, rb_tx, pkt, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, pkt, &end_rx, VAL_RESUME_NEVER, 0);

    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
    cfg_tx.adaptive_tx.allow_streaming = true;
    cfg_tx.adaptive_tx.degrade_error_threshold = 1;
    cfg_tx.adaptive_tx.recovery_success_threshold = 1;

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_8;
    cfg_rx.adaptive_tx.allow_streaming = true;

    val_session_t *tx=NULL, *rx=NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK) return 1;

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = { in1 };
    val_status_t s = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (s != VAL_OK || !ts_files_equal(in1, out1)) return 1;

    val_tx_mode_t m = 0; bool st = false;
    if (val_get_current_tx_mode(tx, &m) != VAL_OK || val_is_streaming_engaged(tx, &st) != VAL_OK) return 1;
    if (m < VAL_TX_WINDOW_16) { fprintf(stderr, "expected mode >=16 after clean run, got %u\n", (unsigned)m); return 1; }
    if (!st) { fprintf(stderr, "expected streaming engaged after clean run, got %d\n", (int)st); return 1; }

    val_session_destroy(tx); val_session_destroy(rx);
    free(in1); free(out1); test_duplex_free(&d);
    free(sb_tx); free(rb_tx); free(sb_rx); free(rb_rx);
    return 0;
}

int main(void)
{
    ts_cancel_token_t guard = ts_start_timeout_guard(8000, "adaptive_escalate");
    int rc = run_escalate();
    ts_cancel_timeout_guard(guard);
    if (rc == 0) printf("OK\n");
    return rc;
}
