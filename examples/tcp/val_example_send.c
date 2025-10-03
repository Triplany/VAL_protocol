#include "common/tcp_util.h"
#include "val_protocol.h"
#include "val_error_strings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
// For summary tracking
#include <assert.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--mtu N] [--resume MODE] [--tail-bytes N] [--streaming on|off] [--accept-streaming on|off] [--log-level "
            "L] [--log-file PATH] [--tx-mode MODE] [--max-mode MODE] [--degrade N] [--upgrade N] <host> <port> <file1> [file2 "
            "...>]\n"
            "  --mtu N          Packet size/MTU (default 4096; min %u, max %u)\n"
            "  --resume MODE    Resume mode: never, skip, tail, tail_or_zero, full, full_or_zero\n"
            "  --tail-bytes N   Tail verification bytes for tail modes (default 1024)\n"
            "  --streaming      Enable sender streaming pacing if peer accepts (default on)\n"
            "  --accept-streaming  Accept incoming peer streaming pacing (default on). Note: --streaming off implies\n"
            "                     --accept-streaming off unless explicitly overridden with --accept-streaming on.\n"
            "  --log-level L    Runtime log verbosity: off, crit, warn, info, debug, trace, or 0..5\n"
            "  --log-file PATH  If set, append logs to this file (else stderr)\n"
            "  --tx-mode MODE   Preferred initial TX mode rung: stop|1|2|4|8|16|32|64 (default 16)\n"
            "  --max-mode MODE  Max TX mode this endpoint supports: stop|1|2|4|8|16|32|64 (default 64)\n"
            "  --degrade N      Errors before degrading a rung (default 3)\n"
            "  --upgrade N      Successes before upgrading a rung (default 10)\n"
            "  --min-timeout MS Minimum adaptive timeout clamp in ms (default 100)\n"
            "  --max-timeout MS Maximum adaptive timeout clamp in ms (default 10000)\n"
            "  --meta-retries N Retries while waiting for SEND_META/RESUME responses (default 4)\n"
            "  --data-retries N Retries while waiting for DATA (recv-side watchdog; default 3)\n"
            "  --ack-retries N  Retries while waiting for ACKs (VERIFY/DONE/EOT; default 3)\n"
            "  --backoff MS     Base backoff between retries (exponential) (default 100)\n",
            prog, (unsigned)VAL_MIN_PACKET_SIZE, (unsigned)VAL_MAX_PACKET_SIZE);
}

static int parse_uint(const char *s, unsigned *out)
{
    if (!s || !*s)
        return -1;
    unsigned v = 0;
    for (const char *p = s; *p; ++p)
    {
        if (*p < '0' || *p > '9')
            return -1;
        unsigned d = (unsigned)(*p - '0');
        unsigned nv = v * 10u + d;
        if (nv < v)
            return -1; // overflow
        v = nv;
    }
    *out = v;
    return 0;
}

static int strieq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static int parse_resume_mode(const char *s, val_resume_mode_t *out)
{
    if (!s || !out)
        return -1;
    if (strieq(s, "never"))
        *out = VAL_RESUME_NEVER;
    else if (strieq(s, "skip") || strieq(s, "skip_existing"))
        *out = VAL_RESUME_SKIP_EXISTING;
    else if (strieq(s, "tail"))
        *out = VAL_RESUME_CRC_TAIL;
    else if (strieq(s, "tail_or_zero"))
        *out = VAL_RESUME_CRC_TAIL_OR_ZERO;
    else if (strieq(s, "full"))
        *out = VAL_RESUME_CRC_FULL;
    else if (strieq(s, "full_or_zero"))
        *out = VAL_RESUME_CRC_FULL_OR_ZERO;
    else
        return -1;
    return 0;
}

static const char *tx_mode_name(val_tx_mode_t m)
{
    switch (m)
    {
    case VAL_TX_STOP_AND_WAIT:
        return "stop(1:1)";
    case VAL_TX_WINDOW_2:
        return "2";
    case VAL_TX_WINDOW_4:
        return "4";
    case VAL_TX_WINDOW_8:
        return "8";
    case VAL_TX_WINDOW_16:
        return "16";
    case VAL_TX_WINDOW_32:
        return "32";
    case VAL_TX_WINDOW_64:
        return "64";
    default:
        return "?";
    }
}

