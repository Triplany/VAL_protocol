#include "../../src/val_internal.h"
#include "test_support.h"
#include <stdio.h>
#include <string.h>

static int fake_is_connected(void *ctx)
{
    (void)ctx;
    return 0; // report disconnected
}

int main(void)
{
    // Build a minimal session config with in-memory duplex and NO optional hooks
    test_duplex_t d;
    test_duplex_init(&d, 2048, 4);
    uint8_t send_buf[2048];
    uint8_t recv_buf[2048];
    val_config_t cfg;
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &d, VAL_RESUME_NEVER, 0);
    // Deliberately do not set is_connected/flush to ensure wrappers treat as connected/no-op
    val_session_t *s = NULL;
    uint32_t detail = 0;
    val_status_t rc = val_session_create(&cfg, &s, &detail);
    if (rc != VAL_OK || !s)
        return 1;
    // Since no handshake happened, just attempt to send a small control packet; should succeed (transport in-mem)
    val_status_t st = val_internal_send_packet(s, VAL_PKT_ERROR, NULL, 0, 0);
    if (st != VAL_OK)
    {
        fprintf(stderr, "send without hooks failed: %d\n", (int)st);
        val_session_destroy(s);
        test_duplex_free(&d);
        return 2;
    }
    val_session_destroy(s);

    // Now set an is_connected hook that reports disconnected; send should fail fast with IO error
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &d, VAL_RESUME_NEVER, 0);
    cfg.transport.is_connected = fake_is_connected;
    s = NULL;
    detail = 0;
    rc = val_session_create(&cfg, &s, &detail);
    if (rc != VAL_OK || !s)
    {
        test_duplex_free(&d);
        return 3;
    }
    st = val_internal_send_packet(s, VAL_PKT_ERROR, NULL, 0, 0);
    if (st == VAL_OK)
    {
        fprintf(stderr, "send should have failed when disconnected\n");
        val_session_destroy(s);
        test_duplex_free(&d);
        return 4;
    }
    val_session_destroy(s);
    test_duplex_free(&d);
    return 0;
}
