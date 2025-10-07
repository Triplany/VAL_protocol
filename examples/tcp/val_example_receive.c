#include "common/tcp_util.h"
#include "val_protocol.h"
#include "val_error_strings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
// For summary report
#include <assert.h>

// --- Optional: packet capture demonstration (metadata only, no payload) ---
static void on_packet_capture_rx(void *ctx, const val_packet_record_t *rec)
{
    (void)ctx;
    if (!rec)
        return;
    const char *dir = (rec->direction == VAL_DIR_TX) ? "TX" : "RX";
    fprintf(stdout, "[VAL][CAP][%s] type=%u wire=%u payload=%u off=%llu crc=%u t=%u\n",
            dir, (unsigned)rec->type, (unsigned)rec->wire_len, (unsigned)rec->payload_len,
        (unsigned long long)rec->offset, (unsigned)(rec->crc_ok ? 1 : 0), (unsigned)rec->timestamp_ms);
    fflush(stdout);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--mtu N] [--resume MODE] [--tail-bytes N] [--streaming on|off] [--accept-streaming on|off] "
            "[--no-validation] [--log-level L] [--log-file PATH] "
            "[--tx-mode MODE] [--max-mode MODE] [--degrade N] [--upgrade N] "
            "<port> "
            "<outdir>\n"
            "  --mtu N          Packet size/MTU (default 4096; min %u, max %u)\n"
            "  --resume MODE    Resume mode: never, skip, tail\n"
            "  --tail-bytes N   Tail verification cap in bytes for tail mode (default 1024)\n"
            "  --streaming      Enable sender streaming pacing if peer accepts (default on)\n"
            "  --accept-streaming  Accept incoming peer streaming pacing (default on). Note: --streaming off implies\n"
            "                     --accept-streaming off unless explicitly overridden with --accept-streaming on.\n"
            "  --no-validation  Disable metadata validation (accept all files)\n"
            "  --tx-mode MODE   Preferred initial TX mode rung: stop|1|2|4|8|16|32|64 (default 16)\n"
            "  --max-mode MODE  Max TX mode this endpoint supports: stop|1|2|4|8|16|32|64 (default 64)\n"
            "  --degrade N      Errors before degrading a rung (default 3)\n"
            "  --upgrade N      Successes before upgrading a rung (default 10)\n"
            "  --min-timeout MS Minimum adaptive timeout clamp in ms (default 100)\n"
            "  --max-timeout MS Maximum adaptive timeout clamp in ms (default 10000)\n"
            "  --meta-retries N Retries while waiting for SEND_META (default 4)\n"
            "  --data-retries N Retries while waiting for DATA (default 3)\n"
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
        *out = VAL_RESUME_TAIL;
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
    // Accept numeric aliases and words
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
            *received = 0; // timeout -> short read
        return 0;
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
    fprintf(out, "[%s][RX][%s] %s:%d: %s\n", ts, lvl_name(level), file, line, message ? message : "");
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
static size_t fs_fread(void *ctx, void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    return fread(buffer, size, count, (FILE *)file);
}
static size_t fs_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    return fwrite(buffer, size, count, (FILE *)file);
}
static int fs_fseek(void *ctx, void *file, int64_t offset, int whence)
{
    (void)ctx;
#ifdef _WIN32
    return _fseeki64((FILE *)file, offset, whence);
#else
    return fseeko((FILE *)file, (off_t)offset, whence);
#endif
}
static int64_t fs_ftell(void *ctx, void *file)
{
    (void)ctx;
#ifdef _WIN32
    return _ftelli64((FILE *)file);
#else
    return (int64_t)ftello((FILE *)file);
#endif
}
static int fs_fclose(void *ctx, void *file)
{
    (void)ctx;
    return fclose((FILE *)file);
}

