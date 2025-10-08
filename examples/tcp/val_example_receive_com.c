#if !defined(_WIN32)
#define _GNU_SOURCE  // for posix_openpt, ptsname, etc.
#endif

#include "val_protocol.h"
#include "val_error_strings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <pthread.h>
#endif

// --- Simple timing functions ---
static uint32_t get_ticks_ms(void) {
#if defined(_WIN32)
	return (uint32_t)GetTickCount();
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		// Fallback to wall clock time if monotonic not available
		clock_gettime(CLOCK_REALTIME, &ts);
	}
	return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
#endif
}

static void delay_ms(uint32_t ms) {
#if defined(_WIN32)
	Sleep(ms);
#else
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000L
	};
	nanosleep(&ts, NULL);
#endif
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

static void usage(const char *prog) {
	fprintf(stderr,
	"Usage: %s [--mtu N] [--resume MODE] [--tail-bytes N] [--streaming on|off] [--accept-streaming on|off] [--tx-mode MODE] [--max-mode MODE] [--serial-verbose] [--host] <port|outdir> [baud] <outdir>\n"
	"  --mtu N            Packet size/MTU (default 4096)\n"
	"  --resume MODE      Resume mode: never, skip, tail\n"
	"  --tail-bytes N     Tail verification cap bytes for tail mode (default 1024)\n"
	"  --streaming V      Allow sender streaming pacing on|off (default on)\n"
	"  --accept-streaming Accept incoming peer streaming (default on)\n"
	"  --tx-mode MODE     Preferred initial TX window cap we announce: stop|1|2|4|8|16|32|64 (default 16)\n"
	"  --max-mode MODE    Max TX window cap we support: stop|1|2|4|8|16|32|64 (default 64)\n"
	"  --log-level L      Runtime log verbosity: off, crit, warn, info, debug, trace, or 0..5 (default info)\n"
	"  --log-file PATH    Append logs to this file instead of stderr\n"
	"  --verbose          Shorthand for --log-level debug\n"
	"  --serial-verbose   Enable verbose serial read/write byte logging (default off)\n"
	"  --host             Create a virtual serial port (Linux only; prints slave path)\n",
		prog);
}

// ---------------- End-of-run summary (receiver) ----------------
typedef struct {
	char **received;
	size_t received_count;
	char **skipped;
	size_t skipped_count;
	struct {
		char *name;
		uint32_t start_ms;
		uint32_t elapsed_ms;
		uint64_t bytes;
		int result;
	} *stats;
	size_t stats_count;
	size_t capacity;
} rx_summary_t;

static rx_summary_t *g_rx_summary = NULL;

static void rx_summary_add(char ***arr, size_t *count, size_t cap, const char *name)
{
	if (!g_rx_summary || !name) return;
	if (*count >= cap) return;
	(*arr)[*count] = strdup(name);
	(*count)++;
}

static size_t rx_stats_start(rx_summary_t *sum, const char *filename, uint64_t bytes)
{
	if (!sum || !filename) return 0;
	size_t idx = sum->stats_count++;
	sum->stats = (typeof(sum->stats))realloc(sum->stats, sizeof(*sum->stats) * sum->stats_count);
	sum->stats[idx].name = strdup(filename);
	sum->stats[idx].start_ms = get_ticks_ms();
	sum->stats[idx].elapsed_ms = 0;
	sum->stats[idx].bytes = bytes;
	sum->stats[idx].result = -1;
	return idx;
}

static void rx_stats_complete(rx_summary_t *sum, const char *filename, int result)
{
	if (!sum || !filename) return;
	for (size_t i = 0; i < sum->stats_count; ++i) {
		if (sum->stats[i].name && strcmp(sum->stats[i].name, filename) == 0) {
			uint32_t now = get_ticks_ms();
			uint32_t start = sum->stats[i].start_ms;
			sum->stats[i].elapsed_ms = (now >= start) ? (now - start) : 0u;
			sum->stats[i].result = result;
			return;
		}
	}
}

