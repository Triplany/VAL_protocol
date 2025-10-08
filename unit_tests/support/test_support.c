#include "test_support.h"
#include "../../src/val_internal.h"
#include "../support/transport_profiles.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <errno.h>
#include <windows.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#endif
typedef CRITICAL_SECTION ts_mutex_t;
typedef CONDITION_VARIABLE ts_cond_t;
static void m_init(ts_mutex_t *m)
{
    InitializeCriticalSection(m);
}
static void m_lock(ts_mutex_t *m)
{
    EnterCriticalSection(m);
}
static void m_unlock(ts_mutex_t *m)
{
    LeaveCriticalSection(m);
}
static void c_init(ts_cond_t *c)
{
    InitializeConditionVariable(c);
}
static void c_wait(ts_cond_t *c, ts_mutex_t *m, DWORD to)
{
    SleepConditionVariableCS(c, m, to);
}
static void c_signal_all(ts_cond_t *c)
{
    WakeAllConditionVariable(c);
}
#if defined(_WIN32)
// Ensure Windows never shows modal error/assert dialogs during unit tests. This runs before main().
static void __cdecl ts_win_invalid_parameter_handler(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
                                                     unsigned int line, uintptr_t pReserved)
{
    (void)pReserved;
    fprintf(stderr, "Invalid parameter detected in function %ls. File: %ls Line: %u Expression: %ls\n",
            function ? function : L"?", file ? file : L"?", line, expression ? expression : L"?");
    fflush(stderr);
    // Avoid invoking any UI, just abort
    _exit(3);
}

static LONG WINAPI ts_win_unhandled_exception_filter(EXCEPTION_POINTERS *info)
{
    fprintf(stderr, "Unhandled exception 0x%08lx at address %p\n",
            info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
            info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : NULL);
    fflush(stderr);
    // Return EXECUTE_HANDLER so the process terminates without WER dialog
    return EXCEPTION_EXECUTE_HANDLER;
}

static void __cdecl ts_win_suppress_error_dialogs_init(void)
{
    // Disable Windows Error Reporting popups and critical-error boxes
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(SetThreadErrorMode)
    DWORD prev = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, &prev);
#endif
#ifdef _MSC_VER
    // Route CRT messages to stderr, not dialogs
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(ts_win_invalid_parameter_handler);
    // Debug CRT: send asserts to stderr
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    SetUnhandledExceptionFilter(ts_win_unhandled_exception_filter);
}

// Use MSVC CRT initialization section to run our init routine before main in each test exe
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void(__cdecl *ts_win_p_init)(void) = ts_win_suppress_error_dialogs_init;
#endif // _WIN32
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
typedef pthread_mutex_t ts_mutex_t;
typedef pthread_cond_t ts_cond_t;
static void m_init(ts_mutex_t *m)
{
    pthread_mutex_init(m, NULL);
}
static void m_lock(ts_mutex_t *m)
{
    pthread_mutex_lock(m);
}
static void m_unlock(ts_mutex_t *m)
{
    pthread_mutex_unlock(m);
}
static void c_init(ts_cond_t *c)
{
    pthread_cond_init(c, NULL);
}
static void c_wait(ts_cond_t *c, ts_mutex_t *m, unsigned to)
{
    if (to == 0)
        pthread_cond_wait(c, m);
    else
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += to / 1000;
        ts.tv_nsec += (to % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(c, m, &ts);
    }
}
static void c_signal_all(ts_cond_t *c)
{
    pthread_cond_broadcast(c);
}
#endif

struct test_fifo_s
{
    uint8_t *buf;
    size_t cap;
    size_t head, tail, count;
    ts_mutex_t lock;
    ts_cond_t cv_ne;
    ts_cond_t cv_nf;
};

test_fifo_t *test_fifo_create(size_t capacity)
{
    test_fifo_t *f = (test_fifo_t *)calloc(1, sizeof(*f));
    f->buf = (uint8_t *)malloc(capacity);
    f->cap = capacity;
    f->head = f->tail = f->count = 0;
    m_init(&f->lock);
    c_init(&f->cv_ne);
    c_init(&f->cv_nf);
    return f;
}
void test_fifo_destroy(test_fifo_t *f)
{
    if (!f)
        return;
    free(f->buf);
    free(f);
}

void test_fifo_push(test_fifo_t *f, const uint8_t *data, size_t len)
{
    m_lock(&f->lock);
    while (f->cap - f->count < len)
        c_wait(&f->cv_nf, &f->lock, 0);
    size_t first = ((f->tail + len) <= f->cap) ? len : (f->cap - f->tail);
    memcpy(f->buf + f->tail, data, first);
    if (first < len)
        memcpy(f->buf, data + first, len - first);
    f->tail = (f->tail + len) % f->cap;
    f->count += len;
    c_signal_all(&f->cv_ne);
    m_unlock(&f->lock);
}

int test_fifo_pop_exact(test_fifo_t *f, uint8_t *out, size_t len, uint32_t timeout_ms)
{
    m_lock(&f->lock);
    if (timeout_ms == 0)
    {
        while (f->count < len)
            c_wait(&f->cv_ne, &f->lock, 0);
    }
    else
    {
        uint32_t waited = 0;
        const uint32_t step = 10; // 10ms polling granularity
        while (f->count < len && waited < timeout_ms)
        {
            c_wait(&f->cv_ne, &f->lock, step);
            waited += step;
        }
        if (f->count < len)
        {
            m_unlock(&f->lock);
            return 0; // timeout
        }
    }
    size_t first = ((f->head + len) <= f->cap) ? len : (f->cap - f->head);
    memcpy(out, f->buf + f->head, first);
    if (first < len)
        memcpy(out + first, f->buf, len - first);
    f->head = (f->head + len) % f->cap;
    f->count -= len;
    c_signal_all(&f->cv_nf);
    m_unlock(&f->lock);
    return 1;
}