// --- Optional: progress callback to announce LOCAL TX mode changes (receiver side) ---
static val_session_t *g_rx_session_for_progress = NULL;            // set after session create
static val_tx_mode_t g_rx_last_mode_reported = (val_tx_mode_t)255; // invalid sentinel
static int g_rx_post_handshake_printed = 0;
static val_tx_mode_t g_peer_last_mode_reported = (val_tx_mode_t)255; // last known peer mode
static int g_peer_stream_last = -1; // unknown
static void on_progress_announce_mode_rx(const val_progress_info_t *info)
{
    (void)info; // only used to trigger periodic checks
    if (!g_rx_session_for_progress)
        return;
    // One-time negotiated summary once handshake has completed
    if (!g_rx_post_handshake_printed)
    {
        size_t mtu = 0;
    bool send_stream_ok = false, recv_stream_ok = false;
        val_tx_mode_t m0 = VAL_TX_STOP_AND_WAIT;
        val_tx_mode_t pm0 = VAL_TX_STOP_AND_WAIT;
        if (val_get_effective_packet_size(g_rx_session_for_progress, &mtu) == VAL_OK)
        {
            (void)val_get_streaming_allowed(g_rx_session_for_progress, &send_stream_ok, &recv_stream_ok);
            (void)val_get_current_tx_mode(g_rx_session_for_progress, &m0);
            (void)val_get_peer_tx_mode(g_rx_session_for_progress, &pm0);
            bool pstream = false;
            (void)val_is_peer_streaming_engaged(g_rx_session_for_progress, &pstream);
        fprintf(stdout, "[VAL][RX] post-handshake: mtu=%zu, send_streaming=%s, accept_streaming=%s, local_tx_init=%s, peer_tx_init=%s, peer_streaming=%s\n",
            mtu, send_stream_ok ? "yes" : "no", recv_stream_ok ? "yes" : "no", tx_mode_name(m0), tx_mode_name(pm0), pstream ? "+streaming" : "off");
            fflush(stdout);
            g_rx_post_handshake_printed = 1;
        }
    }
    val_tx_mode_t m = VAL_TX_STOP_AND_WAIT;
    if (val_get_current_tx_mode(g_rx_session_for_progress, &m) == VAL_OK)
    {
        if (g_rx_last_mode_reported != m)
        {
            g_rx_last_mode_reported = m;
            fprintf(stdout, "[VAL][RX] local-tx mode changed -> %s\n", tx_mode_name(m));
            fflush(stdout);
        }
    }
    // Also show peer's current TX mode when it changes (via mode sync)
    val_tx_mode_t pm = VAL_TX_STOP_AND_WAIT;
    if (val_get_peer_tx_mode(g_rx_session_for_progress, &pm) == VAL_OK)
    {
        if (g_peer_last_mode_reported != pm)
        {
            g_peer_last_mode_reported = pm;
            fprintf(stdout, "[VAL][RX] peer-tx mode changed -> %s\n", tx_mode_name(pm));
            fflush(stdout);
        }
    }
    // And show peer streaming engage/disengage transitions
    bool ps = false;
    if (val_is_peer_streaming_engaged(g_rx_session_for_progress, &ps) == VAL_OK)
    {
        if (g_peer_stream_last != ps)
        {
            g_peer_stream_last = ps;
            fprintf(stdout, "[VAL][RX] peer streaming %s\n", ps ? "ENGAGED" : "OFF");
            fflush(stdout);
        }
    }
}

// Example metadata validator: basic size/type filtering
static val_validation_action_t example_validator(const val_meta_payload_t *meta, const char *target_path, void *context)
{
    (void)context;
    // Skip files larger than 10MB
    if (meta->file_size > 10ull * 1024ull * 1024ull)
    {
        printf("Skipping large file: %s (%llu bytes)\n", meta->filename, (unsigned long long)meta->file_size);
        return VAL_VALIDATION_SKIP;
    }
    const char *ext = strrchr(meta->filename, '.');
    if (ext && (strieq(ext, ".exe") || strieq(ext, ".bat")))
    {
        printf("Aborting session - blocked file type: %s -> %s\n", meta->filename, target_path);
        return VAL_VALIDATION_ABORT;
    }
    if (strstr(meta->filename, "temp") || strstr(meta->filename, ".tmp"))
    {
        printf("Skipping temporary file: %s\n", meta->filename);
        return VAL_VALIDATION_SKIP;
    }
    printf("Accepting file: %s (%llu bytes) -> %s\n", meta->filename, (unsigned long long)meta->file_size, target_path);
    return VAL_VALIDATION_ACCEPT;
}

