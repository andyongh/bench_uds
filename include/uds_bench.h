#pragma once
#ifndef UDS_BENCH_H
#define UDS_BENCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <math.h>

/* ─── Platform detection ─────────────────────────────────────── */
#if defined(__APPLE__)
#  define PLATFORM_MACOS   1
#  define PLATFORM_LINUX   0
#  include <sys/event.h>     /* kqueue */
#  include <mach/mach_time.h>
#elif defined(__linux__)
#  define PLATFORM_MACOS   0
#  define PLATFORM_LINUX   1
#  include <sys/epoll.h>
#  include <sched.h>
#else
#  error "Unsupported platform (Linux or macOS only)"
#endif

/* ─── MSG_NOSIGNAL ───────────────────────────────────────────── */
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0    /* macOS: we set SO_NOSIGPIPE per-socket instead */
#endif

/* ─── accept4 / SOCK_NONBLOCK ────────────────────────────────── */
#if PLATFORM_MACOS
#  ifndef SOCK_NONBLOCK
#    define SOCK_NONBLOCK 0  /* dummy; set via fcntl after accept */
#  endif
static inline int accept4(int sockfd, struct sockaddr *addr,
                           socklen_t *addrlen, int flags)
{
    int fd = accept(sockfd, addr, addrlen);
    if (fd < 0) return fd;
    if (flags & O_NONBLOCK) {
        int f = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, f | O_NONBLOCK);
    }
    /* suppress SIGPIPE per-socket on macOS */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    return fd;
}
#endif

/* ─── sched_yield ────────────────────────────────────────────── */
#if PLATFORM_MACOS
#  include <pthread.h>
static inline void _yield(void) { sched_yield(); }
#else
static inline void _yield(void) { sched_yield(); }
#endif

/* ─── Monotonic clock (ns) ───────────────────────────────────── */
#if PLATFORM_MACOS
/* Use mach_absolute_time for sub-µs accuracy on macOS */
static mach_timebase_info_data_t _mtbi;
static pthread_once_t            _mtbi_once = PTHREAD_ONCE_INIT;
static void _mtbi_init(void) { mach_timebase_info(&_mtbi); }
static inline uint64_t now_ns(void) {
    pthread_once(&_mtbi_once, _mtbi_init);
    return mach_absolute_time() * _mtbi.numer / _mtbi.denom;
}
#else
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

/* ─── kqueue / epoll abstraction ─────────────────────────────── */
/*
 * We expose a minimal event-loop API that maps to epoll on Linux
 * and kqueue on macOS. The server/client code uses these wrappers.
 */
#if PLATFORM_LINUX

typedef struct epoll_event   ub_event_t;
#define UB_EPOLLIN           EPOLLIN
#define UB_EPOLLET           EPOLLET
#define UB_EVENT_FD(e)       ((e).data.fd)
#define UB_EVENT_PTR(e)      ((e).data.ptr)

static inline int ub_mux_create(void) { return epoll_create1(0); }
static inline void ub_mux_close(int mfd) { close(mfd); }

static inline int ub_mux_add_fd(int mfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev;
    ev.events   = events;
    ev.data.ptr = ptr;
    /* if ptr is NULL, store fd directly */
    if (!ptr) { ev.data.fd = fd; }
    return epoll_ctl(mfd, EPOLL_CTL_ADD, fd, &ev);
}
static inline int ub_mux_add_fd_int(int mfd, int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(mfd, EPOLL_CTL_ADD, fd, &ev);
}
static inline int ub_mux_del(int mfd, int fd) {
    return epoll_ctl(mfd, EPOLL_CTL_DEL, fd, NULL);
}
static inline int ub_mux_wait(int mfd, ub_event_t *evs, int maxev, int ms) {
    return epoll_wait(mfd, evs, maxev, ms);
}
static inline int ub_event_is_listen(ub_event_t *e, int lfd) {
    return e->data.fd == lfd;
}

#else  /* PLATFORM_MACOS — kqueue */

typedef struct kevent   ub_event_t;
#define UB_EPOLLIN      EVFILT_READ
#define UB_EPOLLET      0            /* kqueue is always edge-ish */
#define UB_EVENT_FD(e)  ((int)(e).ident)
#define UB_EVENT_PTR(e) ((e).udata)

static inline int ub_mux_create(void) { return kqueue(); }
static inline void ub_mux_close(int mfd) { close(mfd); }

