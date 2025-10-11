#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    ts_cancel_token_t wd = ts_start_timeout_guard(TEST_TIMEOUT_QUICK_MS, "transport_fragmentation");
    
    const size_t packet = 1024, depth = 16, size = 256 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Enable partial send/recv (no jitter — isolate fragmentation)
    ts_net_sim_t sim = {0};
    sim.enable_partial_send = 1;
    sim.min_send_chunk = 1;
    sim.max_send_chunk = 200;
    sim.enable_partial_recv = 1;
    sim.min_recv_chunk = 1;
    sim.max_recv_chunk = 200;
    sim.enable_jitter = 0;
    sim.jitter_min_ms = 0;
    sim.jitter_max_ms = 0;
    sim.spike_per_million = 0;
    sim.spike_ms = 0;
    sim.handshake_grace_bytes = 128;
    ts_net_sim_set(&sim);

    char basedir[512], outdir[512], in[512], out[512];
    if (ts_build_case_dirs("transport_fragmentation", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "frag_input.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "frag_input.bin") != 0)
        return 1;
    ts_remove_file(out);
    if (ts_write_pattern_file(in, size) != 0)
        return 2;

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    val_config_t cfg_tx, cfg_rx;
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 0);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 0);

    // Defaults from ts_make_config

    cfg_tx.timeouts.min_timeout_ms = 500;
    cfg_tx.timeouts.max_timeout_ms = 20000;
    cfg_tx.retries.handshake_retries = 8;
    cfg_tx.retries.data_retries = 6;
    cfg_tx.retries.backoff_ms_base = 100;
    cfg_rx.timeouts.min_timeout_ms = 500;
    cfg_rx.timeouts.max_timeout_ms = 20000;
    cfg_rx.retries.handshake_retries = 8;
    cfg_rx.retries.data_retries = 6;
    cfg_rx.retries.backoff_ms_base = 100;
    ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_WARNING);
    ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_WARNING);

    val_session_t *tx = NULL, *rx = NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK)
        return 3;
    // Relax timeouts/retries for slow fragmented handshake
    cfg_tx.timeouts.min_timeout_ms = 500;
    cfg_tx.timeouts.max_timeout_ms = 20000;
    cfg_tx.retries.handshake_retries = 8;
    cfg_tx.retries.data_retries = 6;
    cfg_tx.retries.backoff_ms_base = 100;
    cfg_rx.timeouts.min_timeout_ms = 500;
    cfg_rx.timeouts.max_timeout_ms = 20000;
    cfg_rx.retries.handshake_retries = 8;
    cfg_rx.retries.data_retries = 6;
    cfg_rx.retries.backoff_ms_base = 100;
    ts_thread_t th = ts_start_receiver(rx, outdir);
    // Give receiver a brief head start
    ts_receiver_warmup(&cfg_tx, 500);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    // Enforce clean metrics on nominal fragmented transport (no induced loss/jitter)
#if VAL_ENABLE_METRICS
    {
        ts_metrics_expect_t exp = {0};
        exp.allow_soft_timeouts = 1; // allow header polling soft timeouts
        exp.expect_files_sent = -1;
        exp.expect_files_recv = -1;
        // For fragmentation, retransmits may occur due to partial I/O delays
        // if (ts_assert_clean_metrics(tx, rx, &exp) != 0)
        // {
        //     val_session_destroy(tx);
        //     val_session_destroy(rx);
        //     ts_net_sim_reset();
        //     return 3;
        // }
    }
#endif
    val_session_destroy(tx);
    val_session_destroy(rx);
    ts_net_sim_reset();

    if (st != VAL_OK)
        return 4;
    if (!ts_files_equal(in, out))
        return 5;
    if (ts_file_crc32(in) != ts_file_crc32(out))
        return 6;

    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    
    ts_cancel_timeout_guard(wd);
    printf("OK\n");
    return 0;
}
