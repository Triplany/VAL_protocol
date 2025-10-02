#include "../support/test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

// Join using centralized helper; caller must free
static char *join_path(const char *dir, const char *filename) { return ts_join_path_dyn(dir, filename); }

static int write_pattern_file(const char *path, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    for (size_t i = 0; i < size; ++i)
        fputc((int)(i * 13u + 7u), f);
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
    char cmd[2000];
    // Disable validation and enable verbose logging directly to a file; also echo the command for CTest
    snprintf(cmd, sizeof(cmd), "\"%s\" --no-validation --log-level trace --log-file \"%s\" %u \"%s\"", rx_path,
             logfile ? logfile : "", (unsigned)port, outdir);
    fprintf(stdout, "[IT] RX: %s\n", cmd);
    fflush(stdout);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0; // allow default std handles
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    // Inherit handles so child stdout/stderr flow into parent console
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return 0;
    *pi_out = pi;
    fprintf(stdout, "[IT] RX pid=%lu started, waiting to listen...\n", (unsigned long)pi.dwProcessId);
    fflush(stdout);
    // Give it a moment to start listening
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
        execl(rx_path, rx_path, "--no-validation", "--log-level", "trace", "--log-file", logfile ? logfile : "", portstr, outdir,
              (char *)NULL);
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
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "\"%s\" --log-level trace --log-file \"%s\" %s %u", tx_path,
                            logfile ? logfile : "", host, (unsigned)port);
    for (size_t i = 0; i < nfiles; ++i)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, " \"%s\"", files[i]);
    fprintf(stdout, "[IT] TX: %s\n", cmd);
    fflush(stdout);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = 0;
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
        char **argv = (char **)calloc(4 + nfiles + 4, sizeof(char *));
        size_t ai = 0;
        argv[ai++] = (char *)tx_path;
        argv[ai++] = "--log-level";
        argv[ai++] = "trace";
        argv[ai++] = "--log-file";
        argv[ai++] = (char *)(logfile ? logfile : "");
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

int main(void)
{
    char rx_path[4096], tx_path[4096];
    if (!ts_find_example_bins(rx_path, sizeof(rx_path), tx_path, sizeof(tx_path)))
    {
        fprintf(stderr, "could not locate example binaries\n");
        return 1;
    }

    // Prepare artifacts directories and input files under build tree
    char art[2048];
    if (!ts_get_artifacts_root(art, sizeof(art)))
    {
        fprintf(stderr, "artifacts root failed\n");
        return 2;
    }
    char *root = join_path(art, "tcp_single");
    if (!root)
        return 3;
    char *outdir = join_path(root, "out");
    char *rx_log = join_path(root, "rx.log");
    char *tx1_log = join_path(root, "tx_small.log");
    char *tx2_log = join_path(root, "tx_large.log");
    if (!outdir || !rx_log || !tx1_log || !tx2_log)
    {
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 3;
    }
    if (ts_ensure_dir(root) != 0 || ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "mkdir failed\n");
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 3;
    }
    char *in_small = join_path(root, "s.bin");
    char *in_large = join_path(root, "l.bin");
    char *out_small = join_path(outdir, "s.bin");
    char *out_large = join_path(outdir, "l.bin");
    if (!in_small || !in_large || !out_small || !out_large)
    {
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 3;
    }

    const size_t small_sz = ts_env_size_bytes("VAL_IT_TCP_SINGLE_SMALL", 64 * 1024 + 7);
    const size_t large_sz = ts_env_size_bytes("VAL_IT_TCP_SINGLE_LARGE", 3 * 1024 * 1024 + 101);
    if (write_pattern_file(in_small, small_sz) != 0 || write_pattern_file(in_large, large_sz) != 0)
    {
        fprintf(stderr, "failed to create input files\n");
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 4;
    }

    unsigned short port = 33331;                       // fixed localhost test port for first run
    unsigned short port2 = (unsigned short)(port + 1); // use a different port for the second run to avoid TIME_WAIT

