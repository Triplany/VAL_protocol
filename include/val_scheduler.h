#ifndef VAL_SCHEDULER_H
#define VAL_SCHEDULER_H

#include "val_internal.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transmit-side scheduler helpers
// Returns true when streaming overlay should be used for tight polling and ungated window, else false.
static VAL_FORCE_INLINE bool val_tx_should_stream(val_session_t *s)
{
    if (!s)
        return false;
#if VAL_ENABLE_STREAMING
    return (s->send_streaming_allowed && s->cfg.adaptive_tx.allow_streaming && s->streaming_engaged) ? true : false;
#else
    (void)s;
    return false;
#endif
}

// Compute a poll timeout to use while waiting for ACKs; returns a tight value when streaming, else base.
static VAL_FORCE_INLINE uint32_t val_tx_poll_ms(val_session_t *s, uint32_t base_timeout_ms)
{
    if (!s)
        return 0;
#if VAL_ENABLE_STREAMING
    if (val_tx_should_stream(s))
    {
        // Tight micro-poll derived from SRTT; clamp to [2, 20] ms
        uint32_t base = (s->timing.samples_taken ? s->timing.srtt_ms
                                                 : (s->cfg.timeouts.min_timeout_ms ? s->cfg.timeouts.min_timeout_ms : 40));
        uint32_t candidate = (base / 4u) ? (base / 4u) : 10u;
        if (candidate < 2u) candidate = 2u;
        if (candidate > 20u) candidate = 20u;
        return candidate;
    }
#endif
    // Non-streaming: caller should use base timeout
    return base_timeout_ms;
}

// Receive-side scheduler helpers
// Map peer mode to ACK stride (once per window rung)
static VAL_FORCE_INLINE uint32_t val_rx_ack_stride_from_mode(val_tx_mode_t peer_mode)
{
    switch (peer_mode)
    {
    case VAL_TX_WINDOW_2:  return 2u;
    case VAL_TX_WINDOW_4:  return 4u;
    case VAL_TX_WINDOW_8:  return 8u;
    case VAL_TX_WINDOW_16: return 16u;
    case VAL_TX_WINDOW_32: return 32u;
    case VAL_TX_WINDOW_64: return 64u;
    case VAL_TX_STOP_AND_WAIT: return 1u;
    default: return 2u;
    }
}

// Best-effort sparse heartbeat ACK when idle; no-ops if streaming disabled or not engaged.
static VAL_FORCE_INLINE void val_rx_maybe_send_heartbeat(val_session_t *s,
                                                         uint64_t high_water,
                                                         uint32_t interval_ms,
                                                         uint32_t *hb_last_ms,
                                                         uint32_t *last_ack_ms,
                                                         uint32_t last_data_ms)
{
    (void)high_water; (void)interval_ms; (void)hb_last_ms; (void)last_ack_ms; (void)last_data_ms;
    if (!s)
        return;
#if VAL_ENABLE_STREAMING
    if (!hb_last_ms || !last_ack_ms)
        return;
    if (!(s->recv_streaming_allowed && s->peer_streaming_engaged))
        return;
    uint32_t now = s->config->system.get_ticks_ms ? s->config->system.get_ticks_ms() : 0u;
    if (now == 0u)
        return;
    int idle_no_data = (last_data_ms == 0 || (now - last_data_ms) >= interval_ms);
    int no_recent_ack = (*last_ack_ms == 0 || (now - *last_ack_ms) >= interval_ms);
    if (idle_no_data && no_recent_ack && (*hb_last_ms == 0 || (now - *hb_last_ms) >= interval_ms))
    {
        VAL_LOG_DEBUGF(s, "data: heartbeat DATA_ACK off=%llu", (unsigned long long)high_water);
        (void)val_internal_send_packet(s, VAL_PKT_DATA_ACK, NULL, 0, high_water);
        *hb_last_ms = now;
        *last_ack_ms = now;
    }
#else
    (void)s; // streaming disabled => no-op
#endif
}

#ifdef __cplusplus
}
#endif

#endif // VAL_SCHEDULER_H