static int parse_tx_mode(const char *s, val_tx_mode_t *out)
{
    if (!s || !out)
        return -1;
    if (strieq(s, "stop") || strieq(s, "saw") || strieq(s, "1") || strieq(s, "1:1") || strieq(s, "stop_and_wait"))
    {
        *out = VAL_TX_STOP_AND_WAIT;
        return 0;
    }
    if (strieq(s, "2") || strieq(s, "win2") || strieq(s, "window2") || strieq(s, "window_2"))
    {
        *out = VAL_TX_WINDOW_2;
        return 0;
    }
    if (strieq(s, "4") || strieq(s, "win4") || strieq(s, "window4") || strieq(s, "window_4"))
    {
        *out = VAL_TX_WINDOW_4;
        return 0;
    }
    if (strieq(s, "8") || strieq(s, "win8") || strieq(s, "window8") || strieq(s, "window_8"))
    {
        *out = VAL_TX_WINDOW_8;
        return 0;
    }
    if (strieq(s, "16") || strieq(s, "win16") || strieq(s, "window16") || strieq(s, "window_16"))
    {
        *out = VAL_TX_WINDOW_16;
        return 0;
    }
    if (strieq(s, "32") || strieq(s, "win32") || strieq(s, "window32") || strieq(s, "window_32"))
    {
        *out = VAL_TX_WINDOW_32;
        return 0;
    }
    if (strieq(s, "64") || strieq(s, "win64") || strieq(s, "window64") || strieq(s, "window_64"))
    {
        *out = VAL_TX_WINDOW_64;
        return 0;
    }
    return -1;
}

static int tp_send(void *ctx, const void *data, size_t len)
{
    int fd = *(int *)ctx;
    int rc = tcp_send_all(fd, data, len);
    return rc == 0 ? (int)len : -1;
}
static int tp_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms)
{
    int fd = *(int *)ctx;
    if (tcp_recv_exact(fd, buffer, buffer_size, timeout_ms) != 0)
    {
        if (received)
            *received = 0; // signal timeout to caller via short read
        return 0;          // transport error is only when socket fails; timeouts are indicated by short read
    }
    if (received)
        *received = buffer_size;
    return 0;
}

static int tp_is_connected(void *ctx)
{
    int fd = *(int *)ctx;
    return tcp_is_connected(fd);
}

static void tp_flush(void *ctx)
{
    int fd = *(int *)ctx;
    tcp_flush(fd);
}

// ---------- Simple logging sink ----------
static FILE *g_logf = NULL;
static const char *lvl_name(int lvl)
{
    switch (lvl)
    {
    case 1:
        return "CRIT";
    case 2:
        return "WARN";
    case 3:
        return "INFO";
    case 4:
        return "DEBUG";
    case 5:
        return "TRACE";
    default:
        return "OFF";
    }
}
static void console_logger(void *ctx, int level, const char *file, int line, const char *message)
{
    (void)ctx;
    FILE *out = g_logf ? g_logf : stderr;
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(out, "[%s][TX][%s] %s:%d: %s\n", ts, lvl_name(level), file, line, message ? message : "");
    fflush(out);
}
static int parse_level(const char *s)
{
    if (!s)
        return 0;
    if (isdigit((unsigned char)s[0]))
    {
        int v = atoi(s);
        if (v < 0)
            v = 0;
        if (v > 5)
            v = 5;
        return v;
    }
    if (strieq(s, "off"))
        return 0;
    if (strieq(s, "crit") || strieq(s, "critical"))
        return 1;
    if (strieq(s, "warn") || strieq(s, "warning"))
        return 2;
    if (strieq(s, "info"))
        return 3;
    if (strieq(s, "debug"))
        return 4;
    if (strieq(s, "trace") || strieq(s, "verbose"))
        return 5;
    return 0;
}

