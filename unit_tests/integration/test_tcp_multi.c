#include "../support/test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define PATH_SEP "\\"
#define EXE_EXT ".exe"
#else
#include <sys/wait.h>
#include <unistd.h>
#define PATH_SEP "/"
#define EXE_EXT ""
#endif

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void dirname_of(const char *path, char *out, size_t outsz)
{
    if (!path || !*path || !out || outsz == 0)
    {
        if (out && outsz)
            out[0] = '\0';
        return;
    }
    size_t n = strlen(path);
    size_t i = n;
    while (i > 0 && (path[i - 1] == '/' || path[i - 1] == '\\'))
        --i; // trim trailing separators
    while (i > 0 && !(path[i - 1] == '/' || path[i - 1] == '\\'))
        --i; // go to previous separator
    if (i == 0)
    {
        snprintf(out, outsz, ".");
        return;
    }
    size_t copy = (i < outsz - 1) ? i : (outsz - 1);
    memcpy(out, path, copy);
    out[copy] = '\0';
}

static int find_example_bins(char *rx_path, size_t rx_sz, char *tx_path, size_t tx_sz)
{
    char art[2048];
    if (!ts_get_artifacts_root(art, sizeof(art)))
        return 0;
    char exe_dir[2048];
    dirname_of(art, exe_dir, sizeof(exe_dir));
    char parent1[2048];
    dirname_of(exe_dir, parent1, sizeof(parent1));
    char build_root[2048];
    dirname_of(parent1, build_root, sizeof(build_root));

    char c_debug_rx[4096], c_debug_tx[4096], c_sc_rx[4096], c_sc_tx[4096], c_sc2_rx[4096], c_sc2_tx[4096], c_cwd_rx[64],
        c_cwd_tx[64], c_rel1_rx[4096], c_rel1_tx[4096];
    snprintf(c_cwd_rx, sizeof(c_cwd_rx), "val_example_receive" EXE_EXT);
    snprintf(c_cwd_tx, sizeof(c_cwd_tx), "val_example_send" EXE_EXT);
    snprintf(c_debug_rx, sizeof(c_debug_rx), "%s" PATH_SEP "Debug" PATH_SEP "val_example_receive" EXE_EXT, build_root);
    snprintf(c_debug_tx, sizeof(c_debug_tx), "%s" PATH_SEP "Debug" PATH_SEP "val_example_send" EXE_EXT, build_root);
    snprintf(c_sc_rx, sizeof(c_sc_rx), "%s" PATH_SEP "val_example_receive" EXE_EXT, build_root);
    snprintf(c_sc_tx, sizeof(c_sc_tx), "%s" PATH_SEP "val_example_send" EXE_EXT, build_root);
    snprintf(c_sc2_rx, sizeof(c_sc2_rx), "%s" PATH_SEP "val_example_receive" EXE_EXT, parent1);
    snprintf(c_sc2_tx, sizeof(c_sc2_tx), "%s" PATH_SEP "val_example_send" EXE_EXT, parent1);
    snprintf(c_rel1_rx, sizeof(c_rel1_rx),
             "%s" PATH_SEP ".." PATH_SEP ".." PATH_SEP "Debug" PATH_SEP "val_example_receive" EXE_EXT, exe_dir);
    snprintf(c_rel1_tx, sizeof(c_rel1_tx), "%s" PATH_SEP ".." PATH_SEP ".." PATH_SEP "Debug" PATH_SEP "val_example_send" EXE_EXT,
             exe_dir);

    const char *rx = NULL, *tx = NULL;
    if (file_exists(c_debug_rx) && file_exists(c_debug_tx))
    {
        rx = c_debug_rx;
        tx = c_debug_tx;
    }
    else if (file_exists(c_sc_rx) && file_exists(c_sc_tx))
    {
        rx = c_sc_rx;
        tx = c_sc_tx;
    }
    else if (file_exists(c_sc2_rx) && file_exists(c_sc2_tx))
    {
        rx = c_sc2_rx;
        tx = c_sc2_tx;
    }
    else if (file_exists(c_cwd_rx) && file_exists(c_cwd_tx))
    {
        rx = c_cwd_rx;
        tx = c_cwd_tx;
    }
    else if (file_exists(c_rel1_rx) && file_exists(c_rel1_tx))
    {
        rx = c_rel1_rx;
        tx = c_rel1_tx;
    }
    else
    {
        fprintf(stderr, "candidates tried:\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n", c_debug_rx, c_debug_tx,
                c_sc_rx, c_sc_tx, c_sc2_rx, c_sc2_tx, c_cwd_rx, c_cwd_tx, c_rel1_rx, c_rel1_tx);
        return 0;
    }
    snprintf(rx_path, rx_sz, "%s", rx);
    snprintf(tx_path, tx_sz, "%s", tx);
    return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

static int run_receiver(const char *rx_path, unsigned short port, const char *outdir, const char *logfile,
#if defined(_WIN32)
                        PROCESS_INFORMATION *pi_out
#else
                        pid_t *pid_out
#endif
)
{
#if defined(_WIN32)
    char cmd[1600];
    snprintf(cmd, sizeof(cmd),
             "cmd /C set VAL_LOG_LEVEL=5&& set VAL_LOG_FILE=\"%s\"&& \"%s\" --no-validation --log-level trace %u \"%s\"",
             logfile ? logfile : "", rx_path, (unsigned)port, outdir);
    fprintf(stdout, "[IT] RX: %s\n", cmd);
    fflush(stdout);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return 0;
    *pi_out = pi;
    ts_delay(750);
    return 1;
#else
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0)
    {
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
        if (logfile && *logfile)
            setenv("VAL_LOG_FILE", logfile, 1);
        setenv("VAL_LOG_LEVEL", "5", 1);
        execl(rx_path, rx_path, "--no-validation", "--log-level", "trace", portstr, outdir, (char *)NULL);
        _exit(127);
    }
    *pid_out = pid;
    ts_delay(750);
    return 1;
#endif
}

