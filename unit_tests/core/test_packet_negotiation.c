#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Peek into session internals to validate negotiated effective_packet_size
#include "../../src/val_internal.h"

static int write_pattern_file(const char *path, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);
    return 0;
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb)
    {
        if (fa)
            fclose(fa);
        if (fb)
            fclose(fb);
        return 0;
    }
    int eq = 1;
    int ca, cb;
    do
    {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb)
        {
            eq = 0;
            break;
        }
    } while (ca != EOF && cb != EOF);
    fclose(fa);
    fclose(fb);
    return eq;
}

static int run_case(size_t sender_pkt, size_t receiver_pkt)
{
    const size_t file_size = 128 * 1024 + 57;
    const size_t depth = 16;
    // Duplex capacity sized to the larger proposed packet
    size_t maxp = sender_pkt > receiver_pkt ? sender_pkt : receiver_pkt;
    test_duplex_t d;
    test_duplex_init(&d, maxp, depth);

    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "artifacts root failed\n");
        return 1;
    }
#if defined(_WIN32)
    char basedir[1024];
    snprintf(basedir, sizeof(basedir), "%s\\pktneg_%zu_%zu", artroot, (size_t)sender_pkt, (size_t)receiver_pkt);
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s\\out", basedir);
    char inpath[1024];
    snprintf(inpath, sizeof(inpath), "%s\\in.bin", basedir);
    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s\\in.bin", outdir);
#else
    char basedir[1024];
    snprintf(basedir, sizeof(basedir), "%s/pktneg_%zu_%zu", artroot, (size_t)sender_pkt, (size_t)receiver_pkt);
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s/out", basedir);
    char inpath[1024];
    snprintf(inpath, sizeof(inpath), "%s/in.bin", basedir);
    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s/in.bin", outdir);
#endif
    if (ts_ensure_dir(basedir) != 0 || ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "mkdirs failed\n");
        return 1;
    }
    if (write_pattern_file(inpath, file_size) != 0)
    {
        fprintf(stderr, "write input failed\n");
        return 1;
    }

    // Allocate buffers sized per-endpoint configured packet
    uint8_t *sb_tx = (uint8_t *)calloc(1, sender_pkt);
    uint8_t *rb_tx = (uint8_t *)calloc(1, sender_pkt);
    uint8_t *sb_rx = (uint8_t *)calloc(1, receiver_pkt);
    uint8_t *rb_rx = (uint8_t *)calloc(1, receiver_pkt);

    val_config_t cfg_tx, cfg_rx;
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, sb_tx, rb_tx, sender_pkt, &end_tx, VAL_RESUME_APPEND, 1024);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, receiver_pkt, &end_rx, VAL_RESUME_APPEND, 1024);

    val_session_t *tx = val_session_create(&cfg_tx);
    val_session_t *rx = val_session_create(&cfg_rx);
    if (!tx || !rx)
    {
        fprintf(stderr, "session create failed\n");
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);

    // Check transfer result
    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 1;
    }
    if (!files_equal(inpath, outpath))
    {
        fprintf(stderr, "file mismatch under pkt neg case %zu/%zu\n", (size_t)sender_pkt, (size_t)receiver_pkt);
        return 1;
    }

    // Validate negotiated effective packet size on both sessions equals min(sender_pkt, receiver_pkt)
    size_t expected = sender_pkt < receiver_pkt ? sender_pkt : receiver_pkt;
    size_t eff_tx = tx->effective_packet_size;
    size_t eff_rx = rx->effective_packet_size;
    if (eff_tx != expected || eff_rx != expected)
    {
        fprintf(stderr, "negotiation mismatch: expected %zu got tx=%zu rx=%zu\n", expected, eff_tx, eff_rx);
        return 1;
    }

    // Now perform reverse-direction transfer (previous receiver becomes sender)