// Minimal filesystem adapters using stdio
static void *fs_fopen(void *ctx, const char *path, const char *mode)
{
    (void)ctx;
    return (void *)fopen(path, mode);
}
static int fs_fread(void *ctx, void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    return (int)fread(buffer, size, count, (FILE *)file);
}
static int fs_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    return (int)fwrite(buffer, size, count, (FILE *)file);
}
static int fs_fseek(void *ctx, void *file, long offset, int whence)
{
    (void)ctx;
    return fseek((FILE *)file, offset, whence);
}
static long fs_ftell(void *ctx, void *file)
{
    (void)ctx;
    return ftell((FILE *)file);
}
static int fs_fclose(void *ctx, void *file)
{
    (void)ctx;
    return fclose((FILE *)file);
}

// --- Optional: progress callback to announce TX mode changes in real time ---
static val_session_t *g_tx_session_for_progress = NULL; // set after session create
static val_tx_mode_t g_last_mode_reported = (val_tx_mode_t)255; // invalid sentinel
static int g_tx_post_handshake_printed = 0;
static int g_tx_stream_last = -1; // unknown at start; track streaming engaged transitions
static void on_progress_announce_mode(const val_progress_info_t *info)
{
    (void)info; // we only use it to trigger periodic checks
    if (!g_tx_session_for_progress)
        return;
    // One-time post-handshake negotiated summary
    if (!g_tx_post_handshake_printed)
    {
            size_t mtu = 0;
        int send_stream_ok = 0, recv_stream_ok = 0;
        val_tx_mode_t m0 = VAL_TX_STOP_AND_WAIT;
        if (val_get_effective_packet_size(g_tx_session_for_progress, &mtu) == VAL_OK)
        {
            (void)val_get_streaming_allowed(g_tx_session_for_progress, &send_stream_ok, &recv_stream_ok);
            (void)val_get_current_tx_mode(g_tx_session_for_progress, &m0);
            int engaged = 0;
            (void)val_is_streaming_engaged(g_tx_session_for_progress, &engaged);
            fprintf(stdout, "[VAL][TX] post-handshake: mtu=%zu, send_streaming=%s, accept_streaming=%s, init_tx_mode=%s, streaming=%s\n",
                    mtu, send_stream_ok ? "yes" : "no", recv_stream_ok ? "yes" : "no", tx_mode_name(m0), engaged ? "on" : "off");
            fflush(stdout);
            g_tx_post_handshake_printed = 1;
        }
    }
    val_tx_mode_t m = VAL_TX_STOP_AND_WAIT;
    if (val_get_current_tx_mode(g_tx_session_for_progress, &m) == VAL_OK)
    {
        if (g_last_mode_reported != m)
        {
            g_last_mode_reported = m;
            // Print a friendly name
            const char *name = tx_mode_name(m);
            int engaged = 0;
            (void)val_is_streaming_engaged(g_tx_session_for_progress, &engaged);
            fprintf(stdout, "[VAL][TX] mode changed -> %s%s\n", name, engaged ? " + streaming" : "");
            fflush(stdout);
        }
    }
    // Also show local streaming engagement transitions even if mode didn't change
    int s = 0;
    if (val_is_streaming_engaged(g_tx_session_for_progress, &s) == VAL_OK)
    {
        if (g_tx_stream_last != s)
        {
            g_tx_stream_last = s;
            fprintf(stdout, "[VAL][TX] streaming %s\n", s ? "ENGAGED" : "OFF");
            fflush(stdout);
        }
    }
}