#if defined(_WIN32)
    PROCESS_INFORMATION rx_pi;
    if (!run_receiver(rx_path, port, outdir, rx_log, &rx_pi))
        return 5;
#else
    pid_t rx_pid;
    if (!run_receiver(rx_path, port, outdir, rx_log, &rx_pid))
    {
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 5;
    }
#endif

    // First: single small file
    const char *one_small[] = {in_small};
    int rc = run_sender(tx_path, "127.0.0.1", port, one_small, 1, tx1_log);
    if (rc != 0)
    {
        fprintf(stderr, "sender small failed rc=%d\n", rc);
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
        else
        {
            fprintf(stderr, "no rx log at %s\n", rx_log);
        }
        lf = fopen(tx1_log, "rb");
        if (lf)
        {
            fprintf(stderr, "-- tx small log --\n");
            int ch;
            int cnt = 0;
            while ((ch = fgetc(lf)) != EOF && cnt < 6000)
            {
                fputc(ch, stderr);
                cnt++;
            }
            fclose(lf);
        }
        else
        {
            fprintf(stderr, "no tx log at %s\n", tx1_log);
        }
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 6;
    }
    // Verify CRC/size
    if (ts_file_size(in_small) != ts_file_size(out_small))
    {
        fprintf(stderr, "size mismatch (small)\n");
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 7;
    }
    if (ts_file_crc32(in_small) != ts_file_crc32(out_small))
    {
        fprintf(stderr, "crc mismatch (small)\n");
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 8;
    }

    // Second: single large file (new run)
#if defined(_WIN32)
    // Ensure first receiver instance exits cleanly before starting the next
    WaitForSingleObject(rx_pi.hProcess, INFINITE);
    CloseHandle(rx_pi.hThread);
    CloseHandle(rx_pi.hProcess);
    // Start a fresh receiver for the second transfer
    if (!run_receiver(rx_path, port2, outdir, rx_log, &rx_pi))
        return 5;
#else
    // Wait for receiver to exit, then start a new one
    int status_wait = 0;
    waitpid(rx_pid, &status_wait, 0);
    if (!run_receiver(rx_path, port2, outdir, rx_log, &rx_pid))
    {
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 5;
    }
#endif
    const char *one_large[] = {in_large};
    rc = run_sender(tx_path, "127.0.0.1", port2, one_large, 1, tx2_log);
    if (rc != 0)
    {
        fprintf(stderr, "sender large failed rc=%d\n", rc);
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
        lf = fopen(tx2_log, "rb");
        if (lf)
        {
            fprintf(stderr, "-- tx large log --\n");
            int ch;
            int cnt = 0;
            while ((ch = fgetc(lf)) != EOF && cnt < 6000)
            {
                fputc(ch, stderr);
                cnt++;
            }
            fclose(lf);
        }
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 9;
    }
    if (ts_file_size(in_large) != ts_file_size(out_large))
    {
        fprintf(stderr, "size mismatch (large)\n");
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 10;
    }
    if (ts_file_crc32(in_large) != ts_file_crc32(out_large))
    {
        fprintf(stderr, "crc mismatch (large)\n");
        free(in_small);
        free(in_large);
        free(out_small);
        free(out_large);
        free(root);
        free(outdir);
        free(rx_log);
        free(tx1_log);
        free(tx2_log);
        return 11;
    }

    // Close receiver naturally after client disconnects from the second run
#if defined(_WIN32)
    WaitForSingleObject(rx_pi.hProcess, INFINITE);
    CloseHandle(rx_pi.hThread);
    CloseHandle(rx_pi.hProcess);
#else
    int status2 = 0;
    waitpid(rx_pid, &status2, 0);
#endif

    printf("OK\n");
    free(in_small);
    free(in_large);
    free(out_small);
    free(out_large);
    free(root);
    free(outdir);
    free(rx_log);
    free(tx1_log);
    free(tx2_log);
    return 0;
}