static int run_sender(const char *tx_path, const char *host, unsigned short port, const char **files, size_t nfiles,
                      const char *logfile)
{
#if defined(_WIN32)
    char cmd[4096];
    size_t off = 0;
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                            "cmd /C set VAL_LOG_LEVEL=5&& set VAL_LOG_FILE=\"%s\"&& \"%s\" --log-level trace %s %u",
                            logfile ? logfile : "", tx_path, host, (unsigned)port);
    for (size_t i = 0; i < nfiles; ++i)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, " \"%s\"", files[i]);
    fprintf(stdout, "[IT] TX: %s\n", cmd);
    fflush(stdout);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    fprintf(stdout, "[IT] TX started, waiting...\n");
    fflush(stdout);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
        if (logfile && *logfile)
            setenv("VAL_LOG_FILE", logfile, 1);
        setenv("VAL_LOG_LEVEL", "5", 1);
        char **argv = (char **)calloc(4 + nfiles + 2, sizeof(char *));
        size_t ai = 0;
        argv[ai++] = (char *)tx_path;
        argv[ai++] = "--log-level";
        argv[ai++] = "trace";
        argv[ai++] = (char *)host;
        argv[ai++] = portstr;
        for (size_t i = 0; i < nfiles; ++i)
            argv[ai++] = (char *)files[i];
        argv[ai] = NULL;
        execv(tx_path, argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
#endif
}

// Join a directory and filename with exactly one path separator, returning a newly allocated string.
// Caller must free the returned pointer.
static char *join_path(const char *dir, const char *filename)
{
    if (!dir)
        dir = "";
    if (!filename)
        filename = "";
    size_t ld = strlen(dir);
    size_t lf = strlen(filename);
    int need_sep = 1;
    if (ld == 0)
        need_sep = 0;
    else
    {
        char last = dir[ld - 1];
        if (last == '/' || last == '\\')
            need_sep = 0;
    }
    size_t total = ld + (size_t)need_sep + lf + 1;
    char *out = (char *)malloc(total);
    if (!out)
        return NULL;
    size_t pos = 0;
    if (ld)
    {
        memcpy(out + pos, dir, ld);
        pos += ld;
    }
    if (need_sep)
        out[pos++] = PATH_SEP[0];
    if (lf)
    {
        memcpy(out + pos, filename, lf);
        pos += lf;
    }
    out[pos] = '\0';
    return out;
}

