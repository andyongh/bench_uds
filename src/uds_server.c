/*
 * uds_server.c — UDS benchmark server
 * Linux: epoll  |  macOS: kqueue   (via ub_mux_* abstraction)
 *
 * Build Linux : gcc -O3 -o uds_server uds_server.c -lpthread -lm
 * Build macOS : clang -O3 -o uds_server uds_server.c -lpthread
 */
#include "../include/uds_bench.h"
#include <getopt.h>

volatile sig_atomic_t g_stop = 0;
static void sig_handler(int s) { (void)s; g_stop = 1; }

typedef struct {
    int             listen_fd;
    bench_config_t *cfg;
    bench_result_t  result;
    int             id;
} server_ctx_t;

/* ─── Make listening socket ──────────────────────────────────── */
static int make_uds_server(const char *path, int type) {
    int fd = socket(AF_UNIX, type, 0);
    if (fd < 0) { perror("socket"); return -1; }
    set_sockbuf(fd, 1 << 20);
    set_nosigpipe(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path is 104 bytes on macOS, 108 on Linux */
    snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (type == SOCK_STREAM) {
        if (listen(fd, 256) < 0) { perror("listen"); close(fd); return -1; }
    }
    return fd;
}

/* ════════ BLOCKING STREAM server ════════════════════════════ */
static void *stream_block_server(void *arg) {
    server_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    uint8_t *buf = malloc(cfg->msg_size);

    int cfd = accept(ctx->listen_fd, NULL, NULL);
    if (cfd < 0) { perror("accept"); free(buf); return NULL; }
    set_sockbuf(cfd, 1 << 20);
    set_nosigpipe(cfd);

    uint64_t t_end = now_ns() + (uint64_t)cfg->duration_sec * 1000000000ULL;
    while (!g_stop && now_ns() < t_end) {
        ssize_t got = 0, need = cfg->msg_size;
        while (got < need) {
            ssize_t n = recv(cfd, buf + got, (size_t)(need - got), 0);
            if (n <= 0) goto done;
            got += n;
        }
        ctx->result.msgs_recv++;
        ctx->result.bytes_recv += (uint64_t)got;

        ssize_t sent = 0;
        while (sent < got) {
            ssize_t n = send(cfd, buf + sent, (size_t)(got - sent), MSG_NOSIGNAL);
            if (n <= 0) goto done;
            sent += n;
        }
        ctx->result.msgs_sent++;
        ctx->result.bytes_sent += (uint64_t)sent;
    }
done:
    close(cfd);
    free(buf);
    return NULL;
}

/* ════════ DGRAM BLOCKING server ════════════════════════════ */
static void *dgram_block_server(void *arg) {
    server_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    uint8_t *buf = malloc(cfg->msg_size + 256);

    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);
    uint64_t t_end = now_ns() + (uint64_t)cfg->duration_sec * 1000000000ULL;

    while (!g_stop && now_ns() < t_end) {
        ssize_t n = recvfrom(ctx->listen_fd, buf, (size_t)(cfg->msg_size + 256),
                             0, (struct sockaddr*)&peer, &plen);
        if (n <= 0) { if (errno == EINTR) continue; break; }
        ctx->result.msgs_recv++;
        ctx->result.bytes_recv += (uint64_t)n;

        ssize_t s = sendto(ctx->listen_fd, buf, (size_t)n, MSG_NOSIGNAL,
                           (struct sockaddr*)&peer, plen);
        if (s > 0) {
            ctx->result.msgs_sent++;
            ctx->result.bytes_sent += (uint64_t)s;
        }
    }
    free(buf);
    return NULL;
}

