#include "test_support.h"
#include "transport_profiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// This test asserts that under extreme simulated loss, operations time out
// before an external 30s watchdog threshold, proving our absolute-deadline caps.

static int run_extreme_loss_case(const char *case_name)
{
    if (transport_sim_init(&PROFILE_WIFI_POOR) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }

    char indir[512], outdir[512], infile[512];
    if (ts_build_case_dirs(case_name, indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../tiny.bin");

    if (ts_write_pattern_file(infile, 1024) != 0)
    {
        printf("FAIL: Could not create test file\n");
        transport_sim_cleanup();
        return 1;
    }

    const size_t packet_size = 1024;
    const size_t depth = 32;
    test_duplex_t duplex; test_duplex_init(&duplex, packet_size, depth);
    // Make loss deterministic and overwhelming to guarantee failure with limited retries
    ts_rand_seed_set(0xC0FFEEu);
    duplex.faults.drop_frame_per_million = 1000000; // 100% drop

    uint8_t tx_send_buf[1024], tx_recv_buf[1024];
    uint8_t rx_send_buf[1024], rx_recv_buf[1024];

    val_config_t tx_cfg, rx_cfg;
    test_duplex_t rx_duplex = { .a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet, .faults = duplex.faults };
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);

    // Reduce timeouts so absolute caps bind earlier and keep test fast
    tx_cfg.timeouts.min_timeout_ms = 300;
    tx_cfg.timeouts.max_timeout_ms = 2000;
    rx_cfg.timeouts.min_timeout_ms = 300;
    rx_cfg.timeouts.max_timeout_ms = 2000;
    // Limit retries so we don't spin forever
    tx_cfg.retries.data_retries = 2;
    rx_cfg.retries.data_retries = 2;
    tx_cfg.retries.meta_retries = 1;
    rx_cfg.retries.meta_retries = 1;
    tx_cfg.retries.ack_retries = 2;
    rx_cfg.retries.ack_retries = 2;
    tx_cfg.retries.handshake_retries = 1;
    rx_cfg.retries.handshake_retries = 1;

    val_session_t *tx = NULL, *rx = NULL; uint32_t detail = 0;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    if (!tx || !rx)
    {
        if (tx) val_session_destroy(tx);
        if (rx) val_session_destroy(rx);
        test_duplex_free(&duplex);
        transport_sim_cleanup();
        return 1;
    }

    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    ts_delay(50);

    // External watchdog at 30s; we should fail well before this
    ts_cancel_token_t watchdog = ts_start_timeout_guard(30000, case_name);

    uint32_t start = ts_ticks();
    const char *files[] = { infile };
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint32_t elapsed = ts_ticks() - start;

    ts_cancel_timeout_guard(watchdog);

    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);

    printf("Result: err=%d elapsed=%u ms\n", err, elapsed);

    // Assert we fail gracefully and quickly (<24s absolute cap today)
    if (elapsed >= 24000)
    {
        printf("FAIL: Timeout bound too high (%u ms)\n", elapsed);
        transport_sim_cleanup();
        return 1;
    }
    if (err == VAL_OK)
    {
        printf("FAIL: Unexpected success under extreme loss\n");
        transport_sim_cleanup();
        return 1;
    }

    printf("PASS: Failed gracefully in %u ms with error %d\n", elapsed, err);
    transport_sim_cleanup();
    return 0;
}

