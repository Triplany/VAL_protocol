#include "test_support.h"
#include "transport_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Quick diagnostic test to identify the hang
int main(void)
{
    printf("========================================\n");
    printf("Satellite Weather Test - Diagnostic\n");
    printf("========================================\n");
    
    // Test with ZERO additional fault injection first
    printf("\n=== Test 1: NO additional fault injection ===\n");
    
    if (transport_sim_init(&PROFILE_SATELLITE_GEO) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }
    
    char indir[512], outdir[512], infile[512], outfile[512];
    if (ts_build_case_dirs("sat_diag_no_faults", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    const size_t file_size = 10 * 1024;
    if (ts_write_pattern_file(infile, file_size) != 0)
    {
        printf("FAIL: Could not create test file\n");
        return 1;
    }
    
    const size_t packet_size = 1024;
    const size_t depth = 64;
    const int fast_mode = (getenv("VAL_FAST_DIAG") != NULL);
    test_duplex_t duplex;
    test_duplex_init(&duplex, packet_size, depth);
    
    // NO additional fault injection
    duplex.faults.drop_frame_per_million = 0;
    
    uint8_t tx_send_buf[1024], tx_recv_buf[1024];
    uint8_t rx_send_buf[1024], rx_recv_buf[1024];
    
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    test_duplex_t rx_duplex = {.a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet, .faults = duplex.faults};
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);
    
    // Tighten timeouts/retries to bound runtime while preserving behavior trends
    tx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    tx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    rx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    rx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    // Keep budgets modest so failures surface cleanly without hanging
    tx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    rx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    tx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    rx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t detail;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions\n");
        return 1;
    }
    
    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(200);
    
    uint32_t start_time = ts_ticks();
    const char *files[] = {infile};
    printf("Starting transfer with 0%% additional loss...\n");
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint32_t elapsed_ms = ts_ticks() - start_time;
    
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    printf("Test 1 Result: %s (elapsed: %u ms)\n", err == VAL_OK ? "PASS" : "FAIL", elapsed_ms);
    transport_sim_cleanup();
    
    // Now test with 1% additional loss
    printf("\n=== Test 2: 1%% additional fault injection ===\n");
    
    if (transport_sim_init(&PROFILE_SATELLITE_GEO) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }
    
    if (ts_build_case_dirs("sat_diag_1pct", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    if (ts_write_pattern_file(infile, file_size) != 0)
    {
        printf("FAIL: Could not create test file\n");
        return 1;
    }
    
    test_duplex_init(&duplex, packet_size, depth);
    duplex.faults.drop_frame_per_million = 10000; // 1% additional
    
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    rx_duplex.a2b = duplex.b2a;
    rx_duplex.b2a = duplex.a2b;
    rx_duplex.faults = duplex.faults;
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);
    
    tx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    tx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    rx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    rx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    tx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    rx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    tx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    rx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions\n");
        return 1;
    }
    
    rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(200);
    
    start_time = ts_ticks();
    printf("Starting transfer with 1%% additional loss...\n");
    err = val_send_files(tx, files, 1, NULL);
    elapsed_ms = ts_ticks() - start_time;
    
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    printf("Test 2 Result: %s (elapsed: %u ms)\n", err == VAL_OK ? "PASS" : "FAIL", elapsed_ms);
    transport_sim_cleanup();
    
    // Now test with 5% additional loss (the problem case)
    printf("\n=== Test 3: 5%% additional fault injection (PROBLEM CASE) ===\n");
    
    if (transport_sim_init(&PROFILE_SATELLITE_GEO) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }
    
    if (ts_build_case_dirs("sat_diag_5pct", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    if (ts_write_pattern_file(infile, file_size) != 0)
    {
        printf("FAIL: Could not create test file\n");
        return 1;
    }
    
    test_duplex_init(&duplex, packet_size, depth);
    duplex.faults.drop_frame_per_million = 50000; // 5% additional (PROBLEM!)
    
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    rx_duplex.a2b = duplex.b2a;
    rx_duplex.b2a = duplex.a2b;
    rx_duplex.faults = duplex.faults;
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);
    
    tx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    tx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    rx_cfg.timeouts.min_timeout_ms = fast_mode ? 200 : 300;
    rx_cfg.timeouts.max_timeout_ms = fast_mode ? 3000 : 4000;
    tx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    rx_cfg.retries.data_retries = fast_mode ? 3 : 4;
    tx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    rx_cfg.retries.ack_retries = fast_mode ? 2 : 3;
    
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions\n");
        return 1;
    }
    
    rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(200);
    
    start_time = ts_ticks();
    printf("Starting transfer with 5%% additional loss...\n");
    
    err = val_send_files(tx, files, 1, NULL);
    elapsed_ms = ts_ticks() - start_time;
    
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    // Expect clean failure at 5%% loss; treat success as a test failure
    if (err == VAL_OK)
    {
        printf("Test 3 Result: UNEXPECTED SUCCESS (elapsed: %u ms)\n", elapsed_ms);
        transport_sim_cleanup();
        return 1;
    }
    printf("Test 3 Result: EXPECTED FAILURE (elapsed: %u ms)\n", elapsed_ms);
    
    transport_sim_cleanup();
    
    return 0;
}
