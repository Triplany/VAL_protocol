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
        fputc((int)(i * 13 & 0xFF), f);
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
    const size_t packet = 4096, depth = 64;
    const size_t size = ts_env_size_bytes("VAL_TEST_CORRUPT_SIZE", 768 * 1024 + 333); // overridable
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    // Inject very low probability corruption and some drops/dups to exercise retransmission
    d.faults.bitflip_per_million = 5;      // 0.0005% bytes flipped
    d.faults.drop_frame_per_million = 800; // ~0.08% frames dropped
    d.faults.dup_frame_per_million = 800;  // ~0.08% frames duplicated

    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
    {
        fprintf(stderr, "failed to determine artifacts root\n");
        return 1;
    }
#if defined(_WIN32)
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s\\corrupt\\out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s\\corrupt\\corrupt.bin", artroot);
    char out[2048];
    snprintf(out, sizeof(out), "%s\\corrupt\\out\\corrupt.bin", artroot);
#else
    char outdir[2048];
    snprintf(outdir, sizeof(outdir), "%s/corrupt/out", artroot);
    char in[2048];
    snprintf(in, sizeof(in), "%s/corrupt/corrupt.bin", artroot);
    char out[2048];
    snprintf(out, sizeof(out), "%s/corrupt/out/corrupt.bin", artroot);
#endif
    if (ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "failed to create artifacts dir\n");
        return 1;
    }
    // Clean any previous input/output files to avoid stale resume behavior when size changes
    (void)remove(in);
    (void)remove(out);
    write_pattern(in, size);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet, .faults = d.faults};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 16384);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 16384);
    ts_set_console_logger(&cfg_tx);
    ts_set_console_logger(&cfg_rx);

    val_session_t *tx = NULL;
    uint32_t dtx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    if (rctx != VAL_OK || !tx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx);
        return 2;
    }
    val_session_t *rx = NULL;
    uint32_t drx = 0;
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);

    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);

    ts_join_thread(th);

#if VAL_ENABLE_METRICS
    {
        val_metrics_t mtx = {0}, mrx = {0};
        if (val_get_metrics(tx, &mtx) == VAL_OK && val_get_metrics(rx, &mrx) == VAL_OK)
        {
            // With injected faults we expect at least one retransmission or timeout
            if (mtx.retransmits == 0 && mtx.timeouts == 0)
            {
                fprintf(stderr, "expected retransmits or timeouts > 0 under fault injection\n");
                return 8;
            }
            if (mtx.bytes_sent == 0 || mrx.bytes_recv == 0)
            {
                fprintf(stderr, "metrics bytes should be non-zero: tx=%llu rx=%llu\n", (unsigned long long)mtx.bytes_sent,
                        (unsigned long long)mrx.bytes_recv);
                return 9;
            }
            if (mtx.files_sent != 1 || mrx.files_recv != 1)
            {
                fprintf(stderr, "metrics files mismatch: tx=%u rx=%u\n", mtx.files_sent, mrx.files_recv);
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
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);

    // 'out' path was constructed above

    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 2;
    }
    if (!files_equal(in, out))
    {
        fprintf(stderr, "corruption recovery mismatch\n");
        return 3;
    }
    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
