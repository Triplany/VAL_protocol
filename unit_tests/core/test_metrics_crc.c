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

// Custom send that corrupts the trailer CRC on the first packet only, then behaves normally.
static int send_corrupt_once(void *ctx, const void *data, size_t len)
{
    static int corrupted = 0;
    test_duplex_t *d = (test_duplex_t *)ctx;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *tmp = (uint8_t *)malloc(len);
    if (!tmp)
        return -1;
    memcpy(tmp, src, len);
    // Wire header layout: first byte is packet 'type'. DATA packets use value 5 in current protocol.
    // Corrupt only the first DATA packet to avoid breaking handshake/control phases.
    if (!corrupted && len >= 1 && tmp[0] == 5 /* VAL_PKT_DATA */)
    {
        // Flip the last byte (part of trailer CRC) to force a trailer CRC mismatch at receiver
        tmp[len - 1] ^= 0xFF;
        corrupted = 1;
    }
    // Push into a2b (TX -> RX)
    test_fifo_push(d->a2b, tmp, len);
    free(tmp);
    return (int)len;
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
#if defined(_WIN32)
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s\\mcrc\\out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s\\mcrc\\f.bin", artroot);
    char out[2048];
    snprintf(out, sizeof(out), "%s\\mcrc\\out\\f.bin", artroot);
#else
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s/mcrc/out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s/mcrc/f.bin", artroot);
    char out[2048];
    snprintf(out, sizeof(out), "%s/mcrc/out/f.bin", artroot);
#endif
    if (ts_ensure_dir(outdir) != 0)
        return 1;

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
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
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
    if (st != VAL_OK)
    {
        fprintf(stderr, "transfer failed st=%d\n", (int)st);
        return 4;
    }

    // CRC errors should have been observed by RX
    val_metrics_t mtx = {0}, mrx = {0};
    if (val_get_metrics(tx, &mtx) != VAL_OK || val_get_metrics(rx, &mrx) != VAL_OK)
        return 5;
    if (mrx.crc_errors == 0)
    {
        fprintf(stderr, "expected crc_errors>0 on RX; got 0\n");
        return 6;
    }
    if (mtx.files_sent != 1 || mrx.files_recv != 1)
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
