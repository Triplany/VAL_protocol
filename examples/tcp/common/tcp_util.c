// Enable required POSIX features on non-Windows before any system headers
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "tcp_util.h"
#include <string.h>

#if defined(_WIN32)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
static int wsa_initialized = 0;
static void ensure_wsa(void)
{
    if (!wsa_initialized)
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0)
            wsa_initialized = 1;
    }
}
static int closesock(int fd)
{
    return closesocket((SOCKET)fd);
}

int tcp_is_connected(int fd)
{
    // Non-blocking connectivity check: SO_ERROR + select + MSG_PEEK
    int err = 0;
    int len = (int)sizeof(err);
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
        return -1;
    if (err != 0)
        return 0; // socket has error
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET((SOCKET)fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int r = select(0 /* ignored on Winsock */, &rfds, NULL, NULL, &tv);
    if (r > 0)
    {
        // If readable, peek to detect orderly shutdown
        char ch;
        int n = recv((SOCKET)fd, &ch, 1, MSG_PEEK);
        if (n == 0)
            return 0; // closed by peer
    }
    return 1;
}

void tcp_flush(int fd)
{
    // Winsock has no explicit flush; disable Nagle to reduce latency
    int yes = 1;
    setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
}

uint32_t tcp_now_ms(void)
{
    static LARGE_INTEGER freq = {0}, start = {0};
    if (freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
    if (elapsed < 0)
        elapsed = 0;
    if (elapsed > 4294967295.0)
        elapsed = fmod(elapsed, 4294967296.0);
    return (uint32_t)(elapsed + 0.5);
}

void tcp_sleep_ms(unsigned ms)
{
    Sleep(ms);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <sys/ioctl.h>   // ioctl, FIONREAD
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h> // clock_gettime, nanosleep
#include <unistd.h>
static void ensure_wsa(void)
{
}
static int closesock(int fd)
{
    return close(fd);
}

int tcp_is_connected(int fd)
{
    // Check SO_ERROR
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return -1;
    if (err != 0)
        return 0;
    // poll for hangup using select with zero timeout
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {0, 0};
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r > 0)
    {
        // If readable, check if peer closed
        char ch;
        ssize_t n = recv(fd, &ch, 1, MSG_PEEK);
        if (n == 0)
            return 0; // closed
    }
    return 1;
}

void tcp_flush(int fd)
{
    // Nothing to do on POSIX for stream sockets; disabling Nagle might help
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

uint32_t tcp_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
    return (uint32_t)(ms & 0xFFFFFFFFu);
}

void tcp_sleep_ms(unsigned ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}
#endif

int tcp_listen(unsigned short port, int backlog)
{
    ensure_wsa();
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
#if defined(_WIN32)
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        tcp_close(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0)
    {
        tcp_close(fd);
        return -1;
    }
    return fd;
}

int tcp_accept(int listen_fd)
{
    struct sockaddr_in a;
    socklen_t alen = (socklen_t)sizeof(a);
    int c = (int)accept(listen_fd, (struct sockaddr *)&a, &alen);
    return c;
}

int tcp_connect(const char *host, unsigned short port)
{
    ensure_wsa();
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (host && host[0])
    {
        unsigned long ip = inet_addr(host);
        if (ip == INADDR_NONE)
        {
            struct hostent *he = gethostbyname(host);
            if (!he || !he->h_addr_list || !he->h_addr_list[0])
            {
                tcp_close(fd);
                return -1;
            }
            memcpy(&a.sin_addr, he->h_addr_list[0], he->h_length);
        }
        else
        {
            a.sin_addr.s_addr = ip;
        }
    }
    else
    {
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        tcp_close(fd);
        return -1;
    }
    return fd;
}

void tcp_close(int fd)
{
    if (fd >= 0)
        closesock(fd);
}

int tcp_send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t left = len;
    while (left)
    {
#if defined(_WIN32)
        int n = send((SOCKET)fd, p, (int)left, 0);
#else
        ssize_t n = send(fd, p, left, 0);
#endif
        if (n <= 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

int tcp_recv_all(int fd, void *buf, size_t len, unsigned timeout_ms)
{
    char *p = (char *)buf;
    size_t left = len;
    while (left)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int r = select(fd + 1, &rfds, NULL, NULL, timeout_ms ? &tv : NULL);
        if (r <= 0)
            return -1; // timeout or error
#if defined(_WIN32)
        int n = recv((SOCKET)fd, p, (int)left, MSG_WAITALL);
#else
        ssize_t n = recv(fd, p, left, MSG_WAITALL);
#endif
        if (n <= 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

int tcp_recv_up_to(int fd, void *buf, size_t len, size_t *out_got, unsigned timeout_ms)
{
    if (out_got)
        *out_got = 0;
    char *p = (char *)buf;
    size_t got = 0;
    uint32_t start = tcp_now_ms();
    for (;;)
    {
        // Compute remaining time budget
        uint32_t now = tcp_now_ms();
        unsigned remaining = timeout_ms;
        if (timeout_ms)
        {
            unsigned elapsed = (now >= start) ? (now - start) : 0;
            remaining = (elapsed >= timeout_ms) ? 0 : (timeout_ms - elapsed);
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;
        int r = select(fd + 1, &rfds, NULL, NULL, timeout_ms ? &tv : NULL);
        if (r <= 0)
            break; // timeout or error; treat as partial
#if defined(_WIN32)
        int n = recv((SOCKET)fd, p, (int)(len - got), 0);
#else
        ssize_t n = recv(fd, p, (len - got), 0);
#endif
        if (n <= 0)
            break; // error or disconnect; treat as partial
        p += n;
        got += (size_t)n;
        if (got >= len)
            break;
    }
    if (out_got)
        *out_got = got;
    return 0;
}

int tcp_recv_exact(int fd, void *buf, size_t len, unsigned timeout_ms)
{
    if (len == 0)
        return 0;
    uint32_t start = tcp_now_ms();
    for (;;)
    {
        unsigned remaining = timeout_ms;
        if (timeout_ms)
        {
            uint32_t now = tcp_now_ms();
            unsigned elapsed = (now >= start) ? (now - start) : 0;
            if (elapsed >= timeout_ms)
                return -1; // timeout
            remaining = timeout_ms - elapsed;
        }
#if defined(_WIN32)
        u_long avail = 0;
        if (ioctlsocket((SOCKET)fd, FIONREAD, &avail) != 0)
            return -1;
        if (avail < (u_long)len)
        {
            // Wait for readability or timeout
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET((SOCKET)fd, &rfds);
            struct timeval tv;
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            int r = select(0, &rfds, NULL, NULL, timeout_ms ? &tv : NULL);
            if (r <= 0)
                return -1; // timeout or error
            continue;      // re-check avail
        }
        // Enough bytes available; read them atomically with MSG_WAITALL
        int n = recv((SOCKET)fd, (char *)buf, (int)len, MSG_WAITALL);
        return (n == (int)len) ? 0 : -1;
#else
        // POSIX: use FIONREAD when available; otherwise select + MSG_WAITALL
        int avail = 0;
        (void)ioctl(fd, FIONREAD, &avail);
        if (avail < (int)len)
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv;
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
            int r = select(fd + 1, &rfds, NULL, NULL, timeout_ms ? &tv : NULL);
            if (r <= 0)
                return -1;
            continue;
        }
        ssize_t n = recv(fd, (char *)buf, len, MSG_WAITALL);
        return (n == (ssize_t)len) ? 0 : -1;
#endif
    }
}
