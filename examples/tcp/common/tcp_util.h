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

    // Recv up to len bytes, stopping when either len is reached or timeout_ms elapses.
    // Returns 0 on success (including partial due to timeout), -1 on error. On success, *out_got is set
    // to the number of bytes actually read (0..len).
    int tcp_recv_up_to(int fd, void *buf, size_t len, size_t *out_got, unsigned timeout_ms);

    // Recv exactly len bytes within timeout_ms without consuming partial data on timeout.
    // Uses a readiness check (FIONREAD/select) and only performs the actual read when enough
    // bytes are available to satisfy 'len'. Returns 0 on success, -1 on timeout or error, in which
    // case the socket buffer remains untouched by this function.
    int tcp_recv_exact(int fd, void *buf, size_t len, unsigned timeout_ms);

    // Optional helpers used by examples for transport hooks
    // Return 1 if connected, 0 if definitely disconnected, -1 if unknown
    int tcp_is_connected(int fd);
    // Best-effort flush pending send buffer (no-op on some platforms)
    void tcp_flush(int fd);

    // Simple cross-platform timing helpers for examples
    // Monotonic milliseconds since an unspecified start (wraps around). Guaranteed to move forward.
    uint32_t tcp_now_ms(void);
    // Sleep/block for approximately ms milliseconds.
    void tcp_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif // TCP_UTIL_H
