#ifndef TRANSPORT_PROFILES_H
#define TRANSPORT_PROFILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Transport profile configuration
    // Simulates realistic network/serial link characteristics for protocol testing
    typedef struct
    {
        const char *name;             // Profile name for logging
        uint32_t bandwidth_bps;       // Bits per second (0 = unlimited)
        uint32_t base_latency_ms;     // Base one-way latency
        uint32_t jitter_ms;           // +/- latency variation
        float loss_rate;              // Baseline packet loss (0.0 - 1.0)
        float burst_loss_rate;        // Loss rate during burst events
        uint32_t burst_duration_ms;   // How long bursts last
        uint32_t burst_interval_ms;   // Time between burst events (0 = no bursts)
        uint32_t mtu_bytes;           // Maximum transmission unit (0 = no limit)
        uint32_t seed;                // RNG seed for determinism
    } transport_profile_t;

    // Pre-defined transport profiles

    // UART 115200 baud: Low bandwidth, low latency, high reliability
    // Effective throughput: ~11.5 KB/s (115200 bps with 8N1 framing overhead)
    // Typical for embedded serial communications
    extern const transport_profile_t PROFILE_UART_115200;

    // WiFi 2.4GHz - Good conditions: Medium bandwidth, moderate latency, occasional loss
    // Simulates good WiFi signal with occasional interference
    extern const transport_profile_t PROFILE_WIFI_GOOD;

    // WiFi 2.4GHz - Poor conditions: Variable bandwidth, high latency, frequent loss
    // Simulates poor WiFi signal with interference and congestion
    extern const transport_profile_t PROFILE_WIFI_POOR;

    // Satellite GEO: Moderate bandwidth, very high latency, occasional loss
    // Simulates geostationary satellite (250-600ms RTT)
    extern const transport_profile_t PROFILE_SATELLITE_GEO;

    // Satellite LEO (e.g., Starlink): Good bandwidth, moderate latency, occasional loss
    // Simulates low-earth orbit satellite (20-40ms RTT)
    extern const transport_profile_t PROFILE_SATELLITE_LEO;

    // Transport profile simulator state (opaque)
    typedef struct transport_sim_state_s transport_sim_state_t;

    // Initialize transport simulator with a profile
    // Must be called before creating test sessions
    // Returns 0 on success, -1 on failure
    int transport_sim_init(const transport_profile_t *profile);

    // Get current profile (NULL if not initialized)
    const transport_profile_t *transport_sim_get_profile(void);

    // Cleanup transport simulator
    void transport_sim_cleanup(void);

    // Advanced: manually trigger a burst loss event
    void transport_sim_trigger_burst(void);

    // Advanced: get statistics
    typedef struct
    {
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint64_t packets_sent;
        uint64_t packets_received;
        uint64_t packets_dropped;
        uint32_t avg_latency_ms;
        uint32_t max_latency_ms;
    } transport_sim_stats_t;

    void transport_sim_get_stats(transport_sim_stats_t *stats);
    void transport_sim_reset_stats(void);

    // Record packet/byte statistics (called by transport layer)
    void transport_sim_record_packet_sent(size_t bytes);
    void transport_sim_record_packet_received(size_t bytes);

#ifdef __cplusplus
}
#endif

#endif // TRANSPORT_PROFILES_H
