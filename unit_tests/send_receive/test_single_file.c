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

int main(void)
{
    const size_t packet = 1024; // ensure multiple packets
    const size_t depth = 16;
    const size_t file_size = ts_env_size_bytes("VAL_TEST_SINGLE_SIZE", 256 * 1024 + 123); // override with env if set

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Temp directories/files under the build's executable directory
    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "failed to determine artifacts root\n");
        return 1;
    }
#if defined(_WIN32)
    char tmpdir[2048];
    snprintf(tmpdir, sizeof(tmpdir), "%s\\single", artroot);
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s\\single\\out", artroot);
    char inpath[2048];
    snprintf(inpath, sizeof(inpath), "%s\\single\\input.bin", artroot);
    char outpath[2048];
    snprintf(outpath, sizeof(outpath), "%s\\single\\out\\input.bin", artroot);
#else
    char tmpdir[2048];
    snprintf(tmpdir, sizeof(tmpdir), "%s/single", artroot);
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s/single/out", artroot);
    char inpath[2048];
    snprintf(inpath, sizeof(inpath), "%s/single/input.bin", artroot);
    char outpath[2048];
    snprintf(outpath, sizeof(outpath), "%s/single/out/input.bin", artroot);
#endif
    if (ts_ensure_dir(tmpdir) != 0 || ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "failed to create artifacts dirs\n");
        return 1;
    }

    if (write_pattern_file(inpath, file_size) != 0)
    {
        fprintf(stderr, "failed to create input file\n");
        return 1;
    }

    uint8_t *send_a = (uint8_t *)calloc(1, packet);
    uint8_t *recv_a = (uint8_t *)calloc(1, packet);
    uint8_t *send_b = (uint8_t *)calloc(1, packet);
    uint8_t *recv_b = (uint8_t *)calloc(1, packet);

    val_config_t cfg_tx, cfg_rx;
    // TX endpoint sees a2b as its outbound queue; RX endpoint must see reversed direction
    test_duplex_t end_tx = d; // a2b -> outbound, b2a -> inbound
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, send_a, recv_a, packet, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
    ts_make_config(&cfg_rx, send_b, recv_b, packet, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);

    val_session_t *tx = NULL;
    val_session_t *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        return 2;
    }

    // Spawn receiver thread using helper
    ts_thread_t th = ts_start_receiver(rx, outdir);

    const char *files[1] = {inpath};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 3;
    }

    // Join receiver and basic metrics validation (if enabled)
    ts_join_thread(th);

#if VAL_ENABLE_METRICS
    {
        val_metrics_t mtx = {0}, mrx = {0};
        if (val_get_metrics(tx, &mtx) == VAL_OK && val_get_metrics(rx, &mrx) == VAL_OK)
        {
            // Expect exactly one file transferred
            if (mtx.files_sent != 1 || mrx.files_recv != 1)
            {
                fprintf(stderr, "metrics mismatch files: tx_sent=%u rx_recv=%u\n", mtx.files_sent, mrx.files_recv);
                return 8;
            }
            if (mtx.bytes_sent == 0 || mrx.bytes_recv == 0)
            {
                fprintf(stderr, "metrics bytes should be non-zero: tx_bytes=%llu rx_bytes=%llu\n",
                        (unsigned long long)mtx.bytes_sent, (unsigned long long)mrx.bytes_recv);
                return 9;
            }
            if (mtx.handshakes == 0 || mrx.handshakes == 0)
            {
                fprintf(stderr, "metrics handshakes should be >=1: tx=%u rx=%u\n", mtx.handshakes, mrx.handshakes);
                return 10;
            }
        }
        else
        {
            fprintf(stderr, "val_get_metrics failed\n");
            return 11;
        }
    }
#endif

    val_session_destroy(tx);
    val_session_destroy(rx);

    free(send_a);
    free(recv_a);
    free(send_b);
    free(recv_b);

    if (st != VAL_OK)
    {
        fprintf(stderr, "receive failed %d\n", (int)st);
        return 4;
    }

    if (!files_equal(inpath, outpath))
    {
        fprintf(stderr, "mismatch in output\n");
        return 5;
    }
    // Verify size and CRC equality
    uint64_t in_sz = ts_file_size(inpath);
    uint64_t out_sz = ts_file_size(outpath);
    if (in_sz != out_sz)
    {
        fprintf(stderr, "size mismatch: in=%llu out=%llu\n", (unsigned long long)in_sz, (unsigned long long)out_sz);
        return 6;
    }
    uint32_t in_crc = ts_file_crc32(inpath);
    uint32_t out_crc = ts_file_crc32(outpath);
    if (in_crc != out_crc)
    {
        fprintf(stderr, "crc mismatch: in=%08x out=%08x\n", (unsigned)in_crc, (unsigned)out_crc);
        return 7;
    }

    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