/* ════════ MUX (epoll/kqueue) STREAM server ═════════════════ */
static void *stream_mux_server(void *arg) {
    server_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    uint8_t *buf = malloc(cfg->msg_size * 4);

    int mfd = ub_mux_create();
    ub_mux_add_fd_int(mfd, ctx->listen_fd, UB_EPOLLIN);

    ub_event_t events[MAX_MUX_EVENTS];
    uint64_t t_end = now_ns() + (uint64_t)cfg->duration_sec * 1000000000ULL;

    while (!g_stop && now_ns() < t_end) {
        int n = ub_mux_wait(mfd, events, MAX_MUX_EVENTS, 200);
        for (int i = 0; i < n; i++) {
            int fd = UB_EVENT_FD(events[i]);
            if (fd == ctx->listen_fd) {
                /* Accept loop */
                while (1) {
                    int cfd = accept4(ctx->listen_fd, NULL, NULL, O_NONBLOCK);
                    if (cfd < 0) break;
                    set_sockbuf(cfd, 1 << 20);
                    set_nosigpipe(cfd);
                    ub_mux_add_fd_int(mfd, cfd, UB_EPOLLIN);
                }
            } else {
                /* Drain read */
                while (1) {
                    ssize_t r = recv(fd, buf, (size_t)(cfg->msg_size * 4), 0);
                    if (r <= 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        ub_mux_del(mfd, fd);
                        close(fd);
                        break;
                    }
                    ctx->result.msgs_recv += (uint64_t)(r / cfg->msg_size);
                    ctx->result.bytes_recv += (uint64_t)r;

                    ssize_t sent = 0;
                    while (sent < r) {
                        ssize_t s = send(fd, buf + sent, (size_t)(r - sent), MSG_NOSIGNAL);
                        if (s <= 0) break;
                        sent += s;
                    }
                    ctx->result.msgs_sent += (uint64_t)(sent / cfg->msg_size);
                    ctx->result.bytes_sent += (uint64_t)sent;
                }
            }
        }
    }
    ub_mux_close(mfd);
    free(buf);
    return NULL;
}

/* ════════ NONBLOCKING STREAM server ════════════════════════ */
static void *stream_nonblock_server(void *arg) {
    server_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    uint8_t *buf = malloc(cfg->msg_size * 2);

    set_nonblocking(ctx->listen_fd);

    int cfd = -1;
    uint64_t t_end = now_ns() + (uint64_t)cfg->duration_sec * 1000000000ULL;
    while (!g_stop && now_ns() < t_end) {
        cfd = accept4(ctx->listen_fd, NULL, NULL, O_NONBLOCK);
        if (cfd >= 0) { set_sockbuf(cfd, 1 << 20); set_nosigpipe(cfd); break; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) { perror("accept"); goto done; }
        _yield();
    }

    while (!g_stop && now_ns() < t_end) {
        ssize_t r = recv(cfd, buf, (size_t)(cfg->msg_size * 2), 0);
        if (r <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { _yield(); continue; }
            break;
        }
        ctx->result.msgs_recv += (uint64_t)(r / cfg->msg_size);
        ctx->result.bytes_recv += (uint64_t)r;

        ssize_t sent = 0;
        while (sent < r) {
            ssize_t s = send(cfd, buf + sent, (size_t)(r - sent), MSG_NOSIGNAL);
            if (s <= 0) { if (errno == EAGAIN) { _yield(); continue; } break; }
            sent += s;
        }
        ctx->result.msgs_sent += (uint64_t)(sent / cfg->msg_size);
        ctx->result.bytes_sent += (uint64_t)sent;
    }
    if (cfd >= 0) close(cfd);
done:
    free(buf);
    return NULL;
}

/* ─── Cleanup ────────────────────────────────────────────────── */
static void cleanup_socks(server_ctx_t *ctxs, int n) {
    for (int i = 0; i < n; i++) {
        if (ctxs[i].listen_fd >= 0) close(ctxs[i].listen_fd);
        char path[256];
        snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, i);
        unlink(path);
    }
}

