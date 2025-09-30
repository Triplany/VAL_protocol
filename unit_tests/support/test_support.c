#include "test_support.h"
#include "../../src/val_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <errno.h>
#include <windows.h>
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

int test_tp_send(void *ctx, const void *data, size_t len)
{
    test_duplex_t *d = (test_duplex_t *)ctx;
    // Potentially drop or duplicate frame; apply corruption to a copy to avoid mutating caller buffer
    uint8_t *tmp = (uint8_t *)malloc(len);
    memcpy(tmp, data, len);
    maybe_corrupt(tmp, len, &d->faults);

    // Drop?
    int drop = (d->faults.drop_frame_per_million && (pcg32() % 1000000u < d->faults.drop_frame_per_million));
    int dup = (!drop && d->faults.dup_frame_per_million && (pcg32() % 1000000u < d->faults.dup_frame_per_million));

    if (!drop)
    {
        test_fifo_push(d->a2b, tmp, len);
    }
    if (dup)
    {
        test_fifo_push(d->a2b, tmp, len);
    }
    free(tmp);
    return (int)len; // behave like a write that accepted len bytes regardless
}

int test_tp_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms)
{
    test_duplex_t *d = (test_duplex_t *)ctx;
    int ok = test_fifo_pop_exact(d->b2a, (uint8_t *)buffer, buffer_size, timeout_ms);
    if (!ok)
    {
        if (received)
            *received = 0; // signal timeout/no data
        return 0;          // timeout is not a hard error; let protocol map it to VAL_ERR_TIMEOUT
    }
    if (received)
        *received = buffer_size;
    return 0;
}

// Platform-native file I/O wrappers (avoid FILE dependency for analyzers)
typedef struct ts_file_s
{
#if defined(_WIN32)
    HANDLE h;
#else
    int fd;
#endif
} ts_file_t;

void *ts_fopen(void *ctx, const char *path, const char *mode)
{
    (void)ctx;
#if defined(_WIN32)
    DWORD access = 0, creation = 0;
    if (mode && mode[0] == 'r')
    {
        access = GENERIC_READ;
        creation = OPEN_EXISTING;
    }
    else
    {
        access = GENERIC_WRITE;
        creation = CREATE_ALWAYS;
    }
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    ts_file_t *f = (ts_file_t *)calloc(1, sizeof(*f));
    f->h = h;
    return f;
#else
    int flags = 0;
    if (mode && mode[0] == 'r')
    {
        flags = O_RDONLY;
    }
    else
    {
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
int ts_fread(void *ctx, void *buffer, size_t size, size_t count, void *file)
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
    while (read_total < total)
    {
        DWORD got = 0;
        size_t want = total - read_total;
        DWORD chunk = (want > 0x7FFFFFFFu) ? 0x7FFFFFFFu : (DWORD)want; // clamp to signed 31-bit to be safe
        if (!ReadFile(f->h, dst + read_total, chunk, &got, NULL))
            break;
        if (got == 0)
            break; // EOF
        read_total += (size_t)got;
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
    return (int)(read_total / (size ? size : 1));
}
int ts_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
    size_t bytes = size * count;
#if defined(_WIN32)
    DWORD put = 0;
    if (!WriteFile(f->h, buffer, (DWORD)bytes, &put, NULL))
        return 0;
    return (int)(put / (size ? size : 1));
#else
    ssize_t put = write(f->fd, buffer, bytes);
    if (put <= 0)
        return 0;
    return (int)(put / (size ? size : 1));
#endif
}
int ts_fseek(void *ctx, void *file, long offset, int whence)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    LARGE_INTEGER li;
    li.QuadPart = offset;
    DWORD movemethod = (whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END);
    return SetFilePointerEx(f->h, li, NULL, movemethod) ? 0 : -1;
#else
    return lseek(f->fd, offset, whence) < 0 ? -1 : 0;
#endif
}
long ts_ftell(void *ctx, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    LARGE_INTEGER zero = {0};
    LARGE_INTEGER pos;
    if (!SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT))
        return -1;
    return (long)pos.QuadPart;
#else
    off_t p = lseek(f->fd, 0, SEEK_CUR);
    return (long)p;
#endif
}
int ts_fclose(void *ctx, void *file)
{
    (void)ctx;
    ts_file_t *f = (ts_file_t *)file;
#if defined(_WIN32)
    int ok = CloseHandle(f->h) ? 0 : -1;
    free(f);
    return ok;
#else
    int ok = close(f->fd);
    free(f);
    return ok;
#endif
}

uint32_t ts_ticks(void)
{
    return 0;
}
void ts_delay(uint32_t ms)
{
    (void)ms;
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
    cfg->system.get_ticks_ms = ts_ticks;
    cfg->system.delay_ms = ts_delay;
    cfg->buffers.send_buffer = send_buf;
    cfg->buffers.recv_buffer = recv_buf;
    cfg->buffers.packet_size = packet_size;
    cfg->resume.mode = mode;
    cfg->resume.verify_bytes = verify;
    // Test-optimized timeouts and bounded retries: in-memory transport is fast and predictable,
    // so we can use much lower values to keep total test time low while still exercising timeouts/retries.
    cfg->timeouts.handshake_ms = 500; // was 2000
    cfg->timeouts.meta_ms = 1500;     // was 5000
    cfg->timeouts.data_ms = 2000;     // was 5000
    cfg->timeouts.ack_ms = 2000;      // was 2000
    cfg->timeouts.idle_ms = 200;      // was 1000
    cfg->retries.handshake_retries = 3;
    cfg->retries.meta_retries = 2;
    cfg->retries.data_retries = 4;
    cfg->retries.ack_retries = 6;
    cfg->retries.backoff_ms_base = 10; // was 50
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
    long pos = ftell(f);
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