static inline int ub_mux_add_fd(int mfd, int fd, uint32_t events, void *ptr) {
    (void)events;
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, ptr);
    return kevent(mfd, &kev, 1, NULL, 0, NULL);
}
static inline int ub_mux_add_fd_int(int mfd, int fd, uint32_t events) {
    (void)events;
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    return kevent(mfd, &kev, 1, NULL, 0, NULL);
}
static inline int ub_mux_del(int mfd, int fd) {
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(mfd, &kev, 1, NULL, 0, NULL);
}
static inline int ub_mux_wait(int mfd, ub_event_t *evs, int maxev, int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    return kevent(mfd, NULL, 0, evs, maxev, ms < 0 ? NULL : &ts);
}
static inline int ub_event_is_listen(ub_event_t *e, int lfd) {
    return (int)e->ident == lfd;
}

#endif  /* platform mux */

/* ─── Constants ─────────────────────────────────────────────── */
#define UDS_PATH_BASE       "/tmp/uds_bench"
#define MAX_SOCKETS         256
#define MAX_MUX_EVENTS      1024
#define MAX_MSG_SIZE        65536
#define DEFAULT_MSG_SIZE    1024
#define DEFAULT_DURATION    10
#define DEFAULT_THREADS     1
#define DEFAULT_SOCKETS     1
#define HDR_MAGIC           0xDEADBEEF

/* ─── HDR Histogram ──────────────────────────────────────────── */
#define HDR_BUCKET_COUNT    10000
typedef struct {
    uint64_t counts[HDR_BUCKET_COUNT];
    uint64_t total_count;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t sum_ns;
} hdr_histogram_t;

static inline void hdr_init(hdr_histogram_t *h) {
    memset(h, 0, sizeof(*h));
    h->min_ns = UINT64_MAX;
}
static inline void hdr_record(hdr_histogram_t *h, uint64_t ns) {
    uint64_t bucket = ns / 1000;
    if (bucket >= HDR_BUCKET_COUNT) bucket = HDR_BUCKET_COUNT - 1;
    h->counts[bucket]++;
    h->total_count++;
    h->sum_ns += ns;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
}
static inline uint64_t hdr_percentile(hdr_histogram_t *h, double pct) {
    if (h->total_count == 0) return 0;
    uint64_t target = (uint64_t)(h->total_count * pct / 100.0);
    uint64_t cumulative = 0;
    for (int i = 0; i < HDR_BUCKET_COUNT; i++) {
        cumulative += h->counts[i];
        if (cumulative >= target) return (uint64_t)i * 1000;
    }
    return (uint64_t)(HDR_BUCKET_COUNT - 1) * 1000;
}
static inline void hdr_merge(hdr_histogram_t *dst, const hdr_histogram_t *src) {
    for (int i = 0; i < HDR_BUCKET_COUNT; i++)
        dst->counts[i] += src->counts[i];
    dst->total_count += src->total_count;
    dst->sum_ns      += src->sum_ns;
    if (src->min_ns < dst->min_ns) dst->min_ns = src->min_ns;
    if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
}

/* ─── Bench Config ───────────────────────────────────────────── */
typedef enum { MODE_STREAM = 0, MODE_DGRAM = 1 } sock_mode_t;
typedef enum { IO_BLOCKING = 0, IO_NONBLOCKING = 1, IO_MUX = 2 } io_mode_t;
/* IO_MUX = epoll on Linux, kqueue on macOS */

typedef struct {
    sock_mode_t  sock_mode;
    io_mode_t    io_mode;
    int          num_sockets;
    int          num_threads;
    int          msg_size;
    int          duration_sec;
    int          pipeline;
    int          warmup_sec;
    char         sock_path[256];
    int          verbose;
} bench_config_t;

typedef struct {
    uint64_t     msgs_sent;
    uint64_t     msgs_recv;
    uint64_t     bytes_sent;
    uint64_t     bytes_recv;
    uint64_t     errors;
    double       elapsed_sec;
    hdr_histogram_t latency;
} bench_result_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint64_t ts_send_ns;
    uint32_t payload_len;
} msg_header_t;

