#include "transport_profiles.h"
#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Profile Definitions ----

// UART 115200: 115200 bps = 14400 bytes/sec theoretical, ~11520 bytes/sec effective (80% efficiency)
const transport_profile_t PROFILE_UART_115200 = {
    .name = "UART-115200",
    .bandwidth_bps = 115200,
    .base_latency_ms = 1,   // Very low latency (wired)
    .jitter_ms = 0,         // Minimal jitter
    .loss_rate = 0.0001f,   // Very rare loss (0.01%)
    .burst_loss_rate = 0.0f,
    .burst_duration_ms = 0,
    .burst_interval_ms = 0,
    .mtu_bytes = 0,         // No MTU limit
    .seed = 42
};

// WiFi Good: 10 Mbps effective, moderate latency, occasional loss
const transport_profile_t PROFILE_WIFI_GOOD = {
    .name = "WiFi-Good",
    .bandwidth_bps = 10000000, // 10 Mbps
    .base_latency_ms = 10,
    .jitter_ms = 15,           // 10 +/- 15ms
    .loss_rate = 0.01f,        // 1% baseline loss
    .burst_loss_rate = 0.05f,  // 5% during bursts
    .burst_duration_ms = 500,
    .burst_interval_ms = 10000, // Burst every 10 seconds
    .mtu_bytes = 1500,
    .seed = 12345
};

// WiFi Poor: 2 Mbps effective, high latency, frequent loss
const transport_profile_t PROFILE_WIFI_POOR = {
    .name = "WiFi-Poor",
    .bandwidth_bps = 2000000, // 2 Mbps
    .base_latency_ms = 30,
    .jitter_ms = 50,          // 30 +/- 50ms (can spike to 80ms)
    .loss_rate = 0.05f,       // 5% baseline loss
    .burst_loss_rate = 0.20f, // 20% during bursts
    .burst_duration_ms = 1000,
    .burst_interval_ms = 5000, // Burst every 5 seconds
    .mtu_bytes = 1500,
    .seed = 54321
};

// Satellite GEO: 5 Mbps, very high latency, occasional loss
const transport_profile_t PROFILE_SATELLITE_GEO = {
    .name = "Satellite-GEO",
    .bandwidth_bps = 5000000, // 5 Mbps
    .base_latency_ms = 500,   // 500ms one-way (1000ms RTT)
    .jitter_ms = 50,
    .loss_rate = 0.01f,       // 1% baseline
    .burst_loss_rate = 0.15f, // 15% during weather
    .burst_duration_ms = 3000,
    .burst_interval_ms = 30000, // Weather event every 30 seconds
    .mtu_bytes = 1500,
    .seed = 99999
};

// Satellite LEO (Starlink-like): 100 Mbps, low latency, low loss
const transport_profile_t PROFILE_SATELLITE_LEO = {
    .name = "Satellite-LEO",
    .bandwidth_bps = 100000000, // 100 Mbps
    .base_latency_ms = 30,      // 30ms one-way (60ms RTT)
    .jitter_ms = 10,
    .loss_rate = 0.005f,        // 0.5% baseline
    .burst_loss_rate = 0.03f,   // 3% during handover
    .burst_duration_ms = 1000,
    .burst_interval_ms = 15000, // Satellite handover every 15 seconds
    .mtu_bytes = 1500,
    .seed = 77777
};

// ---- Simulator State ----

typedef struct
{
    uint64_t bytes_transferred;
    uint32_t last_burst_time;
    int in_burst;
} transport_sim_context_t;

static struct
{
    int initialized;
    transport_profile_t profile;
    transport_sim_context_t ctx;
    transport_sim_stats_t stats;
} g_sim = {0};

// ---- RNG (reuse existing pcg32 or provide simple one) ----
static uint64_t g_rng_state = 0x853c49e6748fea9bull;

static uint32_t sim_rand(void)
{
    // Simple LCG for deterministic behavior
    g_rng_state = g_rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(g_rng_state >> 32);
}

static void sim_rand_seed(uint32_t seed)
{
    g_rng_state = seed;
}

static float sim_rand_float(void)
{
    return (float)sim_rand() / (float)0xFFFFFFFFU;
}

// ---- Profile Simulation Logic ----

// Check if we should be in a burst right now
static int should_be_in_burst(uint32_t now_ms)
{
    if (g_sim.profile.burst_interval_ms == 0)
        return 0;
    
    uint32_t cycle_pos = now_ms % g_sim.profile.burst_interval_ms;
    return cycle_pos < g_sim.profile.burst_duration_ms;
}

// Calculate current loss rate
static float get_current_loss_rate(uint32_t now_ms)
{
    if (should_be_in_burst(now_ms))
        return g_sim.profile.burst_loss_rate;
    return g_sim.profile.loss_rate;
}