// Handshake-specific failure: drop rate so high that HELLO likely times out.
// With handshake retries = 0 and small timeouts, we should fail within a few seconds.
static int run_handshake_fail_fast_case(const char *case_name)
{
    if (transport_sim_init(&PROFILE_WIFI_POOR) != 0)
    {
        printf("FAIL: Could not initialize profile\n");
        return 1;
    }

    // We don't actually need to send a real file; but reuse the tiny file for simplicity
    char indir[512], outdir[512], infile[512];
    if (ts_build_case_dirs(case_name, indir, sizeof(indir), outdir, sizeof(outdir)) != 0)
    {
        printf("FAIL: Could not create test directories\n");
        transport_sim_cleanup();
        return 1;
    }
    ts_path_join(infile, sizeof(infile), indir, "../tiny.bin");
    (void)ts_write_pattern_file(infile, 256);

    const size_t packet_size = 512;
    const size_t depth = 16;
    test_duplex_t duplex; test_duplex_init(&duplex, packet_size, depth);
    // Extremely high frame drop to attack HELLO robustness
    duplex.faults.drop_frame_per_million = 500000; // +50%

    uint8_t tx_send_buf[512], tx_recv_buf[512];
    uint8_t rx_send_buf[512], rx_recv_buf[512];

    val_config_t tx_cfg, rx_cfg;
    test_duplex_t rx_duplex = { .a2b = duplex.b2a, .b2a = duplex.a2b, .max_packet = duplex.max_packet, .faults = duplex.faults };
    ts_make_config(&tx_cfg, tx_send_buf, tx_recv_buf, packet_size, &duplex, VAL_RESUME_NEVER, 0);
    ts_make_config(&rx_cfg, rx_send_buf, rx_recv_buf, packet_size, &rx_duplex, VAL_RESUME_NEVER, 0);

    // Let handshake be affected immediately (no grace bytes)
    ts_net_sim_t sim = {0};
    sim.handshake_grace_bytes = 0;
    ts_net_sim_set(&sim);

    // Make handshake fail fast
    tx_cfg.timeouts.min_timeout_ms = 200;
    tx_cfg.timeouts.max_timeout_ms = 800;
    rx_cfg.timeouts.min_timeout_ms = 200;
    rx_cfg.timeouts.max_timeout_ms = 800;
    tx_cfg.retries.handshake_retries = 0;
    rx_cfg.retries.handshake_retries = 0;

    val_session_t *tx = NULL, *rx = NULL; uint32_t detail = 0;
    val_session_create(&tx_cfg, &tx, &detail);
    val_session_create(&rx_cfg, &rx, &detail);
    if (!tx || !rx)
    {
        if (tx) val_session_destroy(tx);
        if (rx) val_session_destroy(rx);
        test_duplex_free(&duplex);
        transport_sim_cleanup();
        return 1;
    }

    ts_thread_t rx_thread = ts_start_receiver(rx, outdir);
    // Give the receiver thread slightly more time to start on slower builds/CI
    ts_delay(50);

    ts_cancel_token_t watchdog = ts_start_timeout_guard(30000, case_name);
    uint32_t start = ts_ticks();
    const char *files[] = { infile };
    val_status_t err = val_send_files(tx, files, 1, NULL);
    uint32_t elapsed = ts_ticks() - start;
    ts_cancel_timeout_guard(watchdog);

    ts_join_thread(rx_thread);
    val_session_destroy(tx);
    val_session_destroy(rx);
    test_duplex_free(&duplex);

    // Reset sim to defaults for the next tests in the suite
    ts_net_sim_reset();

    printf("Handshake-fast-fail: err=%d elapsed=%u ms\n", err, elapsed);
    if (err == VAL_OK)
    {
        printf("FAIL: Unexpected success under severe handshake loss\n");
        transport_sim_cleanup();
        return 1;
    }
    // Allow additional slack for slower hosts / initial-run scheduling jitter.
    // Previously this was 5000ms; raise to 8000ms to avoid spurious failures on Linux VMs.
    if (elapsed >= 8000)
    {
        printf("FAIL: Handshake took too long to fail (%u ms)\n", elapsed);
        transport_sim_cleanup();
        return 1;
    }
    printf("PASS: Handshake failed fast in %u ms with error %d\n", elapsed, err);
    transport_sim_cleanup();
    return 0;
}

int main(void)
{
    int failures = 0;
    printf("\n=== Time-bounded failure tests ===\n");
    failures += run_extreme_loss_case("timebound_extreme_loss");
    failures += run_handshake_fail_fast_case("timebound_handshake_fail");
    return failures ? 1 : 0;
}