/* ─── Socket helpers ─────────────────────────────────────────── */
static inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static inline void set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
static inline void set_sockbuf(int fd, int size) {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}
/* Suppress SIGPIPE on the fd (macOS: SO_NOSIGPIPE; Linux: noop — use MSG_NOSIGNAL) */
static inline void set_nosigpipe(int fd) {
#if PLATFORM_MACOS
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

/* ─── Result printing ────────────────────────────────────────── */
static inline void print_result(const char *label,
                                 const bench_config_t *cfg,
                                 const bench_result_t *r)
{
    double qps     = r->msgs_recv / r->elapsed_sec;
    double tput_mb = (r->bytes_recv / r->elapsed_sec) / (1024.0 * 1024.0);
    double avg_us  = r->latency.total_count > 0
                   ? (double)r->latency.sum_ns / r->latency.total_count / 1000.0 : 0.0;

    const char *sm = cfg->sock_mode == MODE_STREAM ? "STREAM" : "DGRAM";
    const char *im = cfg->io_mode   == IO_BLOCKING    ? "BLOCK"
                   : cfg->io_mode   == IO_NONBLOCKING  ? "NONBLOCK"
#if PLATFORM_LINUX
                   :                                     "EPOLL";
#else
                   :                                     "KQUEUE";
#endif

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  %-52s║\n", label);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Mode : %-8s  IO: %-10s  Sockets: %-6d   ║\n", sm, im, cfg->num_sockets);
    printf("║  Threads: %-4d  MsgSize: %-6d  Duration: %-4ds     ║\n",
           cfg->num_threads, cfg->msg_size, cfg->duration_sec);
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  QPS         : %15.0f msg/s                ║\n", qps);
    printf("║  Throughput  : %15.2f MB/s                 ║\n", tput_mb);
    printf("║  Total msgs  : %15" PRIu64 "                        ║\n", r->msgs_recv);
    printf("║  Errors      : %15" PRIu64 "                        ║\n", r->errors);
    if (r->latency.total_count > 0) {
        printf("╠══════════════════════════════════════════════════════╣\n");
        printf("║  Latency (RTT)                                       ║\n");
        printf("║    min   : %10.2f µs                            ║\n",
               r->latency.min_ns == UINT64_MAX ? 0.0 : r->latency.min_ns / 1000.0);
        printf("║    avg   : %10.2f µs                            ║\n", avg_us);
        printf("║    p50   : %10.2f µs                            ║\n",
               hdr_percentile((hdr_histogram_t*)&r->latency, 50)   / 1000.0);
        printf("║    p90   : %10.2f µs                            ║\n",
               hdr_percentile((hdr_histogram_t*)&r->latency, 90)   / 1000.0);
        printf("║    p99   : %10.2f µs                            ║\n",
               hdr_percentile((hdr_histogram_t*)&r->latency, 99)   / 1000.0);
        printf("║    p99.9 : %10.2f µs                            ║\n",
               hdr_percentile((hdr_histogram_t*)&r->latency, 99.9) / 1000.0);
        printf("║    max   : %10.2f µs                            ║\n",
               r->latency.max_ns / 1000.0);
    }
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    fflush(stdout);
}

static inline void csv_result(FILE *fp,
                               const char *label,
                               const bench_config_t *cfg,
                               const bench_result_t *r)
{
    if (!fp) return;
    double qps     = r->msgs_recv / r->elapsed_sec;
    double tput_mb = (r->bytes_recv / r->elapsed_sec) / (1024.0 * 1024.0);
    double avg_us  = r->latency.total_count > 0
                   ? (double)r->latency.sum_ns / r->latency.total_count / 1000.0 : 0;
    const char *sm = cfg->sock_mode == MODE_STREAM ? "STREAM" : "DGRAM";
    const char *im = cfg->io_mode   == IO_BLOCKING    ? "BLOCK"
                   : cfg->io_mode   == IO_NONBLOCKING  ? "NONBLOCK" : "MUX";
    fprintf(fp, "%s,%s,%s,%d,%d,%d,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
        label, sm, im, cfg->num_sockets, cfg->num_threads, cfg->msg_size,
        qps, tput_mb, avg_us,
        hdr_percentile((hdr_histogram_t*)&r->latency, 50)   / 1000.0,
        hdr_percentile((hdr_histogram_t*)&r->latency, 90)   / 1000.0,
        hdr_percentile((hdr_histogram_t*)&r->latency, 99)   / 1000.0,
        hdr_percentile((hdr_histogram_t*)&r->latency, 99.9) / 1000.0,
        r->latency.max_ns / 1000.0);
}

extern volatile sig_atomic_t g_stop;

#endif /* UDS_BENCH_H */