static void rx_summary_print_and_free(const rx_summary_t *sum, val_status_t final_status)
{
	if (!sum) return;
	fprintf(stdout, "\n==== File Transfer Details (Receiver) ====\n");
	if (sum->stats_count == 0) {
		fprintf(stdout, "(no files)\n");
	} else {
		for (size_t i = 0; i < sum->stats_count; ++i) {
			const char *name = sum->stats[i].name ? sum->stats[i].name : "<unknown>";
			uint32_t ms = sum->stats[i].elapsed_ms;
			double secs = ms / 1000.0;
			if (sum->stats[i].result == VAL_OK && ms > 0 && sum->stats[i].bytes > 0) {
				double mibps = secs > 0.0 ? ((double)sum->stats[i].bytes / (1024.0*1024.0)) / secs : 0.0;
				fprintf(stdout, "  - %s: time=%.3fs, rate=%.2f MB/s\n", name, secs, mibps);
			} else {
				fprintf(stdout, "  - %s: time=%.3fs, rate=N/A\n", name, secs);
			}
		}
	}
	fprintf(stdout, "\n==== Transfer Summary (Receiver) ====\n");
	fprintf(stdout, "Files Received (%zu):\n", (size_t)sum->received_count);
	for (size_t i = 0; i < sum->received_count; ++i) fprintf(stdout, "  - %s\n", sum->received[i]);
	fprintf(stdout, "Files Skipped (%zu):\n", (size_t)sum->skipped_count);
	for (size_t i = 0; i < sum->skipped_count; ++i) fprintf(stdout, "  - %s\n", sum->skipped[i]);
	const char *status = (final_status == VAL_OK) ? "Success" : (final_status == VAL_ERR_ABORTED ? "Aborted" : "Failed");
	fprintf(stdout, "Final Status: %s\n", status);
	fflush(stdout);
	for (size_t i = 0; i < sum->received_count; ++i) free(sum->received[i]);
	for (size_t i = 0; i < sum->skipped_count; ++i) free(sum->skipped[i]);
	free(sum->received); free(sum->skipped);
	for (size_t i = 0; i < sum->stats_count; ++i) free(sum->stats[i].name);
	free(sum->stats);
}


static int parse_uint(const char *s, unsigned *out) {
	if (!s || !*s) return -1;
	unsigned v = 0;
	for (const char *p = s; *p; ++p) {
		if (*p < '0' || *p > '9') return -1;
		unsigned d = (unsigned)(*p - '0');
		unsigned nv = v * 10u + d;
		if (nv < v) return -1;
		v = nv;
	}
	*out = v;
	return 0;
}

static int strieq(const char *a, const char *b) {
	if (!a || !b) return 0;
	while (*a && *b) {
		if ((unsigned char)tolower(*a) != (unsigned char)tolower(*b)) return 0;
		++a; ++b;
	}
	return *a == 0 && *b == 0;
}

static const char *tx_mode_name(val_tx_mode_t m)
{
	switch (m) {
		case VAL_TX_STOP_AND_WAIT: return "stop(1:1)";
		case VAL_TX_WINDOW_2: return "2";
		case VAL_TX_WINDOW_4: return "4";
		case VAL_TX_WINDOW_8: return "8";
		case VAL_TX_WINDOW_16: return "16";
		case VAL_TX_WINDOW_32: return "32";
		case VAL_TX_WINDOW_64: return "64";
		default: return "?";
	}
}

static int parse_level(const char *s)
{
	if (!s) return 0;
	if (isdigit((unsigned char)s[0])) { int v = atoi(s); if (v < 0) v = 0; if (v > 5) v = 5; return v; }
	if (strieq(s, "off")) return 0;
	if (strieq(s, "crit") || strieq(s, "critical")) return 1;
	if (strieq(s, "warn") || strieq(s, "warning")) return 2;
	if (strieq(s, "info")) return 3;
	if (strieq(s, "debug")) return 4;
	if (strieq(s, "trace") || strieq(s, "verbose")) return 5;
	return 0;
}

// Progress announcer (receiver side)
static val_session_t *g_rx_session_for_progress = NULL;
static val_tx_mode_t g_rx_last_mode_reported = (val_tx_mode_t)255; // invalid
static int g_rx_post_handshake_printed = 0;
static val_tx_mode_t g_peer_last_mode_reported = (val_tx_mode_t)255;
static int g_peer_stream_last = -1;
static void on_progress_announce_mode_rx(const val_progress_info_t *info)
{
	(void)info;
	if (!g_rx_session_for_progress) return;
	if (!g_rx_post_handshake_printed) {
		size_t mtu = 0; bool send_stream_ok=false, recv_stream_ok=false; val_tx_mode_t m0 = VAL_TX_STOP_AND_WAIT; val_tx_mode_t pm0 = VAL_TX_STOP_AND_WAIT;
		if (val_get_effective_packet_size(g_rx_session_for_progress, &mtu) == VAL_OK) {
			(void)val_get_streaming_allowed(g_rx_session_for_progress, &send_stream_ok, &recv_stream_ok);
			(void)val_get_current_tx_mode(g_rx_session_for_progress, &m0);
			(void)val_get_peer_tx_mode(g_rx_session_for_progress, &pm0);
			bool pstream=false; (void)val_is_peer_streaming_engaged(g_rx_session_for_progress, &pstream);
			fprintf(stdout, "[VAL][RX] post-handshake: mtu=%zu, send_streaming=%s, accept_streaming=%s, local_tx_init=%s, peer_tx_init=%s, peer_streaming=%s\n",
					mtu, send_stream_ok?"yes":"no", recv_stream_ok?"yes":"no", tx_mode_name(m0), tx_mode_name(pm0), pstream?"+streaming":"off");
			fflush(stdout);
			g_rx_post_handshake_printed = 1;
		}
	}
	val_tx_mode_t m = VAL_TX_STOP_AND_WAIT;
	if (val_get_current_tx_mode(g_rx_session_for_progress, &m) == VAL_OK) {
		if (g_rx_last_mode_reported != m) { g_rx_last_mode_reported = m; fprintf(stdout, "[VAL][RX] local-tx mode changed -> %s\n", tx_mode_name(m)); fflush(stdout); }
	}
	val_tx_mode_t pm = VAL_TX_STOP_AND_WAIT;
	if (val_get_peer_tx_mode(g_rx_session_for_progress, &pm) == VAL_OK) {
		if (g_peer_last_mode_reported != pm) { g_peer_last_mode_reported = pm; fprintf(stdout, "[VAL][RX] peer-tx mode changed -> %s\n", tx_mode_name(pm)); fflush(stdout); }
	}
	bool ps=false; if (val_is_peer_streaming_engaged(g_rx_session_for_progress, &ps) == VAL_OK) { if (g_peer_stream_last != ps) { g_peer_stream_last = ps; fprintf(stdout, "[VAL][RX] peer streaming %s\n", ps?"ENGAGED":"OFF"); fflush(stdout); } }
}