// Pretty metrics section (receiver)
#if VAL_ENABLE_METRICS
static void print_metrics_rx(val_session_t *rx)
{
    if (!rx)
        return;
    val_metrics_t m;
    if (val_get_metrics(rx, &m) != VAL_OK)
        return;
    fprintf(stdout, "\n==== Session Metrics (Receiver) ====\n");
    fprintf(stdout, "Packets: sent=%llu recv=%llu\n", (unsigned long long)m.packets_sent, (unsigned long long)m.packets_recv);
    fprintf(stdout, "Bytes:   sent=%llu recv=%llu\n", (unsigned long long)m.bytes_sent, (unsigned long long)m.bytes_recv);
                bool send_stream_ok = false, recv_stream_ok = false;
    fprintf(stdout, "Session: handshakes=%u files_recv=%u rtt_samples=%u\n", (unsigned)m.handshakes, (unsigned)m.files_recv, (unsigned)m.rtt_samples);
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
    names[VAL_PKT_CANCEL & 31u] = "CANCEL";
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

// ---------------- End-of-run summary (receiver) ----------------
// Per-file timing/throughput stat
typedef struct rx_file_stat_s {
    char *name;
    uint64_t bytes;
    uint32_t start_ms;
    uint32_t elapsed_ms;
    int result;
} rx_file_stat_t;

typedef struct
{
    char **received;
    size_t received_count;
    char **skipped;
    size_t skipped_count;
    size_t capacity_hint; // not strictly needed; we’ll grow as needed
    // Timing/throughput
    rx_file_stat_t *stats;
    size_t stats_count;
    size_t stats_cap;
} rx_summary_t;

static rx_summary_t *g_rx_summary = NULL;

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

static void arr_push(char ***arr, size_t *count, const char *name)
{
    if (!arr || !count || !name)
        return;
    size_t idx = *count;
    char **cur = *arr;
    if (idx % 8 == 0)
    {
        size_t newcap = idx + 8;
        char **nn = (char **)realloc(cur, newcap * sizeof(char *));
        if (!nn)
            return;
        *arr = nn;
        cur = nn;
    }
    char *cp = dup_cstr(name);
    if (!cp)
        return;
    cur[idx] = cp;
    *count = idx + 1;
}

static size_t rx_stats_start(rx_summary_t *sum, const char *filename, uint64_t bytes)
{
    if (!sum || !filename)
        return (size_t)-1;
    if (sum->stats_count == sum->stats_cap)
    {
        size_t newcap = sum->stats_cap ? (sum->stats_cap * 2) : 8;
        void *nn = realloc(sum->stats, newcap * sizeof(*sum->stats));
        if (!nn)
            return (size_t)-1;
        sum->stats = (rx_file_stat_t *)nn;
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

static void rx_stats_complete(rx_summary_t *sum, const char *filename, int result)
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

static void on_file_start_rx(const char *filename, const char *sender_path, uint64_t file_size, uint64_t resume_offset)
{
    (void)sender_path;
    if (!g_rx_summary || !filename)
        return;
    uint64_t bytes = (file_size > resume_offset) ? (file_size - resume_offset) : 0ull;
    (void)rx_stats_start(g_rx_summary, filename, bytes);
}

static void on_file_complete_rx(const char *filename, const char *sender_path, val_status_t result)
{
    (void)sender_path;
    if (!g_rx_summary || !filename)
        return;
    if (result == VAL_OK)
        arr_push(&g_rx_summary->received, &g_rx_summary->received_count, filename);
    else if (result == VAL_SKIPPED)
        arr_push(&g_rx_summary->skipped, &g_rx_summary->skipped_count, filename);
    rx_stats_complete(g_rx_summary, filename, (int)result);
}

static void rx_summary_print_and_free(const rx_summary_t *sum, val_status_t final_status)
{
    if (!sum)
        return;
    // Detailed per-file timings
    fprintf(stdout, "\n==== File Transfer Details (Receiver) ====\n");
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

    fprintf(stdout, "\n==== Transfer Summary (Receiver) ====\n");
    fprintf(stdout, "Files Received (%zu):\n", (size_t)sum->received_count);
    for (size_t i = 0; i < sum->received_count; ++i)
        fprintf(stdout, "  - %s\n", sum->received[i]);
    fprintf(stdout, "Files Skipped (%zu):\n", (size_t)sum->skipped_count);
    for (size_t i = 0; i < sum->skipped_count; ++i)
        fprintf(stdout, "  - %s\n", sum->skipped[i]);
    const char *status = (final_status == VAL_OK) ? "Success" : (final_status == VAL_ERR_ABORTED ? "Aborted" : "Failed");
    fprintf(stdout, "Final Status: %s\n", status);
    fflush(stdout);

    for (size_t i = 0; i < sum->received_count; ++i)
        free(sum->received[i]);
    for (size_t i = 0; i < sum->skipped_count; ++i)
        free(sum->skipped[i]);
    free(sum->received);
    free(sum->skipped);
    for (size_t i = 0; i < sum->stats_count; ++i)
        free(sum->stats[i].name);
    free(sum->stats);
}

int main(int argc, char **argv)
{
    // Defaults
    size_t packet = 4096; // example MTU
    val_resume_mode_t resume_mode = VAL_RESUME_TAIL;
    unsigned tail_bytes = 16384; // cap for tail verification window
    int log_level = -1;
    const char *log_file_path = NULL;
    int opt_streaming = 1;        // default on
    int opt_accept_streaming = 1; // default on
    int accept_streaming_explicit = 0; // track if user provided explicit override
    // Adaptive TX customizations
    val_tx_mode_t opt_tx_mode = VAL_TX_WINDOW_16;
    val_tx_mode_t opt_max_mode = VAL_TX_WINDOW_64;
    unsigned opt_degrade = 3;
    unsigned opt_upgrade = 10;
    // Timeout/retry defaults (overridable)
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
        else if (strcmp(arg, "--no-validation") == 0)
        {
            // marker handled later; nothing to parse here
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
        else if (strcmp(arg, "--log-level") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            log_level = parse_level(argv[argi++]);
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
            log_level = 4;
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

    if (argc - argi < 2)
    {
        usage(argv[0]);
        return 1;
    }
    unsigned short port = (unsigned short)atoi(argv[argi++]);
    const char *outdir = argv[argi++];

    int l = tcp_listen(port, 1);
    if (l < 0)
    {
        fprintf(stderr, "listen failed\n");
        return 2;
    }
    fprintf(stdout, "Waiting for connection on port %u...\n", (unsigned)port);
    int fd = tcp_accept(l);
    if (fd < 0)
    {
        fprintf(stderr, "accept failed\n");
        tcp_close(l);
        return 3;
    }
    tcp_close(l);

    uint8_t *send_buf = (uint8_t *)malloc(packet);
    uint8_t *recv_buf = (uint8_t *)malloc(packet);
    if (!send_buf || !recv_buf)
    {
        fprintf(stderr, "oom\n");
        return 4;
    }

    // Unified streaming semantics: if streaming is off and accept-streaming wasn't explicitly set,
    // default to refusing incoming streaming as well.
    if (!opt_streaming && !accept_streaming_explicit)
        opt_accept_streaming = 0;

    val_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport.send = tp_send;
    cfg.transport.recv = tp_recv;
    // Provide is_connected to reduce spurious polling when connection breaks
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
    // Optional debug logger setup
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

    // Attach optional packet capture hook when VAL_CAPTURE env var is set
    const char *cap = getenv("VAL_CAPTURE");
    if (cap && (cap[0] == '1' || cap[0] == 'y' || cap[0] == 'Y' || cap[0] == 't' || cap[0] == 'T'))
    {
        cfg.capture.on_packet = on_packet_capture_rx;
        cfg.capture.context = NULL;
    }

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
    cfg.resume.tail_cap_bytes = tail_bytes;
    cfg.resume.min_verify_bytes = 0;
    cfg.resume.mismatch_skip = false; // default: restart on mismatch; set to true to skip on mismatch
    // Adaptive TX defaults; allow flags to override
    cfg.adaptive_tx.max_performance_mode = opt_max_mode;
    cfg.adaptive_tx.preferred_initial_mode = opt_tx_mode;
    // Single policy: allow_streaming governs both directions
    cfg.adaptive_tx.allow_streaming = ((opt_streaming || opt_accept_streaming) ? true : false);
    cfg.adaptive_tx.retransmit_cache_enabled = false;
    cfg.adaptive_tx.degrade_error_threshold = (uint16_t)opt_degrade;
    cfg.adaptive_tx.recovery_success_threshold = (uint16_t)opt_upgrade;
    cfg.adaptive_tx.mode_sync_interval = 0;
    // Register progress callback to announce TX mode changes during receive
    cfg.callbacks.on_progress = on_progress_announce_mode_rx;

    // Enable example metadata validation by default unless --no-validation present
    int use_validation = 1;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--no-validation") == 0)
        {
            use_validation = 0;
            break;
        }
    }
    if (use_validation)
    {
        val_config_set_validator(&cfg, example_validator, NULL);
        printf("Metadata validation enabled (example filter)\n");
    }
    else
    {
        val_config_validation_disabled(&cfg);
        printf("Metadata validation disabled\n");
    }

    // Attach callbacks to capture per-file outcomes BEFORE session create
    rx_summary_t sum = {0};
    g_rx_summary = &sum;
    cfg.callbacks.on_file_start = on_file_start_rx;
    cfg.callbacks.on_file_complete = on_file_complete_rx;

    val_session_t *rx = NULL;
    uint32_t init_detail = 0;
    val_status_t init_rc = val_session_create(&cfg, &rx, &init_detail);
    if (init_rc != VAL_OK || !rx)
    {
        fprintf(stderr, "session create failed (rc=%d detail=0x%08X)\n", (int)init_rc, (unsigned)init_detail);
        return 5;
    }
    // Store the session so the progress callback can query current mode
    g_rx_session_for_progress = rx;

    // Print initial mode preference/cap for visibility
    fprintf(stdout, "[VAL][RX] config: local_tx_init=%s, local_tx_cap<=%s, degrade=%u, upgrade=%u\n", tx_mode_name(cfg.adaptive_tx.preferred_initial_mode),
        tx_mode_name(cfg.adaptive_tx.max_performance_mode), (unsigned)cfg.adaptive_tx.degrade_error_threshold,
        (unsigned)cfg.adaptive_tx.recovery_success_threshold);
    fflush(stdout);
    // Note: current mode is initialized to stop(1:1) before handshake; it will switch after handshake.
    {
        val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT;
        if (val_get_current_tx_mode(rx, &mode) == VAL_OK)
        {
            fprintf(stdout, "[VAL][RX] local-tx-mode(pre-handshake)=%s\n", tx_mode_name(mode));
            fflush(stdout);
        }
    }

    // (callbacks already attached before session create)

    val_status_t st = val_receive_files(rx, outdir);
    if (st != VAL_OK)
    {
        val_status_t lc = VAL_OK;
        uint32_t det = 0;
        (void)val_get_last_error(rx, &lc, &det);
#if VAL_ENABLE_ERROR_STRINGS
        char buf[256];
        val_format_error_report(lc, det, buf, sizeof(buf));
        fprintf(stderr, "receive failed: %s\n", buf);
#else
        fprintf(stderr, "receive failed: code=%d detail=0x%08X\n", (int)lc, (unsigned)det);
#endif
    }
    else
    {
        // After successful handshake/receive, print negotiated settings
        size_t mtu = 0;
        if (val_get_effective_packet_size(rx, &mtu) == VAL_OK)
        {
            bool send_stream_ok = false, recv_stream_ok = false;
            (void)val_get_streaming_allowed(rx, &send_stream_ok, &recv_stream_ok);
        val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT;
        (void)val_get_current_tx_mode(rx, &mode);
        fprintf(stdout, "[VAL][RX] negotiated: mtu=%zu, send_streaming=%s, accept_streaming=%s, local_tx_init=%s\n", mtu,
            send_stream_ok ? "yes" : "no", recv_stream_ok ? "yes" : "no", tx_mode_name(mode));
            fflush(stdout);
        }
    }

#if VAL_ENABLE_METRICS
    print_metrics_rx(rx);
#endif

    val_session_destroy(rx);
    tcp_close(fd);
    free(send_buf);
    free(recv_buf);
    rx_summary_print_and_free(&sum, st);
    return st == VAL_OK ? 0 : 6;
}
