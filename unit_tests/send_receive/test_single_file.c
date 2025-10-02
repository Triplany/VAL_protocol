#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Using common helpers from test_support.h

int main(void)
{
    const size_t packet = 1024; // ensure multiple packets
    const size_t depth = 16;
    const size_t file_size = ts_env_size_bytes("VAL_TEST_SINGLE_SIZE", 256 * 1024 + 123); // override with env if set

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    // Temp directories/files under the build's executable directory
    char tmpdir[2048];
    char outdir[2048];
    if (ts_build_case_dirs("single", tmpdir, sizeof(tmpdir), outdir, sizeof(outdir)) != 0)
    {
        fprintf(stderr, "failed to create artifacts dirs\n");
        return 1;
    }
    char inpath[2048];
    if (ts_path_join(inpath, sizeof(inpath), tmpdir, "input.bin") != 0)
    {
        fprintf(stderr, "path join failed\n");
        return 1;
    }
    char outpath[2048];
    if (ts_path_join(outpath, sizeof(outpath), outdir, "input.bin") != 0)
    {
        fprintf(stderr, "path join failed\n");
        return 1;
    }

    ts_remove_file(outpath);
    ts_remove_file(inpath);
    if (ts_write_pattern_file(inpath, file_size) != 0)
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
    ts_receiver_warmup(&cfg_tx, 5);

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

#if VAL_ENABLE_WIRE_AUDIT
    {
        val_wire_audit_t a_tx = {0}, a_rx = {0};
        if (val_get_wire_audit(tx, &a_tx) == VAL_OK && val_get_wire_audit(rx, &a_rx) == VAL_OK)
        {
            // Receiver should not send DATA
            if (a_rx.sent_data != 0)
            {
                fprintf(stderr, "wire_audit: receiver sent DATA unexpectedly (count=%llu)\n", (unsigned long long)a_rx.sent_data);
                return 12;
            }
            // Sender should not send DONE_ACK/EOT_ACK
            if (a_tx.sent_done_ack != 0 || a_tx.sent_eot_ack != 0)
            {
                fprintf(stderr, "wire_audit: sender sent *_ACK unexpectedly (done_ack=%llu eot_ack=%llu)\n",
                        (unsigned long long)a_tx.sent_done_ack, (unsigned long long)a_tx.sent_eot_ack);
                return 13;
            }
        }
        else
        {
            fprintf(stderr, "val_get_wire_audit failed\n");
            return 14;
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

    if (!ts_files_equal(inpath, outpath))
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