static uint32_t pcg32(void)
{
    static uint64_t state = 0x853c49e6748fea9bull; // arbitrary seed
    static uint64_t inc = 0xda3e39cb94b95bdbull;   // arbitrary stream
    uint64_t oldstate = state;
    state = oldstate * 6364136223846793005ULL + (inc | 1);
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// Debug tracing for the transport simulator (enabled with TS_NET_SIM_TRACE=1)
static int ts_net_trace_enabled(void)
{
    static int inited = 0;
    static int enabled = 0;
    if (!inited)
    {
#if defined(_WIN32)
        char buf[8];
        DWORD n = GetEnvironmentVariableA("TS_NET_SIM_TRACE", buf, (DWORD)sizeof(buf));
        enabled = (n > 0 && buf[0] == '1') ? 1 : 0;
#else
        const char *v = getenv("TS_NET_SIM_TRACE");
        enabled = (v && v[0] == '1') ? 1 : 0;
#endif
        inited = 1;
    }
    return enabled;
}

static void ts_net_tracef(const char *fmt, ...)
{
    if (!ts_net_trace_enabled())
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

// Optional low-level transport tracing (enabled with TS_TP_TRACE=1)
static int ts_tp_trace_enabled(void)
{
    static int inited = 0;
    static int enabled = 0;
    if (!inited)
    {
#if defined(_WIN32)
        char buf[8];
        DWORD n = GetEnvironmentVariableA("TS_TP_TRACE", buf, (DWORD)sizeof(buf));
        enabled = (n > 0 && buf[0] == '1') ? 1 : 0;
#else
        const char *v = getenv("TS_TP_TRACE");
        enabled = (v && v[0] == '1') ? 1 : 0;
#endif
        inited = 1;
    }
    return enabled;
}
static void ts_tp_tracef(const char *fmt, ...)
{
    if (!ts_tp_trace_enabled())
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

static void ts_dirname(const char *path, char *out, size_t outsz)
{
    if (!path || !*path || !out || outsz == 0)
    {
        if (out && outsz)
            out[0] = '\0';
        return;
    }
    size_t n = strlen(path);
    size_t i = n;
    while (i > 0 && (path[i - 1] == '/' || path[i - 1] == '\\'))
        --i;
    while (i > 0 && !(path[i - 1] == '/' || path[i - 1] == '\\'))
        --i;
    if (i == 0)
    {
        ts_str_copy(out, outsz, ".");
        return;
    }
    size_t copy = (i < outsz - 1) ? i : (outsz - 1);
    memcpy(out, path, copy);
    out[copy] = '\0';
}

static int file_exists(const char *path)
{
    if (!path || !*path)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int ts_find_example_bins(char *rx_out, size_t rx_out_size, char *tx_out, size_t tx_out_size)
{
    if (!rx_out || !tx_out || rx_out_size == 0 || tx_out_size == 0)
        return 0;
    char art[2048];
    if (!ts_get_artifacts_root(art, sizeof(art)))
        return 0;
        char exe_dir[2048];
        ts_dirname(art, exe_dir, sizeof(exe_dir)); // directory containing the test exe (e.g., .../bin/unit_tests)

        // Simple rule: the examples live one directory above the test exe directory.
        // For example, when exe_dir is <build>/<preset>/bin/unit_tests, parent is <build>/<preset>/bin
        char parent_dir[2048];
        ts_dirname(exe_dir, parent_dir, sizeof(parent_dir));

        char rx[4096], tx[4096];
    #if defined(_WIN32)
        snprintf(rx, sizeof(rx), "%s\\val_example_receive_tcp.exe", parent_dir);
        snprintf(tx, sizeof(tx), "%s\\val_example_send_tcp.exe", parent_dir);
    #else
        snprintf(rx, sizeof(rx), "%s/val_example_receive_tcp", parent_dir);
        snprintf(tx, sizeof(tx), "%s/val_example_send_tcp", parent_dir);
    #endif

        // Optional debug: print computed parent_dir and candidate paths and whether they exist
        {
            int dbg = 0;
    #if defined(_WIN32)
            char bufdbg[8];
            DWORD nd = GetEnvironmentVariableA("TS_FIND_BINS_DEBUG", bufdbg, (DWORD)sizeof(bufdbg));
            dbg = (nd > 0 && bufdbg[0] == '1') ? 1 : 0;
    #else
            const char *v = getenv("TS_FIND_BINS_DEBUG");
            dbg = (v && v[0] == '1') ? 1 : 0;
    #endif
            if (dbg)
            {
                fprintf(stderr, "ts_find_example_bins: parent_dir='%s'\n  rx='%s' exists=%d\n  tx='%s' exists=%d\n",
                        parent_dir, rx, file_exists(rx), tx, file_exists(tx));
            }
        }
    if (file_exists(rx) && file_exists(tx))
    {
        ts_str_copy(rx_out, rx_out_size, rx);
        ts_str_copy(tx_out, tx_out_size, tx);
        return 1;
    }

    // Optional debug output controlled by TS_FIND_BINS_DEBUG env var.
    // Set TS_FIND_BINS_DEBUG=1 when running tests to see which candidates are tried.
    {
        int dbg = 0;
#if defined(_WIN32)
        char bufdbg[8];
        DWORD nd = GetEnvironmentVariableA("TS_FIND_BINS_DEBUG", bufdbg, (DWORD)sizeof(bufdbg));
        dbg = (nd > 0 && bufdbg[0] == '1') ? 1 : 0;
#else
        const char *v = getenv("TS_FIND_BINS_DEBUG");
        dbg = (v && v[0] == '1') ? 1 : 0;
#endif
        if (dbg)
        {
            fprintf(stderr, "ts_find_example_bins: tried primary candidates:\n  %s\n  %s\n", rx, tx);
        }
    }

    // Fallbacks for atypical layouts
    char build_root[2048];
    // Derive the build/preset root from the parent_dir we computed above.
    // parent_dir is expected to be <build>/<preset>/bin, so dirname(parent_dir)
    // yields <build>/<preset> which matches the historical "preset_root" intent.
    ts_dirname(parent_dir, build_root, sizeof(build_root)); // <build/preset>
    char dbg_rx[4096], dbg_tx[4096], root_rx[4096], root_tx[4096];
#if defined(_WIN32)
    snprintf(dbg_rx, sizeof(dbg_rx), "%s\\Debug\\val_example_receive_tcp.exe", build_root);
    snprintf(dbg_tx, sizeof(dbg_tx), "%s\\Debug\\val_example_send_tcp.exe", build_root);
    snprintf(root_rx, sizeof(root_rx), "%s\\val_example_receive_tcp.exe", build_root);
    snprintf(root_tx, sizeof(root_tx), "%s\\val_example_send_tcp.exe", build_root);
#else
    snprintf(dbg_rx, sizeof(dbg_rx), "%s/Debug/val_example_receive_tcp", build_root);
    snprintf(dbg_tx, sizeof(dbg_tx), "%s/Debug/val_example_send_tcp", build_root);
    snprintf(root_rx, sizeof(root_rx), "%s/val_example_receive_tcp", build_root);
    snprintf(root_tx, sizeof(root_tx), "%s/val_example_send_tcp", build_root);
#endif
    const char *crx = NULL, *ctx = NULL;
    if (file_exists(dbg_rx) && file_exists(dbg_tx))
    {
        crx = dbg_rx; ctx = dbg_tx;
    }
    else if (file_exists(root_rx) && file_exists(root_tx))
    {
        crx = root_rx; ctx = root_tx;
    }
    if (crx && ctx)
    {
        ts_str_copy(rx_out, rx_out_size, crx);
        ts_str_copy(tx_out, tx_out_size, ctx);
        return 1;
    }

    {
        int dbg = 0;
#if defined(_WIN32)
        char bufdbg[8];
        DWORD nd = GetEnvironmentVariableA("TS_FIND_BINS_DEBUG", bufdbg, (DWORD)sizeof(bufdbg));
        dbg = (nd > 0 && bufdbg[0] == '1') ? 1 : 0;
#else
        const char *v = getenv("TS_FIND_BINS_DEBUG");
        dbg = (v && v[0] == '1') ? 1 : 0;
#endif
        if (dbg)
        {
            fprintf(stderr, "ts_find_example_bins: tried fallback candidates:\n  %s\n  %s\n  %s\n  %s\n",
                    dbg_rx, dbg_tx, root_rx, root_tx);
        }
    }
    return 0;
}

void ts_str_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void ts_str_append(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src)
        return;
    size_t cur = strnlen(dst, dst_size);
    if (cur >= dst_size)
    {
        dst[dst_size - 1] = '\0';
        return;
    }
    size_t rem = dst_size - 1 - cur;
    size_t n = strnlen(src, rem);
    memcpy(dst + cur, src, n);
    dst[cur + n] = '\0';
}

void ts_rand_seed_set(uint64_t seed)
{
    // Advance the internal generator a seed-dependent number of steps to vary sequences deterministically.
    uint64_t steps = (seed % 10000ull);
    for (uint64_t i = 0; i < steps; ++i)
        (void)pcg32();
}

void maybe_corrupt(uint8_t *data, size_t len, const fault_injection_t *f)
{
    if (!f)
        return;
    if (f->bitflip_per_million)
    {
        for (size_t i = 0; i < len; ++i)
        {
            if (pcg32() % 1000000u < f->bitflip_per_million)
            {
                uint8_t bit = (uint8_t)(1u << (pcg32() % 8));
                data[i] ^= bit;
            }
        }
    }
}

void test_duplex_init(test_duplex_t *d, size_t max_packet, size_t depth_packets)
{
    d->a2b = test_fifo_create(max_packet * depth_packets);
    d->b2a = test_fifo_create(max_packet * depth_packets);
    d->max_packet = max_packet;
    d->faults.bitflip_per_million = 0;
    d->faults.drop_frame_per_million = 0;
    d->faults.dup_frame_per_million = 0;
}
void test_duplex_free(test_duplex_t *d)
{
    test_fifo_destroy(d->a2b);
    test_fifo_destroy(d->b2a);
}

// ---- Network simulation implementation (disabled by default) ----
static ts_net_sim_t g_net = {0};
typedef struct
{
    uint8_t *data;
    size_t len;
} ts_frame_t;
#define TS_REORDER_MAX 64
typedef struct
{
    test_duplex_t *key; // transport context for which this queue applies (direction-specific)
    ts_frame_t q[TS_REORDER_MAX];
    size_t n;
    size_t bytes_sent;
} ts_reorder_entry_t;
static ts_reorder_entry_t g_reorder_tbl[32];
static ts_mutex_t g_reorder_lock;
static int g_reorder_lock_inited = 0;
static void reorder_lock_init_once(void)
{
    if (!g_reorder_lock_inited)
    {
        m_init(&g_reorder_lock);
        g_reorder_lock_inited = 1;
    }
}
static ts_reorder_entry_t *reorder_entry_for(test_duplex_t *d)
{
    reorder_lock_init_once();
    m_lock(&g_reorder_lock);
    ts_reorder_entry_t *slot = NULL;
    for (size_t i = 0; i < sizeof(g_reorder_tbl) / sizeof(g_reorder_tbl[0]); ++i)
    {
        if (g_reorder_tbl[i].key == d)
        {
            slot = &g_reorder_tbl[i];
            break;
        }
        if (!slot && g_reorder_tbl[i].key == NULL)
            slot = &g_reorder_tbl[i];
    }
    if (slot && slot->key == NULL)
    {
        slot->key = d;
        slot->n = 0;
        slot->bytes_sent = 0;
    }
    m_unlock(&g_reorder_lock);
    return slot;
}

void ts_net_sim_set(const ts_net_sim_t *cfg)
{
    g_net = cfg ? *cfg : (ts_net_sim_t){0};
}
void ts_net_sim_reset(void)
{
    memset(&g_net, 0, sizeof(g_net));
    reorder_lock_init_once();
    m_lock(&g_reorder_lock);
    for (size_t i = 0; i < sizeof(g_reorder_tbl) / sizeof(g_reorder_tbl[0]); ++i)
    {
        for (size_t j = 0; j < g_reorder_tbl[i].n; ++j)
            free(g_reorder_tbl[i].q[j].data);
        g_reorder_tbl[i].key = NULL;
        g_reorder_tbl[i].n = 0;
    }
    m_unlock(&g_reorder_lock);
}

static size_t rnd_between(size_t lo, size_t hi)
{
    if (lo == 0)
        lo = 1;
    if (hi < lo)
        hi = lo;
    uint32_t r = pcg32();
    size_t span = hi - lo + 1;
    return lo + (r % span);
}

static void maybe_delay(void)
{
    if (!g_net.enable_jitter)
        return;
    uint32_t base = (uint32_t)rnd_between(g_net.jitter_min_ms, g_net.jitter_max_ms);
    uint32_t d = base;
    if (g_net.spike_per_million && (pcg32() % 1000000u) < g_net.spike_per_million)
        d += g_net.spike_ms;
    if (d)
        ts_delay(d);
}

static void reorder_enqueue(test_duplex_t *d, const uint8_t *data, size_t len)
{
    ts_reorder_entry_t *e = reorder_entry_for(d);
    if (!e)
        return;
    m_lock(&g_reorder_lock);
    if (e->n >= TS_REORDER_MAX)
    {
        free(e->q[0].data);
        memmove(&e->q[0], &e->q[1], sizeof(e->q[0]) * (TS_REORDER_MAX - 1));
        e->n = TS_REORDER_MAX - 1;
    }
    e->q[e->n].data = (uint8_t *)malloc(len);
    e->q[e->n].len = len;
    memcpy(e->q[e->n].data, data, len);
    e->n++;
    m_unlock(&g_reorder_lock);
}

static int reorder_dequeue_some(test_duplex_t *d)
{
    ts_reorder_entry_t *e = reorder_entry_for(d);
    if (!e)
        return 0;
    m_lock(&g_reorder_lock);
    if (e->n == 0)
    {
        m_unlock(&g_reorder_lock);
        return 0;
    }
    size_t n = 1 + (pcg32() % e->n);
    for (size_t i = 0; i < n; ++i)
    {
        size_t idx = e->n - 1; // LIFO emit for out-of-order
        test_fifo_push(d->a2b, e->q[idx].data, e->q[idx].len);
        free(e->q[idx].data);
        e->n--;
    }
    m_unlock(&g_reorder_lock);
    return (int)n;
}

static void net_sim_send(const void *data, size_t len, test_duplex_t *d)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    ts_reorder_entry_t *e = reorder_entry_for(d);
    while (left > 0)
    {
        size_t chunk = left;
        if (g_net.enable_partial_send)
        {
            // Bound both lo and hi by the remaining bytes to avoid overruns
            size_t lo = g_net.min_send_chunk ? g_net.min_send_chunk : 1;
            if (lo > left)
                lo = left;
            size_t hi = g_net.max_send_chunk ? g_net.max_send_chunk : left;
            if (hi > left)
                hi = left;
            if (hi < lo)
                hi = lo;
            chunk = rnd_between(lo, hi);
        }
        maybe_delay();
        int drop = (d->faults.drop_frame_per_million && (pcg32() % 1000000u < d->faults.drop_frame_per_million));
        int dup = (!drop && d->faults.dup_frame_per_million && (pcg32() % 1000000u < d->faults.dup_frame_per_million));
        if (!drop)
        {
            // Avoid reordering/fragmenting the very first bytes to prevent HELLO/control desync.
            size_t sent = 0;
            if (e)
                sent = e->bytes_sent;
            size_t grace = g_net.handshake_grace_bytes;
            int allow_reorder = g_net.enable_reorder && (!grace || sent >= grace);
            if (allow_reorder && (pcg32() % 1000000u < g_net.reorder_per_million))
            {
                ts_net_tracef("SEND d=%p chunk=%zu REORDER enqueue (q later)", (void *)d, chunk);
                reorder_enqueue(d, p, chunk);
                (void)reorder_dequeue_some(d);
            }
            else
            {
                uint8_t *tmp = (uint8_t *)malloc(chunk);
                memcpy(tmp, p, chunk);
                maybe_corrupt(tmp, chunk, &d->faults);
                test_fifo_push(d->a2b, tmp, chunk);
                free(tmp);
                ts_net_tracef("SEND d=%p chunk=%zu DIRECT", (void *)d, chunk);
            }
        }
        if (dup)
        {
            uint8_t *tmp = (uint8_t *)malloc(chunk);
            memcpy(tmp, p, chunk);
            maybe_corrupt(tmp, chunk, &d->faults);
            test_fifo_push(d->a2b, tmp, chunk);
            free(tmp);
            ts_net_tracef("SEND d=%p DUP chunk=%zu", (void *)d, chunk);
        }
        if (e)
        {
            m_lock(&g_reorder_lock);
            e->bytes_sent += chunk;
            m_unlock(&g_reorder_lock);
        }
        p += chunk;
        left -= chunk;
    }
    // Flush any remaining reordered frames to ensure nothing is stuck after sender completes
    while (reorder_dequeue_some(d) > 0)
        ;
}

static int net_sim_recv(void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms, test_duplex_t *d)
{
    if (buffer_size == 0)
    {
        if (received)
            *received = 0;
        return 0;
    }
    size_t total = 0;
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t waited = 0;
    const uint32_t step = 10; // poll interval for timeout
    while (total < buffer_size)
    {
        size_t remaining = buffer_size - total;
        size_t want = remaining;
        if (g_net.enable_partial_recv)
        {
            size_t lo = g_net.min_recv_chunk ? g_net.min_recv_chunk : 1;
            if (lo > remaining)
                lo = remaining;
            size_t hi = g_net.max_recv_chunk && g_net.max_recv_chunk <= remaining ? g_net.max_recv_chunk : remaining;
            if (hi < lo)
                hi = lo;
            want = rnd_between(lo, hi);
        }
        // For stream semantics, do not reorder bytes here; just allow jitter.
        maybe_delay();
        int ok = test_fifo_pop_exact(d->b2a, dst + total, want, step);
        if (!ok)
        {
            waited += step;
            if (timeout_ms && waited >= timeout_ms)
            {
                if (received)
                    *received = 0; // indicate no data read
                ts_net_tracef("RECV d=%p TIMEOUT after %u ms (need=%zu got=%zu)", (void *)d, waited, buffer_size, total);
                // Return 0 with received=0 to signal timeout (recoverable) to core
                return 0;
            }
            continue;
        }
        total += want;
        ts_net_tracef("RECV d=%p got=%zu/%zu", (void *)d, total, buffer_size);
    }
    if (received)
        *received = buffer_size;
    ts_net_tracef("RECV d=%p DONE size=%zu", (void *)d, buffer_size);
    return 0;
}

int test_tp_send(void *ctx, const void *data, size_t len)
{
    test_duplex_t *d = (test_duplex_t *)ctx;

    // Update transport simulator stats if active
    if (transport_sim_get_profile() != NULL) {
        transport_sim_record_packet_sent(len);
    }

    // Use simulation when any knob is enabled; otherwise use the direct path
    if (g_net.enable_partial_send || g_net.enable_reorder || g_net.enable_jitter)
    {
        net_sim_send(data, len, d);
        return (int)len;
    }
    // Direct path
    uint8_t *tmp = (uint8_t *)malloc(len);
    memcpy(tmp, data, len);
    maybe_corrupt(tmp, len, &d->faults);
    int drop = (d->faults.drop_frame_per_million && (pcg32() % 1000000u < d->faults.drop_frame_per_million));
    int dup = (!drop && d->faults.dup_frame_per_million && (pcg32() % 1000000u < d->faults.dup_frame_per_million));
    if (!drop)
        test_fifo_push(d->a2b, tmp, len);
    if (dup)
        test_fifo_push(d->a2b, tmp, len);
    free(tmp);
    return (int)len;
}

int test_tp_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms)
{
    test_duplex_t *d = (test_duplex_t *)ctx;
    if (g_net.enable_partial_recv || g_net.enable_reorder || g_net.enable_jitter) {
        int result = net_sim_recv(buffer, buffer_size, received, timeout_ms, d);

        // Update transport simulator stats if active - successful receive
        if (transport_sim_get_profile() != NULL && result == 0 && received && *received > 0) {
            transport_sim_record_packet_received(*received);
        }

        return result;
    }

    int ok = test_fifo_pop_exact(d->b2a, (uint8_t *)buffer, buffer_size, timeout_ms);
    if (!ok)
    {
        if (received)
            *received = 0;
        // Return 0 to indicate timeout; core will treat got!=expected as VAL_ERR_TIMEOUT
    ts_tp_tracef("DIRECT RECV d=%p TIMEOUT need=%zu", (void *)d, buffer_size);
        return 0;
    }
    if (received)
        *received = buffer_size;

    // Update transport simulator stats if active - successful receive
    if (transport_sim_get_profile() != NULL && received && *received > 0) {
        transport_sim_record_packet_received(*received);
    }

    ts_tp_tracef("DIRECT RECV d=%p DONE size=%zu", (void *)d, buffer_size);
    return 0;
}

// ---- Filesystem fault injection (Windows path only; no-op elsewhere) ----
static ts_fs_faults_t g_fs = {0};
void ts_fs_faults_set(const ts_fs_faults_t *f)
{
    g_fs = f ? *f : (ts_fs_faults_t){0};
}
void ts_fs_faults_reset(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
}

// Platform-native file I/O wrappers (avoid FILE dependency for analyzers)
typedef struct ts_file_s
{
#if defined(_WIN32)
    FILE *fp;
#else
    int fd;
#endif
} ts_file_t;

void *ts_fopen(void *ctx, const char *path, const char *mode)
{
    (void)ctx;
#if defined(_WIN32)
    (void)path;
    (void)mode;
    // Use CRT FILE* on Windows for simpler semantics and to avoid rare ReadFile() 0-byte anomalies under AV/scanners
    const char *m = "rb";
    if (mode && mode[0] == 'r')
        m = "rb";
    else if (mode && mode[0] == 'a')
        m = "ab";
    else
        m = "wb";
    FILE *fp = fopen(path, m);
    if (!fp)
        return NULL;
    ts_file_t *f = (ts_file_t *)calloc(1, sizeof(*f));
    f->fp = fp;
    return f;
#else
    int flags = 0;
    if (mode && mode[0] == 'r')
    {
        flags = O_RDONLY;
    }
    else if (mode && mode[0] == 'a')
    {
        // Append: do not truncate, writes go to end
        flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    else
    {
        // Default write: truncate/create new
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }
    int fd = open(path, flags, 0666);
    if (fd < 0)
        return NULL;
    ts_file_t *f = (ts_file_t *)calloc(1, sizeof(*f));
    f->fd = fd;
    return f;
#endif
}
size_t ts_fread(void *ctx, void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
    if (!buffer || !f)
        return 0;
    if (size == 0 || count == 0)
        return 0;
    size_t total = size * count;
    uint8_t *dst = (uint8_t *)buffer;
    size_t read_total = 0;
#if defined(_WIN32)
    // Use fread for robustness with CRT FILE*
    while (read_total < total)
    {
        size_t want = total - read_total;
        size_t got = fread(dst + read_total, 1, want, f->fp);
        if (got == 0)
            break; // EOF or error
        read_total += got;
    }
#else
    while (read_total < total)
    {
        size_t want = total - read_total;
        ssize_t got = read(f->fd, dst + read_total, want);
        if (got <= 0)
            break; // EOF or error
        read_total += (size_t)got;
    }
#endif
    return (size ? (read_total / size) : 0);
}
size_t ts_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
    size_t bytes = size * count;
#if defined(_WIN32)
    // Simulate failures/short writes if configured
    size_t allowed = bytes;
    if (g_fs.mode != TS_FS_FAIL_NONE)
    {
        int64_t pos;
#if defined(_WIN32)
        pos = _ftelli64(f->fp);
#else
        pos = ftello64(f->fp);
#endif
        if (pos < 0)
            pos = 0;
        uint64_t sofar = (uint64_t)pos;
        if (g_fs.deny_write_prefix)
        {
            // Without path here we approximate by denying any write when deny_write_prefix is set
            return 0;
        }
        if (g_fs.fail_after_total_bytes)
        {
            if (sofar >= g_fs.fail_after_total_bytes)
                allowed = 0;
            else if (sofar + allowed > g_fs.fail_after_total_bytes)
                allowed = (size_t)(g_fs.fail_after_total_bytes - sofar);
        }
    }
    size_t put = fwrite(buffer, 1, allowed, f->fp);
    if (put == 0)
        return 0;
    return (size ? (put / size) : 0);
#else
    ssize_t put = write(f->fd, buffer, bytes);
    if (put <= 0)
        return 0;
    return (size ? ((size_t)put / size) : 0);
#endif
}
int ts_fseek(void *ctx, void *file, int64_t offset, int whence)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    return _fseeki64(f->fp, offset, whence);
#else
    return lseek64(f->fd, offset, whence) < 0 ? -1 : 0;
#endif
}
int64_t ts_ftell(void *ctx, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    return _ftelli64(f->fp);
#else
    return lseek64(f->fd, 0, SEEK_CUR);
#endif
}
int ts_fclose(void *ctx, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    int ok = fclose(f->fp);
    free(f);
    return ok;
#else
    int ok = close(f->fd);
    free(f);
    return ok;
#endif
}