// Callbacks for per-file summary (match val_config signatures)
static void on_file_start_rx(const char *filename, const char *sender_path, uint64_t file_size, uint64_t resume_offset)
{
	(void)sender_path;
	if (!g_rx_summary || !filename) return;
	uint64_t bytes = (file_size > resume_offset) ? (file_size - resume_offset) : 0ull;
	(void)rx_stats_start(g_rx_summary, filename, bytes);
}

static void on_file_complete_rx(const char *filename, const char *sender_path, val_status_t result)
{
	(void)sender_path;
	if (!g_rx_summary || !filename) return;
	if (result == VAL_OK) rx_summary_add(&g_rx_summary->received, &g_rx_summary->received_count, g_rx_summary->capacity, filename);
	else if (result == VAL_SKIPPED) rx_summary_add(&g_rx_summary->skipped, &g_rx_summary->skipped_count, g_rx_summary->capacity, filename);
	rx_stats_complete(g_rx_summary, filename, (int)result);
}


// --- Serial port abstraction ---
typedef struct {
#if defined(_WIN32)
	HANDLE h;
#else
	int fd;
#endif
} serial_port_t;

// Serial verbose flag (off by default; enabled by --serial-verbose)
static int g_serial_verbose = 0;

static int serial_open(serial_port_t *sp, const char *port, unsigned baud, int host_mode) {
#if defined(_WIN32)
	char fullport[64];
	snprintf(fullport, sizeof(fullport), "\\\\.\\%s", port);
	sp->h = CreateFileA(fullport, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (sp->h == INVALID_HANDLE_VALUE) return -1;
	DCB dcb = {0};
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(sp->h, &dcb)) return -2;
	dcb.BaudRate = baud;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	if (!SetCommState(sp->h, &dcb)) return -3;
	COMMTIMEOUTS timeouts = {0};
	timeouts.ReadIntervalTimeout = 50;
	timeouts.ReadTotalTimeoutConstant = 50;
	timeouts.ReadTotalTimeoutMultiplier = 10;
	timeouts.WriteTotalTimeoutConstant = 50;
	timeouts.WriteTotalTimeoutMultiplier = 10;
	SetCommTimeouts(sp->h, &timeouts);
	return 0;
#else
	if (host_mode) {
		// Create PTY master
		sp->fd = posix_openpt(O_RDWR | O_NOCTTY);
		if (sp->fd == -1) return -1;
		if (grantpt(sp->fd) == -1) { close(sp->fd); return -2; }
		if (unlockpt(sp->fd) == -1) { close(sp->fd); return -3; }
		char *slave_name = ptsname(sp->fd);
		if (!slave_name) {
			fprintf(stderr, "ptsname() returned NULL\n");
			close(sp->fd);
			return -4;
		}
		printf("Virtual serial port created. Connect sender to: %s\n", slave_name);
		fflush(stdout);
	/* Configure termios on the PTY slave to ensure raw mode and
	 * no line processing. The termios attributes belong to the
	 * slave device; setting them on the slave is reliable. */
	int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
	if (slave_fd < 0) { close(sp->fd); return -5; }
	struct termios tty;
	if (tcgetattr(slave_fd, &tty) != 0) { close(slave_fd); close(sp->fd); return -6; }
	/* Set speed and raw mode */
	cfsetospeed(&tty, B115200);
	cfsetispeed(&tty, B115200);
	cfmakeraw(&tty);
	/* Conservative read responsiveness for PTY: VMIN=0, VTIME=5 (0.5s) */
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 5;
	if (tcsetattr(slave_fd, TCSANOW, &tty) != 0) { close(slave_fd); close(sp->fd); return -7; }
	close(slave_fd);
		return 0;
	} else {
		sp->fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
		if (sp->fd < 0) return -1;
		struct termios tty;
		if (tcgetattr(sp->fd, &tty) != 0) return -2;
		cfsetospeed(&tty, baud);
		cfsetispeed(&tty, baud);
		/* Enforce raw mode to prevent any line processing */
		cfmakeraw(&tty);
		/* Conservative read responsiveness */
		tty.c_cc[VMIN] = 0;
		tty.c_cc[VTIME] = 5; /* 0.5s */
		/* Basic 8N1, no flow control */
		tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
		tty.c_cflag |= (CLOCAL | CREAD);
		tty.c_cflag &= ~(PARENB | PARODD);
		tty.c_cflag &= ~CSTOPB;
		tty.c_cflag &= ~CRTSCTS;
		if (tcsetattr(sp->fd, TCSANOW, &tty) != 0) return -3;
		return 0;
	}
#endif
}

