#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#endif

static int write_pattern_file(const char *path, size_t size, int mode)
{
    // mode 0: i & 0xFF; mode 1: (i*13)&0xFF
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    for (size_t i = 0; i < size; ++i)
    {
        int v = (mode == 0) ? (int)(i & 0xFF) : (int)((i * 13) & 0xFF);
        fputc(v, f);
    }
    fclose(f);
    return 0;
}

static int copy_prefix(const char *src, const char *dst, size_t bytes)
{
    FILE *fi = fopen(src, "rb");
    FILE *fo = fopen(dst, "wb");
    if (!fi || !fo)
    {
        if (fi)
            fclose(fi);
        if (fo)
            fclose(fo);
        return -1;
    }
    uint8_t buf[8192];
    size_t left = bytes;
    while (left)
    {
        size_t n = left > sizeof(buf) ? sizeof(buf) : left;
        size_t r = fread(buf, 1, n, fi);
        if (r == 0)
            break;
        fwrite(buf, 1, r, fo);
        left -= r;
    }
    fclose(fi);
    fclose(fo);
    return 0;
}

static void make_paths(char *root, size_t rootsz, char *dir, size_t dirsz, char *indir, size_t indirsz, char *outdir,
                       size_t outdirsz, char *infile, size_t infilesz, char *outfile, size_t outfilesz, const char *subdir)
{
    // Build paths under artifacts root: <root>/<subdir>/{in,out}
    if (!ts_get_artifacts_root(root, rootsz))
    {
#if defined(_WIN32)
        snprintf(root, rootsz, ".\\ut_artifacts");
#else
        snprintf(root, rootsz, "./ut_artifacts");
#endif
    }
#if defined(_WIN32)
    snprintf(dir, dirsz, "%s\\%s", root, subdir);
    snprintf(indir, indirsz, "%s\\in", dir);
    snprintf(outdir, outdirsz, "%s\\out", dir);
    snprintf(infile, infilesz, "%s\\file.bin", indir);
    snprintf(outfile, outfilesz, "%s\\file.bin", outdir);
#else
    snprintf(dir, dirsz, "%s/%s", root, subdir);
    snprintf(indir, indirsz, "%s/in", dir);
    snprintf(outdir, outdirsz, "%s/out", dir);
    snprintf(infile, infilesz, "%s/file.bin", indir);
    snprintf(outfile, outfilesz, "%s/file.bin", outdir);
#endif
    ts_ensure_dir(root);
    ts_ensure_dir(dir);
    ts_ensure_dir(indir);
    ts_ensure_dir(outdir);
}

static void make_cfgs(val_config_t *cfg_tx, val_config_t *cfg_rx, test_duplex_t *d_tx, test_duplex_t *d_rx, void *sb_a,
                      void *rb_a, void *sb_b, void *rb_b, size_t packet)
{
    ts_make_config(cfg_tx, sb_a, rb_a, packet, d_tx, VAL_RESUME_CRC_TAIL_OR_ZERO, 8192);
    ts_make_config(cfg_rx, sb_b, rb_b, packet, d_rx, VAL_RESUME_CRC_TAIL_OR_ZERO, 8192);
    ts_set_console_logger(cfg_tx);
    ts_set_console_logger(cfg_rx);
}

