#ifndef VAL_SCHEDULER_H
#define VAL_SCHEDULER_H

#include "val_internal.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transmit-side scheduler helpers
// In 0.7 bounded-window protocol, streaming overlay is removed. Always return false.
static VAL_FORCE_INLINE bool val_tx_should_stream(val_session_t *s)
{
    (void)s;
    return false; // streaming removed
}

// Compute a poll timeout to use while waiting for ACKs; returns a tight value when streaming, else base.
static VAL_FORCE_INLINE uint32_t val_tx_poll_ms(val_session_t *s, uint32_t base_timeout_ms)
{
    if (!s)
        return 0;
    (void)s;
    // Non-streaming: caller should use base timeout
    return base_timeout_ms;
}

// Receive-side: legacy mode mapping removed. ACK policy is governed by negotiated ack_stride/window.

// Best-effort sparse heartbeat ACK when idle; no-ops if streaming disabled or not engaged.
static VAL_FORCE_INLINE void val_rx_maybe_send_heartbeat(val_session_t *s,
                                                         uint64_t high_water,
                                                         uint32_t interval_ms,
                                                         uint32_t *hb_last_ms,
                                                         uint32_t *last_ack_ms,
                                                         uint32_t last_data_ms)
{
    (void)s; (void)high_water; (void)interval_ms; (void)hb_last_ms; (void)last_ack_ms; (void)last_data_ms;
    return; // no-op in bounded-window model
}

#ifdef __cplusplus
}
#endif

#endif // VAL_SCHEDULER_H