static void serial_close(serial_port_t *sp) {
#if defined(_WIN32)
	if (sp->h && sp->h != INVALID_HANDLE_VALUE) CloseHandle(sp->h);
#else
	if (sp->fd >= 0) close(sp->fd);
#endif
}

static int serial_write(serial_port_t *sp, const void *data, size_t len) {
#if defined(_WIN32)
	DWORD written = 0;
	if (!WriteFile(sp->h, data, (DWORD)len, &written, NULL)) {
		fprintf(stderr, "[SERIAL][ERR] WriteFile failed\n");
		return -1;
	}
	fprintf(stderr, "[SERIAL][WRITE] %d bytes\n", (int)written);
	return (int)written;
#else
	/* Robust write loop to handle partial writes and EINTR/EAGAIN */
	const uint8_t *p = (const uint8_t *)data;
	size_t remaining = len;
	while (remaining > 0) {
		ssize_t rc = write(sp->fd, p, remaining);
		if (rc > 0) {
			p += rc;
			remaining -= (size_t)rc;
			continue;
		}
		if (rc < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				usleep(1000);
				continue;
			}
			/* Rate-limited write error */
			static uint32_t last_werr = 0;
			uint32_t now = get_ticks_ms();
			if (now - last_werr > 5000) {
				fprintf(stderr, "[SERIAL][ERR] write: %s\n", strerror(errno));
				last_werr = now;
			}
			return -1;
		}
		/* rc == 0, unexpected; small sleep to avoid tight loop */
		usleep(1000);
	}
	size_t written = len - remaining;
	if (g_serial_verbose) {
		fprintf(stderr, "[SERIAL][WRITE] %zu bytes\n", written);
	}
	/* Optional raw hex debug */
	if (g_serial_verbose && getenv("VAL_COM_DEBUG_RAW")) {
		size_t dump = written < 64 ? written : 64;
		fprintf(stderr, "[SERIAL][WRITE][HEX] ");
		for (size_t i = 0; i < dump; ++i) fprintf(stderr, "%02X ", ((const uint8_t *)data)[i]);
		fprintf(stderr, "\n");
	}
	return (int)written;
#endif
}

static int serial_read(serial_port_t *sp, void *buf, size_t len, uint32_t timeout_ms) {
#if defined(_WIN32)
	DWORD read = 0;
	if (!ReadFile(sp->h, buf, (DWORD)len, &read, NULL)) {
		fprintf(stderr, "[SERIAL][ERR] ReadFile failed\n");
		return -1;
	}
	fprintf(stderr, "[SERIAL][READ] %d bytes\n", (int)read);
	return (int)read;
#else
	uint8_t *p = (uint8_t *)buf;
	size_t total = 0;
	uint32_t start = get_ticks_ms();
	for (;;) {
		/* Compute remaining time */
		uint32_t now = get_ticks_ms();
		uint32_t elapsed = (now >= start) ? (now - start) : 0u;
		if (elapsed >= timeout_ms) break;
		uint32_t remain_ms = timeout_ms - elapsed;

		fd_set readfds;
		struct timeval tv;
		FD_ZERO(&readfds);
		FD_SET(sp->fd, &readfds);
		tv.tv_sec = remain_ms / 1000u;
		tv.tv_usec = (remain_ms % 1000u) * 1000u;
		int sel_ret = select(sp->fd + 1, &readfds, NULL, NULL, &tv);
		if (sel_ret <= 0) {
			break; // timeout or select error -> treat as timeout (return what we have)
		}
		ssize_t rc = read(sp->fd, p + total, len - total);
		if (rc > 0) {
			total += (size_t)rc;
			if (total >= len) break; // satisfied request size
			continue;
		}
		if (rc < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* brief backoff */
				usleep(1000);
				continue;
			}
			static uint32_t last_err_print = 0;
			uint32_t nowe = get_ticks_ms();
			if (nowe - last_err_print > 5000) {
				fprintf(stderr, "[SERIAL][ERR] read: %s\n", strerror(errno));
				last_err_print = nowe;
			}
			break; // fatal -> return what we have
		}
		// rc == 0 -> no data, loop until timeout
	}
	if (total == 0) {
		static uint32_t last_toerr = 0;
		uint32_t now2 = get_ticks_ms();
		if (now2 - last_toerr > 5000) {
			if (g_serial_verbose) fprintf(stderr, "[SERIAL][READ] timeout or error\n");
			last_toerr = now2;
		}
		return 0;
	}
	if (g_serial_verbose) fprintf(stderr, "[SERIAL][READ] %zu bytes\n", total);
	if (g_serial_verbose && getenv("VAL_COM_DEBUG_RAW")) {
		size_t dump = total < 64 ? total : 64;
		fprintf(stderr, "[SERIAL][READ][HEX] ");
		for (size_t i = 0; i < dump; ++i) fprintf(stderr, "%02X ", p[i]);
		fprintf(stderr, "\n");
	}
	return (int)total;