// Pretty metrics section (sender)
#if VAL_ENABLE_METRICS
static void print_metrics_tx(val_session_t *tx)
{
    if (!tx)
        return;
    val_metrics_t m;
    if (val_get_metrics(tx, &m) != VAL_OK)
        return;
    fprintf(stdout, "\n==== Session Metrics (Sender) ====\n");
    fprintf(stdout, "Packets: sent=%llu recv=%llu\n", (unsigned long long)m.packets_sent, (unsigned long long)m.packets_recv);
    fprintf(stdout, "Bytes:   sent=%llu recv=%llu\n", (unsigned long long)m.bytes_sent, (unsigned long long)m.bytes_recv);
    fprintf(stdout, "Reliab:  timeouts=%u retrans=%u crc_err=%u\n", (unsigned)m.timeouts, (unsigned)m.retransmits, (unsigned)m.crc_errors);
    fprintf(stdout, "Session: handshakes=%u files_sent=%u rtt_samples=%u\n", (unsigned)m.handshakes, (unsigned)m.files_sent, (unsigned)m.rtt_samples);
    // Per-type breakdown (indexes map to packet type modulo 32; we print known types by name)
    const char *names[32] = {0};
    names[VAL_PKT_HELLO] = "HELLO";
    names[VAL_PKT_SEND_META] = "SEND_META";
    names[VAL_PKT_RESUME_REQ] = "RESUME_REQ";
    names[VAL_PKT_RESUME_RESP] = "RESUME_RESP";
    names[VAL_PKT_DATA] = "DATA";
    names[VAL_PKT_DATA_ACK] = "DATA_ACK";
    names[VAL_PKT_VERIFY] = "VERIFY";
    names[VAL_PKT_DONE] = "DONE";
    names[VAL_PKT_ERROR] = "ERROR";
    names[VAL_PKT_EOT] = "EOT";
    names[VAL_PKT_EOT_ACK] = "EOT_ACK";
    names[VAL_PKT_DONE_ACK] = "DONE_ACK";
    names[VAL_PKT_MODE_SYNC] = "MODE_SYNC";
    names[VAL_PKT_MODE_SYNC_ACK] = "MODE_SYNC_ACK";
    names[VAL_PKT_CANCEL & 31u] = "CANCEL"; // CANCEL is 0x18; we store modulo 32
    fprintf(stdout, "Per-type (send/recv):\n");
    for (unsigned i = 0; i < 32; ++i)
    {
        if (m.send_by_type[i] || m.recv_by_type[i])
        {
            const char *nm = names[i] ? names[i] : "T";
            fprintf(stdout, "  - %-12s s=%llu r=%llu (idx %u)\n", nm, (unsigned long long)m.send_by_type[i], (unsigned long long)m.recv_by_type[i], i);
        }
    }
    fflush(stdout);
}
#endif

// ---------------- End-of-run summary (sender) ----------------
// Per-file timing/throughput stat
typedef struct tx_file_stat_s {
    char *name;
    uint64_t bytes;      // bytes attempted to transfer (size - resume_offset)
    uint32_t start_ms;   // start timestamp
    uint32_t elapsed_ms; // completion duration
    int result;          // val_status_t as int
} tx_file_stat_t;

typedef struct
{
    char **sent;
    size_t sent_count;
    char **skipped;
    size_t skipped_count;
    size_t capacity; // allocate arrays up to number of input files
    // Timing/throughput
    tx_file_stat_t *stats;
    size_t stats_count;
    size_t stats_cap;
} tx_summary_t;

static tx_summary_t *g_tx_summary = NULL; // used by callbacks