/* ─── main ───────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    bench_config_t cfg = {
        .sock_mode    = MODE_STREAM,
        .io_mode      = IO_BLOCKING,
        .num_sockets  = 1,
        .num_threads  = 1,
        .msg_size     = DEFAULT_MSG_SIZE,
        .duration_sec = DEFAULT_DURATION,
        .verbose      = 0,
    };

    int opt;
    while ((opt = getopt(argc, argv, "m:i:n:t:s:d:v")) != -1) {
        switch (opt) {
            case 'm': cfg.sock_mode    = atoi(optarg); break;
            case 'i': cfg.io_mode      = atoi(optarg); break;
            case 'n': cfg.num_sockets  = atoi(optarg); break;
            case 't': cfg.num_threads  = atoi(optarg); break;
            case 's': cfg.msg_size     = atoi(optarg); break;
            case 'd': cfg.duration_sec = atoi(optarg); break;
            case 'v': cfg.verbose      = 1; break;
        }
    }

    struct rlimit rl = { .rlim_cur = 65536, .rlim_max = 65536 };
    setrlimit(RLIMIT_NOFILE, &rl);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int nsocks = cfg.num_sockets;
    int type   = cfg.sock_mode == MODE_STREAM ? SOCK_STREAM : SOCK_DGRAM;

    const char *mux_name =
#if PLATFORM_LINUX
        "EPOLL";
#else
        "KQUEUE";
#endif

    printf("[server] platform=%s mode=%s io=%s sockets=%d msgsize=%d duration=%ds\n",
#if PLATFORM_LINUX
           "Linux",
#else
           "macOS",
#endif
           cfg.sock_mode == MODE_STREAM ? "STREAM" : "DGRAM",
           cfg.io_mode   == IO_BLOCKING    ? "BLOCK"
           : cfg.io_mode == IO_NONBLOCKING ? "NONBLOCK" : mux_name,
           nsocks, cfg.msg_size, cfg.duration_sec);
    fflush(stdout);

    server_ctx_t *ctxs = calloc((size_t)nsocks, sizeof(server_ctx_t));
    pthread_t    *tids  = calloc((size_t)nsocks, sizeof(pthread_t));

    for (int i = 0; i < nsocks; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, i);
        ctxs[i].listen_fd = make_uds_server(path, type);
        if (ctxs[i].listen_fd < 0) { cleanup_socks(ctxs, i); return 1; }
        ctxs[i].cfg = &cfg;
        ctxs[i].id  = i;
        hdr_init(&ctxs[i].result.latency);

        void *(*fn)(void*) =
            (cfg.sock_mode == MODE_DGRAM)     ? dgram_block_server   :
            (cfg.io_mode   == IO_MUX)         ? stream_mux_server    :
            (cfg.io_mode   == IO_NONBLOCKING) ? stream_nonblock_server:
                                                stream_block_server;
        pthread_create(&tids[i], NULL, fn, &ctxs[i]);
    }

    for (int i = 0; i < nsocks; i++) pthread_join(tids[i], NULL);

    /* aggregate */
    bench_result_t agg = {0};
    hdr_init(&agg.latency);
    for (int i = 0; i < nsocks; i++) {
        agg.msgs_recv  += ctxs[i].result.msgs_recv;
        agg.msgs_sent  += ctxs[i].result.msgs_sent;
        agg.bytes_recv += ctxs[i].result.bytes_recv;
        agg.bytes_sent += ctxs[i].result.bytes_sent;
        agg.errors     += ctxs[i].result.errors;
    }
    printf("\n[SERVER] msgs_recv=%" PRIu64 " bytes_recv=%" PRIu64
           " qps=%.0f tput=%.2f MB/s\n",
           agg.msgs_recv, agg.bytes_recv,
           agg.msgs_recv / (double)cfg.duration_sec,
           agg.bytes_recv / (double)cfg.duration_sec / 1e6);

    cleanup_socks(ctxs, nsocks);
    free(ctxs);
    free(tids);
    return 0;
}
