#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Using shared helpers from test_support

int main(void)
{
    const size_t packet = 2048, depth = 32;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char basedir[2048];
    char outdir[2048];
    if (ts_build_case_dirs("multi", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
    {
        fprintf(stderr, "failed to create artifacts dir\n");
        return 1;
    }
    char in1[2048];
    if (ts_path_join(in1, sizeof(in1), basedir, "a.bin") != 0)
        return 1;
    char in2[2048];
    if (ts_path_join(in2, sizeof(in2), basedir, "b.bin") != 0)
        return 1;

    // Two different sizes to cross packet boundaries (overridable via env)
    const size_t s1 = ts_env_size_bytes("VAL_TEST_MULTI_A_SIZE", 300 * 1024 + 17);
    const size_t s2 = ts_env_size_bytes("VAL_TEST_MULTI_B_SIZE", 123 * 1024 + 9);
    uint8_t *buf1 = (uint8_t *)malloc(s1);
    for (size_t i = 0; i < s1; ++i)
        buf1[i] = (uint8_t)(i * 7);
    uint8_t *buf2 = (uint8_t *)malloc(s2);
    for (size_t i = 0; i < s2; ++i)
        buf2[i] = (uint8_t)(255 - (i * 3));
    ts_remove_file(in1);
    ts_remove_file(in2);
    FILE *f1 = fopen(in1, "wb");
    if (!f1)
        return 1;
    fwrite(buf1, 1, s1, f1);
    fclose(f1);
    FILE *f2 = fopen(in2, "wb");
    if (!f2)
        return 1;
    fwrite(buf2, 1, s2, f2);
    fclose(f2);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet);
    uint8_t *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet);
    uint8_t *rb_b = (uint8_t *)calloc(1, packet);

    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_TAIL, 2048);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_TAIL, 2048);

    val_session_t *tx = NULL;
    val_session_t *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(&cfg_tx, 5);

    const char *files[2] = {in1, in2};
    val_status_t st = val_send_files(tx, files, 2, NULL);

    ts_join_thread(th);

#if VAL_ENABLE_METRICS
    {
        val_metrics_t mtx = {0}, mrx = {0};
        if (val_get_metrics(tx, &mtx) == VAL_OK && val_get_metrics(rx, &mrx) == VAL_OK)
        {
            if (mtx.files_sent != 2 || mrx.files_recv != 2)
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
            if (mtx.timeouts != 0 || mtx.timeouts_soft != 0 || mtx.timeouts_hard != 0 ||
                mtx.retransmits != 0 || mtx.crc_errors != 0 ||
                mrx.timeouts != 0 || mrx.timeouts_soft != 0 || mrx.timeouts_hard != 0 ||
                mrx.retransmits != 0 || mrx.crc_errors != 0)
            {
                fprintf(stderr, "unexpected reliability events (multi): tx[t=%u s=%u h=%u r=%u c=%u] rx[t=%u s=%u h=%u r=%u c=%u]\n",
                        mtx.timeouts, mtx.timeouts_soft, mtx.timeouts_hard, mtx.retransmits, mtx.crc_errors,
                        mrx.timeouts, mrx.timeouts_soft, mrx.timeouts_hard, mrx.retransmits, mrx.crc_errors);
                return 12;
            }
        }
        else
        {
            fprintf(stderr, "val_get_metrics failed\n");
            return 10;
        }
    }
#endif

/* Functional outcomes validate correctness for this test */

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    free(buf1);
    free(buf2);

    // Check outputs
    char out1[2048];
    if (ts_path_join(out1, sizeof(out1), outdir, "a.bin") != 0)
        return 1;
    char out2[2048];
    if (ts_path_join(out2, sizeof(out2), outdir, "b.bin") != 0)
        return 1;
    if (st != VAL_OK)
    {
        fprintf(stderr, "send failed %d\n", (int)st);
        return 2;
    }
    if (!ts_files_equal(in1, out1) || !ts_files_equal(in2, out2))
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