// Cross-platform monotonic millisecond clock and delay for tests
// See test_support.h for policy notes. These are used as defaults by
// ts_make_config() when the caller doesn't provide their own hooks.
uint32_t ts_ticks(void)
{
#if defined(_WIN32)
    // GetTickCount64 is monotonic since boot; convert to 32-bit (wrap is acceptable for tests)
    ULONGLONG t = GetTickCount64();
    return (uint32_t)(t & 0xFFFFFFFFu);
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
        uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
        return (uint32_t)(ms & 0xFFFFFFFFu);
    }
#endif
    // Fallback: coarse time()
    time_t now = time(NULL);
    return (uint32_t)((uint64_t)now * 1000ull);
#endif
}
void ts_delay(uint32_t ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000L);
    nanosleep(&req, NULL);
#endif
}

static void ts_path_dirname(const char *path, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    size_t n = path ? strlen(path) : 0;
    size_t i = n;
    while (i > 0)
    {
        char c = path[i - 1];
        if (c == '/' || c == '\\')
            break;
        --i;
    }
    if (i == 0)
    {
        snprintf(out, outsz, ".");
        return;
    }
    size_t copy = (i < outsz - 1) ? i : (outsz - 1);
    memcpy(out, path, copy);
    out[copy] = '\0';
}