int main(void)
{
    char rx_path[4096], tx_path[4096];
    if (!find_example_bins(rx_path, sizeof(rx_path), tx_path, sizeof(tx_path)))
    {
        fprintf(stderr, "could not locate example binaries\n");
        return 1;
    }

    char art[2048];
    if (!ts_get_artifacts_root(art, sizeof(art)))
        return 2;
    char *root = join_path(art, "tcp_multi");
    if (!root)
        return 3;
    char *rx_log = join_path(root, "rx.log");
    char *tx_log = join_path(root, "tx.log");
    char *outdir = join_path(root, "out");
    if (!rx_log || !tx_log || !outdir)
    {
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 3;
    }
    if (ts_ensure_dir(root) != 0 || ts_ensure_dir(outdir) != 0)
    {
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 3;
    }

    // Create 3 files: tiny, small, larger
    char *f1 = join_path(root, "tiny.txt");
    char *f2 = join_path(root, "small.bin");
    char *f3 = join_path(root, "big.bin");
    if (!f1 || !f2 || !f3)
    {
        free(f1);
        free(f2);
        free(f3);
        fprintf(stderr, "out of memory building input paths\n");
        return 3;
    }
    const char *t1 = "hello world\n";
    write_file(f1, (const uint8_t *)t1, strlen(t1));
    size_t s_small = ts_env_size_bytes("VAL_IT_TCP_MULTI_SMALL", 80 * 1024 + 3);
    size_t s_big = ts_env_size_bytes("VAL_IT_TCP_MULTI_BIG", 5 * 1024 * 1024 + 777);
    uint8_t *buf2 = (uint8_t *)malloc(s_small);
    for (size_t i = 0; i < s_small; ++i)
        buf2[i] = (uint8_t)(i * 5u + 1u);
    uint8_t *buf3 = (uint8_t *)malloc(s_big);
    for (size_t i = 0; i < s_big; ++i)
        buf3[i] = (uint8_t)(255u - (i * 11u));
    write_file(f2, buf2, s_small);
    write_file(f3, buf3, s_big);

    unsigned short port = 33332; // fixed localhost test port (different from single)

#if defined(_WIN32)
    PROCESS_INFORMATION rx_pi;
    if (!run_receiver(rx_path, port, outdir, rx_log, &rx_pi))
        return 4;
#else
    pid_t rx_pid;
    if (!run_receiver(rx_path, port, outdir, rx_log, &rx_pid))
        return 4;
#endif

    const char *files[3] = {f1, f2, f3};
    int rc = run_sender(tx_path, "127.0.0.1", port, files, 3, tx_log);
    if (rc != 0)
    {
        free(f1);
        free(f2);
        free(f3);
        fprintf(stderr, "sender failed rc=%d\n", rc);
        FILE *lf = fopen(rx_log, "rb");
        if (lf)
        {
            fprintf(stderr, "-- rx log --\n");
            int ch;
            int cnt = 0;
            while ((ch = fgetc(lf)) != EOF && cnt < 6000)
            {
                fputc(ch, stderr);
                cnt++;
            }
            fclose(lf);
        }
        lf = fopen(tx_log, "rb");
        if (lf)
        {
            fprintf(stderr, "-- tx log --\n");
            int ch;
            int cnt = 0;
            while ((ch = fgetc(lf)) != EOF && cnt < 6000)
            {
                fputc(ch, stderr);
                cnt++;
            }
            fclose(lf);
        }
        free(buf2);
        free(buf3);
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 5;
    }

    // Verify all outputs exist and match size+CRC
    char *o1 = join_path(outdir, "tiny.txt");
    char *o2 = join_path(outdir, "small.bin");
    char *o3 = join_path(outdir, "big.bin");
    if (!o1 || !o2 || !o3)
    {
        free(o1);
        free(o2);
        free(o3);
        fprintf(stderr, "out of memory building output paths\n");
        free(buf2);
        free(buf3);
        free(f1);
        free(f2);
        free(f3);
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 6;
    }
    if (ts_file_size(f1) != ts_file_size(o1) || ts_file_crc32(f1) != ts_file_crc32(o1))
    {
        fprintf(stderr, "mismatch tiny\n");
        free(o1);
        free(o2);
        free(o3);
        free(buf2);
        free(buf3);
        free(f1);
        free(f2);
        free(f3);
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 6;
    }
    if (ts_file_size(f2) != ts_file_size(o2) || ts_file_crc32(f2) != ts_file_crc32(o2))
    {
        fprintf(stderr, "mismatch small\n");
        free(o1);
        free(o2);
        free(o3);
        free(buf2);
        free(buf3);
        free(f1);
        free(f2);
        free(f3);
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 7;
    }
    if (ts_file_size(f3) != ts_file_size(o3) || ts_file_crc32(f3) != ts_file_crc32(o3))
    {
        fprintf(stderr, "mismatch big\n");
        free(o1);
        free(o2);
        free(o3);
        free(buf2);
        free(buf3);
        free(f1);
        free(f2);
        free(f3);
        free(root);
        free(rx_log);
        free(tx_log);
        free(outdir);
        return 8;
    }

    free(o1);
    free(o2);
    free(o3);

    free(buf2);
    free(buf3);

    free(f1);
    free(f2);
    free(f3);

    free(root);
    free(rx_log);
    free(tx_log);
    free(outdir);

#if defined(_WIN32)
    WaitForSingleObject(rx_pi.hProcess, 2000);
    CloseHandle(rx_pi.hThread);
    CloseHandle(rx_pi.hProcess);
#else
    int status = 0;
    waitpid(rx_pid, &status, 0);
#endif

    printf("OK\n");
    return 0;
}