#endif
}

// --- VAL transport wrappers ---
static int tp_send(void *ctx, const void *data, size_t len) {
	serial_port_t *sp = (serial_port_t *)ctx;
	int rc = serial_write(sp, data, len);
	return rc == (int)len ? (int)len : -1;
}
static int tp_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms) {
	serial_port_t *sp = (serial_port_t *)ctx;
	int rc = serial_read(sp, buffer, buffer_size, timeout_ms);
	if (rc < 0) {
		if (received) *received = 0;
		return 0;
	}
	if (received) *received = (size_t)rc;
	return 0;
}
static int tp_is_connected(void *ctx) {
	(void)ctx;
	return 1; // always true for serial
}
static void tp_flush(void *ctx) {
	(void)ctx;
}

// ---- No-progress watchdog (POSIX only) ----
#if VAL_ENABLE_METRICS && !defined(_WIN32)
static volatile int g_watchdog_running_rx = 0;
static volatile int g_watchdog_stop_rx = 0;
static val_session_t *g_watchdog_session_rx = NULL;
static void *watchdog_thread_rx(void *arg)
{
	(void)arg;
	val_metrics_t last = {0};
	uint32_t last_change_ms = get_ticks_ms();
	int first = 1;
	while (!g_watchdog_stop_rx) {
		if (!g_watchdog_session_rx) { usleep(1000*100); continue; }
		val_metrics_t cur;
		if (val_get_metrics(g_watchdog_session_rx, &cur) == VAL_OK) {
			if (first || cur.packets_sent != last.packets_sent || cur.packets_recv != last.packets_recv ||
				cur.bytes_sent != last.bytes_sent || cur.bytes_recv != last.bytes_recv) {
				last = cur;
				last_change_ms = get_ticks_ms();
				first = 0;
			} else {
				uint32_t now = get_ticks_ms();
				if (now - last_change_ms > 20000u) { // 20s without progress -> cancel session
					fprintf(stderr, "[VAL][RX][watchdog] No progress for >20s; cancelling session to avoid hang...\n");
					val_emergency_cancel(g_watchdog_session_rx);
					break;
				}
			}
		}
		usleep(1000*500); // 500ms
	}
	g_watchdog_running_rx = 0;
	return NULL;
}
#endif

// --- Minimal filesystem adapters using stdio (same as TCP) ---
static void *fs_fopen(void *ctx, const char *path, const char *mode) {
	(void)ctx; return (void *)fopen(path, mode);
}
static size_t fs_fread(void *ctx, void *buffer, size_t size, size_t count, void *file) {
	(void)ctx; return fread(buffer, size, count, (FILE *)file);
}
static size_t fs_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file) {
	(void)ctx; return fwrite(buffer, size, count, (FILE *)file);
}
static int fs_fseek(void *ctx, void *file, int64_t offset, int whence) {
	(void)ctx;
#ifdef _WIN32
	return _fseeki64((FILE *)file, offset, whence);
#else
	return fseeko((FILE *)file, (off_t)offset, whence);
#endif
}
static int64_t fs_ftell(void *ctx, void *file) {
	(void)ctx;
#ifdef _WIN32
	return _ftelli64((FILE *)file);
#else
	return (int64_t)ftello((FILE *)file);
#endif
}
static int fs_fclose(void *ctx, void *file) {
	(void)ctx; return fclose((FILE *)file);
}

