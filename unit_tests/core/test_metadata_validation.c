#include "../../src/val_internal.h"
#include "test_support.h"
#include <stdio.h>
#include <string.h>

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
    const size_t packet = 1024, depth = 8, size = 64 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);

    char artroot[1024];
    if (!ts_get_artifacts_root(artroot, sizeof(artroot)))
        return 1;
#if defined(_WIN32)
    char basedir[1024];
    snprintf(basedir, sizeof(basedir), "%s\\meta_%s", artroot, tag);
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s\\out", basedir);
    char inpath[1024];
    snprintf(inpath, sizeof(inpath), "%s\\in.bin", basedir);
    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s\\in.bin", outdir);
#else
    char basedir[1024];
    snprintf(basedir, sizeof(basedir), "%s/meta_%s", artroot, tag);
    char outdir[1024];
    snprintf(outdir, sizeof(outdir), "%s/out", basedir);
    char inpath[1024];
    snprintf(inpath, sizeof(inpath), "%s/in.bin", basedir);
    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s/in.bin", outdir);
#endif
    if (ts_ensure_dir(basedir) != 0 || ts_ensure_dir(outdir) != 0)
        return 1;
    (void)remove(inpath);
    (void)remove(outpath);
    if (write_file(inpath, size) != 0)
        return 1;

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};

    val_config_t cfg_tx, cfg_rx;
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_APPEND, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_APPEND, 1024);

    // install validator on receiver
    cfg_rx.metadata_validation.validator = validator;
    cfg_rx.metadata_validation.validator_context = NULL;

    val_session_t *tx = val_session_create(&cfg_tx);
    val_session_t *rx = val_session_create(&cfg_rx);
    if (!tx || !rx)
        return 1;

    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {inpath};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);

    int ok = 0;
    if (validator == accept_validator)
    {
        ok = (st == VAL_OK) && (ts_file_size(outpath) == ts_file_size(inpath));
    }
    else if (validator == skip_validator)
    {
        ok = (st == VAL_OK) && (ts_file_size(outpath) == 0); // skipped means receiver didn't write
    }
    else if (validator == abort_validator)
    {
        ok = (st != VAL_OK); // session aborts
    }

    val_session_destroy(tx);
    val_session_destroy(rx);
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);
    return ok ? 0 : 1;
}

int main(void)
{
    int rc = 0;
    if (run_case(accept_validator, "accept", 0, 0) != 0)
        rc = 1;
    if (run_case(skip_validator, "skip", 1, 0) != 0)
        rc = 1;
    if (run_case(abort_validator, "abort", 0, 1) != 0)
        rc = 1;
    if (rc == 0)
        printf("OK\n");
    return rc;
}
