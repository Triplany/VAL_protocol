#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static int write_pattern(const char *path, size_t size)
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

int main(void)
{
    const size_t packet = 1024, depth = 16;
    const size_t size = 512 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

#if defined(_WIN32)
    _mkdir(".\\ut_artifacts");
    _mkdir(".\\ut_artifacts\\resume");
    _mkdir(".\\ut_artifacts\\resume\\out");
    const char *outdir = ".\\ut_artifacts\\resume\\out";
    const char *in = ".\\ut_artifacts\\resume\\big.bin";
#else
    mkdir("./ut_artifacts", 0777);
    mkdir("./ut_artifacts/resume", 0777);
    mkdir("./ut_artifacts/resume/out", 0777);
    const char *outdir = "./ut_artifacts/resume/out";
    const char *in = "./ut_artifacts/resume/big.bin";
#endif
    write_pattern(in, size);

    // Pre-create a partial output file to simulate an interrupted previous transfer
#if defined(_WIN32)
    const char *out = ".\\ut_artifacts\\resume\\out\\big.bin";
#else
    const char *out = "./ut_artifacts/resume/out/big.bin";
#endif
    {
        FILE *fi = fopen(in, "rb");
        FILE *fo = fopen(out, "wb");
        if (!fi || !fo)
        {
            fprintf(stderr, "precreate out failed\n");
            return 1;
        }
        size_t half = size / 2;
        const size_t chunk = 8192;
        uint8_t *tmp = (uint8_t *)malloc(chunk);
        size_t to_copy = half;
        while (to_copy)
        {
            size_t n = to_copy > chunk ? chunk : to_copy;
            size_t r = fread(tmp, 1, n, fi);
            if (r == 0)
                break;
            fwrite(tmp, 1, r, fo);
            to_copy -= r;
        }
        free(tmp);
        fclose(fi);
        fclose(fo);
    }

    // Setup sessions with CRC_VERIFY resume to validate trailing window
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_CRC_VERIFY, 8192);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_CRC_VERIFY, 8192);
    ts_set_console_logger(&cfg_tx);
    ts_set_console_logger(&cfg_rx);
    val_session_t *tx = val_session_create(&cfg_tx);
    val_session_t *rx = val_session_create(&cfg_rx);
    if (!tx || !rx)
    {
        fprintf(stderr, "session create failed\n");
        return 1;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);

    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 2;
    }
    if (!files_equal(in, out))
    {
        fprintf(stderr, "resume output mismatch\n");
        return 3;
    }
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    if (sz_in != sz_out)
    {
        fprintf(stderr, "resume size mismatch: in=%llu out=%llu\n", (unsigned long long)sz_in, (unsigned long long)sz_out);
        return 4;
    }
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    if (crc_in != crc_out)
    {
        fprintf(stderr, "resume crc mismatch: in=%08x out=%08x\n", (unsigned)crc_in, (unsigned)crc_out);
        return 5;
    }
    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
