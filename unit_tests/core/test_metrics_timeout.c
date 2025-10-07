#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !VAL_ENABLE_METRICS
int main(void)
{
    printf("metrics disabled\n");
    return 0;
}
#else

// Wrap receiver's send to DROP the first DATA_ACK to force a sender timeout/retransmit in stop-and-wait mode, then behave normally.
static int send_delay_first_ack(void *ctx, const void *data, size_t len)
{
    static int drop_once = 1; // drop exactly one DATA_ACK
    test_duplex_t *d = (test_duplex_t *)ctx;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return -1;
    memcpy(tmp, src, len);
    // DATA_ACK type is 6 (first byte of header)
    if (drop_once > 0 && len >= 1 && tmp[0] == 6)
    {
        // Drop this ACK; in stop-and-wait this guarantees sender times out and retransmits the same DATA
        drop_once--;
        free(tmp);
        return (int)len;
    }
    // Receiver send -> push into a2b of its duplex end (which points to global d.b2a)
    test_fifo_push(d->a2b, tmp, len);
    free(tmp);
    return (int)len;
}

int main(void)
{
    const size_t packet = 1024, depth = 16;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Prepare artifacts and a medium file
    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
        return 1;
    char outdir[2048];
    ts_str_copy(outdir, sizeof(outdir), artroot);
#if defined(_WIN32)
    ts_str_append(outdir, sizeof(outdir), "\\mto\\out");
#else
    ts_str_append(outdir, sizeof(outdir), "/mto/out");
#endif
    char in[2048];
    ts_str_copy(in, sizeof(in), artroot);
#if defined(_WIN32)
    ts_str_append(in, sizeof(in), "\\mto\\f.bin");
#else
    ts_str_append(in, sizeof(in), "/mto/f.bin");
#endif
    if (ts_ensure_dir(outdir) != 0)
        return 1;
    // Remove previous output file if present to force data path
    ts_remove_file(in);
    FILE *f = fopen(in, "wb");
    if (!f)
        return 2;
    const size_t size = 64 * 1024 + 17;
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);

    // Buffers and configs
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d; // sender end (a2b outbound)
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 1024);
    cfg_tx.system.get_ticks_ms = ts_ticks;
    cfg_tx.system.delay_ms = ts_delay;
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 1024);
    cfg_rx.system.get_ticks_ms = ts_ticks;
    cfg_rx.system.delay_ms = ts_delay;
    // Verbose logs to watch retransmit/timeout logic closely
    ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_TRACE);
    ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_TRACE);

    // Use stop-and-wait to make a single ACK drop cause a retransmission deterministically
    cfg_tx.adaptive_tx.max_performance_mode = VAL_TX_STOP_AND_WAIT;
    cfg_tx.adaptive_tx.preferred_initial_mode = VAL_TX_STOP_AND_WAIT;
    cfg_tx.adaptive_tx.allow_streaming = 0;
    cfg_rx.adaptive_tx.max_performance_mode = VAL_TX_STOP_AND_WAIT;
    cfg_rx.adaptive_tx.preferred_initial_mode = VAL_TX_STOP_AND_WAIT;
    cfg_rx.adaptive_tx.allow_streaming = 0;
    // Timeouts short to keep test fast
    cfg_tx.timeouts.min_timeout_ms = 50;
    cfg_tx.timeouts.max_timeout_ms = 200;
    cfg_rx.timeouts.min_timeout_ms = 50;
    cfg_rx.timeouts.max_timeout_ms = 200;
    // Keep other defaults from ts_make_config(); our ACK drops will now reliably trigger a timeout + retransmit
    // Install delayed ACK sender on RX side
    cfg_rx.transport.send = send_delay_first_ack;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    if (val_session_create(&cfg_tx, &tx, &dtx) != VAL_OK || val_session_create(&cfg_rx, &rx, &drx) != VAL_OK)
        return 3;

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "transfer failed st=%d\n", (int)st);
        return 4;
    }

    val_metrics_t mtx = {0};
    if (val_get_metrics(tx, &mtx) != VAL_OK)
        return 5;
    if (mtx.timeouts == 0 || mtx.retransmits == 0)
    {
        fprintf(stderr, "expected timeouts>0 and retransmits>0; got to=%u rt=%u\n", mtx.timeouts, mtx.retransmits);
        fprintf(stderr, "TX metrics: sent=%llu recv=%llu bytes_sent=%llu bytes_recv=%llu timeouts=%u retrans=%u crc=%u files_sent=%u handshakes=%u rtt_samples=%u\n",
                (unsigned long long)mtx.packets_sent, (unsigned long long)mtx.packets_recv,
                (unsigned long long)mtx.bytes_sent, (unsigned long long)mtx.bytes_recv, mtx.timeouts, mtx.retransmits,
                mtx.crc_errors, mtx.files_sent, mtx.handshakes, mtx.rtt_samples);
        return 6;
    }

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
#endif