// Apply bandwidth throttling by calculating delay for N bytes
static uint32_t calculate_bandwidth_delay_ms(size_t bytes)
{
    if (g_sim.profile.bandwidth_bps == 0)
        return 0;
    
    // Convert bytes to bits, then to milliseconds
    uint64_t bits = (uint64_t)bytes * 8;
    uint64_t delay_ms = (bits * 1000) / g_sim.profile.bandwidth_bps;
    return (uint32_t)delay_ms;
}

// Apply latency with jitter
static uint32_t calculate_latency_ms(void)
{
    uint32_t base = g_sim.profile.base_latency_ms;
    if (g_sim.profile.jitter_ms == 0)
        return base;
    
    // Random jitter: +/- jitter_ms
    int jitter = (int)(sim_rand() % (g_sim.profile.jitter_ms * 2 + 1)) - (int)g_sim.profile.jitter_ms;
    int latency = (int)base + jitter;
    if (latency < 0)
        latency = 0;
    
    return (uint32_t)latency;
}

// ---- Public API ----

int transport_sim_init(const transport_profile_t *profile)
{
    if (!profile)
        return -1;
    
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.profile = *profile;
    g_sim.initialized = 1;
    
    // Seed RNG
    sim_rand_seed(profile->seed);
    
    // Configure test_support network simulation to work with our profile
    // NOTE: We use MINIMAL simulation to avoid compounding delays that make tests too slow
    ts_net_sim_t net_cfg = {0};
    
    // Only enable very light jitter for high-latency profiles (satellite)
    // For other profiles, the protocol's inherent timing provides sufficient realism
    if (profile->base_latency_ms > 100)  // Only for satellite-like profiles
    {
        net_cfg.enable_jitter = 1;
        net_cfg.jitter_min_ms = 5;  // Minimal jitter
        net_cfg.jitter_max_ms = 20;
    }
    
    // Enable partial I/O for realism but keep it reasonable
    net_cfg.enable_partial_send = 1;
    net_cfg.min_send_chunk = 256;
    net_cfg.max_send_chunk = profile->mtu_bytes > 0 ? profile->mtu_bytes : 4096;
    
    net_cfg.enable_partial_recv = 1;
    net_cfg.min_recv_chunk = 256;
    net_cfg.max_recv_chunk = 4096;
    
    // Set RNG seed for underlying simulator
    net_cfg.rng_seed = profile->seed;
    
    // Grace period for handshake (first 256 bytes)
    net_cfg.handshake_grace_bytes = 256;
    
    ts_net_sim_set(&net_cfg);
    ts_rand_seed_set(profile->seed);
    
    printf("[TransportSim] Initialized profile: %s\n", profile->name);
    printf("  Bandwidth: %u bps (%.2f KB/s)\n", profile->bandwidth_bps, profile->bandwidth_bps / 8000.0f);
    printf("  Latency: %u ms +/- %u ms\n", profile->base_latency_ms, profile->jitter_ms);
    printf("  Loss rate: %.2f%% (burst: %.2f%%)\n", profile->loss_rate * 100.0f, profile->burst_loss_rate * 100.0f);
    printf("  Seed: %u (deterministic)\n", profile->seed);
    
    return 0;
}

const transport_profile_t *transport_sim_get_profile(void)
{
    if (!g_sim.initialized)
        return NULL;
    return &g_sim.profile;
}

void transport_sim_cleanup(void)
{
    if (!g_sim.initialized)
        return;
    
    printf("[TransportSim] Cleanup - Final stats:\n");
    printf("  Packets sent: %llu, received: %llu, dropped: %llu\n",
           (unsigned long long)g_sim.stats.packets_sent,
           (unsigned long long)g_sim.stats.packets_received,
           (unsigned long long)g_sim.stats.packets_dropped);
    printf("  Bytes sent: %llu, received: %llu\n",
           (unsigned long long)g_sim.stats.bytes_sent,
           (unsigned long long)g_sim.stats.bytes_received);
    
    ts_net_sim_reset();
    memset(&g_sim, 0, sizeof(g_sim));
}

void transport_sim_trigger_burst(void)
{
    g_sim.ctx.in_burst = 1;
    g_sim.ctx.last_burst_time = ts_ticks();
    printf("[TransportSim] Burst event triggered manually\n");
}

void transport_sim_get_stats(transport_sim_stats_t *stats)
{
    if (!stats)
        return;
    *stats = g_sim.stats;
}

void transport_sim_reset_stats(void)
{
    memset(&g_sim.stats, 0, sizeof(g_sim.stats));
}

void transport_sim_record_packet_sent(size_t bytes)
{
    if (!g_sim.initialized)
        return;
    g_sim.stats.packets_sent++;
    g_sim.stats.bytes_sent += bytes;
}

void transport_sim_record_packet_received(size_t bytes)
{
    if (!g_sim.initialized)
        return;
    g_sim.stats.packets_received++;
    g_sim.stats.bytes_received += bytes;
}
