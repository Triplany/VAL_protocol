#include "../../src/val_internal.h"
#include "val_protocol.h"
#include <stdio.h>
#include <string.h>

// Deterministic fake clock for tests
static uint32_t g_now_ms = 0;
static uint32_t ut_ticks(void)
{
    return g_now_ms;
}

static int expect_eq_u32(const char *name, uint32_t a, uint32_t b)
{
    if (a != b)
    {
        fprintf(stderr, "EXPECT %s: %u != %u\n", name, (unsigned)a, (unsigned)b);
        return 0;
    }
    return 1;
}

static val_session_t *make_session_with_bounds(uint32_t min_ms, uint32_t max_ms, int with_clock)
{
    uint8_t buf[1024];
    val_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffers.send_buffer = buf;
    cfg.buffers.recv_buffer = buf;
    cfg.buffers.packet_size = sizeof(buf);
    // Minimal non-null hooks so validation passes
    cfg.filesystem.fopen = (void *(*)(void *, const char *, const char *))0x1;
    cfg.filesystem.fread = (int (*)(void *, void *, size_t, size_t, void *))0x1;
    cfg.filesystem.fwrite = (int (*)(void *, const void *, size_t, size_t, void *))0x1;
    cfg.filesystem.fseek = (int (*)(void *, void *, long, int))0x1;
    cfg.filesystem.ftell = (long (*)(void *, void *))0x1;
    cfg.filesystem.fclose = (int (*)(void *, void *))0x1;
    cfg.transport.send = (int (*)(void *, const void *, size_t))0x1;
    cfg.transport.recv = (int (*)(void *, void *, size_t, size_t *, uint32_t))0x1;
    if (with_clock)
        cfg.system.get_ticks_ms = ut_ticks;
    cfg.timeouts.min_timeout_ms = min_ms;
    cfg.timeouts.max_timeout_ms = max_ms;
    val_session_t *s = NULL;
    uint32_t d = 0;
    val_status_t rc = val_session_create(&cfg, &s, &d);
    (void)rc;
    (void)d;
    return s;
}

int main(void)
{
    int ok = 1;

    // Case 1: Initialization and clamping to max when seed RTO exceeds bounds
    {
        val_session_t *s = make_session_with_bounds(100, 10000, 1);
        if (!s)
            return 1;
        // Seed per implementation: srtt = max/2 = 5000, rttvar = max/4 = 2500
        // base = 5000 + 4*2500 = 15000; DATA_ACK mul=3 -> 45000; clamp to 10000
        uint32_t rto_data = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
        ok &= expect_eq_u32("init clamp to max (DATA_ACK)", rto_data, 10000);
        // HANDSHAKE mul=5 -> 75000; clamp to 10000
        uint32_t rto_hs = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
        ok &= expect_eq_u32("init clamp to max (HANDSHAKE)", rto_hs, 10000);
        val_session_destroy(s);
    }

    // Case 2: First RTT sample sets SRTT/RTTVAR to RTT and RTT/2 respectively
    {
        val_session_t *s = make_session_with_bounds(100, 10000, 1);
        if (!s)
            return 2;
        // Simulate measuring a 200 ms RTT
        g_now_ms = 1000;
        s->timing.in_retransmit = 0;
        val_internal_record_rtt(s, 200);
        // After first sample: srtt = 200, rttvar = 100
        // DATA_ACK: base = 200 + 4*100 = 600; *3 => 1800 (between 100..10000)
        uint32_t to_data = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
        ok &= expect_eq_u32("first sample DATA_ACK", to_data, 1800);
        // HANDSHAKE: mul=5 => 3000
        uint32_t to_hs = val_internal_get_timeout(s, VAL_OP_HANDSHAKE);
        ok &= expect_eq_u32("first sample HANDSHAKE", to_hs, 3000);

        // Case 3: Subsequent sample updates SRTT/RTTVAR with alpha=1/8, beta=1/4
        // New RTT = 400
        val_internal_record_rtt(s, 400);
        // srtt' = 7/8*200 + 1/8*400 = 175 + 50 = 225
        // rttvar' = 3/4*100 + 1/4*|200-400| = 75 + 50 = 125
        // DATA_ACK: base = 225 + 4*125 = 725; *3 => 2175
        uint32_t to_data2 = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
        ok &= expect_eq_u32("second sample DATA_ACK", to_data2, 2175);

        // Case 4: Karn's algorithm — ignore RTT during retransmit
        s->timing.in_retransmit = 1;
        // Try to "record" an RTT of 5000ms; should be ignored and keep same RTO
        val_internal_record_rtt(s, 5000);
        uint32_t to_data3 = val_internal_get_timeout(s, VAL_OP_DATA_ACK);
        ok &= expect_eq_u32("karn ignore retransmit sample", to_data3, 2175);
        s->timing.in_retransmit = 0;

        val_session_destroy(s);
    }

    // No no-clock fallback: a clock is always required in VAL builds.

    return ok ? 0 : 10;
}
