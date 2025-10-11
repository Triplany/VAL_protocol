#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static val_session_t *g_tx_for_cancel = NULL;
static volatile int g_cancel_done = 0;
static void on_progress_cancel_tx(const val_progress_info_t *info)
{
    if (!info || g_cancel_done)
        return;
    if (info->current_file_bytes >= (64u * 1024u))
    {
        g_cancel_done = 1;
        fprintf(stdout, "[TEST] TX progress reached %llu bytes, issuing CANCEL...\n",
                (unsigned long long)info->current_file_bytes);
        fflush(stdout);
        if (g_tx_for_cancel)
        {
            val_status_t cr = val_emergency_cancel(g_tx_for_cancel);
            fprintf(stdout, "[TEST] val_emergency_cancel(tx) returned %d\n", (int)cr);
            fflush(stdout);
        }
    }
}

int main(void)
{
    ts_cancel_token_t wd = ts_start_timeout_guard(TEST_TIMEOUT_QUICK_MS, "cancel_mid_data_sender");
    
    const size_t packet = 2048;
    const size_t depth = 64;
    const size_t file_size = ts_env_size_bytes("VAL_TEST_CANCEL_TX_SIZE", 8 * 1024 * 1024 + 13);

    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char basedir[2048];
    char outdir[2048];
    if (ts_build_case_dirs("cancel_tx", basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
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

    cfg_tx.system.get_ticks_ms = ts_ticks;
    cfg_tx.system.delay_ms = ts_delay;
    cfg_rx.system.get_ticks_ms = ts_ticks;
    cfg_rx.system.delay_ms = ts_delay;

    // Disable resume for this test to avoid inheriting offsets from previous runs
    cfg_tx.resume.mode = VAL_RESUME_NEVER;
    cfg_rx.resume.mode = VAL_RESUME_NEVER;

    // Enable detailed console logging for this test on both ends
    ts_set_console_logger(&cfg_tx);
    ts_set_console_logger(&cfg_rx);

    // Install a progress callback on sender to cancel once data starts flowing
    cfg_tx.callbacks.on_progress = on_progress_cancel_tx;
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

    ts_thread_t th_rx = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(&cfg_tx, 5);

    // Keep a handle for cancel
    g_tx_for_cancel = tx;

    const char *files[1] = {inpath};
    val_status_t st_send = val_send_files(tx, files, 1, NULL);
    fprintf(stdout, "[TEST] val_send_files(tx) returned %d\n", (int)st_send);
    fflush(stdout);

    // Join receiver
    ts_join_thread(th_rx);

    (void)g_cancel_done;

    // Gather receiver last error
    val_status_t last_rx = VAL_OK;
    val_status_t code = VAL_OK;
    uint32_t detail = 0;
    (void)val_get_last_error(rx, &code, &detail);
    fprintf(stdout, "[TEST] rx last_error: code=%d detail=0x%08X\n", (int)code, (unsigned)detail);
    fflush(stdout);
    last_rx = code;

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(send_a);
    free(recv_a);
    free(send_b);
    free(recv_b);

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
    // Partial output expected
    uint64_t in_sz = ts_file_size(inpath);
    uint64_t out_sz = ts_file_size(outpath);
    if (out_sz == 0 || out_sz > in_sz)
    {
        fprintf(stderr, "unexpected output size: in=%llu out=%llu\n", (unsigned long long)in_sz, (unsigned long long)out_sz);
        return 12;
    }

    test_duplex_free(&d);
    
    ts_cancel_timeout_guard(wd);
    printf("OK\n");
    return 0;
}
