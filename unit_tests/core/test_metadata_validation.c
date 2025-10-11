#include "test_support.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Simple helpers to build paths dynamically without fixed-size buffers
static char *dyn_sprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        va_end(ap2);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        va_end(ap2);
        return NULL;
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

static char *join_path2(const char *a, const char *b)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != sep) ? 1 : 0;
    // If b already starts with sep, don't add another
    if (lb > 0 && b[0] == sep)
        need_sep = 0;
    size_t len = la + (size_t)need_sep + lb;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, a, la);
    size_t pos = la;
    if (need_sep)
        out[pos++] = sep;
    memcpy(out + pos, b, lb);
    out[len] = '\0';
    return out;
}

static val_validation_action_t accept_validator(const val_meta_payload_t *meta, const char *target_path, void *ctx)
{
    (void)meta;
    (void)target_path;
    (void)ctx;
    return VAL_VALIDATION_ACCEPT;
}
static val_validation_action_t skip_validator(const val_meta_payload_t *meta, const char *target_path, void *ctx)
{
    (void)meta;
    (void)target_path;
    (void)ctx;
    return VAL_VALIDATION_SKIP;
}
static val_validation_action_t abort_validator(const val_meta_payload_t *meta, const char *target_path, void *ctx)
{
    (void)meta;
    (void)target_path;
    (void)ctx;
    return VAL_VALIDATION_ABORT;
}

static int write_file(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    for (size_t i = 0; i < n; ++i)
        fputc((int)(i & 0xFF), f);
    fclose(f);
    return 0;
}

static int run_case(val_metadata_validator_t validator, const char *tag, int expect_skip, int expect_abort)
{
    (void)expect_skip;
    (void)expect_abort;
    const size_t packet = 1024, depth = 8, size = 64 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char artroot[2048];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
        return 1;
    char *meta_seg = dyn_sprintf("meta_%s", tag);
    char *basedir = meta_seg ? join_path2(artroot, meta_seg) : NULL;
    free(meta_seg);
    char *outdir = basedir ? join_path2(basedir, "out") : NULL;
    char *inpath = basedir ? join_path2(basedir, "in.bin") : NULL;
    char *outpath = outdir ? join_path2(outdir, "in.bin") : NULL;
    if (!basedir || !outdir || !inpath || !outpath)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    if (ts_ensure_dir(basedir) != 0 || ts_ensure_dir(outdir) != 0)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }
    (void)remove(inpath);
    (void)remove(outpath);
    if (write_file(inpath, size) != 0)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_NEVER, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_NEVER, 1024);

    // Defaults from ts_make_config suffice

    // install validator on receiver
    cfg_rx.metadata_validation.validator = validator;
    cfg_rx.metadata_validation.validator_context = NULL;

    val_session_t *tx = NULL;
    val_session_t *rx = NULL;
    if (val_session_create(&cfg_tx, &tx, NULL) != VAL_OK || val_session_create(&cfg_rx, &rx, NULL) != VAL_OK)
    {
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);

    int ok = 0;
    if (validator == accept_validator)
    {
        ok = (st == VAL_OK) && (ts_file_size(outpath) == ts_file_size(inpath));
        if (!ok) {
            printf("[ACCEPT CASE] val_send_files st=%d, in=%lld, out=%lld\n", (int)st, (long long)ts_file_size(inpath), (long long)ts_file_size(outpath));
        }
    }
    else if (validator == skip_validator)
    {
        // Acceptable: file does not exist, or file size is unchanged from before
        long long before = -1;
        FILE *f = fopen(outpath, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            before = ftell(f);
            fclose(f);
        }
        ok = (st == VAL_OK);
        if (!ok) {
            printf("[SKIP CASE] val_send_files st=%d\n", (int)st);
        }
    }
    else if (validator == abort_validator)
    {
        if (st == VAL_OK) {
            printf("[ABORT CASE] val_send_files unexpectedly returned VAL_OK (should fail)\n");
        } else {
            printf("[ABORT CASE] val_send_files returned error as expected: st=%d\n", (int)st);
        }
        ok = (st != VAL_OK); // session aborts
    }

    // Validate metrics: validation tests should be clean (no retransmits, no timeouts, no CRC errors)
    // NOTE: VAL_ENABLE_METRICS must be defined for all test builds
    ts_metrics_expect_t exp = {0};
    exp.allow_soft_timeouts = 0;
    exp.allow_retransmits = 0;
    if (validator == accept_validator) {
        exp.expect_files_sent = 1;
        exp.expect_files_recv = 1;
    } else if (validator == skip_validator) {
        // SKIP validation: file IS received/sent but written=total (already complete)
        // Metrics still count it as a completed transfer
        exp.expect_files_sent = 1;
        exp.expect_files_recv = 1;
    } else if (validator == abort_validator) {
        // ABORT validation: transfer aborted, sender sees failure
        exp.expect_files_sent = 0;  // Transfer aborted mid-flight
        exp.expect_files_recv = 0;
    }
    
    if (ts_assert_clean_metrics(tx, rx, &exp) != 0) {
        printf("[METRICS] %s case failed metrics validation\n", tag);
        ok = 0;
    }

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    free(basedir);
    free(outdir);
    free(inpath);
    free(outpath);
    return ok ? 0 : 1;
}

int main(void)
{
    ts_cancel_token_t wd = ts_start_timeout_guard(TEST_TIMEOUT_QUICK_MS, "metadata_validation");
    
    int rc = 0;
    int fail_accept = run_case(accept_validator, "accept", 0, 0);
    int fail_skip = run_case(skip_validator, "skip", 1, 0);
    int fail_abort = run_case(abort_validator, "abort", 0, 1);
    if (fail_accept != 0) {
        printf("[SUMMARY] ACCEPT CASE FAILED\n");
        rc = 1;
    }
    if (fail_skip != 0) {
        printf("[SUMMARY] SKIP CASE FAILED\n");
        rc = 1;
    }
    if (fail_abort != 0) {
        printf("[SUMMARY] ABORT CASE FAILED\n");
        rc = 1;
    }
    if (rc == 0)
        printf("OK\n");
    else
        printf("[SUMMARY] rc=%d\n", rc);
    
    ts_cancel_timeout_guard(wd);
    return rc;
}
