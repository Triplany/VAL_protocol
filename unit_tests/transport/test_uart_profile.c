#include "test_support.h"
#include "transport_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// UART is slow - use small test files
#define TEST_FILE_SIZE_115200 (8 * 1024) // 8KB
#define TEST_FILE_SIZE_9600   (2 * 1024) // 2KB

static int test_uart_basic_transfer_115200(void)
{
    printf("\n=== Test: UART Basic Transfer (10KB) ===\n");
    
    // Initialize profile
    if (transport_sim_init(&PROFILE_UART_115200) != 0)
    {
        printf("FAIL: Could not initialize UART profile\n");
        return 1;
    }
    
    // Setup paths
    char indir[512], outdir[512], infile[512], outfile[512];
    if (ts_build_case_dirs("uart_basic", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");
    
    // Create test file
    if (ts_write_pattern_file(infile, TEST_FILE_SIZE_115200) != 0)
    {
        printf("FAIL: Could not create test file\n");
        transport_sim_cleanup();
        return 1;
    }
    
    // Setup duplex transport
    const size_t packet_size = 1024;
    const size_t depth = 16;
    test_duplex_t duplex;
    test_duplex_init(&duplex, packet_size, depth);
    
    // Create sender and receiver sessions
    uint8_t tx_send_buf[1024], tx_recv_buf[1024];
    uint8_t rx_send_buf[1024], rx_recv_buf[1024];
    
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    
    // Receiver uses reverse direction
    test_duplex_t rx_duplex = {.a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet};
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);

    // Create sessions
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
    
    // Start receiver in background
    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(50); // Small warmup
    
    // Record start time
    uint32_t start_time = ts_ticks();
    
    // Send file
    const char *files[] = {infile};
    val_status_t err = val_send_files(tx, files, 1, NULL);
    
    uint32_t elapsed_ms = ts_ticks() - start_time;
    
    // Wait for receiver
    ts_join_thread(rx_thread);
    
    // Cleanup sessions
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    
    // Get stats
    transport_sim_stats_t stats;
    transport_sim_get_stats(&stats);
    
    printf("Transfer complete - Error: %d\n", err);
    printf("Elapsed time: %u ms (%.2f seconds)\n", elapsed_ms, elapsed_ms / 1000.0f);
    printf("Bytes sent: %llu, received: %llu\n", 
           (unsigned long long)stats.bytes_sent,
           (unsigned long long)stats.bytes_received);
    
    // Verify integrity
    if (err != VAL_OK)
    {
        printf("FAIL: Transfer error: %d\n", err);
        transport_sim_cleanup();
        return 1;
    }
    
    if (!ts_files_equal(infile, outfile))
    {
        printf("FAIL: File integrity check failed\n");
        transport_sim_cleanup();
        return 1;
    }
    
    // Verify timing expectations for UART
    // 10KB at 11.5 KB/s = ~870ms minimum
    // With protocol overhead and retries, expect 1-3 seconds
    if (elapsed_ms < 500)
    {
        printf("WARN: Transfer too fast for UART profile (%u ms)\n", elapsed_ms);
    }
    else if (elapsed_ms > 10000)
    {
        printf("WARN: Transfer took longer than expected (%u ms)\n", elapsed_ms);
    }
    
    // Calculate effective throughput
    float throughput_kbps = (TEST_FILE_SIZE_115200 * 8.0f) / elapsed_ms;
    printf("Effective throughput: %.2f kbps (%.2f KB/s)\n", 
           throughput_kbps, throughput_kbps / 8.0f);
    
    printf("PASS: UART 115200 basic transfer successful\n");
    transport_sim_cleanup();
    return 0;
}

static int test_uart_basic_transfer_9600(void)
{
    printf("\n=== Test: UART Basic Transfer (9600 baud, 2KB) ===\n");

    if (transport_sim_init(&PROFILE_UART_9600) != 0)
    {
        printf("FAIL: Could not initialize UART-9600 profile\n");
        return 1;
    }

    char indir[512], outdir[512], infile[512], outfile[512];
    if (ts_build_case_dirs("uart_9600", indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../test.bin");
    ts_path_join(outfile, sizeof(outfile), outdir, "test.bin");

    if (ts_write_pattern_file(infile, TEST_FILE_SIZE_9600) != 0)
    {
        printf("FAIL: Could not create test file\n");
        transport_sim_cleanup();
        return 1;
    }

    const size_t packet_size = 512; // small MTU typical on MCU side
    const size_t depth = 8;
    test_duplex_t duplex; test_duplex_init(&duplex, packet_size, depth);

    uint8_t tx_send_buf[512], tx_recv_buf[512];
    uint8_t rx_send_buf[512], rx_recv_buf[512];
    val_config_t tx_cfg, rx_cfg;
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    test_duplex_t rx_duplex = {.a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet};
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);

    // Tighten tx window for 9600 to be MCU-friendly
    tx_cfg.tx_flow.window_cap_packets = 4;
    tx_cfg.tx_flow.initial_cwnd_packets = 1;

    val_session_t *tx = NULL, *rx = NULL; uint32_t detail;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    if (!tx || !rx)
    {
        printf("FAIL: Could not create sessions (9600)\n");
        if (tx) val_session_destroy(tx);
        if (rx) val_session_destroy(rx);
        test_duplex_free(&duplex);
        transport_sim_cleanup();
        return 1;
    }
    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(50);
    uint32_t start_time = ts_ticks();
    const char *files[] = {infile};
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint32_t elapsed_ms = ts_ticks() - start_time;
    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);
    transport_sim_stats_t stats; transport_sim_get_stats(&stats);

    printf("Transfer complete - Error: %d\n", err);
    printf("Elapsed time: %u ms (%.2f seconds)\n", elapsed_ms, elapsed_ms / 1000.0f);
    printf("Bytes sent: %llu, received: %llu\n",
           (unsigned long long)stats.bytes_sent,
           (unsigned long long)stats.bytes_received);

    if (err != VAL_OK)
    {
        printf("FAIL: Transfer error: %d\n", err);
        transport_sim_cleanup();
        return 1;
    }
    if (!ts_files_equal(infile, outfile))
    {
        printf("FAIL: File integrity check failed (9600)\n");
        transport_sim_cleanup();
        return 1;
    }

    // Timing expectation: 2KB at ~1.2 KB/s -> ~1.7s minimum, allow 1-6s
    if (elapsed_ms < 800) {
        printf("WARN: Transfer too fast for UART-9600 profile (%u ms)\n", elapsed_ms);
    } else if (elapsed_ms > 6000) {
        printf("WARN: Transfer took longer than expected for UART-9600 (%u ms)\n", elapsed_ms);
    }
    float kbps = (TEST_FILE_SIZE_9600 * 8.0f) / elapsed_ms;
    printf("Effective throughput: %.2f kbps (%.2f KB/s)\n", kbps, kbps / 8.0f);
    printf("PASS: UART 9600 basic transfer successful\n");
    transport_sim_cleanup();
    return 0;
}

static int test_uart_mode_selection(void)
{
    printf("\n=== Test: UART Mode Selection ===\n");
    printf("UART should stay in windowed mode (low bandwidth)\n");
    
    // Initialize profile
    if (transport_sim_init(&PROFILE_UART_115200) != 0)
    {
        printf("FAIL: Could not initialize UART profile\n");
        return 1;
    }
    
    // For this test, we just verify the profile is configured correctly
    const transport_profile_t *profile = transport_sim_get_profile();
    if (!profile)
    {
        printf("FAIL: Could not get profile\n");
        transport_sim_cleanup();
        return 1;
    }
    
    printf("Profile: %s\n", profile->name);
    printf("Bandwidth: %u bps (%.2f KB/s)\n", 
           profile->bandwidth_bps, profile->bandwidth_bps / 8000.0f);
    
    // Expected: bandwidth <= 1 Mbps should prefer windowed mode
    if (profile->bandwidth_bps <= 1000000)
    {
        printf("PASS: Low bandwidth profile suitable for windowed mode\n");
        transport_sim_cleanup();
        return 0;
    }
    else
    {
        printf("FAIL: UART profile has unexpected high bandwidth\n");
        transport_sim_cleanup();
        return 1;
    }
}

int main(void)
{
    int failures = 0;
    
    printf("========================================\n");
    printf("UART 115200 Transport Profile Tests\n");
    printf("========================================\n");
    
    failures += test_uart_basic_transfer_115200();
    failures += test_uart_basic_transfer_9600();
    failures += test_uart_mode_selection();
    
    printf("\n========================================\n");
    if (failures == 0)
    {
        printf("ALL UART TESTS PASSED\n");
    }
    else
    {
        printf("FAILED: %d test(s)\n", failures);
    }
    printf("========================================\n");
    
    return failures > 0 ? 1 : 0;
}
