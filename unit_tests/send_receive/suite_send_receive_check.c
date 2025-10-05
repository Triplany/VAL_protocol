#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

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
static int test_single_file_check(void)
{
    const size_t packet = 1024, depth = 16, size = 256 * 1024 + 123;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
#if defined(_WIN32)
    _mkdir(".\\ut_artifacts");
    _mkdir(".\\ut_artifacts\\single_ck");
    _mkdir(".\\ut_artifacts\\single_ck\\out");
    const char *outdir = ".\\ut_artifacts\\single_ck\\out";
    const char *inpath = ".\\ut_artifacts\\single_ck\\input.bin";
    const char *outpath = ".\\ut_artifacts\\single_ck\\out\\input.bin";
#else
    mkdir("./ut_artifacts", 0777);
    mkdir("./ut_artifacts/single_ck", 0777);
    mkdir("./ut_artifacts/single_ck/out", 0777);
    const char *outdir = "./ut_artifacts/single_ck/out";
    const char *inpath = "./ut_artifacts/single_ck/input.bin";
    const char *outpath = "./ut_artifacts/single_ck/out/input.bin";
#endif
    // write input
    {
        FILE *f = fopen(inpath, "wb");
        if (!f)
        {
            fprintf(stderr, "fopen failed for %s\n", inpath);
            return 1;
        }
        for (size_t i = 0; i < size; ++i)
            fputc((int)(i & 0xFF), f);
        fclose(f);
    }

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 1024);
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 2;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "val_send_files failed: %d\n", st);
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }
    if (!files_equal(inpath, outpath))
    {
        fprintf(stderr, "file contents differ for %s vs %s\n", inpath, outpath);
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }
    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 0;
}

static int test_multi_file_check(void)
{
    const size_t packet = 2048, depth = 32;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
#if defined(_WIN32)
    _mkdir(".\\ut_artifacts");
    _mkdir(".\\ut_artifacts\\multi_ck");
    _mkdir(".\\ut_artifacts\\multi_ck\\out");
    const char *outdir = ".\\ut_artifacts\\multi_ck\\out";
    const char *in1 = ".\\ut_artifacts\\multi_ck\\a.bin";
    const char *in2 = ".\\ut_artifacts\\multi_ck\\b.bin";
    const char *out1 = ".\\ut_artifacts\\multi_ck\\out\\a.bin";
    const char *out2 = ".\\ut_artifacts\\multi_ck\\out\\b.bin";
#else
    mkdir("./ut_artifacts", 0777);
    mkdir("./ut_artifacts/multi_ck", 0777);
    mkdir("./ut_artifacts/multi_ck/out", 0777);
    const char *outdir = "./ut_artifacts/multi_ck/out";
    const char *in1 = "./ut_artifacts/multi_ck/a.bin";
    const char *in2 = "./ut_artifacts/multi_ck/b.bin";
    const char *out1 = "./ut_artifacts/multi_ck/out/a.bin";
    const char *out2 = "./ut_artifacts/multi_ck/out/b.bin";
#endif
    // write inputs
    {
        size_t n1 = 300 * 1024 + 17, n2 = 123 * 1024 + 9;
        FILE *f = fopen(in1, "wb");
        if (!f)
        {
            fprintf(stderr, "fopen failed for %s\n", in1);
            return 1;
        }
        for (size_t i = 0; i < n1; ++i)
            fputc((i * 7) & 0xFF, f);
        fclose(f);
        f = fopen(in2, "wb");
        if (!f)
        {
            fprintf(stderr, "fopen failed for %s\n", in2);
            return 1;
        }
        for (size_t i = 0; i < n2; ++i)
            fputc((255 - (i * 3)) & 0xFF, f);
        fclose(f);
    }
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 2048);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 2048);
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 2;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[2] = {in1, in2};
    val_status_t st = val_send_files(tx, files, 2, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "val_send_files failed: %d\n", st);
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }
    if (!files_equal(in1, out1))
    {
        fprintf(stderr, "file a mismatch\n");
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }
    if (!files_equal(in2, out2))
    {
        fprintf(stderr, "file b mismatch\n");
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        return 1;
    }
    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return 0;
}

