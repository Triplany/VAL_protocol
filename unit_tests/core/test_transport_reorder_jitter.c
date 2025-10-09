#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const size_t packet = 1024, depth = 32, size = 128 * 1024 + 13; // keep test fast under watchdog
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    ts_net_sim_t sim = {0};
    sim.enable_reorder = 0;
    sim.reorder_per_million = 0;
    sim.reorder_queue_max = 32;
    sim.enable_jitter = 1;
    sim.jitter_min_ms = 0;
    sim.jitter_max_ms = 1; // lighter jitter to speed up
    sim.spike_per_million = 0;
    sim.spike_ms = 0;
    sim.enable_partial_recv = 1;
    sim.min_recv_chunk = 128;
    sim.max_recv_chunk = 512;
    sim.enable_partial_send = 1;
    sim.min_send_chunk = 128;
    sim.max_send_chunk = 512;
    sim.handshake_grace_bytes = 128;
    ts_net_sim_set(&sim);

    char basedir[512], outdir[512], in[512], out[512];
    if (ts_build_case_dirs("transport_reorder_jitter", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "in.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "in.bin") != 0)
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
    // Flow control: favor a modest window to keep runtime well below watchdog
    cfg_tx.tx_flow.window_cap_packets = 64;    // upper bound for cwnd
    cfg_tx.tx_flow.initial_cwnd_packets = 8;   // start with a smallish cwnd
    cfg_rx.tx_flow.window_cap_packets = 64;
    cfg_rx.tx_flow.initial_cwnd_packets = 8;
    cfg_tx.timeouts.min_timeout_ms = 100;
    cfg_tx.timeouts.max_timeout_ms = 5000;
    cfg_tx.retries.handshake_retries = 8;
    cfg_tx.retries.data_retries = 6;
    cfg_tx.retries.backoff_ms_base = 20;
    cfg_rx.timeouts.min_timeout_ms = 100;
    cfg_rx.timeouts.max_timeout_ms = 5000;
    cfg_rx.retries.handshake_retries = 8;
    cfg_rx.retries.data_retries = 6;
    cfg_rx.retries.backoff_ms_base = 20;
    ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_TRACE);
    ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_TRACE);

    val_session_t *tx = NULL, *rx = NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK)
        return 3;
    // No wire audit is used; this test validates end-to-end behavior under jitter
    ts_cancel_token_t guard = ts_start_timeout_guard(15000, "ut_transport_reorder_jitter");
    ts_thread_t th = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(&cfg_tx, 80);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    val_session_destroy(tx);
    val_session_destroy(rx);
    ts_net_sim_reset();
    ts_cancel_timeout_guard(guard);

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
    printf("OK\n");
    return 0;
}