#if VAL_ENABLE_METRICS
static void print_metrics_rx(val_session_t *rx)
{
	if (!rx) return;
	val_metrics_t m;
	if (val_get_metrics(rx, &m) != VAL_OK) return;
	fprintf(stdout, "\n==== Session Metrics (Receiver) ====\n");
	fprintf(stdout, "Packets: sent=%llu recv=%llu\n", (unsigned long long)m.packets_sent, (unsigned long long)m.packets_recv);
	fprintf(stdout, "Bytes:   sent=%llu recv=%llu\n", (unsigned long long)m.bytes_sent, (unsigned long long)m.bytes_recv);
	fprintf(stdout, "Session: handshakes=%u files_recv=%u rtt_samples=%u\n", (unsigned)m.handshakes, (unsigned)m.files_recv, (unsigned)m.rtt_samples);
	const char *names[32] = {0};
	names[VAL_PKT_HELLO] = "HELLO";
	names[VAL_PKT_SEND_META] = "SEND_META";
	names[VAL_PKT_RESUME_REQ] = "RESUME_REQ";
	names[VAL_PKT_RESUME_RESP] = "RESUME_RESP";
	names[VAL_PKT_DATA] = "DATA";
	names[VAL_PKT_DATA_ACK] = "DATA_ACK";
	names[VAL_PKT_VERIFY] = "VERIFY";
	names[VAL_PKT_DONE] = "DONE";
	names[VAL_PKT_ERROR] = "ERROR";
	names[VAL_PKT_EOT] = "EOT";
	names[VAL_PKT_EOT_ACK] = "EOT_ACK";
	names[VAL_PKT_DONE_ACK] = "DONE_ACK";
	names[VAL_PKT_MODE_SYNC] = "MODE_SYNC";
	names[VAL_PKT_MODE_SYNC_ACK] = "MODE_SYNC_ACK";
	names[VAL_PKT_CANCEL & 31u] = "CANCEL";
	fprintf(stdout, "Per-type (send/recv):\n");
	for (unsigned i = 0; i < 32; ++i) {
		if (m.send_by_type[i] || m.recv_by_type[i]) {
			const char *nm = names[i] ? names[i] : "T";
			fprintf(stdout, "  - %-12s s=%llu r=%llu (idx %u)\n", nm, (unsigned long long)m.send_by_type[i], (unsigned long long)m.recv_by_type[i], i);
		}
	}
	fflush(stdout);
}
#endif

