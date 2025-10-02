#include "../../src/val_internal.h"
#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
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

// Use shared helpers: ts_dyn_sprintf, ts_join_path_dyn, ts_build_case_dirs, ts_remove_file, ts_write_pattern_file

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
    char *meta_seg = ts_dyn_sprintf("meta_%s", tag);
    char *basedir = meta_seg ? ts_join_path_dyn(artroot, meta_seg) : NULL;
    free(meta_seg);
    char *outdir = basedir ? ts_join_path_dyn(basedir, "out") : NULL;
    char *inpath = basedir ? ts_join_path_dyn(basedir, "in.bin") : NULL;
    char *outpath = outdir ? ts_join_path_dyn(outdir, "in.bin") : NULL;
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
    ts_remove_file(inpath);
    ts_remove_file(outpath);
    if (ts_write_pattern_file(inpath, size) != 0)
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
    ts_make_config(&cfg_tx, sb_a, rb_a, packet, &end_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);
    ts_make_config(&cfg_rx, sb_b, rb_b, packet, &end_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 1024);

    // install validator on receiver
    cfg_rx.metadata_validation.validator = validator;
    cfg_rx.metadata_validation.validator_context = NULL;

    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(&cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(&cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X, rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        free(basedir);
        free(outdir);
        free(inpath);
        free(outpath);
        return 1;
    }

    ts_thread_t th = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(&cfg_tx, 5);
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
    free(basedir);
    free(outdir);
    free(inpath);
    free(outpath);
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