static int run_send_recv(const char *in, const char *outdir, val_config_t *cfg_tx, val_config_t *cfg_rx, val_status_t *st_out)
{
    val_session_t *tx = NULL, *rx = NULL;
    uint32_t dtx = 0, drx = 0;
    val_status_t rctx = val_session_create(cfg_tx, &tx, &dtx);
    val_status_t rcrx = val_session_create(cfg_rx, &rx, &drx);
    if (rctx != VAL_OK || rcrx != VAL_OK || !tx || !rx)
    {
        fprintf(stderr, "session create failed (tx rc=%d d=0x%08X rx rc=%d d=0x%08X)\n", (int)rctx, (unsigned)dtx, (int)rcrx,
                (unsigned)drx);
        if (tx)
            val_session_destroy(tx);
        if (rx)
            val_session_destroy(rx);
        return -1;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    val_session_destroy(tx);
    val_session_destroy(rx);
    if (st_out)
        *st_out = st;
    return 0;
}

static int test_always_skip_if_exists(void)
{
    const size_t packet = 1024, depth = 16, size = 128 * 1024 + 9;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char root[512], dir[512], indir[512], outdir[512], in[512], out[512];
    make_paths(root, sizeof(root), dir, sizeof(dir), indir, sizeof(indir), outdir, sizeof(outdir), in, sizeof(in), out,
               sizeof(out), "policies_skip_exist");

    // Create input and an identical existing output
    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    if (copy_prefix(in, out, size) != 0)
        return 1;
    uint64_t before_size = ts_file_size(out);
    uint32_t before_crc = ts_file_crc32(out);

    // Configure sessions; set receiver policy ALWAYS_SKIP_IF_EXISTS
    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = VAL_RESUME_SKIP_EXISTING;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
    {
        fprintf(stderr, "ALWAYS_SKIP_IF_EXISTS: send status %d\n", st);
        return 3;
    }
    // Ensure output unchanged
    uint64_t after_size = ts_file_size(out);
    uint32_t after_crc = ts_file_crc32(out);
    if (before_size != after_size || before_crc != after_crc)
    {
        fprintf(stderr, "ALWAYS_SKIP_IF_EXISTS: output changed\n");
        return 4;
    }
    return 0;
}

static int test_strict_resume_only_abort_on_mismatch(void)
{
    const size_t packet = 1024, depth = 16, size = 192 * 1024 + 5;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char root[512], dir[512], indir[512], outdir[512], in[512], out[512];
    make_paths(root, sizeof(root), dir, sizeof(dir), indir, sizeof(indir), outdir, sizeof(outdir), in, sizeof(in), out,
               sizeof(out), "policies_strict_abort");

    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    // Create an existing different output of same size to force verify mismatch
    if (write_pattern_file(out, size, 1) != 0)
        return 1;
    uint64_t before_size = ts_file_size(out);
    uint32_t before_crc = ts_file_crc32(out);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = VAL_RESUME_CRC_FULL;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    // New behavior: CRC_FULL mismatch should skip the file (no overwrite) and continue session.
    if (st != VAL_OK)
    {
        fprintf(stderr, "CRC_FULL mismatch expected skip/continue, got status %d\n", st);
        return 3;
    }
    // Ensure output unchanged after skip
    uint64_t after_size = ts_file_size(out);
    uint32_t after_crc = ts_file_crc32(out);
    if (before_size != after_size || before_crc != after_crc)
    {
        fprintf(stderr, "STRICT_RESUME_ONLY: output changed after abort\n");
        return 4;
    }
    return 0;
}

static int test_always_start_zero_overwrite(void)
{
    const size_t packet = 1024, depth = 16, size = 256 * 1024 + 17;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char root[512], dir[512], indir[512], outdir[512], in[512], out[512];
    make_paths(root, sizeof(root), dir, sizeof(dir), indir, sizeof(indir), outdir, sizeof(outdir), in, sizeof(in), out,
               sizeof(out), "policies_start_zero");

    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    // Precreate partial output to ensure overwrite
    if (copy_prefix(in, out, size / 3) != 0)
        return 1;

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = VAL_RESUME_NEVER;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
    {
        fprintf(stderr, "ALWAYS_START_ZERO: send status %d\n", st);
        return 3;
    }
    // Validate full size and CRC equality
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    if (sz_in != sz_out)
    {
        fprintf(stderr, "ALWAYS_START_ZERO: size mismatch %llu vs %llu\n", (unsigned long long)sz_in, (unsigned long long)sz_out);
        return 4;
    }
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    if (crc_in != crc_out)
    {
        fprintf(stderr, "ALWAYS_START_ZERO: CRC mismatch %08x vs %08x\n", (unsigned)crc_in, (unsigned)crc_out);
        return 5;
    }
    return 0;
}

int main(void)
{
    int rc = 0;
    if (test_always_skip_if_exists() != 0)
        rc = 1;
    // Verify that when contents differ, SKIP_IF_DIFFERENT leaves the existing output unchanged
    if (rc == 0)
    {
        const size_t packet = 1024, depth = 16, size = 160 * 1024 + 11;
        test_duplex_t d;
        test_duplex_init(&d, packet, depth);
        char root[512], dir[512], indir[512], outdir[512], in[512], out[512];
        make_paths(root, sizeof(root), dir, sizeof(dir), indir, sizeof(indir), outdir, sizeof(outdir), in, sizeof(in), out,
                   sizeof(out), "policies_skip_if_diff");
        write_pattern_file(in, size, 0);
        write_pattern_file(out, size, 1); // different content, same size
        uint64_t before_size = ts_file_size(out);
        uint32_t before_crc = ts_file_crc32(out);
        uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
        uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
        test_duplex_t end_tx = d;
        test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
        val_config_t cfg_tx, cfg_rx;
        make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
        cfg_rx.resume.mode = VAL_RESUME_CRC_FULL;
        val_status_t st = VAL_OK;
        run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st);
        free(sb_a);
        free(rb_a);
        free(sb_b);
        free(rb_b);
        test_duplex_free(&d);
        if (st != VAL_OK)
            rc = 1;
        else
        {
            uint64_t after_size = ts_file_size(out);
            uint32_t after_crc = ts_file_crc32(out);
            if (before_size != after_size || before_crc != after_crc)
                rc = 1;
        }
    }
    if (test_strict_resume_only_abort_on_mismatch() != 0)
        rc = 1;
    if (test_always_start_zero_overwrite() != 0)
        rc = 1;
    if (rc == 0)
        printf("OK\n");
    return rc;
}