#if defined(_WIN32)
    char outdir2[1024];
    snprintf(outdir2, sizeof(outdir2), "%s\\out2", basedir);
    char inpath2[1024];
    snprintf(inpath2, sizeof(inpath2), "%s\\in2.bin", basedir);
    char outpath2[1024];
    snprintf(outpath2, sizeof(outpath2), "%s\\in2.bin", outdir2);
#else
    char outdir2[1024];
    snprintf(outdir2, sizeof(outdir2), "%s/out2", basedir);
    char inpath2[1024];
    snprintf(inpath2, sizeof(inpath2), "%s/in2.bin", basedir);
    char outpath2[1024];
    snprintf(outpath2, sizeof(outpath2), "%s/in2.bin", outdir2);
#endif
    if (ts_ensure_dir(outdir2) != 0)
    {
        fprintf(stderr, "mkdir outdir2 failed\n");
        return 1;
    }
    if (write_pattern_file(inpath2, file_size + 11) != 0)
    {
        fprintf(stderr, "write input2 failed\n");
        return 1;
    }

    // New sessions with swapped roles: new sender uses receiver_pkt, new receiver uses sender_pkt
    uint8_t *sb_tx2 = (uint8_t *)calloc(1, receiver_pkt);
    uint8_t *rb_tx2 = (uint8_t *)calloc(1, receiver_pkt);
    uint8_t *sb_rx2 = (uint8_t *)calloc(1, sender_pkt);
    uint8_t *rb_rx2 = (uint8_t *)calloc(1, sender_pkt);

    val_config_t cfg_tx2, cfg_rx2;
    // For reverse direction, the endpoint that was previously RX (end_rx) becomes TX
    test_duplex_t end_tx2 = end_rx;
    test_duplex_t end_rx2 = end_tx;
    ts_make_config(&cfg_tx2, sb_tx2, rb_tx2, receiver_pkt, &end_tx2, VAL_RESUME_APPEND, 1024);
    ts_make_config(&cfg_rx2, sb_rx2, rb_rx2, sender_pkt, &end_rx2, VAL_RESUME_APPEND, 1024);
    val_session_t *tx2 = val_session_create(&cfg_tx2);
    val_session_t *rx2 = val_session_create(&cfg_rx2);
    if (!tx2 || !rx2)
    {
        fprintf(stderr, "session create 2 failed\n");
        return 1;
    }
    ts_thread_t th2 = ts_start_receiver(rx2, outdir2);
    const char *files2[1] = {inpath2};
    val_status_t st2 = val_send_files(tx2, files2, 1, NULL);
    ts_join_thread(th2);
    if (st2 != VAL_OK)
    {
        fprintf(stderr, "reverse send failed %d\n", (int)st2);
        return 1;
    }
    if (!files_equal(inpath2, outpath2))
    {
        fprintf(stderr, "reverse transfer mismatch\n");
        return 1;
    }
    // Validate negotiation again (min of original sizes)
    size_t eff_tx2 = tx2->effective_packet_size;
    size_t eff_rx2 = rx2->effective_packet_size;
    if (eff_tx2 != expected || eff_rx2 != expected)
    {
        fprintf(stderr, "reverse negotiation mismatch: expected %zu got tx=%zu rx=%zu\n", expected, eff_tx2, eff_rx2);
        return 1;
    }

    // Cleanup
    val_session_destroy(tx2);
    val_session_destroy(rx2);
    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_tx);
    free(rb_tx);
    free(sb_rx);
    free(rb_rx);
    free(sb_tx2);
    free(rb_tx2);
    free(sb_rx2);
    free(rb_rx2);
    test_duplex_free(&d);
    return 0;
}

int main(void)
{
    int rc = 0;
    // Case 1: sender smaller than receiver
    if (run_case(1024, 4096) != 0)
        rc = 1;
    // Case 2: receiver smaller than sender
    if (run_case(8192, 1536) != 0)
        rc = 1;
    if (rc == 0)
        printf("OK\n");
    return rc;
}