int ts_get_artifacts_root(char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;
#if defined(_WIN32)
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if (len == 0 || len >= sizeof(exe))
        return 0;
    char dir[MAX_PATH];
    ts_path_dirname(exe, dir, sizeof(dir));
    // Compose <exe_dir>\\ut_artifacts
    if (snprintf(out, out_size, "%s\\ut_artifacts", dir) <= 0)
        return 0;
#else
    char exe[1024];
    ssize_t r = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (r <= 0)
        return 0;
    exe[r] = '\0';
    char dir[1024];
    ts_path_dirname(exe, dir, sizeof(dir));
    if (snprintf(out, out_size, "%s/ut_artifacts", dir) <= 0)
        return 0;
#endif
    return 1;
}

static int ts_mkdir_one(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int ts_ensure_dir(const char *path)
{
    if (!path || !*path)
        return -1;
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    // Normalize separators to platform default
#if defined(_WIN32)
    for (size_t i = 0; i < len; ++i)
        if (tmp[i] == '/')
            tmp[i] = '\\';
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    // Iterate components, creating progressively
    for (size_t i = 1; i <= len; ++i)
    {
        if (tmp[i] == '\0' || tmp[i] == sep)
        {
            char c = tmp[i];
            tmp[i] = '\0';
            if (strlen(tmp) > 0 && ts_mkdir_one(tmp) != 0)
                return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

void ts_make_config(val_config_t *cfg, void *send_buf, void *recv_buf, size_t packet_size, test_duplex_t *end_as_ctx,
                    val_resume_mode_t mode, uint32_t verify)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->transport.send = test_tp_send;
    cfg->transport.recv = test_tp_recv;
    cfg->transport.io_context = end_as_ctx;
    cfg->filesystem.fopen = ts_fopen;
    cfg->filesystem.fread = ts_fread;
    cfg->filesystem.fwrite = ts_fwrite;
    cfg->filesystem.fseek = ts_fseek;
    cfg->filesystem.ftell = ts_ftell;
    cfg->filesystem.fclose = ts_fclose;
    // Install default real system hooks for tests. Tests may override after this call.
    if (!cfg->system.get_ticks_ms)
        cfg->system.get_ticks_ms = ts_ticks;
    if (!cfg->system.delay_ms)
        cfg->system.delay_ms = ts_delay;
    cfg->buffers.send_buffer = send_buf;
    cfg->buffers.recv_buffer = recv_buf;
    cfg->buffers.packet_size = packet_size;
    cfg->resume.mode = mode;
    // Map the test verify parameter to tail-only config
    cfg->resume.tail_cap_bytes = verify;      // 0 = use default
    cfg->resume.min_verify_bytes = 0;         // tests can override
    // Default mismatch policy matches TAIL_OR_ZERO semantics: restart on mismatch
    cfg->resume.mismatch_skip = 0;            // tests can override to 1 to force skip on mismatch
    // Adaptive timeout bounds (tests run in-memory; keep low to speed up failures while allowing retries)
    cfg->timeouts.min_timeout_ms = 50;   // floor for RTO
    cfg->timeouts.max_timeout_ms = 2000; // ceiling for RTO
    cfg->retries.handshake_retries = 3;
    cfg->retries.meta_retries = 2;
    cfg->retries.data_retries = 4;
    cfg->retries.ack_retries = 6;
    cfg->retries.backoff_ms_base = 10; // was 50

    // Install a default console logger for unit tests so library diagnostics are visible
    // when VAL_LOG_LEVEL permits. Tests can override the level via ts_set_console_logger* helpers.
    ts_set_console_logger(cfg);
}

static void ts_console_log(void *ctx, int level, const char *file, int line, const char *message)
{
    (void)ctx;
    const char *lvl = "";
    switch (level)
    {
    case 1:
        lvl = "CRITICAL";
        break;
    case 2:
        lvl = "WARNING";
        break;
    case 3:
        lvl = "INFO";
        break;
    case 4:
        lvl = "DEBUG";
        break;
    case 5:
        lvl = "TRACE";
        break;
    default:
        lvl = "LOG";
        break;
    }
    fprintf(stdout, "[%s] %s:%d: %s\n", lvl, file, line, message ? message : "");
    fflush(stdout);
}

void ts_set_console_logger(val_config_t *cfg)
{
    if (!cfg)
        return;
    cfg->debug.log = ts_console_log;
    cfg->debug.context = NULL;
    // Ensure we see DEBUG by default in tests (subject to compile-time gating)
    if (cfg->debug.min_level == 0)
        cfg->debug.min_level = VAL_LOG_DEBUG;
}

void ts_set_console_logger_with_level(val_config_t *cfg, int min_level)
{
    if (!cfg)
        return;
    cfg->debug.log = ts_console_log;
    cfg->debug.context = NULL;
    cfg->debug.min_level = min_level;
}

// Receiver thread wrapper
typedef struct
{
    val_session_t *rx;
    char outdir[512];
} rx_args_t;

#if defined(_WIN32)
static DWORD WINAPI rx_thread_fn(LPVOID p)
{
    rx_args_t *a = (rx_args_t *)p;
    (void)val_receive_files(a->rx, a->outdir);
    free(a);
    return 0;
}
typedef HANDLE ts_handle_t;
#else
static void *rx_thread_fn(void *p)
{
    rx_args_t *a = (rx_args_t *)p;
    (void)val_receive_files(a->rx, a->outdir);
    free(a);
    return NULL;
}
typedef pthread_t ts_handle_t;
#endif

ts_thread_t ts_start_receiver(val_session_t *rx, const char *outdir)
{
    rx_args_t *a = (rx_args_t *)calloc(1, sizeof(*a));
    a->rx = rx;
    snprintf(a->outdir, sizeof(a->outdir), "%s", outdir ? outdir : "");
#if defined(_WIN32)
    return (ts_thread_t)CreateThread(NULL, 0, rx_thread_fn, a, 0, NULL);
#else
    pthread_t *ph = (pthread_t *)malloc(sizeof(pthread_t));
    pthread_create(ph, NULL, rx_thread_fn, a);
    return (ts_thread_t)ph;
#endif
}

void ts_join_thread(ts_thread_t th)
{
#if defined(_WIN32)
    WaitForSingleObject((HANDLE)th, INFINITE);
    CloseHandle((HANDLE)th);
#else
    pthread_t *ph = (pthread_t *)th;
    pthread_join(*ph, NULL);
    free(ph);
#endif
}

// ---- Timeout guard (watchdog) ----
typedef struct
{
    uint32_t to_ms;
    volatile int cancel;
    char name[64];
} ts_guard_t;
#if defined(_WIN32)
static DWORD WINAPI ts_guard_fn(LPVOID p)
{
    ts_guard_t *g = (ts_guard_t *)p;
    uint32_t waited = 0;
    while (!g->cancel && waited < g->to_ms)
    {
        Sleep(50);
        waited += 50;
    }
    if (!g->cancel)
    {
        fprintf(stderr, "[WATCHDOG] Test '%s' exceeded %u ms; terminating.\n", g->name, g->to_ms);
        fflush(stderr);
        ExitProcess(3);
    }
    free(g);
    return 0;
}
#else
static void *ts_guard_fn(void *p)
{
    ts_guard_t *g = (ts_guard_t *)p;
    uint32_t waited = 0;
    while (!g->cancel && waited < g->to_ms)
    {
        struct timespec req = {0};
        req.tv_sec = 0;
        req.tv_nsec = 50 * 1000000L;
        nanosleep(&req, NULL);
        waited += 50;
    }
    if (!g->cancel)
    {
        fprintf(stderr, "[WATCHDOG] Test '%s' exceeded %u ms; terminating.\n", g->name, g->to_ms);
        fflush(stderr);
        _exit(3);
    }
    free(g);
    return NULL;
}
#endif

ts_cancel_token_t ts_start_timeout_guard(uint32_t timeout_ms, const char *name)
{
    ts_guard_t *g = (ts_guard_t *)calloc(1, sizeof(*g));
    g->to_ms = timeout_ms;
    g->cancel = 0;
    snprintf(g->name, sizeof(g->name), "%s", name ? name : "test");
#if defined(_WIN32)
    (void)CreateThread(NULL, 0, ts_guard_fn, g, 0, NULL);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, ts_guard_fn, g);
    pthread_detach(tid);
#endif
    return (ts_cancel_token_t)g;
}
void ts_cancel_timeout_guard(ts_cancel_token_t token)
{
    if (!token)
        return;
    ts_guard_t *g = (ts_guard_t *)token;
    g->cancel = 1;
}

uint64_t ts_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }
    int64_t pos;
#if defined(_WIN32)
    pos = _ftelli64(f);
#else
    pos = ftello64(f);
#endif
    fclose(f);
    return pos < 0 ? 0 : (uint64_t)pos;
}

