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
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &link, VAL_RESUME_NONE, 0);

    // Remove clock on purpose and expect session creation to fail when VAL_REQUIRE_CLOCK=1
    cfg.system.get_ticks_ms = NULL;
    val_session_t *s = val_session_create(&cfg);
#if VAL_REQUIRE_CLOCK
    if (s != NULL)
    {
        fprintf(stderr, "Expected val_session_create to fail without clock when VAL_REQUIRE_CLOCK=1\n");
        return 1;
    }
#else
    if (s == NULL)
    {
        fprintf(stderr, "Expected val_session_create to succeed without clock when VAL_REQUIRE_CLOCK=0\n");
        return 1;
    }
    val_session_destroy(s);
#endif

    // Provide a clock and expect success
    ts_make_config(&cfg, send_buf, recv_buf, sizeof(send_buf), &link, VAL_RESUME_NONE, 0);
    // Ensure we have a clock
    if (!cfg.system.get_ticks_ms)
    {
        fprintf(stderr, "Test helper did not provide a clock\n");
        return 1;
    }
    s = val_session_create(&cfg);
    if (!s)
    {
        fprintf(stderr, "val_session_create failed unexpectedly with a clock present\n");
        return 1;
    }
    val_session_destroy(s);

    test_duplex_free(&link);
    return 0;
}
