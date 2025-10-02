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

// Wrap receiver's send to DROP the first DATA_ACK to cause a sender timeout/retransmit, then behave normally.
static int send_delay_first_ack(void *ctx, const void *data, size_t len)
{
    static int delayed = 0;
    test_duplex_t *d = (test_duplex_t *)ctx;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return -1;
    memcpy(tmp, src, len);
    // DATA_ACK type is 6 (first byte of header)
    if (!delayed && len >= 1 && tmp[0] == 6)
    {
        // Drop this ACK: simulate one lost ACK frame
        delayed = 1;
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
#if defined(_WIN32)
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s\\mto\\out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s\\mto\\f.bin", artroot);
#else
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s/mto/out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s/mto/f.bin", artroot);
#endif
    if (ts_ensure_dir(outdir) != 0)
        return 1;
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
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);

    // Keep defaults from ts_make_config() which are moderate; a single ACK drop will trigger a timeout + retransmit
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
