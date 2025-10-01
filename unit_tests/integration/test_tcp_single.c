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
    while (i > 0)
    {
        char c = path[i - 1];
        if (c == '/' || c == '\\')
            break;
        --i;
    }
    if (i == 0)
    {
        snprintf(out, outsz, ".");
        return;
    }
    size_t copy = (i < outsz - 1) ? i : (outsz - 1);
    memcpy(out, path, copy);
    out[copy] = '\0';
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

// Find example executables relative to the test exe dir
static int find_example_bins(char *rx_path, size_t rx_sz, char *tx_path, size_t tx_sz)
{
    char art[1024];
    if (!ts_get_artifacts_root(art, sizeof(art)))
        return 0;
    // artifacts root = <exe_dir>/ut_artifacts
    char exe_dir[1024];
    dirname_of(art, exe_dir, sizeof(exe_dir));

    // Compute build_root = dirname(dirname(exe_dir))  i.e., .../build/<config root>
    char parent1[1024];
    dirname_of(exe_dir, parent1, sizeof(parent1)); // unit_tests
    char build_root[1024];
    dirname_of(parent1, build_root, sizeof(build_root)); // build root

    // Candidates to try (absolute):
    char c_debug_rx[1024], c_debug_tx[1024], c_sc_rx[1024], c_sc_tx[1024], c_rel1_rx[1024], c_rel1_tx[1024];
    snprintf(c_debug_rx, sizeof(c_debug_rx), "%s" PATH_SEP "Debug" PATH_SEP "val_example_receive" EXE_EXT, build_root);
    snprintf(c_debug_tx, sizeof(c_debug_tx), "%s" PATH_SEP "Debug" PATH_SEP "val_example_send" EXE_EXT, build_root);
    snprintf(c_sc_rx, sizeof(c_sc_rx), "%s" PATH_SEP "val_example_receive" EXE_EXT, build_root);
    snprintf(c_sc_tx, sizeof(c_sc_tx), "%s" PATH_SEP "val_example_send" EXE_EXT, build_root);
    // Relative from exe_dir two levels up to top Debug
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
    else if (file_exists(c_rel1_rx) && file_exists(c_rel1_tx))
    {
        rx = c_rel1_rx;
        tx = c_rel1_tx;
    }
    else
    {
        fprintf(stderr, "candidates tried:\n  %s\n  %s\n  %s\n  %s\n  %s\n  %s\n", c_debug_rx, c_debug_tx, c_sc_rx, c_sc_tx,
                c_rel1_rx, c_rel1_tx);
        return 0;
    }

    snprintf(rx_path, rx_sz, "%s", rx);
    snprintf(tx_path, tx_sz, "%s", tx);
    return 1;
}

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
    char rx_path[1024], tx_path[1024];
    if (!find_example_bins(rx_path, sizeof(rx_path), tx_path, sizeof(tx_path)))
    {
        fprintf(stderr, "could not locate example binaries\n");
        return 1;
    }

    // Prepare artifacts directories and input files under build tree
    char art[1024];
    if (!ts_get_artifacts_root(art, sizeof(art)))
    {
        fprintf(stderr, "artifacts root failed\n");
        return 2;
    }
    char root[1024], outdir[1024], in_small[1024], in_large[1024], out_small[1024], out_large[1024];
    snprintf(root, sizeof(root), "%s" PATH_SEP "tcp_single", art);
    snprintf(outdir, sizeof(outdir), "%s" PATH_SEP "out", root);
    char rx_log[1024], tx1_log[1024], tx2_log[1024];
    snprintf(rx_log, sizeof(rx_log), "%s" PATH_SEP "rx.log", root);
    snprintf(tx1_log, sizeof(tx1_log), "%s" PATH_SEP "tx_small.log", root);
    snprintf(tx2_log, sizeof(tx2_log), "%s" PATH_SEP "tx_large.log", root);
    if (ts_ensure_dir(root) != 0 || ts_ensure_dir(outdir) != 0)
    {
        fprintf(stderr, "mkdir failed\n");
        return 3;
    }
    snprintf(in_small, sizeof(in_small), "%s" PATH_SEP "s.bin", root);
    snprintf(in_large, sizeof(in_large), "%s" PATH_SEP "l.bin", root);
    snprintf(out_small, sizeof(out_small), "%s" PATH_SEP "s.bin", outdir);
    snprintf(out_large, sizeof(out_large), "%s" PATH_SEP "l.bin", outdir);

    const size_t small_sz = ts_env_size_bytes("VAL_IT_TCP_SINGLE_SMALL", 64 * 1024 + 7);
    const size_t large_sz = ts_env_size_bytes("VAL_IT_TCP_SINGLE_LARGE", 3 * 1024 * 1024 + 101);
    if (write_pattern_file(in_small, small_sz) != 0 || write_pattern_file(in_large, large_sz) != 0)
    {
        fprintf(stderr, "failed to create input files\n");
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
    if (!run_receiver(rx_path, port, outdir, &rx_pid))
        return 5;
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
        return 6;
    }
    // Verify CRC/size
    if (ts_file_size(in_small) != ts_file_size(out_small))
    {
        fprintf(stderr, "size mismatch (small)\n");
        return 7;
    }
    if (ts_file_crc32(in_small) != ts_file_crc32(out_small))
    {
        fprintf(stderr, "crc mismatch (small)\n");
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
        return 5;
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
        return 9;
    }
    if (ts_file_size(in_large) != ts_file_size(out_large))
    {
        fprintf(stderr, "size mismatch (large)\n");
        return 10;
    }
    if (ts_file_crc32(in_large) != ts_file_crc32(out_large))
    {
        fprintf(stderr, "crc mismatch (large)\n");
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
    return 0;
}
