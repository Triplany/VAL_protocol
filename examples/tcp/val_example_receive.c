#include "common/tcp_util.h"
#include "val_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--mtu N] [--policy NAME|ID] [--no-validation] [--log-level L] [--log-file PATH] <port> <outdir>\n"
            "  --mtu N        Packet size/MTU (default 4096; min %u, max %u)\n"
            "  --policy P     Resume policy name or numeric ID:\n"
            "                 none(0), safe(1), start_zero(2), skip_if_exists(3),\n"
            "                 skip_if_different(4), always_skip(5), strict_only(6)\n"
            "  --no-validation  Disable metadata validation (accept all files)\n",
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

static int parse_policy(const char *s, val_resume_policy_t *out)
{
    if (!s || !out)
        return -1;
    // Accept numeric IDs
    if (s[0] >= '0' && s[0] <= '9')
    {
        unsigned u = 0;
        if (parse_uint(s, &u) != 0)
            return -1;
        if (u > 6)
            return -1;
        *out = (val_resume_policy_t)u;
        return 0;
    }
    // Accept common names (case-insensitive)
    if (strieq(s, "none"))
        *out = VAL_RESUME_POLICY_NONE;
    else if (strieq(s, "safe") || strieq(s, "safe_default"))
        *out = VAL_RESUME_POLICY_SAFE_DEFAULT;
    else if (strieq(s, "start_zero") || strieq(s, "always_start_zero"))
        *out = VAL_RESUME_POLICY_ALWAYS_START_ZERO;
    else if (strieq(s, "skip_if_exists") || strieq(s, "always_skip_if_exists"))
        *out = VAL_RESUME_POLICY_ALWAYS_SKIP_IF_EXISTS;
    else if (strieq(s, "skip_if_different"))
        *out = VAL_RESUME_POLICY_SKIP_IF_DIFFERENT;
    else if (strieq(s, "always_skip"))
        *out = VAL_RESUME_POLICY_ALWAYS_SKIP;
    else if (strieq(s, "strict_only") || strieq(s, "strict_resume_only"))
        *out = VAL_RESUME_POLICY_STRICT_RESUME_ONLY;
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
    if (_stricmp(s, "off") == 0)
        return 0;
    if (_stricmp(s, "crit") == 0 || _stricmp(s, "critical") == 0)
        return 1;
    if (_stricmp(s, "warn") == 0 || _stricmp(s, "warning") == 0)
        return 2;
    if (_stricmp(s, "info") == 0)
        return 3;
    if (_stricmp(s, "debug") == 0)
        return 4;
    if (_stricmp(s, "trace") == 0 || _stricmp(s, "verbose") == 0)
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
    if (ext && (_stricmp(ext, ".exe") == 0 || _stricmp(ext, ".bat") == 0))
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

int main(int argc, char **argv)
{
    // Defaults
    size_t packet = 4096;                                // example MTU
    val_resume_policy_t policy = VAL_RESUME_POLICY_NONE; // legacy unless specified
    int log_level = -1;
    const char *log_file_path = NULL;

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
        else if (strcmp(arg, "--policy") == 0)
        {
            if (argi >= argc)
            {
                usage(argv[0]);
                return 1;
            }
            if (parse_policy(argv[argi++], &policy) != 0)
            {
                fprintf(stderr, "Invalid --policy value\n");
                return 1;
            }
        }
        else if (strcmp(arg, "--no-validation") == 0)
        {
            // marker handled later; nothing to parse here
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

    val_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.transport.send = tp_send;
    cfg.transport.recv = tp_recv;
    // Leave is_connected NULL to let core assume connection until I/O fails
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

    // Adaptive timeout bounds (RFC6298-based): min/max clamp for computed RTO
    cfg.timeouts.min_timeout_ms = 100;   // floor for timeouts
    cfg.timeouts.max_timeout_ms = 10000; // ceiling for timeouts
    cfg.retries.handshake_retries = 3;
    cfg.retries.meta_retries = 2;
    cfg.retries.data_retries = 2;
    cfg.retries.ack_retries = 2;
    cfg.retries.backoff_ms_base = 100;
    // Resume configuration: keep legacy defaults unless policy provided
    cfg.resume.mode = VAL_RESUME_CRC_VERIFY;
    cfg.resume.verify_bytes = 16384;
    cfg.resume.policy = policy;

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

    val_session_t *rx = val_session_create(&cfg);
    if (!rx)
    {
        fprintf(stderr, "session create failed\n");
        return 5;
    }

    val_status_t st = val_receive_files(rx, outdir);
    if (st != VAL_OK)
    {
        val_status_t lc = VAL_OK;
        uint32_t det = 0;
        (void)val_get_last_error(rx, &lc, &det);
#ifdef VAL_HOST_UTILITIES
        char buf[256];
        val_format_error_report(lc, det, buf, sizeof(buf));
        fprintf(stderr, "receive failed: %s\n", buf);
#else
        fprintf(stderr, "receive failed: code=%d detail=0x%08X\n", (int)lc, (unsigned)det);
#endif
    }

    val_session_destroy(rx);
    tcp_close(fd);
    free(send_buf);
    free(recv_buf);
    return st == VAL_OK ? 0 : 6;
}
