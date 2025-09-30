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

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, size, f);
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
    const size_t packet = 2048, depth = 32;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "failed to determine artifacts root\n");
        return 1;
    }
#if defined(_WIN32)
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s\\multi\\out", artroot);
    char in1[1024];
    snprintf(in1, sizeof(in1), "%s\\multi\\a.bin", artroot);
    char in2[1024];
    snprintf(in2, sizeof(in2), "%s\\multi\\b.bin", artroot);
#else
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s/multi/out", artroot);
    char in1[1024];
    snprintf(in1, sizeof(in1), "%s/multi/a.bin", artroot);
    char in2[1024];
    snprintf(in2, sizeof(in2), "%s/multi/b.bin", artroot);
#endif
    if (ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "failed to create artifacts dir\n");
        return 1;
    }

    // Two different sizes to cross packet boundaries (overridable via env)
    const size_t s1 = ts_env_size_bytes("VAL_TEST_MULTI_A_SIZE", 300 * 1024 + 17);
    const size_t s2 = ts_env_size_bytes("VAL_TEST_MULTI_B_SIZE", 123 * 1024 + 9);
    uint8_t *buf1 = (uint8_t *)malloc(s1);
    for (size_t i = 0; i < s1; ++i)
        buf1[i] = (uint8_t)(i * 7);
    uint8_t *buf2 = (uint8_t *)malloc(s2);
    for (size_t i = 0; i < s2; ++i)
        buf2[i] = (uint8_t)(255 - (i * 3));
    write_file(in1, buf1, s1);
    write_file(in2, buf2, s2);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet);
    uint8_t *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet);
    uint8_t *rb_b = (uint8_t *)calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_APPEND, 2048);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_APPEND, 2048);

    val_session_t *tx = val_session_create(&cfg_tx);
    val_session_t *rx = val_session_create(&cfg_rx);
    if (!tx || !rx)
    {
        fprintf(stderr, "session create failed\n");
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);

    const char *files[2] = {in1, in2};
    val_status_t st = val_send_files(tx, files, 2, NULL);

    ts_join_thread(th);

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    free(buf1);
    free(buf2);

    // Check outputs
    char out1[1024];
    char out2[1024];
#if defined(_WIN32)
    snprintf(out1, sizeof(out1), "%s\\multi\\out\\a.bin", artroot);
    snprintf(out2, sizeof(out2), "%s\\multi\\out\\b.bin", artroot);
#else
    snprintf(out1, sizeof(out1), "%s/multi/out/a.bin", artroot);
    snprintf(out2, sizeof(out2), "%s/multi/out/b.bin", artroot);
#endif
    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 2;
    }
    if (!files_equal(in1, out1) || !files_equal(in2, out2))
    {
        fprintf(stderr, "output mismatch\n");
        return 3;
    }
    // Verify sizes
    uint64_t sz1_in = ts_file_size(in1), sz1_out = ts_file_size(out1);
    uint64_t sz2_in = ts_file_size(in2), sz2_out = ts_file_size(out2);
    if (sz1_in != sz1_out || sz2_in != sz2_out)
    {
        fprintf(stderr, "size mismatch: a in=%llu out=%llu; b in=%llu out=%llu\n", (unsigned long long)sz1_in,
                (unsigned long long)sz1_out, (unsigned long long)sz2_in, (unsigned long long)sz2_out);
        return 4;
    }
    // Verify CRCs
    uint32_t c1_in = ts_file_crc32(in1), c1_out = ts_file_crc32(out1);
    uint32_t c2_in = ts_file_crc32(in2), c2_out = ts_file_crc32(out2);
    if (c1_in != c1_out || c2_in != c2_out)
    {
        fprintf(stderr, "crc mismatch: a in=%08x out=%08x; b in=%08x out=%08x\n", (unsigned)c1_in, (unsigned)c1_out,
                (unsigned)c2_in, (unsigned)c2_out);
        return 5;
    }
    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
