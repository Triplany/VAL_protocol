#ifndef TCP_UTIL_H
#define TCP_UTIL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

    // Cross-platform minimal TCP helpers for examples
    // Returns socket fd/handle on success (>=0), or -1 on error.
    int tcp_listen(unsigned short port, int backlog);
    int tcp_accept(int listen_fd);
    int tcp_connect(const char *host, unsigned short port);
    void tcp_close(int fd);

    // Send exactly len bytes; returns 0 on success, -1 on error
    int tcp_send_all(int fd, const void *buf, size_t len);
    // Recv exactly len bytes, unless timeout_ms elapses; returns 0 on success, -1 on error
    int tcp_recv_all(int fd, void *buf, size_t len, unsigned timeout_ms);

    // Optional helpers used by examples for transport hooks
    // Return 1 if connected, 0 if definitely disconnected, -1 if unknown
    int tcp_is_connected(int fd);
    // Best-effort flush pending send buffer (no-op on some platforms)
    void tcp_flush(int fd);

#ifdef __cplusplus
}
#endif

#endif // TCP_UTIL_H
