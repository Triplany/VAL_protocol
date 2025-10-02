#include "common/tcp_util.h"
#include "val_protocol.h"
#include "val_error_strings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--mtu N] [--resume MODE] [--tail-bytes N] [--streaming on|off] [--accept-streaming on|off] [--log-level "
            "L] [--log-file PATH] <host> <port> <file1> [file2 "
            "...>]\n"
            "  --mtu N          Packet size/MTU (default 4096; min %u, max %u)\n"
            "  --resume MODE    Resume mode: never, skip, tail, tail_or_zero, full, full_or_zero\n"
            "  --tail-bytes N   Tail verification bytes for tail modes (default 1024)\n"
            "  --streaming      Enable sender streaming pacing if peer accepts (default on)\n"
            "  --accept-streaming  Accept incoming peer streaming pacing (default on)\n"
            "  --log-level L    Runtime log verbosity: off, crit, warn, info, debug, trace, or 0..5\n"
            "  --log-file PATH  If set, append logs to this file (else stderr)\n",
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
    cfg.timeouts.min_timeout_ms = 100;   // floor for timeouts
    cfg.timeouts.max_timeout_ms = 10000; // ceiling for timeouts
    cfg.retries.handshake_retries = 3;
    cfg.retries.meta_retries = 2;
    cfg.retries.data_retries = 2;
    cfg.retries.ack_retries = 2;
    cfg.retries.backoff_ms_base = 100;
    // Resume configuration: use resume.mode defaults unless policy provided
    cfg.resume.mode = resume_mode;
    cfg.resume.crc_verify_bytes = tail_bytes;
    // Adaptive TX defaults; allow flags to override
    cfg.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    cfg.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_16;
    cfg.adaptive_tx.streaming_enabled = (uint8_t)(opt_streaming ? 1 : 0);
    cfg.adaptive_tx.accept_incoming_streaming = (uint8_t)(opt_accept_streaming ? 1 : 0);
    cfg.adaptive_tx.retransmit_cache_enabled = 0;
    cfg.adaptive_tx.degrade_error_threshold = 3;
    cfg.adaptive_tx.recovery_success_threshold = 10;
    cfg.adaptive_tx.mode_sync_interval = 0;

    val_session_t *tx = NULL;
    uint32_t init_detail = 0;
    val_status_t init_rc = val_session_create(&cfg, &tx, &init_detail);
    if (init_rc != VAL_OK || !tx)
    {
        fprintf(stderr, "session create failed (rc=%d detail=0x%08X)\n", (int)init_rc, (unsigned)init_detail);
        return 4;
    }

    const char **files = (const char **)&argv[argi];
    size_t nfiles = (size_t)(argc - argi);
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
            fprintf(stdout, "[VAL][TX] negotiated: mtu=%zu, send_streaming=%s, accept_streaming=%s, init_tx_mode=%u\n", mtu,
                    send_stream_ok ? "yes" : "no", recv_stream_ok ? "yes" : "no", (unsigned)mode);
            fflush(stdout);
        }
    }

#if VAL_ENABLE_METRICS
    {
        val_metrics_t m;
        if (val_get_metrics(tx, &m) == VAL_OK)
        {
            fprintf(stdout,
                    "[VAL][TX][metrics] pkts_sent=%llu pkts_recv=%llu bytes_sent=%llu bytes_recv=%llu timeouts=%u retrans=%u "
                    "crc_err=%u handshakes=%u files_sent=%u rtt_samples=%u\n",
                    (unsigned long long)m.packets_sent, (unsigned long long)m.packets_recv, (unsigned long long)m.bytes_sent,
                    (unsigned long long)m.bytes_recv, (unsigned)m.timeouts, (unsigned)m.retransmits, (unsigned)m.crc_errors,
                    (unsigned)m.handshakes, (unsigned)m.files_sent, (unsigned)m.rtt_samples);
            fflush(stdout);
        }
    }
#endif

    val_session_destroy(tx);
    tcp_close(fd);
    free(send_buf);
    free(recv_buf);
    return st == VAL_OK ? 0 : 5;
}
