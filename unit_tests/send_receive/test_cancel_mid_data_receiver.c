#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static val_session_t *g_rx_for_cancel = NULL;
static volatile int g_cancel_done = 0;
static void on_progress_cancel_rx(const val_progress_info_t *info)
{
    if (!info || g_cancel_done)
        return;
    if (info->current_file_bytes > 0)
    {
        g_cancel_done = 1;
        if (g_rx_for_cancel)
            (void)val_emergency_cancel(g_rx_for_cancel);
    }
}

int main(void)
{
    const size_t packet = 2048; // ensure multiple packets
    const size_t depth = 64;
    // Big file so transfer is long enough to cancel mid-flight (overridable)
    const size_t file_size = ts_env_size_bytes("VAL_TEST_CANCEL_RX_SIZE", 8 * 1024 * 1024 + 13);

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char basedir[2048];
    char outdir[2048];
    if (ts_build_case_dirs("cancel_rx", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
    {
        fprintf(stderr, "failed to create artifacts dirs\n");
        return 1;
    }
    char inpath[2048];
    if (ts_path_join(inpath, sizeof(inpath), basedir, "input.bin") != 0)
        return 1;
    char outpath[2048];
    if (ts_path_join(outpath, sizeof(outpath), outdir, "input.bin") != 0)
        return 1;
    // Ensure no stale output exists from previous runs which could alter resume offset
    ts_remove_file(outpath);
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
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = {.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    ts_make_config(&cfg_tx, send_a, recv_a, packet, &end_tx, VAL_RESUME_TAIL, 2048);
    ts_make_config(&cfg_rx, send_b, recv_b, packet, &end_rx, VAL_RESUME_TAIL, 2048);

    // ts_make_config installs real system hooks by default

    // Disable resume for this test to avoid inheriting offsets from previous runs
    cfg_tx.resume.mode = VAL_RESUME_NEVER;
    cfg_rx.resume.mode = VAL_RESUME_NEVER;
    ts_set_console_logger(&cfg_tx);
    ts_set_console_logger(&cfg_rx);

    // Install a progress callback on receiver to cancel once data starts flowing
    cfg_rx.callbacks.on_progress = on_progress_cancel_rx;
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        return 2;
    }

    // Start receiver
    g_rx_for_cancel = rx;
    ts_thread_t th_rx = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(&cfg_tx, 5);

    const char *files[1] = {inpath};
    val_status_t st_send = val_send_files(tx, files, 1, NULL);

    // Join receiver
    ts_join_thread(th_rx);

    (void)g_cancel_done;

    // Cleanup sessions and buffers
    val_status_t last_rx = VAL_OK;
    val_status_t code = VAL_OK;
    uint32_t detail = 0;
    if (val_get_last_error(rx, &code, &detail) == VAL_OK)
        last_rx = code;

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(send_a);
    free(recv_a);
    free(send_b);
    free(recv_b);

    // Expectations: sender aborted, receiver aborted
    if (st_send != VAL_ERR_ABORTED)
    {
        fprintf(stderr, "expected sender VAL_ERR_ABORTED, got %d\n", (int)st_send);
        return 10;
    }
    if (last_rx != VAL_ERR_ABORTED)
    {
        fprintf(stderr, "expected receiver last error ABORTED, got %d (detail=0x%08x)\n", (int)last_rx, (unsigned)detail);
        return 11;
    }

    // Output file should exist but be partial (<= input size)
    uint64_t in_sz = ts_file_size(inpath);
    uint64_t out_sz = ts_file_size(outpath);
    if (out_sz == 0 || out_sz > in_sz)
    {
        fprintf(stderr, "unexpected output size: in=%llu out=%llu\n", (unsigned long long)in_sz, (unsigned long long)out_sz);
        return 12;
    }

    test_duplex_free(&d);
    printf("OK\n");
    return 0;
}