static char *dup_cstr(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void tx_summary_add(char ***arr, size_t *count, size_t cap, const char *name)
{
    if (!arr || !count || !name)
        return;
    if (*count >= cap)
        return; // ignore extras beyond declared inputs
    char *cp = dup_cstr(name);
    if (!cp)
        return;
    (*arr)[*count] = cp;
    (*count)++;
}

static size_t tx_stats_start(tx_summary_t *sum, const char *filename, uint64_t bytes)
{
    if (!sum || !filename)
        return (size_t)-1;
    if (sum->stats_count == sum->stats_cap)
    {
        size_t newcap = sum->stats_cap ? (sum->stats_cap * 2) : 8;
        void *nn = realloc(sum->stats, newcap * sizeof(*sum->stats));
        if (!nn)
            return (size_t)-1;
        sum->stats = (tx_file_stat_t *)nn;
        sum->stats_cap = newcap;
    }
    size_t idx = sum->stats_count++;
    sum->stats[idx].name = dup_cstr(filename);
    sum->stats[idx].bytes = bytes;
    sum->stats[idx].start_ms = tcp_now_ms();
    sum->stats[idx].elapsed_ms = 0u;
    sum->stats[idx].result = 0;
    return idx;
}

static void tx_stats_complete(tx_summary_t *sum, const char *filename, int result)
{
    if (!sum || !filename)
        return;
    for (size_t i = sum->stats_count; i > 0; --i)
    {
        size_t idx = i - 1;
        if (sum->stats[idx].name && sum->stats[idx].elapsed_ms == 0u && strcmp(sum->stats[idx].name, filename) == 0)
        {
            uint32_t now = tcp_now_ms();
            uint32_t start = sum->stats[idx].start_ms;
            sum->stats[idx].elapsed_ms = (now >= start) ? (now - start) : 0u;
            sum->stats[idx].result = result;
            return;
        }
    }
}

static void on_file_start_tx(const char *filename, const char *sender_path, uint64_t file_size, uint64_t resume_offset)
{
    (void)sender_path;
    if (!g_tx_summary || !filename)
        return;
    uint64_t bytes = (file_size > resume_offset) ? (file_size - resume_offset) : 0ull;
    (void)tx_stats_start(g_tx_summary, filename, bytes);
}

static void on_file_complete_tx(const char *filename, const char *sender_path, val_status_t result)
{
    (void)sender_path;
    if (!g_tx_summary || !filename)
        return;
    if (result == VAL_OK)
    {
        tx_summary_add(&g_tx_summary->sent, &g_tx_summary->sent_count, g_tx_summary->capacity, filename);
    }
    else if (result == VAL_SKIPPED)
    {
        tx_summary_add(&g_tx_summary->skipped, &g_tx_summary->skipped_count, g_tx_summary->capacity, filename);
    }
    tx_stats_complete(g_tx_summary, filename, (int)result);
}

static void tx_summary_print_and_free(const tx_summary_t *sum, val_status_t final_status)
{
    if (!sum)
        return;
    // Detailed per-file timings
    fprintf(stdout, "\n==== File Transfer Details (Sender) ====\n");
    if (sum->stats_count == 0)
    {
        fprintf(stdout, "(no files)\n");
    }
    else
    {
        for (size_t i = 0; i < sum->stats_count; ++i)
        {
            const char *name = sum->stats[i].name ? sum->stats[i].name : "<unknown>";
            uint32_t ms = sum->stats[i].elapsed_ms;
            double secs = ms / 1000.0;
            if (sum->stats[i].result == VAL_OK && ms > 0 && sum->stats[i].bytes > 0)
            {
                double mibps = secs > 0.0 ? ((double)sum->stats[i].bytes / (1024.0 * 1024.0)) / secs : 0.0;
                fprintf(stdout, "  - %s: time=%.3fs, rate=%.2f MB/s\n", name, secs, mibps);
            }
            else
            {
                fprintf(stdout, "  - %s: time=%.3fs, rate=N/A\n", name, secs);
            }
        }
    }

    fprintf(stdout, "\n==== Transfer Summary (Sender) ====\n");
    fprintf(stdout, "Files Sent (%zu):\n", (size_t)sum->sent_count);
    for (size_t i = 0; i < sum->sent_count; ++i)
        fprintf(stdout, "  - %s\n", sum->sent[i]);
    fprintf(stdout, "Files Skipped (%zu):\n", (size_t)sum->skipped_count);
    for (size_t i = 0; i < sum->skipped_count; ++i)
        fprintf(stdout, "  - %s\n", sum->skipped[i]);
    const char *status = (final_status == VAL_OK) ? "Success" : (final_status == VAL_ERR_ABORTED ? "Aborted" : "Failed");
    fprintf(stdout, "Final Status: %s\n", status);
    fflush(stdout);

    // free
    for (size_t i = 0; i < sum->sent_count; ++i)
        free(sum->sent[i]);
    for (size_t i = 0; i < sum->skipped_count; ++i)
        free(sum->skipped[i]);
    free(sum->sent);
    free(sum->skipped);
    for (size_t i = 0; i < sum->stats_count; ++i)
        free(sum->stats[i].name);
    free(sum->stats);
}

int main(int argc, char **argv)
{
    // Defaults
    size_t packet = 4096; // example MTU
    val_resume_mode_t resume_mode = VAL_RESUME_CRC_TAIL_OR_ZERO;
    unsigned tail_bytes = 16384;
    int log_level = -1; // -1 means: derive from env or default
    const char *log_file_path = NULL;
    int opt_streaming = 1;        // default on
    int opt_accept_streaming = 1; // default on
    int accept_streaming_explicit = 0; // track if user overrode accept-streaming
    // Adaptive TX config options
    val_tx_mode_t opt_tx_mode = VAL_TX_WINDOW_16;
    val_tx_mode_t opt_max_mode = VAL_TX_WINDOW_64;
    unsigned opt_degrade = 3;
    unsigned opt_upgrade = 10;
    // Timeout/retry defaults (can be overridden by flags)
    unsigned opt_min_timeout = 100;
    unsigned opt_max_timeout = 10000;
    unsigned opt_meta_retries = 4;
    unsigned opt_data_retries = 3;
    unsigned opt_ack_retries = 3;
    unsigned opt_backoff_ms = 100;

    // Parse optional flags
    int argi = 1;
    while (argi < argc && strncmp(argv[argi], "-", 1) == 0)
    {
        const char *arg = argv[argi++];
        if (strcmp(arg, "--mtu") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            unsigned v = 0;
            if (parse_uint(argv[argi++], &v) != 0)
            {
                fprintf(stderr, "Invalid --mtu value\n");
                return 1;
            }
            if (v < VAL_MIN_PACKET_SIZE || v > VAL_MAX_PACKET_SIZE)
            {
                fprintf(stderr, "--mtu must be between %u and %u\n", (unsigned)VAL_MIN_PACKET_SIZE,
                        (unsigned)VAL_MAX_PACKET_SIZE);
                return 1;
            }
            packet = (size_t)v;
        }
        else if (strcmp(arg, "--resume") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            if (parse_resume_mode(argv[argi++], &resume_mode) != 0)
            {
                fprintf(stderr, "Invalid --resume value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--tail-bytes") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            unsigned v = 0;
            if (parse_uint(argv[argi++], &v) != 0)
            {
                fprintf(stderr, "Invalid --tail-bytes value\n");
                return 1;
            }
            tail_bytes = v;
        }
        else if (strcmp(arg, "--log-level") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            log_level = parse_level(argv[argi++]);
        }
        else if (strcmp(arg, "--streaming") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            const char *v = argv[argi++];
            if (strieq(v, "on") || strieq(v, "true") || strieq(v, "1"))
                opt_streaming = 1;
            else if (strieq(v, "off") || strieq(v, "false") || strieq(v, "0"))
                opt_streaming = 0;
            else
            {
                fprintf(stderr, "Invalid --streaming value; use on|off\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--accept-streaming") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            const char *v = argv[argi++];
            if (strieq(v, "on") || strieq(v, "true") || strieq(v, "1"))
                opt_accept_streaming = 1;
            else if (strieq(v, "off") || strieq(v, "false") || strieq(v, "0"))
                opt_accept_streaming = 0;
            else
            {
                fprintf(stderr, "Invalid --accept-streaming value; use on|off\n");
                return 1;
            }
            accept_streaming_explicit = 1;
        }
        else if (strcmp(arg, "--log-file") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            log_file_path = argv[argi++];
        }
        else if (strcmp(arg, "--verbose") == 0)
        {
            log_level = 4; // debug
        }
        else if (strcmp(arg, "--tx-mode") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            val_tx_mode_t m;
            if (parse_tx_mode(argv[argi++], &m) != 0)
            {
                fprintf(stderr, "Invalid --tx-mode; use stop|1|2|4|8|16|32|64\n");
                return 1;
            }
            opt_tx_mode = m;
        }
        else if (strcmp(arg, "--max-mode") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            val_tx_mode_t m;
            if (parse_tx_mode(argv[argi++], &m) != 0)
            {
                fprintf(stderr, "Invalid --max-mode; use stop|1|2|4|8|16|32|64\n");
                return 1;
            }
            opt_max_mode = m;
        }
        else if (strcmp(arg, "--degrade") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            unsigned v = 0;
            if (parse_uint(argv[argi++], &v) != 0)
            {
                fprintf(stderr, "Invalid --degrade value\n");
                return 1;
            }
            opt_degrade = v;
        }
        else if (strcmp(arg, "--upgrade") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            unsigned v = 0;
            if (parse_uint(argv[argi++], &v) != 0)
            {
                fprintf(stderr, "Invalid --upgrade value\n");
                return 1;
            }
            opt_upgrade = v;
        }
        else if (strcmp(arg, "--min-timeout") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_min_timeout) != 0)
            {
                fprintf(stderr, "Invalid --min-timeout value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--max-timeout") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_max_timeout) != 0)
            {
                fprintf(stderr, "Invalid --max-timeout value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--meta-retries") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_meta_retries) != 0)
            {
                fprintf(stderr, "Invalid --meta-retries value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--data-retries") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_data_retries) != 0)
            {
                fprintf(stderr, "Invalid --data-retries value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--ack-retries") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_ack_retries) != 0)
            {
                fprintf(stderr, "Invalid --ack-retries value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--backoff") == 0)
        {
            if (argi >= argc || parse_uint(argv[argi++], &opt_backoff_ms) != 0)
            {
                fprintf(stderr, "Invalid --backoff value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", arg);
            usage(argv[0]);
            return 1;
        }
    }

    // Required positionals after options
    if (argc - argi < 3)
    {
        usage(argv[0]);
        return 1;
    }
    const char *host = argv[argi++];
    unsigned short port = (unsigned short)atoi(argv[argi++]);
    int fd = tcp_connect(host, port);
    if (fd < 0)
    {
        fprintf(stderr, "connect failed\n");
        return 2;
    }

    uint8_t *send_buf = (uint8_t *)malloc(packet);
    uint8_t *recv_buf = (uint8_t *)malloc(packet);
    if (!send_buf || !recv_buf)
    {
        fprintf(stderr, "oom\n");
        return 3;
    }

    // Unified streaming semantics: if streaming is off and user didn't explicitly allow accepting,
    // then also refuse incoming streaming by default.
    if (!opt_streaming && !accept_streaming_explicit)
        opt_accept_streaming = 0;

    val_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport.send = tp_send;
    cfg.transport.recv = tp_recv;
    // Provide is_connected callback to help detect disconnections promptly
    cfg.transport.is_connected = tp_is_connected;
    cfg.transport.flush = tp_flush;
    cfg.transport.io_context = &fd;
    cfg.filesystem.fopen = fs_fopen;
    cfg.filesystem.fread = fs_fread;
    cfg.filesystem.fwrite = fs_fwrite;
    cfg.filesystem.fseek = fs_fseek;
    cfg.filesystem.ftell = fs_ftell;
    cfg.filesystem.fclose = fs_fclose;
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = packet;
    cfg.system.get_ticks_ms = tcp_now_ms;
    cfg.system.delay_ms = tcp_sleep_ms;
    // Optional debug logger setup (env overrides)
    if (log_level < 0)
    {
        const char *lvl_env = getenv("VAL_LOG_LEVEL");
        if (lvl_env)
            log_level = parse_level(lvl_env);
    }
    if (!log_file_path)
    {
        log_file_path = getenv("VAL_LOG_FILE");
    }
    if (log_file_path && log_file_path[0])
    {
        g_logf = fopen(log_file_path, "a");
    }
    cfg.debug.log = console_logger;
    cfg.debug.min_level = (log_level >= 0 ? log_level : 3);

    // Adaptive timeout bounds (RFC6298-based): min/max clamp for computed RTO
    cfg.timeouts.min_timeout_ms = opt_min_timeout;   // floor for timeouts
    cfg.timeouts.max_timeout_ms = opt_max_timeout;   // ceiling for timeouts
    cfg.retries.handshake_retries = 4;
    cfg.retries.meta_retries = (uint8_t)opt_meta_retries;
    cfg.retries.data_retries = (uint8_t)opt_data_retries;
    cfg.retries.ack_retries = (uint8_t)opt_ack_retries;
    cfg.retries.backoff_ms_base = opt_backoff_ms;
    // Resume configuration: use resume.mode defaults unless policy provided
    cfg.resume.mode = resume_mode;
    cfg.resume.crc_verify_bytes = tail_bytes;
    // Adaptive TX defaults; allow flags to override
    cfg.adaptive_tx.max_performance_mode = opt_max_mode;
    cfg.adaptive_tx.preferred_initial_mode = opt_tx_mode;
    // Single policy: allow_streaming governs both directions
    cfg.adaptive_tx.allow_streaming = (uint8_t)((opt_streaming || opt_accept_streaming) ? 1 : 0);
    cfg.adaptive_tx.retransmit_cache_enabled = 0;
    cfg.adaptive_tx.degrade_error_threshold = (uint16_t)opt_degrade;
    cfg.adaptive_tx.recovery_success_threshold = (uint16_t)opt_upgrade;
    cfg.adaptive_tx.mode_sync_interval = 0;
    // Register progress callback now so the session captures it
    cfg.callbacks.on_progress = on_progress_announce_mode;
    // Prepare summary tracking and register file callbacks BEFORE session create
    tx_summary_t sum = {0};
    // We'll set capacity after we know nfiles (post-args), but callbacks can be registered now
    g_tx_summary = &sum;
    cfg.callbacks.on_file_start = on_file_start_tx;
    cfg.callbacks.on_file_complete = on_file_complete_tx;

    val_session_t *tx = NULL;
    uint32_t init_detail = 0;
    val_status_t init_rc = val_session_create(&cfg, &tx, &init_detail);
    if (init_rc != VAL_OK || !tx)
    {
        fprintf(stderr, "session create failed (rc=%d detail=0x%08X)\n", (int)init_rc, (unsigned)init_detail);
        return 4;
    }
    // Store the session for the callback to query current mode
    g_tx_session_for_progress = tx;

    const char **files = (const char **)&argv[argi];
    size_t nfiles = (size_t)(argc - argi);
    fprintf(stdout, "[VAL][TX] config: tx_init=%s, tx_cap<=%s, degrade=%u, upgrade=%u\n", tx_mode_name(cfg.adaptive_tx.preferred_initial_mode),
            tx_mode_name(cfg.adaptive_tx.max_performance_mode), (unsigned)cfg.adaptive_tx.degrade_error_threshold,
            (unsigned)cfg.adaptive_tx.recovery_success_threshold);
    fflush(stdout);

    // Optional: print current mode at start
    {
        val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT;
        if (val_get_current_tx_mode(tx, &mode) == VAL_OK)
        {
            int engaged = 0;
            (void)val_is_streaming_engaged(tx, &engaged);
            fprintf(stdout, "[VAL][TX] current-mode=%s%s\n", tx_mode_name(mode), engaged ? " + streaming" : "");
            fflush(stdout);
        }
    }

    // Finalize summary buffers now that we know nfiles
    sum.capacity = nfiles;
    sum.sent = (char **)calloc(nfiles ? nfiles : 1, sizeof(char *));
    sum.skipped = (char **)calloc(nfiles ? nfiles : 1, sizeof(char *));

    val_status_t st = val_send_files(tx, files, nfiles, NULL);
    if (st != VAL_OK)
    {
        val_status_t lc = VAL_OK;
        uint32_t det = 0;
        (void)val_get_last_error(tx, &lc, &det);
#if VAL_ENABLE_ERROR_STRINGS
        char buf[256];
        val_format_error_report(lc, det, buf, sizeof(buf));
        fprintf(stderr, "send failed: %s\n", buf);
#else
        fprintf(stderr, "send failed: code=%d detail=0x%08X\n", (int)lc, (unsigned)det);
#endif
    }
    else
    {
        // After successful handshake/transfer, print negotiated settings
        size_t mtu = 0;
        if (val_get_effective_packet_size(tx, &mtu) == VAL_OK)
        {
            int send_stream_ok = 0, recv_stream_ok = 0;
            (void)val_get_streaming_allowed(tx, &send_stream_ok, &recv_stream_ok);
            val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT;
            (void)val_get_current_tx_mode(tx, &mode);
            int engaged = 0;
            (void)val_is_streaming_engaged(tx, &engaged);
            fprintf(stdout, "[VAL][TX] negotiated: mtu=%zu, send_streaming=%s, accept_streaming=%s, init_tx_mode=%s, streaming=%s\n", mtu,
                    send_stream_ok ? "yes" : "no", recv_stream_ok ? "yes" : "no", tx_mode_name(mode), engaged ? "on" : "off");
            fflush(stdout);
        }
    }

#if VAL_ENABLE_METRICS
    print_metrics_tx(tx);
#endif

    val_session_destroy(tx);
    tcp_close(fd);
    free(send_buf);
    free(recv_buf);
    tx_summary_print_and_free(&sum, st);
    return st == VAL_OK ? 0 : 5;
}