int main(int argc, char **argv) {
	size_t packet = 4096;
	val_resume_mode_t resume_mode = VAL_RESUME_TAIL;
	unsigned tail_bytes = 1024;
	int host_mode = 0;
	int opt_streaming = 1; // default on
	int opt_accept_streaming = 1;
	int accept_streaming_explicit = 0;
	val_tx_mode_t opt_tx_mode = VAL_TX_WINDOW_16;
	val_tx_mode_t opt_max_mode = VAL_TX_WINDOW_64;
	int log_level = -1; const char *log_file_path = NULL;
	int argi = 1;
	while (argi < argc && strncmp(argv[argi], "-", 1) == 0) {
		const char *arg = argv[argi++];
		if (strcmp(arg, "--mtu") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			unsigned v = 0;
			if (parse_uint(argv[argi++], &v) != 0) { fprintf(stderr, "Invalid --mtu value\n"); return 1; }
			packet = v;
		} else if (strcmp(arg, "--resume") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			const char *mode = argv[argi++];
			if (strcmp(mode, "never") == 0) resume_mode = VAL_RESUME_NEVER;
			else if (strcmp(mode, "skip") == 0) resume_mode = VAL_RESUME_SKIP_EXISTING;
			else if (strcmp(mode, "tail") == 0) resume_mode = VAL_RESUME_TAIL;
			else { fprintf(stderr, "Invalid --resume value\n"); return 1; }
		} else if (strcmp(arg, "--tail-bytes") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			unsigned v = 0;
			if (parse_uint(argv[argi++], &v) != 0) { fprintf(stderr, "Invalid --tail-bytes value\n"); return 1; }
			tail_bytes = v;
		} else if (strcmp(arg, "--streaming") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			const char *v = argv[argi++];
			if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0) opt_streaming = 1;
			else if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcmp(v, "false") == 0) opt_streaming = 0;
			else { fprintf(stderr, "Invalid --streaming value; use on|off\n"); return 1; }
		} else if (strcmp(arg, "--accept-streaming") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			const char *v = argv[argi++];
			if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0) opt_accept_streaming = 1;
			else if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcmp(v, "false") == 0) opt_accept_streaming = 0;
			else { fprintf(stderr, "Invalid --accept-streaming value; use on|off\n"); return 1; }
			accept_streaming_explicit = 1;
		} else if (strcmp(arg, "--tx-mode") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			const char *m = argv[argi++];
			if (strcmp(m, "stop") == 0 || strcmp(m, "1") == 0 || strcmp(m, "1:1") == 0) opt_tx_mode = VAL_TX_STOP_AND_WAIT;
			else if (strcmp(m, "2") == 0) opt_tx_mode = VAL_TX_WINDOW_2;
			else if (strcmp(m, "4") == 0) opt_tx_mode = VAL_TX_WINDOW_4;
			else if (strcmp(m, "8") == 0) opt_tx_mode = VAL_TX_WINDOW_8;
			else if (strcmp(m, "16") == 0) opt_tx_mode = VAL_TX_WINDOW_16;
			else if (strcmp(m, "32") == 0) opt_tx_mode = VAL_TX_WINDOW_32;
			else if (strcmp(m, "64") == 0) opt_tx_mode = VAL_TX_WINDOW_64;
			else { fprintf(stderr, "Invalid --tx-mode; use stop|1|2|4|8|16|32|64\n"); return 1; }
		} else if (strcmp(arg, "--max-mode") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			const char *m = argv[argi++];
			if (strcmp(m, "stop") == 0 || strcmp(m, "1") == 0 || strcmp(m, "1:1") == 0) opt_max_mode = VAL_TX_STOP_AND_WAIT;
			else if (strcmp(m, "2") == 0) opt_max_mode = VAL_TX_WINDOW_2;
			else if (strcmp(m, "4") == 0) opt_max_mode = VAL_TX_WINDOW_4;
			else if (strcmp(m, "8") == 0) opt_max_mode = VAL_TX_WINDOW_8;
			else if (strcmp(m, "16") == 0) opt_max_mode = VAL_TX_WINDOW_16;
			else if (strcmp(m, "32") == 0) opt_max_mode = VAL_TX_WINDOW_32;
			else if (strcmp(m, "64") == 0) opt_max_mode = VAL_TX_WINDOW_64;
			else { fprintf(stderr, "Invalid --max-mode; use stop|1|2|4|8|16|32|64\n"); return 1; }
		} else if (strcmp(arg, "--log-level") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			log_level = parse_level(argv[argi++]);
		} else if (strcmp(arg, "--log-file") == 0) {
			if (argi >= argc) { usage(argv[0]); return 1; }
			log_file_path = argv[argi++];
		} else if (strcmp(arg, "--verbose") == 0) {
			log_level = 4;
		} else if (strcmp(arg, "--serial-verbose") == 0) {
			g_serial_verbose = 1;
		} else if (strcmp(arg, "--host") == 0) {
			host_mode = 1;
		} else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			usage(argv[0]); return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", arg); usage(argv[0]); return 1;
		}
	}
	int required_args = host_mode ? 1 : 3;
	if (argc - argi < required_args) { usage(argv[0]); return 1; }
	const char *port_or_outdir = argv[argi++];
	const char *outdir;
	unsigned baud = 0;
	if (host_mode) {
		outdir = port_or_outdir;
	} else {
		if (parse_uint(argv[argi++], &baud) != 0) { fprintf(stderr, "Invalid baud rate\n"); return 1; }
		outdir = argv[argi++];
	}
	serial_port_t sp = {0};
	if (serial_open(&sp, host_mode ? NULL : port_or_outdir, baud, host_mode) != 0) {
		if (host_mode) {
			fprintf(stderr, "Failed to create virtual serial port\n");
		} else {
			fprintf(stderr, "Failed to open serial port %s\n", port_or_outdir);
		}
		return 2;
	}

	uint8_t *send_buf = (uint8_t *)malloc(packet);
	uint8_t *recv_buf = (uint8_t *)malloc(packet);
	if (!send_buf || !recv_buf) { fprintf(stderr, "oom\n"); serial_close(&sp); return 3; }

	val_config_t cfg = {0};
	cfg.transport.send = tp_send;
	cfg.transport.recv = tp_recv;
	cfg.transport.is_connected = tp_is_connected;
	cfg.transport.flush = tp_flush;
	cfg.transport.io_context = &sp;
	cfg.filesystem.fopen = fs_fopen;
	cfg.filesystem.fread = fs_fread;
	cfg.filesystem.fwrite = fs_fwrite;
	cfg.filesystem.fseek = fs_fseek;
	cfg.filesystem.ftell = fs_ftell;
	cfg.filesystem.fclose = fs_fclose;
	cfg.buffers.send_buffer = send_buf;
	cfg.buffers.recv_buffer = recv_buf;
	cfg.buffers.packet_size = packet;
	cfg.resume.mode = resume_mode;
	cfg.resume.tail_cap_bytes = tail_bytes;
	cfg.system.get_ticks_ms = get_ticks_ms;
	cfg.system.delay_ms = delay_ms;
	// Timeouts tuned for local serial/PTy transfers (more conservative)
	cfg.timeouts.min_timeout_ms = 500;    // 500 ms minimum timeout
	cfg.timeouts.max_timeout_ms = 20000;  // 20 second maximum timeout
	cfg.retries.handshake_retries = 5;
	cfg.retries.meta_retries = 4;
	cfg.retries.data_retries = 4;
	cfg.retries.ack_retries = 4;
	cfg.retries.backoff_ms_base = 100;    // 100 ms base backoff

	// Logger setup like TCP (env overrides)
	if (log_level < 0) {
		const char *lvl_env = getenv("VAL_LOG_LEVEL");
		if (lvl_env) log_level = parse_level(lvl_env);
	}
	if (!log_file_path) log_file_path = getenv("VAL_LOG_FILE");
	if (log_file_path && log_file_path[0]) { g_logf = fopen(log_file_path, "a"); }
	cfg.debug.log = console_logger;
	cfg.debug.context = NULL;
	cfg.debug.min_level = (log_level >= 0 ? log_level : 3);

	// Optional packet capture when VAL_CAPTURE is set
	const char *cap = getenv("VAL_CAPTURE");
	if (cap && (cap[0]=='1'||cap[0]=='y'||cap[0]=='Y'||cap[0]=='t'||cap[0]=='T')) {
		cfg.capture.on_packet = NULL; // RX example doesn’t inspect payloads; leave as null or provide on_packet
		cfg.capture.context = NULL;
	}

	// If streaming is off and user didn't explicitly allow accepting, also refuse incoming streaming
	if (!opt_streaming && !accept_streaming_explicit) opt_accept_streaming = 0;
	cfg.adaptive_tx.max_performance_mode = opt_max_mode;
	cfg.adaptive_tx.preferred_initial_mode = opt_tx_mode;
	cfg.adaptive_tx.allow_streaming = ((opt_streaming || opt_accept_streaming) ? true : false);
	cfg.adaptive_tx.retransmit_cache_enabled = false;
	cfg.adaptive_tx.degrade_error_threshold = 3;
	cfg.adaptive_tx.recovery_success_threshold = 10;
	cfg.adaptive_tx.mode_sync_interval = 0;

	// Attach callbacks and initialize summary tracking
	rx_summary_t sum = {0};
	g_rx_summary = &sum;
	sum.capacity = 16; // default until we know nfiles; COM example doesn't know ahead of time
	sum.received = (char **)calloc(1, sizeof(char *));
	sum.skipped = (char **)calloc(1, sizeof(char *));
	cfg.callbacks.on_file_start = on_file_start_rx;
	cfg.callbacks.on_file_complete = on_file_complete_rx;
	cfg.callbacks.on_progress = on_progress_announce_mode_rx;

	val_session_t *rx = NULL;
	uint32_t init_detail = 0;
	val_status_t init_rc = val_session_create(&cfg, &rx, &init_detail);
	if (init_rc != VAL_OK || !rx) {
		fprintf(stderr, "session create failed (rc=%d detail=0x%08X)\n", (int)init_rc, (unsigned)init_detail);
		serial_close(&sp); free(send_buf); free(recv_buf); return 4;
	}
	// Store for progress callback and print initial config
	g_rx_session_for_progress = rx;
