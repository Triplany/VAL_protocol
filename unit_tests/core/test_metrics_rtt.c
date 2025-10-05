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
int main(void)
{
    const size_t packet = 1024, depth = 16;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Prepare a smallish file
    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
        return 1;
    char outdir[2048];
    ts_str_copy(outdir, sizeof(outdir), artroot);
#if defined(_WIN32)
    ts_str_append(outdir, sizeof(outdir), "\\mrtt\\out");
#else
    ts_str_append(outdir, sizeof(outdir), "/mrtt/out");
#endif
    char in[2048];
    ts_str_copy(in, sizeof(in), artroot);
#if defined(_WIN32)
    ts_str_append(in, sizeof(in), "\\mrtt\\f.bin");
#else
    ts_str_append(in, sizeof(in), "/mrtt/f.bin");
#endif
    if (ts_ensure_dir(outdir) != 0)
        return 1;
    FILE *f = fopen(in, "wb");
    if (!f)
        return 2;
    const size_t size = 32 * 1024 + 5; // multiple packets to ensure multiple ACKs
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);

    // Setup sessions
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 1024);

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
    if (mtx.rtt_samples == 0)
    {
        fprintf(stderr, "expected rtt_samples > 0; got %u\n", mtx.rtt_samples);
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
