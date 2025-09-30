#include "val_protocol.h"
#ifdef VAL_HOST_UTILITIES
#include "val_error_strings.h"
#endif
#include <stdio.h>
#include <string.h>

int main(void)
{
    // Build a minimal config with dummy pointers just to create a session object
    uint8_t buf[1024];
    val_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.buffers.send_buffer = buf;
    cfg.buffers.recv_buffer = buf;
    cfg.buffers.packet_size = sizeof(buf);
    cfg.filesystem.fopen = (void *(*)(void *, const char *, const char *))0x1; // non-null to pass validation
    cfg.filesystem.fread = (int (*)(void *, void *, size_t, size_t, void *))0x1;
    cfg.filesystem.fwrite = (int (*)(void *, const void *, size_t, size_t, void *))0x1;
    cfg.filesystem.fseek = (int (*)(void *, void *, long, int))0x1;
    cfg.filesystem.ftell = (long (*)(void *, void *))0x1;
    cfg.filesystem.fclose = (int (*)(void *, void *))0x1;
    cfg.transport.send = (int (*)(void *, const void *, size_t))0x1;
    cfg.transport.recv = (int (*)(void *, void *, size_t, size_t *, uint32_t))0x1;
    val_session_t *s = val_session_create(&cfg);
    if (!s)
    {
        printf("create failed\n");
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
#ifdef VAL_HOST_UTILITIES
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
    return 0;
}
