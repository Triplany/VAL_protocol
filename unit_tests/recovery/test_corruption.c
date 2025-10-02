#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Using shared helpers from test_support

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

    char basedir[2048];
    char outdir[2048];
    if (ts_build_case_dirs("corrupt", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
    {
        fprintf(stderr, "failed to create artifacts dir\n");
        return 1;
    }
    char in[2048];
    if (ts_path_join(in, sizeof(in), basedir, "corrupt.bin") != 0)
        return 1;
    char out[2048];
    if (ts_path_join(out, sizeof(out), outdir, "corrupt.bin") != 0)
        return 1;
    // Clean any previous input/output files to avoid stale resume behavior when size changes
    ts_remove_file(in);
    ts_remove_file(out);
    if (ts_write_pattern_file(in, size) != 0)
        return 1;

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
    ts_receiver_warmup(&cfg_tx, 5);

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
    if (!ts_files_equal(in, out))
    {
        fprintf(stderr, "corruption recovery mismatch\n");
        return 3;
    }
    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
