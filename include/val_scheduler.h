#ifndef VAL_SCHEDULER_H
#define VAL_SCHEDULER_H

#include "val_internal.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transmit-side scheduler helpers for bounded-window flow control
// The protocol uses packet-count based windowing with adaptive congestion control (AIMD).
// These functions provide compatibility shims for internal scheduler queries.

// Query whether streaming mode is active (always returns false in bounded-window protocol)
static VAL_FORCE_INLINE bool val_tx_should_stream(val_session_t *s)
{
    (void)s;
    return false; // bounded-window protocol only
}

// Compute poll timeout for waiting on ACKs
// Returns the base timeout value as bounded-window protocol uses fixed timeout semantics
static VAL_FORCE_INLINE uint32_t val_tx_poll_ms(val_session_t *s, uint32_t base_timeout_ms)
{
    if (!s)
        return 0;
    (void)s;
    return base_timeout_ms;
}

// Receive-side ACK policy is governed by negotiated ack_stride and window parameters

// Send heartbeat ACK when idle (no-op in bounded-window protocol)
// Bounded-window protocol uses stride-based ACKs; no separate heartbeat mechanism
static VAL_FORCE_INLINE void val_rx_maybe_send_heartbeat(val_session_t *s,
                                                         uint64_t high_water,
                                                         uint32_t interval_ms,
                                                         uint32_t *hb_last_ms,
                                                         uint32_t *last_ack_ms,
                                                         uint32_t last_data_ms)
{
    (void)s; (void)high_water; (void)interval_ms; (void)hb_last_ms; (void)last_ack_ms; (void)last_data_ms;
    return; // no-op in bounded-window protocol
}

#ifdef __cplusplus
}
#endif

#endif // VAL_SCHEDULER_H