#if VAL_ENABLE_METRICS && !defined(_WIN32)
	// Start watchdog thread
	g_watchdog_session_rx = rx;
	g_watchdog_stop_rx = 0;
	if (!g_watchdog_running_rx) {
		pthread_t th; if (pthread_create(&th, NULL, watchdog_thread_rx, NULL) == 0) { pthread_detach(th); g_watchdog_running_rx = 1; }
	}
#endif
	fprintf(stdout, "[VAL][RX] config: local_tx_init=%s, local_tx_cap<=%s, degrade=%u, upgrade=%u\n",
			tx_mode_name(cfg.adaptive_tx.preferred_initial_mode),
			tx_mode_name(cfg.adaptive_tx.max_performance_mode),
			(unsigned)cfg.adaptive_tx.degrade_error_threshold, (unsigned)cfg.adaptive_tx.recovery_success_threshold);
	fflush(stdout);
	{
		val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT;
		if (val_get_current_tx_mode(rx, &mode) == VAL_OK) {
			fprintf(stdout, "[VAL][RX] local-tx-mode(pre-handshake)=%s\n", tx_mode_name(mode));
			fflush(stdout);
		}
	}
	val_status_t st = val_receive_files(rx, outdir);
	cfg.transport.flush(cfg.transport.io_context); // Ensure all data is flushed
	fprintf(stderr, "[VAL][RX] Final protocol message sent and flushed. Waiting briefly before exit...\n");
	delay_ms(200); // 200ms delay to help sender receive last bytes
	if (st == VAL_OK) {
		// negotiated summary like TCP receiver
		size_t mtu = 0;
		if (val_get_effective_packet_size(rx, &mtu) == VAL_OK) {
			bool send_stream_ok=false, recv_stream_ok=false;
			(void)val_get_streaming_allowed(rx, &send_stream_ok, &recv_stream_ok);
			val_tx_mode_t mode = VAL_TX_STOP_AND_WAIT; (void)val_get_current_tx_mode(rx, &mode);
			fprintf(stdout, "[VAL][RX] negotiated: mtu=%zu, send_streaming=%s, accept_streaming=%s, local_tx_init=%s\n",
					mtu, send_stream_ok?"yes":"no", recv_stream_ok?"yes":"no", tx_mode_name(mode));
			fflush(stdout);
		}
		fprintf(stderr, "[VAL][RX] All files received or skipped. Exiting cleanly.\n");
	} else {
		val_status_t lc = VAL_OK; uint32_t det = 0;
		(void)val_get_last_error(rx, &lc, &det);
#if VAL_ENABLE_ERROR_STRINGS
		char buf[256];
		val_format_error_report(lc, det, buf, sizeof(buf));
		fprintf(stderr, "receive failed: %s\n", buf);
#else
		fprintf(stderr, "receive failed: code=%d detail=0x%08X\n", (int)lc, (unsigned)det);
#endif
	}
	#if VAL_ENABLE_METRICS
	print_metrics_rx(rx);
	#endif
	val_session_destroy(rx);
#if VAL_ENABLE_METRICS && !defined(_WIN32)
	g_watchdog_stop_rx = 1;
	g_watchdog_session_rx = NULL;
#endif
	serial_close(&sp);
	free(send_buf);
	free(recv_buf);
	rx_summary_print_and_free(&sum, st);
	return st == VAL_OK ? 0 : 5;
}
