#include "test_support.h"
#include "transport_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Stress tests for extreme network conditions
// These tests validate that the protocol fails GRACEFULLY under unrealistic conditions

#define SMALL_FILE_SIZE (10 * 1024) // 10KB

// Test >5% loss - should fail gracefully
static int test_extreme_packet_loss(void)
{
    printf("\n=== Stress Test: Extreme Packet Loss (>10%%) ===\n");
    printf("Expected: Protocol should fail gracefully with timeout or error\n");
    
    if (transport_sim_init(&PROFILE_WIFI_POOR) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }
    
    char indir[512], outdir[512], infile[512], outfile[512];
    if (ts_build_case_dirs("stress_extreme_loss", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    if (ts_write_pattern_file(infile, SMALL_FILE_SIZE) != 0)
    {
        printf("FAIL: Could not create test file\n");
        transport_sim_cleanup();
        return 1;
    }
    
    const size_t packet_size = 1024;
    const size_t depth = 64;
    test_duplex_t duplex;
    test_duplex_init(&duplex, packet_size, depth);
    
    // EXTREME loss: 10% additional on top of profile's 5% baseline = ~15% total
    duplex.faults.drop_frame_per_million = 100000; // 10% additional
    
    uint8_t tx_send_buf[1024], tx_recv_buf[1024];
    uint8_t rx_send_buf[1024], rx_recv_buf[1024];
    
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    test_duplex_t rx_duplex = {.a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet, .faults = duplex.faults};
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);
    
    // Enable debug logging to see health check activity
    ts_set_console_logger_with_level(&tx_cfg, 4);  // DEBUG level
    ts_set_console_logger_with_level(&rx_cfg, 4);
    
    // Keep timeouts reasonable to fail faster; bound max to avoid exceeding 30s watchdog
    tx_cfg.timeouts.min_timeout_ms = 500;
    tx_cfg.timeouts.max_timeout_ms = 4000; // cap to 4s per try for quicker fail
    rx_cfg.timeouts.min_timeout_ms = 500;
    rx_cfg.timeouts.max_timeout_ms = 4000;
    tx_cfg.retries.data_retries = 0;  // No retries under extreme loss to fail fast
    rx_cfg.retries.data_retries = 0;
    tx_cfg.retries.meta_retries = 0;  // No retries for metadata
    rx_cfg.retries.meta_retries = 0;
    tx_cfg.retries.handshake_retries = 0;  // Fail immediately on handshake timeout under extreme loss
    rx_cfg.retries.handshake_retries = 0;
    // Also reduce ACK retries so worst-case total wait remains < 30s (4 * 4s = 16s)
    tx_cfg.retries.ack_retries = 4;
    rx_cfg.retries.ack_retries = 4;
    
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t detail;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions\n");
        if (tx) val_session_destroy(tx);
        if (rx) val_session_destroy(rx);
        test_duplex_free(&duplex);
        transport_sim_cleanup();
        return 1;
    }
    
    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(100);
    
    // Set a 30-second watchdog - test should complete or fail within this time
    ts_cancel_token_t watchdog = ts_start_timeout_guard(30000, "stress_extreme_loss");
    
    uint64_t start_us = ts_ticks_us();
    const char *files[] = {infile};
    printf("Starting transfer with ~15%% packet loss...\n");
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint64_t elapsed_us = ts_ticks_us() - start_us;
    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000ull);
    
    ts_cancel_timeout_guard(watchdog);
    
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    printf("Transfer result: error=%d, elapsed=%u ms\n", err, elapsed_ms);
    
    // Success criteria: Either completed (unlikely) OR failed gracefully
    if (err == VAL_OK)
    {
        printf("PASS: Protocol completed transfer despite extreme loss (remarkable!)\n");
        transport_sim_cleanup();
        return 0;
    }
    else if (err != VAL_OK && elapsed_ms < 30000)
    {
        printf("PASS: Protocol failed gracefully with error code %d after %u ms\n", err, elapsed_ms);
        printf("      This is expected behavior for >10%% packet loss\n");
        transport_sim_cleanup();
        return 0;
    }
    else
    {
        printf("FAIL: Protocol took too long (>30s) or hung - not graceful\n");
        transport_sim_cleanup();
        return 1;
    }
}

