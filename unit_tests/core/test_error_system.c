#include "../../src/val_internal.h"
#include "../support/test_support.h"
#include "val_protocol.h"
#if VAL_ENABLE_ERROR_STRINGS
#include "val_error_strings.h"
#endif
#include <stdio.h>
#include <string.h>

// Note: ts_make_config installs real system hooks by default; no local stubs needed.

int main(void)
{
    ts_cancel_token_t wd = ts_start_timeout_guard(TEST_TIMEOUT_QUICK_MS, "error_system");

    // Create a valid session using the in-memory duplex transport via test support
    test_duplex_t d;
    test_duplex_init(&d, 1024, 4);
    uint8_t sb[1024];
    uint8_t rb[1024];
    val_config_t cfg;
    ts_make_config(&cfg, sb, rb, sizeof(sb), &d, VAL_RESUME_NEVER, 0);
    // Be explicit for this focused test
    cfg.system.get_ticks_ms = ts_ticks;
    cfg.system.delay_ms = ts_delay;
    val_session_t *s = NULL;
    uint32_t create_detail = 0;
    val_status_t rc = val_session_create(&cfg, &s, &create_detail);
    if (rc != VAL_OK || !s)
    {
        fprintf(stderr,
                "create failed rc=%d d=0x%08X\n"
                "  transport.send=%p recv=%p io_ctx=%p\n"
                "  system.ticks=%p delay=%p\n"
                "  buffers.sb=%p rb=%p P=%zu\n",
                (int)rc, (unsigned)create_detail,
                (void*)cfg.transport.send, (void*)cfg.transport.recv, cfg.transport.io_context,
                (void*)cfg.system.get_ticks_ms, (void*)cfg.system.delay_ms,
                cfg.buffers.send_buffer, cfg.buffers.recv_buffer, cfg.buffers.packet_size);
        fflush(stderr);
        return 1;
    }
    // Set and get a feature negotiation error with missing features context
    val_internal_set_last_error(s, VAL_ERR_FEATURE_NEGOTIATION, VAL_SET_MISSING_FEATURE(0x5));
    val_status_t code = VAL_OK;
    uint32_t detail = 0;
    (void)val_get_last_error(s, &code, &detail);
    if (code != VAL_ERR_FEATURE_NEGOTIATION)
    {
        printf("code mismatch\n");
        return 2;
    }
    {
        unsigned ctx = VAL_ERROR_CONTEXT(detail);
        unsigned low24 = detail & 0x00FFFFFFu;
        unsigned has_feat_flag = (detail & VAL_ERROR_DETAIL_FEATURE_MISSING) ? 1u : 0u;
        uint32_t mf = VAL_GET_MISSING_FEATURE(detail);
#if VAL_ENABLE_ERROR_STRINGS
        const char *dstr = val_error_detail_to_string(detail);
        fprintf(stderr, "DEBUG: detail=0x%08X ctx=%u low24=0x%06X FEAT_FLAG=%u decoded=0x%06X (%s)\n", (unsigned)detail, ctx,
                low24, has_feat_flag, (unsigned)mf, dstr);
#else
        fprintf(stderr, "DEBUG: detail=0x%08X ctx=%u low24=0x%06X FEAT_FLAG=%u decoded=0x%06X\n", (unsigned)detail, ctx, low24,
                has_feat_flag, (unsigned)mf);
#endif
        fflush(stderr);
        if (mf != 0x5)
        {
            fprintf(stderr, "detail payload mismatch (detail=0x%08X decoded=0x%06X)\n", (unsigned)detail, (unsigned)mf);
            fprintf(stdout, "detail payload mismatch (detail=0x%08X decoded=0x%06X)\n", (unsigned)detail, (unsigned)mf);
            fflush(stderr);
            fflush(stdout);
            return 3;
        }
    }
#ifdef VAL_HOST_UTILITIES
    const char *st = val_status_to_string(code);
    const char *ds = val_error_detail_to_string(detail);
    if (!st || !ds)
    {
        printf("strings null\n");
        return 4;
    }
    char rep[128];
    val_format_error_report(code, detail, rep, sizeof(rep));
    if (strlen(rep) == 0)
    {
        printf("report empty\n");
        return 5;
    }
#endif
    val_session_destroy(s);
    test_duplex_free(&d);
    
    ts_cancel_timeout_guard(wd);
    return 0;
}