uint32_t ts_file_crc32(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t buf[4096];
    size_t r;
    uint32_t state = val_crc32_init_state();
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        state = val_crc32_update_state(state, buf, r);
    }
    fclose(f);
    return val_crc32_finalize_state(state);
}

size_t ts_env_size_bytes(const char *env_name, size_t default_value)
{
    if (!env_name || !*env_name)
        return default_value;
    char buf[64];
#if defined(_WIN32)
    DWORD n = GetEnvironmentVariableA(env_name, buf, (DWORD)sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return default_value;
#else
    const char *v = getenv(env_name);
    if (!v || !*v)
        return default_value;
    snprintf(buf, sizeof(buf), "%s", v);
#endif
    char *end = NULL;
    unsigned long long base = strtoull(buf, &end, 10);
    if (base == 0ULL)
        return default_value;
    // Skip spaces
    while (end && *end == ' ')
        ++end;
    unsigned long long mul = 1ULL;
    if (end && *end)
    {
        char c = *end;
        if (c == 'k' || c == 'K')
            mul = 1024ULL;
        else if (c == 'm' || c == 'M')
            mul = 1024ULL * 1024ULL;
        else if (c == 'g' || c == 'G')
            mul = 1024ULL * 1024ULL * 1024ULL;
        else
            mul = 1ULL; // unknown suffix, treat as bytes
    }
    unsigned long long res = base * mul;
    if (res == 0ULL)
        return default_value;
    // Clamp to size_t
    if (res > (unsigned long long)~(size_t)0)
        res = (unsigned long long)~(size_t)0;
    return (size_t)res;
}

// Extra cross-platform helpers for tests
int ts_remove_file(const char *path)
{
    if (!path || !*path)
        return 0;
#if defined(_WIN32)
    DeleteFileA(path);
    return 0;
#else
    (void)remove(path);
    return 0;
#endif
}

int ts_write_pattern_file(const char *path, size_t size_bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    for (size_t i = 0; i < size_bytes; ++i)
    {
        unsigned char b = (unsigned char)(i & 0xFF);
        if (fputc(b, f) == EOF)
        {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

int ts_files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb)
    {
        if (fa)
            fclose(fa);
        if (fb)
            fclose(fb);
        return 0;
    }
    int eq = 1;
    const size_t K = 4096;
    unsigned char *ba = (unsigned char *)malloc(K);
    unsigned char *bb = (unsigned char *)malloc(K);
    if (!ba || !bb)
    {
        free(ba);
        free(bb);
        fclose(fa);
        fclose(fb);
        return 0;
    }
    for (;;)
    {
        size_t ra = fread(ba, 1, K, fa);
        size_t rb = fread(bb, 1, K, fb);
        if (ra != rb || memcmp(ba, bb, ra) != 0)
        {
            eq = 0;
            break;
        }
        if (ra == 0) // both EOF
            break;
    }
    free(ba);
    free(bb);
    fclose(fa);
    fclose(fb);
    return eq;
}

int ts_path_join(char *dst, size_t dst_size, const char *a, const char *b)
{
    if (!dst || dst_size == 0 || !a || !b)
        return -1;
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != sep) ? 1 : 0;
    if (lb > 0 && b[0] == sep)
        need_sep = 0;
    size_t need = la + (size_t)need_sep + lb + 1;
    if (need > dst_size)
        return -1;
    memcpy(dst, a, la);
    size_t pos = la;
    if (need_sep)
        dst[pos++] = sep;
    memcpy(dst + pos, b, lb);
    dst[pos + lb] = '\0';
    return 0;
}

char *ts_dyn_sprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        va_end(ap2);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf)
    {
        va_end(ap2);
        return NULL;
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

char *ts_join_path_dyn(const char *a, const char *b)
{
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_sep = (la > 0 && a[la - 1] != sep) ? 1 : 0;
    if (lb > 0 && b[0] == sep)
        need_sep = 0;
    size_t len = la + (size_t)need_sep + lb;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, a, la);
    size_t pos = la;
    if (need_sep)
        out[pos++] = sep;
    memcpy(out + pos, b, lb);
    out[len] = '\0';
    return out;
}

