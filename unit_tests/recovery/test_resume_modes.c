#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#endif

// Local helpers: write a file with a selectable pattern; mode 0 = i&0xFF, mode 1 = (i*13)&0xFF
static int write_pattern_file(const char *path, size_t size, int mode)
{
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

// Paths are now built using shared helpers (ts_build_case_dirs, ts_path_join)

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
        if (tx)
            val_session_destroy(tx);
        if (rx)
            val_session_destroy(rx);
        return -1;
    }
    ts_thread_t th = ts_start_receiver(rx, outdir);
    ts_receiver_warmup(cfg_tx, 5);
    const char *files[1] = {in};
    val_status_t st = val_send_files(tx, files, 1, NULL);
    ts_join_thread(th);
    val_session_destroy(tx);
    val_session_destroy(rx);
    if (st_out)
        *st_out = st;
    return 0;
}

// Scenario 1: No existing output
static int scenario_no_existing(val_resume_mode_t mode)
{
    const size_t packet = 1024, depth = 16, size = 128 * 1024 + 7;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char basedir[512], outdir[512];
    char in[512], out[512];
    char sub[64];
    snprintf(sub, sizeof(sub), "resume_no_existing_%d", (int)mode);
    if (ts_build_case_dirs(sub, basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "file.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "file.bin") != 0)
        return 1;
    if (write_pattern_file(in, size, 0) != 0)
        return 1;

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = mode;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
        return 3;
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    if (sz_in != sz_out || crc_in != crc_out)
        return 4;
    return 0;
}

// Scenario 2: Existing identical output
static int scenario_existing_identical(val_resume_mode_t mode)
{
    const size_t packet = 1024, depth = 16, size = 192 * 1024 + 3;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char basedir[512], outdir[512];
    char in[512], out[512];
    char sub[64];
    snprintf(sub, sizeof(sub), "resume_identical_%d", (int)mode);
    if (ts_build_case_dirs(sub, basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "file.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "file.bin") != 0)
        return 1;
    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    if (copy_prefix(in, out, size) != 0)
        return 1;

    uint64_t before_size = ts_file_size(out);
    uint32_t before_crc = ts_file_crc32(out);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = mode;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
        return 3;
    uint64_t after_size = ts_file_size(out);
    uint32_t after_crc = ts_file_crc32(out);
    if (mode == VAL_RESUME_NEVER)
    {
        // Overwrite from zero: expect equality with input (may change mtime but CRC same as input)
        if (after_size != ts_file_size(in) || after_crc != ts_file_crc32(in))
            return 4;
    }
    else
    {
        // Skip or quick-verify path should leave file identical to input as well (unchanged content)
        if (before_size != after_size || before_crc != after_crc)
            return 5;
    }
    return 0;
}

// Scenario 3: Existing partial prefix (resume expected except SKIP_EXISTING)
static int scenario_existing_partial_prefix(val_resume_mode_t mode)
{
    const size_t packet = 1024, depth = 16, size = 256 * 1024 + 11;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char basedir[512], outdir[512];
    char in[512], out[512];
    char sub[64];
    snprintf(sub, sizeof(sub), "resume_partial_%d", (int)mode);
    if (ts_build_case_dirs(sub, basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "file.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "file.bin") != 0)
        return 1;
    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    if (copy_prefix(in, out, size / 2) != 0)
        return 1;

    uint64_t before_size = ts_file_size(out);
    uint32_t before_crc = ts_file_crc32(out);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = mode;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
        return 3;
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    if (mode == VAL_RESUME_SKIP_EXISTING)
    {
        // Should skip incomplete file
        if (sz_out != before_size || crc_out != before_crc)
            return 4;
    }
    else
    {
        // Should complete to match input
        if (sz_out != sz_in || crc_out != crc_in)
            return 5;
    }
    return 0;
}

// Scenario 4: Existing different content, same size
static int scenario_existing_diff_same_size(val_resume_mode_t mode)
{
    const size_t packet = 1024, depth = 16, size = 160 * 1024 + 9;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char basedir[512], outdir[512];
    char in[512], out[512];
    char sub[64];
    snprintf(sub, sizeof(sub), "resume_diff_same_%d", (int)mode);
    if (ts_build_case_dirs(sub, basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "file.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "file.bin") != 0)
        return 1;
    if (write_pattern_file(in, size, 0) != 0)
        return 1;
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
    cfg_rx.resume.mode = mode;
    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
        return 3;
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    switch (mode)
    {
    case VAL_RESUME_NEVER:
    case VAL_RESUME_CRC_TAIL_OR_ZERO:
    case VAL_RESUME_CRC_FULL_OR_ZERO:
        if (sz_out != sz_in || crc_out != crc_in)
            return 4; // overwrite/resume to input
        break;
    case VAL_RESUME_SKIP_EXISTING:
    case VAL_RESUME_CRC_TAIL:
    case VAL_RESUME_CRC_FULL:
        if (sz_out != before_size || crc_out != before_crc)
            return 5; // skip
        break;
    default:
        return 6;
    }
    return 0;
}

// Scenario 5: Existing larger than incoming
static int scenario_existing_larger(val_resume_mode_t mode)
{
    const size_t packet = 1024, depth = 16, size = 96 * 1024 + 5;
    test_duplex_t d;
    test_duplex_init(&d, packet, depth);
    char basedir[512], outdir[512];
    char in[512], out[512];
    char sub[64];
    snprintf(sub, sizeof(sub), "resume_larger_%d", (int)mode);
    if (ts_build_case_dirs(sub, basedir, sizeof(basedir), outdir, sizeof(outdir)) != 0)
        return 1;
    if (ts_path_join(in, sizeof(in), basedir, "file.bin") != 0)
        return 1;
    if (ts_path_join(out, sizeof(out), outdir, "file.bin") != 0)
        return 1;
    if (write_pattern_file(in, size, 0) != 0)
        return 1;
    if (write_pattern_file(out, size + 777, 0) != 0)
        return 1; // larger local
    uint64_t before_size = ts_file_size(out);
    uint32_t before_crc = ts_file_crc32(out);

    uint8_t *sb_a = (uint8_t *)calloc(1, packet), *rb_a = (uint8_t *)calloc(1, packet);
    uint8_t *sb_b = (uint8_t *)calloc(1, packet), *rb_b = (uint8_t *)calloc(1, packet);
    test_duplex_t end_tx = d;
    test_duplex_t end_rx = (test_duplex_t){.a2b = d.b2a, .b2a = d.a2b, .max_packet = d.max_packet};
    val_config_t cfg_tx, cfg_rx;
    make_cfgs(&cfg_tx, &cfg_rx, &end_tx, &end_rx, sb_a, rb_a, sb_b, rb_b, packet);
    cfg_rx.resume.mode = mode;

    val_status_t st = VAL_OK;
    if (run_send_recv(in, outdir, &cfg_tx, &cfg_rx, &st) != 0)
        return 2;
    free(sb_a);
    free(rb_a);
    free(sb_b);
    free(rb_b);
    test_duplex_free(&d);

    if (st != VAL_OK)
        return 3;
    uint64_t sz_in = ts_file_size(in), sz_out = ts_file_size(out);
    uint32_t crc_in = ts_file_crc32(in), crc_out = ts_file_crc32(out);
    switch (mode)
    {
    case VAL_RESUME_NEVER:
    case VAL_RESUME_CRC_TAIL_OR_ZERO:
    case VAL_RESUME_CRC_FULL_OR_ZERO:
        if (sz_out != sz_in || crc_out != crc_in)
            return 4; // overwrite to input
        break;
    case VAL_RESUME_SKIP_EXISTING:
    case VAL_RESUME_CRC_TAIL:
    case VAL_RESUME_CRC_FULL:
        if (sz_out != before_size || crc_out != before_crc)
            return 5; // skip existing larger file
        break;
    default:
        return 6;
    }
    return 0;
}

int main(void)
{
    val_resume_mode_t modes[] = {VAL_RESUME_NEVER,    VAL_RESUME_SKIP_EXISTING,
                                 VAL_RESUME_CRC_TAIL, VAL_RESUME_CRC_TAIL_OR_ZERO,
                                 VAL_RESUME_CRC_FULL, VAL_RESUME_CRC_FULL_OR_ZERO};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
    {
        val_resume_mode_t m = modes[i];
        if (scenario_no_existing(m) != 0)
            return 10 + (int)m;
        if (scenario_existing_identical(m) != 0)
            return 20 + (int)m;
        if (scenario_existing_partial_prefix(m) != 0)
            return 30 + (int)m;
        if (scenario_existing_diff_same_size(m) != 0)
            return 40 + (int)m;
        if (scenario_existing_larger(m) != 0)
            return 50 + (int)m;
    }
    printf("OK\n");
    return 0;
}
