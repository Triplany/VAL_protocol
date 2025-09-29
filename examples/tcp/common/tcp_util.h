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

#ifdef __cplusplus
}
#endif

#endif // TCP_UTIL_H
