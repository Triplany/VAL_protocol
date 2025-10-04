#include "test_support.h"
#include "val_protocol.h"
#include <stdio.h>
#include <stdlib.h>

static int write_file(const char *path, size_t bytes) { return ts_write_pattern_file(path, bytes); }

static int run_deescalate(void)
{
    const size_t pkt = 2048, depth = 64;
    const size_t FILESZ = 128 * 1024; // short run with faults (enough frames to trigger errors deterministically)

    test_duplex_t d; test_duplex_init(&d, pkt, depth);

    char basedir[512], outdir[512];
    if (ts_build_case_dirs("adaptive_deescalate", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0) return 1;
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

    // Enable detailed console logging to observe adaptive transitions and streaming state
    ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_TRACE);
    ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_TRACE);

    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64; // start at max for faster path
    cfg_tx.adaptive_tx.allow_streaming = 1;
    cfg_tx.adaptive_tx.degrade_error_threshold = 1;        // drop mode immediately on any error
    cfg_tx.adaptive_tx.recovery_success_threshold = 100;    // require many clean successes to (re)engage streaming

    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_64;
    cfg_rx.adaptive_tx.allow_streaming = 1;

    val_session_t *tx=NULL, *rx=NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK) return 1;

    // Make fault sequence deterministic and introduce loss to trigger downgrade and disengage
    ts_rand_seed_set(42);
    end_tx.faults.drop_frame_per_million = 300000; // 30% drops to ensure at least one error occurs quickly

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = { in1 };
    val_status_t s = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (s != VAL_OK || !ts_files_equal(in1, out1)) return 1;

    val_tx_mode_t m = 0; int st = 1;
    if (val_get_current_tx_mode(tx, &m) != VAL_OK || val_is_streaming_engaged(tx, &st) != VAL_OK) return 1;
    if (st != 0) { fprintf(stderr, "expected streaming disengaged under faults, got %d\n", st); return 1; }
    if (m >= VAL_TX_WINDOW_64) { fprintf(stderr, "expected downgrade from max rung, got mode=%u\n", (unsigned)m); return 1; }

    val_session_destroy(tx); val_session_destroy(rx);
    free(in1); free(out1); test_duplex_free(&d);
    free(sb_tx); free(rb_tx); free(sb_rx); free(rb_rx);
    return 0;
}

int main(void)
{
    ts_cancel_token_t guard = ts_start_timeout_guard(12000, "adaptive_deescalate");
    int rc = run_deescalate();
    ts_cancel_timeout_guard(guard);
    if (rc == 0) printf("OK\n");
    return rc;
}
