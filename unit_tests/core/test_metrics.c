#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
#if !VAL_ENABLE_METRICS
    // When metrics are disabled at compile time, this test is a no-op success.
    printf("metrics disabled\n");
    return 0;
#else
    const size_t packet = 1024, depth = 8;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Prepare tiny input
    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "failed artifacts root\n");
        return 1;
    }
    char outdir[2048];
    ts_str_copy(outdir, sizeof(outdir), artroot);
#if defined(_WIN32)
    ts_str_append(outdir, sizeof(outdir), "\\metrics\\out");
#else
    ts_str_append(outdir, sizeof(outdir), "/metrics/out");
#endif
    char in[2048];
    ts_str_copy(in, sizeof(in), artroot);
#if defined(_WIN32)
    ts_str_append(in, sizeof(in), "\\metrics\\m.bin");
#else
    ts_str_append(in, sizeof(in), "/metrics/m.bin");
#endif
    if (ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "mkdir fail\n");
        return 1;
    }
    // Write small file (spans multiple packets to exercise counters)
    FILE *f = fopen(in, "wb");
    if (!f)
        return 2;
    const size_t size = 10 * 1024 + 7;
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);

    // Sessions
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
    {
        fprintf(stderr, "session create failed\n");
        return 3;
    }

    // Immediately after create, metrics must be zero
    val_metrics_t mt = {0}, mr = {0};
    if (val_get_metrics(tx, &mt) != VAL_OK || val_get_metrics(rx, &mr) != VAL_OK)
    {
        fprintf(stderr, "get metrics failed\n");
        return 4;
    }
    if (mt.packets_sent || mt.bytes_sent || mt.files_sent || mr.packets_recv || mr.bytes_recv || mr.files_recv)
    {
        fprintf(stderr, "metrics not zero after create\n");
        return 5;
    }

    // Transfer
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 6;
    }

    // Validate counters populated
    if (val_get_metrics(tx, &mt) != VAL_OK || val_get_metrics(rx, &mr) != VAL_OK)
    {
        fprintf(stderr, "get metrics post failed\n");
        return 7;
    }
    if (mt.files_sent != 1 || mr.files_recv != 1)
    {
        fprintf(stderr, "files counters wrong: tx=%u rx=%u\n", mt.files_sent, mr.files_recv);
        return 8;
    }
    if (mt.bytes_sent == 0 || mr.bytes_recv == 0)
    {
        fprintf(stderr, "bytes counters zero\n");
        return 9;
    }

    // Reset and ensure zero again
    if (val_reset_metrics(tx) != VAL_OK || val_reset_metrics(rx) != VAL_OK)
    {
        fprintf(stderr, "reset failed\n");
        return 10;
    }
    memset(&mt, 0, sizeof(mt));
    memset(&mr, 0, sizeof(mr));
    if (val_get_metrics(tx, &mt) != VAL_OK || val_get_metrics(rx, &mr) != VAL_OK)
    {
        fprintf(stderr, "get after reset failed\n");
        return 11;
    }
    if (mt.packets_sent || mt.bytes_sent || mt.files_sent || mr.packets_recv || mr.bytes_recv || mr.files_recv)
    {
        fprintf(stderr, "metrics not zero after reset\n");
        return 12;
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
#endif
}