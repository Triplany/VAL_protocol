#include "../../src/val_internal.h"
#include "../support/test_support.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    // Prepare a valid config using test helpers
    uint8_t send_buf[2048];
    uint8_t recv_buf[2048];
    test_duplex_t link;
    test_duplex_init(&link, sizeof(send_buf), 8);

    val_config_t cfg;
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &link, VAL_RESUME_NEVER, 0);
    // Ensure hooks are explicitly set for this test
    cfg.system.get_ticks_ms = ts_ticks;
    cfg.system.delay_ms = ts_delay;

    // Remove clock on purpose and expect session creation to fail (clock is always required)
    cfg.system.get_ticks_ms = NULL;
    val_session_t *s = NULL;
    uint32_t d = 0;
    val_status_t rc = val_session_create(&cfg, &s, &d);
    if (rc == VAL_OK || s != NULL)
    {
        fprintf(stderr, "Expected val_session_create to fail without clock (always required)\n");
        return 1;
    }

    // Provide a clock and expect success
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &link, VAL_RESUME_NEVER, 0);
    cfg.system.get_ticks_ms = ts_ticks;
    cfg.system.delay_ms = ts_delay;
    s = NULL;
    d = 0;
    rc = val_session_create(&cfg, &s, &d);
    if (rc != VAL_OK || !s)
    {
        fprintf(stderr, "val_session_create failed unexpectedly with a clock present (rc=%d d=0x%08X)\n", (int)rc, (unsigned)d);
        return 1;
    }
    val_session_destroy(s);

    test_duplex_free(&link);
    return 0;
}