// Test continuous medium loss (3-4%) - should handle but be slow
static int test_sustained_moderate_loss(void)
{
    printf("\n=== Stress Test: Sustained Moderate Loss (3-4%%) ===\n");
    printf("Expected: Protocol should succeed but be slower than normal\n");
    
    if (transport_sim_init(&PROFILE_WIFI_GOOD) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }
    
    char indir[512], outdir[512], infile[512], outfile[512];
    if (ts_build_case_dirs("stress_moderate_loss", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    if (ts_write_pattern_file(infile, SMALL_FILE_SIZE) != 0)
    {
        printf("FAIL: Could not create test file\n");
        transport_sim_cleanup();
        return 1;
    }
    
    const size_t packet_size = 1024;
    const size_t depth = 32;
    test_duplex_t duplex;
    test_duplex_init(&duplex, packet_size, depth);
    
    // Moderate loss: 3% additional on top of profile's 1% = 4% total
    duplex.faults.drop_frame_per_million = 30000; // 3% additional
    
    uint8_t tx_send_buf[1024], tx_recv_buf[1024];
    uint8_t rx_send_buf[1024], rx_recv_buf[1024];
    
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    test_duplex_t rx_duplex = {.a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet, .faults = duplex.faults};
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);
    
    tx_cfg.retries.data_retries = 8;  // More retries for moderate loss
    rx_cfg.retries.data_retries = 8;
    
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t detail;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions\n");
        if (tx) val_session_destroy(tx);
        if (rx) val_session_destroy(rx);
        test_duplex_free(&duplex);
        transport_sim_cleanup();
        return 1;
    }
    
    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(100);
    
    uint64_t start_us = ts_ticks_us();
    const char *files[] = {infile};
    printf("Starting transfer with ~4%% packet loss...\n");
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint64_t elapsed_us = ts_ticks_us() - start_us;
    uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000ull);
    
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    printf("Transfer result: error=%d, elapsed=%u ms\n", err, elapsed_ms);
    
    // Success criteria: Must complete successfully
    if (err != VAL_OK)
    {
        printf("FAIL: Protocol should handle 4%% loss, but failed with error %d\n", err);
        transport_sim_cleanup();
        return 1;
    }
    
    if (!ts_files_equal(infile, outfile))
    {
        printf("FAIL: File integrity check failed\n");
        transport_sim_cleanup();
        return 1;
    }
    
    // Should take longer than normal but not excessively long
    if (elapsed_ms > 15000)
    {
        printf("WARN: Transfer took longer than expected (%u ms) but completed\n", elapsed_ms);
    }
    
    printf("PASS: Protocol handled 4%% loss successfully (elapsed: %u ms)\n", elapsed_ms);
    transport_sim_cleanup();
    return 0;
}

int main(void)
{
    int failures = 0;
    
    printf("========================================\n");
    printf("Transport Profile STRESS TESTS\n");
    printf("========================================\n");
    printf("\nThese tests validate graceful failure under extreme conditions.\n");
    printf("\nTest Philosophy:\n");
    printf("  0-2%% loss:  MUST succeed (normal tests)\n");
    printf("  2-5%% loss:  SHOULD succeed but slower (stress tests)\n");
    printf("  >5%% loss:   CAN fail gracefully (stress tests)\n");
    printf("\n========================================\n");
    
    failures += test_sustained_moderate_loss();
    failures += test_extreme_packet_loss();
    
    printf("\n========================================\n");
    if (failures == 0)
    {
        printf("ALL STRESS TESTS PASSED\n");
        printf("Protocol handles extreme conditions appropriately\n");
    }
    else
    {
        printf("FAILED: %d stress test(s)\n", failures);
    }
    printf("========================================\n");
    
    return failures > 0 ? 1 : 0;
}
