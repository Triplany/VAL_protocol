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

static int test_resume_crc_verify_check(void)
{
    // Larger packet lowers per-packet overhead; slightly smaller file still exercises resume+verify.
    const size_t packet = 2048, depth = 16, size = 384 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
#if defined(_WIN32)
    _mkdir(".\\ut_artifacts");
    _mkdir(".\\ut_artifacts\\resume_ck");
    _mkdir(".\\ut_artifacts\\resume_ck\\out");
    const char *outdir = ".\\ut_artifacts\\resume_ck\\out";
    const char *in = ".\\ut_artifacts\\resume_ck\\big.bin";
    const char *out = ".\\ut_artifacts\\resume_ck\\out\\big.bin";
#else
    mkdir("./ut_artifacts", 0777);
    mkdir("./ut_artifacts/resume_ck", 0777);
    mkdir("./ut_artifacts/resume_ck/out", 0777);
    const char *outdir = "./ut_artifacts/resume_ck/out";
    const char *in = "./ut_artifacts/resume_ck/big.bin";
    const char *out = "./ut_artifacts/resume_ck/out/big.bin";
#endif
    // input
    {
        FILE *f = fopen(in, "wb");
        if (!f)
        {
            fprintf(stderr, "fopen failed for %s\n", in);
            return 1;
        }
        for (size_t i = 0; i < size; ++i)
            fputc((int)(i & 0xFF), f);
        fclose(f);
    }
    // precreate partial output
    {
        FILE *fi = fopen(in, "rb");
        FILE *fo = fopen(out, "wb");
        if (!fi || !fo)
        {
            if (fi)
                fclose(fi);
            if (fo)
                fclose(fo);
            fprintf(stderr, "fopen failed for precreate\n");
            return 1;
        }
        size_t half = size / 2;
        const size_t chunk = 8192;
        uint8_t *tmp = (uint8_t *)malloc(chunk);
        size_t to = half;
        while (to)
        {
            size_t n = to > chunk ? chunk : to;
            size_t r = fread(tmp, 1, n, fi);
            if (r == 0)
                break;
            fwrite(tmp, 1, r, fo);
            to -= r;
        }
        free(tmp);
        fclose(fi);
        fclose(fo);
    }
    // sessions with CRC_VERIFY
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 8192);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 8192);
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || !tx)
    {
        fprintf(stderr, "val_session_create tx failed rc=%d d=0x%08X\n", (int)rctx, (unsigned)dtx);
        return 1;
    }
    if (rcrx != VAL_OK || !rx)
    {
        fprintf(stderr, "val_session_create rx failed rc=%d d=0x%08X\n", (int)rcrx, (unsigned)drx);
        val_session_destroy(tx);
        return 1;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "val_send_files failed: %d\n", st);
        return 1;
    }
    if (!files_equal(in, out))
    {
        fprintf(stderr, "file mismatch after resume crc verify\n");
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

static int test_corruption_recovery_check(void)
{
    // Maintain corruption stress but reduce runtime via smaller file and same packet size.
    const size_t packet = 4096, depth = 64, size = 768 * 1024 + 333;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    d.faults.bitflip_per_million = 5;
    d.faults.drop_frame_per_million = 800;
    d.faults.dup_frame_per_million = 800;
#if defined(_WIN32)
    _mkdir(".\\ut_artifacts");
    _mkdir(".\\ut_artifacts\\corrupt_ck");
    _mkdir(".\\ut_artifacts\\corrupt_ck\\out");
    const char *outdir = ".\\ut_artifacts\\corrupt_ck\\out";
    const char *in = ".\\ut_artifacts\\corrupt_ck\\corrupt.bin";
    const char *out = ".\\ut_artifacts\\corrupt_ck\\out\\corrupt.bin";
#else
    mkdir("./ut_artifacts", 0777);
    mkdir("./ut_artifacts/corrupt_ck", 0777);
    mkdir("./ut_artifacts/corrupt_ck/out", 0777);
    const char *outdir = "./ut_artifacts/corrupt_ck/out";
    const char *in = "./ut_artifacts/corrupt_ck/corrupt.bin";
    const char *out = "./ut_artifacts/corrupt_ck/out/corrupt.bin";
#endif
    {
        FILE *f = fopen(in, "wb");
        if (!f)
        {
            fprintf(stderr, "fopen failed for %s\n", in);
            return 1;
        }
        for (size_t i = 0; i < size; ++i)
            fputc((int)((i * 13) & 0xFF), f);
        fclose(f);
    }
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet, .faults = d.faults};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 16384);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 16384);
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || !tx)
    {
        fprintf(stderr, "val_session_create tx failed rc=%d d=0x%08X\n", (int)rctx, (unsigned)dtx);
        return 1;
    }
    if (rcrx != VAL_OK || !rx)
    {
        fprintf(stderr, "val_session_create rx failed rc=%d d=0x%08X\n", (int)rcrx, (unsigned)drx);
        val_session_destroy(tx);
        return 1;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    if (st != VAL_OK)
    {
        fprintf(stderr, "val_send_files failed: %d\n", st);
        return 1;
    }
    if (!files_equal(in, out))
    {
        fprintf(stderr, "file mismatch in corruption recovery test\n");
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
int main(void)
{
    int rc = 0;
    if (test_resume_crc_verify_check() != 0)
        rc = 1;
    if (test_corruption_recovery_check() != 0)
        rc = 1;
    return rc;
}
