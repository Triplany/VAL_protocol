#include "common/tcp_util.h"
#include "val_protocol.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--mtu N] [--policy NAME|ID] <port> <outdir>\n"
            "  --mtu N        Packet size/MTU (default 4096; min %u, max %u)\n"
            "  --policy P     Resume policy name or numeric ID:\n"
            "                 none(0), safe(1), start_zero(2), skip_if_exists(3),\n"
            "                 skip_if_different(4), always_skip(5), strict_only(6)\n",
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
    if (tcp_recv_all(fd, buffer, buffer_size, timeout_ms) != 0)
    {
        if (received)
            *received = 0;
        return -1;
    }
    if (received)
        *received = buffer_size;
    return 0;
}

int main(int argc, char **argv)
{
    // Defaults
    size_t packet = 4096;                                // example MTU
    val_resume_policy_t policy = VAL_RESUME_POLICY_NONE; // legacy unless specified

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
    cfg.transport.io_context = &fd;
    cfg.buffers.send_buffer = send_buf;
    cfg.buffers.recv_buffer = recv_buf;
    cfg.buffers.packet_size = packet;
    // Minimal timeouts/backoff
    cfg.timeouts.handshake_ms = 5000;
    cfg.timeouts.meta_ms = 10000;
    cfg.timeouts.data_ms = 10000;
    cfg.timeouts.ack_ms = 5000;
    cfg.timeouts.idle_ms = 1000;
    cfg.retries.handshake_retries = 3;
    cfg.retries.meta_retries = 2;
    cfg.retries.data_retries = 2;
    cfg.retries.ack_retries = 2;
    cfg.retries.backoff_ms_base = 100;
    // Resume configuration: keep legacy defaults unless policy provided
    cfg.resume.mode = VAL_RESUME_CRC_VERIFY;
    cfg.resume.verify_bytes = 16384;
    cfg.resume.policy = policy;

    val_session_t *rx = val_session_create(&cfg);
    if (!rx)
    {
        fprintf(stderr, "session create failed\n");
        return 5;
    }

    val_status_t st = val_receive_files(rx, outdir);
    if (st != VAL_OK)
        fprintf(stderr, "receive failed: %d\n", (int)st);

    val_session_destroy(rx);
    tcp_close(fd);
    free(send_buf);
    free(recv_buf);
    return st == VAL_OK ? 0 : 6;
}
