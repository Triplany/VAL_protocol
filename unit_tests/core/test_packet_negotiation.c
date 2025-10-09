#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>

static char *dyn_sprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        va_end(ap2);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        va_end(ap2);
        return NULL;
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

static char *join_path2(const char *a, const char *b)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != sep) ? 1 : 0;
    if (lb > 0 && b[0] == sep)
        need_sep = 0;
    size_t len = la + (size_t)need_sep + lb;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, a, la);
    size_t pos = la;
    if (need_sep)
        out[pos++] = sep;
    memcpy(out + pos, b, lb);
    out[len] = '\0';
    return out;
}

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

    char artroot[2048];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "artifacts root failed\n");
        return 1;
    }
    char *case_seg = dyn_sprintf("pktneg_%zu_%zu", (size_t)sender_pkt, (size_t)receiver_pkt);
    char *basedir = case_seg ? join_path2(artroot, case_seg) : NULL;
    free(case_seg);
    char *outdir = basedir ? join_path2(basedir, "out") : NULL;
    char *inpath = basedir ? join_path2(basedir, "in.bin") : NULL;
    char *outpath = outdir ? join_path2(outdir, "in.bin") : NULL;
    if (!basedir || !outdir || !inpath || !outpath)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        fprintf(stderr, "path alloc failed\n");
        return 1;
    }
    if (ts_ensure_dir(basedir) != 0 || ts_ensure_dir(outdir) != 0)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        fprintf(stderr, "mkdirs failed\n");
        return 1;
    }
    if (write_pattern_file(inpath, file_size) != 0)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
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
    ts_make_config(&cfg_tx, sb_tx, rb_tx, sender_pkt, &end_tx, VAL_RESUME_NEVER, 1024);
    ts_make_config(&cfg_rx, sb_rx, rb_rx, receiver_pkt, &end_rx, VAL_RESUME_NEVER, 1024);

    val_session_t *tx = NULL;
    val_session_t *rx = NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
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
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    // Enforce clean metrics on nominal run
#if VAL_ENABLE_METRICS
    {
        ts_metrics_expect_t exp = {0};
        exp.allow_soft_timeouts = 0;
        exp.expect_files_sent = 1;
        exp.expect_files_recv = 1;
        if (ts_assert_clean_metrics(tx, rx, &exp) != 0)
        {
            free(basedir); free(outdir); free(inpath); free(outpath);
            return 1;
        }
    }
#endif
    if (!files_equal(inpath, outpath))
    {
        fprintf(stderr, "file mismatch under pkt neg case %zu/%zu\n", (size_t)sender_pkt, (size_t)receiver_pkt);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }

    // Validate negotiated effective packet size on both sessions equals min(sender_pkt, receiver_pkt)
    size_t expected = sender_pkt < receiver_pkt ? sender_pkt : receiver_pkt;
    size_t eff_tx = 0, eff_rx = 0;
    if (val_get_effective_packet_size(tx, &eff_tx) != VAL_OK ||
        val_get_effective_packet_size(rx, &eff_rx) != VAL_OK)
    {
        fprintf(stderr, "failed to query effective packet size\n");
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    if (eff_tx != expected || eff_rx != expected)
    {
        fprintf(stderr, "negotiation mismatch: expected %zu got tx=%zu rx=%zu\n", expected, eff_tx, eff_rx);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }

    // Now perform reverse-direction transfer (previous receiver becomes sender)
    char *outdir2 = join_path2(basedir, "out2");
    char *inpath2 = join_path2(basedir, "in2.bin");
    char *outpath2 = outdir2 ? join_path2(outdir2, "in2.bin") : NULL;
    if (!outdir2 || !inpath2 || !outpath2)
    {
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        fprintf(stderr, "path2 alloc failed\n");
        return 1;
    }
    if (ts_ensure_dir(outdir2) != 0)
    {
        fprintf(stderr, "mkdir outdir2 failed\n");
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    if (write_pattern_file(inpath2, file_size + 11) != 0)
    {
        fprintf(stderr, "write input2 failed\n");
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
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
    ts_make_config(&cfg_tx2, sb_tx2, rb_tx2, receiver_pkt, &end_tx2, VAL_RESUME_NEVER, 1024);
    ts_make_config(&cfg_rx2, sb_rx2, rb_rx2, sender_pkt, &end_rx2, VAL_RESUME_NEVER, 1024);
    val_session_t *tx2 = NULL;
    val_session_t *rx2 = NULL;
    if (val_session_create(&cfg_tx2, &tx2, NULL) != VAL_OK || val_session_create(&cfg_rx2, &rx2, NULL) != VAL_OK)
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
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    // Enforce clean metrics on reverse nominal run
#if VAL_ENABLE_METRICS
    {
        ts_metrics_expect_t exp = {0};
        exp.allow_soft_timeouts = 0;
        exp.expect_files_sent = 1;
        exp.expect_files_recv = 1;
        if (ts_assert_clean_metrics(tx2, rx2, &exp) != 0)
        {
            free(outdir2); free(inpath2); free(outpath2);
            free(basedir); free(outdir); free(inpath); free(outpath);
            return 1;
        }
    }
#endif
    if (!files_equal(inpath2, outpath2))
    {
        fprintf(stderr, "reverse transfer mismatch\n");
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    // Validate negotiation again (min of original sizes)
    size_t eff_tx2 = 0, eff_rx2 = 0;
    if (val_get_effective_packet_size(tx2, &eff_tx2) != VAL_OK ||
        val_get_effective_packet_size(rx2, &eff_rx2) != VAL_OK)
    {
        fprintf(stderr, "failed to query effective packet size (2)\n");
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    if (eff_tx2 != expected || eff_rx2 != expected)
    {
        fprintf(stderr, "reverse negotiation mismatch: expected %zu got tx=%zu rx=%zu\n", expected, eff_tx2, eff_rx2);
        free(outdir2);
        free(inpath2);
        free(outpath2);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
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
    free(basedir);
    free(outdir);
    free(inpath);
    free(outpath);
    free(outdir2);
    free(inpath2);
    free(outpath2);
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
