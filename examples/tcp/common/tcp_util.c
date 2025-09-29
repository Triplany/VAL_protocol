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
static int last_err(void)
{
    return WSAGetLastError();
}
static void set_nonblock(int fd, int nb)
{
    u_long m = nb ? 1 : 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &m);
}
static int closesock(int fd)
{
    return closesocket((SOCKET)fd);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
static void ensure_wsa(void)
{
}
static int last_err(void)
{
    return errno;
}
static void set_nonblock(int fd, int nb)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return;
    if (nb)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}
static int closesock(int fd)
{
    return close(fd);
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