int ts_build_case_dirs(const char *case_name, char *basedir, size_t basedir_sz, char *outdir, size_t outdir_sz)
{
    char root[512];
    if (!ts_get_artifacts_root(root, sizeof(root)))
        return -1;
    char tmp[1024];
    if (ts_path_join(tmp, sizeof(tmp), root, case_name) != 0)
        return -1;
    if (ts_ensure_dir(tmp) != 0)
        return -1;
    if (ts_path_join(basedir, basedir_sz, root, case_name) != 0)
        return -1;
    if (ts_path_join(outdir, outdir_sz, basedir, "out") != 0)
        return -1;
    if (ts_ensure_dir(outdir) != 0)
        return -1;
    return 0;
}

void ts_receiver_warmup(const val_config_t *cfg, uint32_t ms)
{
    if (cfg && cfg->system.delay_ms)
        cfg->system.delay_ms(ms);
}

// ---- Fake clock support ----
static struct
{
    int installed;
    ts_fake_clock_t clk;
} g_fake = {0};
uint32_t ts_fake_get_ticks_ms(void)
{
    if (!g_fake.installed)
        return ts_ticks();
    uint32_t now = g_fake.clk.now_ms;
    if (g_fake.clk.enable_wrap)
    {
        uint32_t mod = g_fake.clk.wrap_after ? g_fake.clk.wrap_after : 0u;
        if (mod)
            now = now % mod;
    }
    return now;
}
void ts_fake_clock_install(const ts_fake_clock_t *init)
{
    g_fake.installed = 1;
    g_fake.clk = init ? *init : (ts_fake_clock_t){0};
}
void ts_fake_clock_uninstall(void)
{
    g_fake.installed = 0;
    memset(&g_fake.clk, 0, sizeof(g_fake.clk));
}
void ts_fake_clock_advance(uint32_t delta_ms)
{
    if (!g_fake.installed)
        return;
    g_fake.clk.now_ms += delta_ms;
}
void ts_fake_clock_set(uint32_t now_ms)
{
    if (!g_fake.installed)
        return;
    g_fake.clk.now_ms = now_ms;
}
void ts_fake_delay_ms(uint32_t ms)
{
    if (g_fake.installed)
        ts_fake_clock_advance(ms);
    else
        ts_delay(ms);
}