static int test_packet_size_sweep(void)
{
    /* Sweep a set of packet sizes including MIN and larger sizes to validate resume/verify streaming CRC. */
    const size_t sizes[] = {512, 1024, 2048, 4096, 8192};
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si)
    {
        size_t packet = sizes[si];
        size_t depth = 8;
        size_t size = 64 * 1024 + 13; // moderate file size
        test_duplex_t d;
        test_duplex_init(&d, packet, depth);
#if defined(_WIN32)
    char basepath[512];
    _mkdir(".\\ut_artifacts");
    snprintf(basepath, sizeof(basepath), ".\\ut_artifacts\\sweep_%zu", packet);
    _mkdir(basepath);
    _mkdir(".\\ut_artifacts\\sweep_out");
    const char *outdir = ".\\ut_artifacts\\sweep_out";
    char inpath[512];
    char outpath[512];
    ts_str_copy(inpath, sizeof(inpath), basepath);
    ts_str_append(inpath, sizeof(inpath), "\\input.bin");
    ts_str_copy(outpath, sizeof(outpath), outdir);
    ts_str_append(outpath, sizeof(outpath), "\\input.bin");
#else
    char basepath[512];
    mkdir("./ut_artifacts", 0777);
    snprintf(basepath, sizeof(basepath), "./ut_artifacts/sweep_%zu", packet);
    mkdir(basepath, 0777);
    mkdir("./ut_artifacts/sweep_out", 0777);
    const char *outdir = "./ut_artifacts/sweep_out";
    char inpath[512];
    char outpath[512];
    ts_str_copy(inpath, sizeof(inpath), basepath);
    ts_str_append(inpath, sizeof(inpath), "/input.bin");
    ts_str_copy(outpath, sizeof(outpath), outdir);
    ts_str_append(outpath, sizeof(outpath), "/input.bin");
#endif
        // write input
        {
            FILE *f = fopen(inpath, "wb");
            if (!f)
            {
                fprintf(stderr, "fopen failed for %s\n", inpath);
                return 1;
            }
            for (size_t i = 0; i < size; ++i)
                fputc((int)((i * 13) & 0xFF), f);
            fclose(f);
        }

        uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
        uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
        test_duplex_t end_tx = d;
        test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
        val_config_t cfg_tx, cfg_rx;
        /* Use a verify_bytes larger than a small packet to force streaming path in receiver */
        uint32_t verify = (uint32_t)(packet * 2);
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, verify);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, verify);
        val_session_t *tx = NULL, *rx = NULL;
        uint32_t dtx = 0, drx = 0;
        val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
        val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
        if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
        {
            fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                    (unsigned)drx);
            free(sb_a);
            free(rb_a);
            free(sb_b);
            free(rb_b);
            test_duplex_free(&d);
            return 2;
        }
        ts_thread_t th = ts_start_receiver(rx, outdir);
        const char *files[1] = {inpath};
        val_status_t st = val_send_files(tx, files, 1, NULL);
        ts_join_thread(th);
        if (st != VAL_OK)
        {
            fprintf(stderr, "val_send_files failed: %d\n", st);
            val_session_destroy(tx);
            val_session_destroy(rx);
            free(sb_a);
            free(rb_a);
            free(sb_b);
            free(rb_b);
            test_duplex_free(&d);
            return 1;
        }
        if (!files_equal(inpath, outpath))
        {
            fprintf(stderr, "file mismatch in packet sweep for packet %zu\n", packet);
            val_session_destroy(tx);
            val_session_destroy(rx);
            free(sb_a);
            free(rb_a);
            free(sb_b);
            free(rb_b);
            test_duplex_free(&d);
            return 1;
        }
        val_session_destroy(tx);
        val_session_destroy(rx);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
    }
    return 0;
}

int main(void)
{
    int rc = 0;
    if (test_single_file_check() != 0)
        rc = 1;
    if (test_multi_file_check() != 0)
        rc = 1;
    if (test_packet_size_sweep() != 0)
        rc = 1;
    return rc;
}