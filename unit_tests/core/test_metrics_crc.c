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

// Custom send that corrupts the trailer CRC on the first two DATA packets to ensure at least one
// CRC mismatch is observed on RX across platforms/timing. Do NOT corrupt the header to allow
// retransmissions to recover and the transfer to complete on some runs.
static int send_corrupt_once(void *ctx, const void *data, size_t len)
{
    static int corrupt_left = 2;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return -1;
    memcpy(tmp, src, len);
    // Wire header layout: first byte is packet 'type'. DATA packets use value 5 in current protocol.
    // Corrupt only the first DATA packet to avoid breaking handshake/control phases.
    if (corrupt_left > 0 && len >= 1 && tmp[0] == 5 /* VAL_PKT_DATA */)
    {
        // Flip the last byte (part of trailer CRC) to force a trailer CRC mismatch
        tmp[len - 1] ^= 0xFF;
        corrupt_left--;
    }
    // Send through the standard test transport to preserve framing semantics
    int r = test_tp_send(ctx, tmp, len);
    free(tmp);
    return r;
}

int main(void)
{
    const size_t packet = 1024, depth = 16;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Prepare artifacts
    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
        return 1;
    char outdir[2048];
    ts_str_copy(outdir, sizeof(outdir), artroot);
#if defined(_WIN32)
    ts_str_append(outdir, sizeof(outdir), "\\mcrc\\out");
#else
    ts_str_append(outdir, sizeof(outdir), "/mcrc/out");
#endif
    char in[2048];
    ts_str_copy(in, sizeof(in), artroot);
#if defined(_WIN32)
    ts_str_append(in, sizeof(in), "\\mcrc\\f.bin");
#else
    ts_str_append(in, sizeof(in), "/mcrc/f.bin");
#endif
    char out[2048];
    ts_str_copy(out, sizeof(out), outdir);
#if defined(_WIN32)
    ts_str_append(out, sizeof(out), "\\f.bin");
#else
    ts_str_append(out, sizeof(out), "/f.bin");
#endif
    if (ts_ensure_dir(outdir) != 0)
        return 1;
    // Ensure previous run artifacts don't exist; otherwise resume may skip sending DATA entirely
    ts_remove_file(out);

    // Write small file spanning a few packets
    FILE *f = fopen(in, "wb");
    if (!f)
        return 2;
    const size_t size = 8 * 1024 + 33;
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);

    // Buffers and configs
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d; // normal duplex object
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 1024);
    // Enable verbose tracing to observe CRC handling paths
    ts_set_console_logger_with_level(&cfg_tx, VAL_LOG_TRACE);
    ts_set_console_logger_with_level(&cfg_rx, VAL_LOG_TRACE);
    // Install custom corrupting send on TX side only
    cfg_tx.transport.send = send_corrupt_once;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    if (val_session_create(&cfg_tx, &tx, &dtx) != VAL_OK || val_session_create(&cfg_rx, &rx, &drx) != VAL_OK)
        return 3;

    // Run transfer
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    // Allow timeouts under aggressive corruption, we still expect crc_errors to be >0
    if (st != VAL_OK && st != VAL_ERR_TIMEOUT && st != VAL_ERR_CRC)
    {
        fprintf(stderr, "transfer ended with unexpected status st=%d\n", (int)st);
        // continue to metrics check to aid debugging
    }

    // CRC errors should have been observed by RX
    val_metrics_t mtx = {0}, mrx = {0};
    if (val_get_metrics(tx, &mtx) != VAL_OK || val_get_metrics(rx, &mrx) != VAL_OK)
        return 5;
    if (mrx.crc_errors == 0)
    {
        fprintf(stderr, "expected crc_errors>0 on RX; got 0\n");
        fprintf(stderr, "TX metrics: sent=%llu recv=%llu bytes_sent=%llu bytes_recv=%llu timeouts=%u retrans=%u crc=%u files_sent=%u handshakes=%u rtt_samples=%u\n",
                (unsigned long long)mtx.packets_sent, (unsigned long long)mtx.packets_recv,
                (unsigned long long)mtx.bytes_sent, (unsigned long long)mtx.bytes_recv, mtx.timeouts, mtx.retransmits,
                mtx.crc_errors, mtx.files_sent, mtx.handshakes, mtx.rtt_samples);
        fprintf(stderr, "RX metrics: sent=%llu recv=%llu bytes_sent=%llu bytes_recv=%llu timeouts=%u retrans=%u crc=%u files_recv=%u handshakes=%u rtt_samples=%u\n",
                (unsigned long long)mrx.packets_sent, (unsigned long long)mrx.packets_recv,
                (unsigned long long)mrx.bytes_sent, (unsigned long long)mrx.bytes_recv, mrx.timeouts, mrx.retransmits,
                mrx.crc_errors, mrx.files_recv, mrx.handshakes, mrx.rtt_samples);
        return 6;
    }
    if (st == VAL_OK && (mtx.files_sent != 1 || mrx.files_recv != 1))
    {
        fprintf(stderr, "files counters wrong: tx=%u rx=%u\n", mtx.files_sent, mrx.files_recv);
        return 7;
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
