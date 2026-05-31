/*
 * uds_client.c — UDS benchmark client
 * Measures QPS, Throughput, RTT Latency with HDR histogram
 *
 * Build Linux : gcc -O3 -o uds_client uds_client.c -lpthread -lm
 * Build macOS : clang -O3 -o uds_client uds_client.c -lpthread
 */
#include "../include/uds_bench.h"
#include <getopt.h>

volatile sig_atomic_t g_stop = 0;
static void sig_handler(int s) { (void)s; g_stop = 1; }

typedef struct {
    bench_config_t *cfg;
    bench_result_t  result;
    int             id;
    int             sock_idx;
    FILE           *csv_fp;
} client_ctx_t;

/* ─── Connect helper ─────────────────────────────────────────── */
static int uds_connect(const char *server_path, int type, int client_id) {
    int fd = socket(AF_UNIX, type, 0);
    if (fd < 0) { perror("socket"); return -1; }
    set_sockbuf(fd, 1 << 20);
    set_nosigpipe(fd);

    if (type == SOCK_DGRAM) {
        /* bind a unique path for the client so server can reply */
        char self_path[108];  /* 104 bytes safe for macOS */
        snprintf(self_path, sizeof(self_path) - 1,
                 "/tmp/uds_bench_c%d_%d", getpid(), client_id);
        unlink(self_path);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path) - 1, "%s", self_path);
        if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("client bind"); close(fd); return -1;
        }
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s", server_path);

    for (int retry = 0; retry < 100; retry++) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        usleep(100000);
    }
    perror("connect");
    close(fd);
    return -1;
}

/* ─── Payload helpers ────────────────────────────────────────── */
static void fill_payload(uint8_t *buf, int size, uint64_t ts_ns, uint32_t seq) {
    if (size < (int)sizeof(msg_header_t)) return;
    msg_header_t *hdr = (msg_header_t*)buf;
    hdr->magic       = HDR_MAGIC;
    hdr->seq         = seq;
    hdr->ts_send_ns  = ts_ns;
    hdr->payload_len = (uint32_t)(size - sizeof(msg_header_t));
    memset(buf + sizeof(msg_header_t), 0xAB, (size_t)(size - (int)sizeof(msg_header_t)));
}

/* ════════ BLOCKING STREAM ════════════════════════════════════ */
static void *stream_block_client(void *arg) {
    client_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    int sz    = cfg->msg_size;
    uint8_t *buf  = malloc((size_t)sz);
    uint8_t *rbuf = malloc((size_t)sz);

    char path[256];
    snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, ctx->sock_idx);
    int fd = uds_connect(path, SOCK_STREAM, ctx->id);
    if (fd < 0) { ctx->result.errors++; goto done_alloc; }

    uint64_t t_start = now_ns();
    uint64_t t_end   = t_start + (uint64_t)cfg->duration_sec * 1000000000ULL;
    uint32_t seq = 0;

    while (!g_stop && now_ns() < t_end) {
        uint64_t ts = now_ns();
        fill_payload(buf, sz, ts, seq++);

        ssize_t sent = 0;
        while (sent < sz) {
            ssize_t n = send(fd, buf + sent, (size_t)(sz - sent), MSG_NOSIGNAL);
            if (n <= 0) goto done;
            sent += n;
        }
        ctx->result.msgs_sent++;
        ctx->result.bytes_sent += (uint64_t)sent;

        ssize_t got = 0;
        while (got < sz) {
            ssize_t n = recv(fd, rbuf + got, (size_t)(sz - got), 0);
            if (n <= 0) goto done;
            got += n;
        }
        hdr_record(&ctx->result.latency, now_ns() - ts);
        ctx->result.msgs_recv++;
        ctx->result.bytes_recv += (uint64_t)got;
    }
done:
    ctx->result.elapsed_sec = (now_ns() - t_start) / 1e9;
    close(fd);
done_alloc:
    free(buf); free(rbuf);
    return NULL;
}

/* ════════ DGRAM BLOCKING ════════════════════════════════════ */
static void *dgram_block_client(void *arg) {
    client_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    int sz    = cfg->msg_size;
    uint8_t *buf  = malloc((size_t)(sz + 256));
    uint8_t *rbuf = malloc((size_t)(sz + 256));

    char path[256];
    snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, ctx->sock_idx);
    int fd = uds_connect(path, SOCK_DGRAM, ctx->id);
    if (fd < 0) { ctx->result.errors++; goto done_alloc; }

    uint64_t t_start = now_ns();
    uint64_t t_end   = t_start + (uint64_t)cfg->duration_sec * 1000000000ULL;
    uint32_t seq = 0;

    while (!g_stop && now_ns() < t_end) {
        uint64_t ts = now_ns();
        fill_payload(buf, sz, ts, seq++);

        ssize_t s = send(fd, buf, (size_t)sz, 0);
        if (s <= 0) { ctx->result.errors++; continue; }
        ctx->result.msgs_sent++;
        ctx->result.bytes_sent += (uint64_t)s;

        ssize_t r = recv(fd, rbuf, (size_t)(sz + 256), 0);
        if (r <= 0) { ctx->result.errors++; continue; }
        hdr_record(&ctx->result.latency, now_ns() - ts);
        ctx->result.msgs_recv++;
        ctx->result.bytes_recv += (uint64_t)r;
    }
    ctx->result.elapsed_sec = (now_ns() - t_start) / 1e9;

    /* cleanup self-bound path */
    char self_path[108];
    snprintf(self_path, sizeof(self_path) - 1,
             "/tmp/uds_bench_c%d_%d", getpid(), ctx->id);
    close(fd);
    unlink(self_path);
done_alloc:
    free(buf); free(rbuf);
    return NULL;
}

/* ════════ NONBLOCKING STREAM ════════════════════════════════ */
static void *stream_nonblock_client(void *arg) {
    client_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    int sz    = cfg->msg_size;
    uint8_t *buf  = malloc((size_t)sz);
    uint8_t *rbuf = malloc((size_t)sz);

    char path[256];
    snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, ctx->sock_idx);
    int fd = uds_connect(path, SOCK_STREAM, ctx->id);
    if (fd < 0) { ctx->result.errors++; goto done_alloc; }
    set_nonblocking(fd);

    uint64_t t_start = now_ns();
    uint64_t t_end   = t_start + (uint64_t)cfg->duration_sec * 1000000000ULL;
    uint32_t seq = 0;

    while (!g_stop && now_ns() < t_end) {
        uint64_t ts = now_ns();
        fill_payload(buf, sz, ts, seq++);

        ssize_t sent = 0;
        while (sent < sz) {
            ssize_t n = send(fd, buf + sent, (size_t)(sz - sent), MSG_NOSIGNAL);
            if (n < 0) { if (errno == EAGAIN) { _yield(); continue; } goto done; }
            sent += n;
        }
        ctx->result.msgs_sent++;
        ctx->result.bytes_sent += (uint64_t)sent;

        ssize_t got = 0;
        while (got < sz) {
            ssize_t n = recv(fd, rbuf + got, (size_t)(sz - got), 0);
            if (n < 0) { if (errno == EAGAIN) { _yield(); continue; } goto done; }
            if (n == 0) goto done;
            got += n;
        }
        hdr_record(&ctx->result.latency, now_ns() - ts);
        ctx->result.msgs_recv++;
        ctx->result.bytes_recv += (uint64_t)got;
    }
done:
    ctx->result.elapsed_sec = (now_ns() - t_start) / 1e9;
    close(fd);
done_alloc:
    free(buf); free(rbuf);
    return NULL;
}

/* ════════ MUX (epoll/kqueue) pipeline client ════════════════ */
typedef struct {
    int     fd;
    uint8_t *sbuf;
    uint8_t *rbuf;
    int      sz;
    ssize_t  rpos;
    uint64_t ts_send;
    uint32_t seq;
} mux_conn_t;

static void *stream_mux_client(void *arg) {
    client_ctx_t   *ctx = arg;
    bench_config_t *cfg = ctx->cfg;
    int sz    = cfg->msg_size;
    int nconn = cfg->pipeline > 0 ? cfg->pipeline : 4;

    char path[256];
    snprintf(path, sizeof(path), "%s.%d", UDS_PATH_BASE, ctx->sock_idx);

    int mfd = ub_mux_create();
    mux_conn_t *conns = calloc((size_t)nconn, sizeof(mux_conn_t));
    uint32_t global_seq = (uint32_t)nconn;

    for (int i = 0; i < nconn; i++) {
        int fd = uds_connect(path, SOCK_STREAM, ctx->id * 1000 + i);
        if (fd < 0) { ctx->result.errors++; goto done; }
        set_nonblocking(fd);
        set_sockbuf(fd, 1 << 20);

        conns[i].fd   = fd;
        conns[i].sbuf = malloc((size_t)sz);
        conns[i].rbuf = malloc((size_t)sz);
        conns[i].sz   = sz;
        conns[i].rpos = 0;
        conns[i].seq  = (uint32_t)i;

        uint64_t ts = now_ns();
        conns[i].ts_send = ts;
        fill_payload(conns[i].sbuf, sz, ts, conns[i].seq);
        send(fd, conns[i].sbuf, (size_t)sz, MSG_NOSIGNAL);
        ctx->result.msgs_sent++;
        ctx->result.bytes_sent += (uint64_t)sz;

        ub_mux_add_fd(mfd, fd, UB_EPOLLIN, &conns[i]);
    }

    ub_event_t events[MAX_MUX_EVENTS];
    uint64_t t_start = now_ns();
    uint64_t t_end   = t_start + (uint64_t)cfg->duration_sec * 1000000000ULL;

    while (!g_stop && now_ns() < t_end) {
        int n = ub_mux_wait(mfd, events, MAX_MUX_EVENTS, 200);
        for (int i = 0; i < n; i++) {
            mux_conn_t *c = UB_EVENT_PTR(events[i]);
            if (!c) continue;

            while (1) {
                ssize_t r = recv(c->fd, c->rbuf + c->rpos,
                                 (size_t)(sz - c->rpos), 0);
                if (r <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    goto done;
                }
                c->rpos += r;
                if (c->rpos >= sz) {
                    hdr_record(&ctx->result.latency, now_ns() - c->ts_send);
                    ctx->result.msgs_recv++;
                    ctx->result.bytes_recv += (uint64_t)sz;
                    c->rpos = 0;

                    /* Send next */
                    uint64_t ts = now_ns();
                    c->ts_send = ts;
                    c->seq = global_seq++;
                    fill_payload(c->sbuf, sz, ts, c->seq);
                    ssize_t sent = 0;
                    while (sent < sz) {
                        ssize_t s = send(c->fd, c->sbuf + sent,
                                         (size_t)(sz - sent), MSG_NOSIGNAL);
                        if (s <= 0) break;
                        sent += s;
                    }
                    ctx->result.msgs_sent++;
                    ctx->result.bytes_sent += (uint64_t)sent;
                }
            }
        }
    }
    ctx->result.elapsed_sec = (now_ns() - t_start) / 1e9;

done:
    ub_mux_close(mfd);
    for (int i = 0; i < nconn; i++) {
        if (conns[i].fd > 0) close(conns[i].fd);
        free(conns[i].sbuf);
        free(conns[i].rbuf);
    }
    free(conns);
    return NULL;
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
        .pipeline     = 4,
        .warmup_sec   = 1,
        .verbose      = 0,
    };

    const char *csv_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "m:i:n:t:s:d:p:w:o:v")) != -1) {
        switch (opt) {
            case 'm': cfg.sock_mode    = atoi(optarg); break;
            case 'i': cfg.io_mode      = atoi(optarg); break;
            case 'n': cfg.num_sockets  = atoi(optarg); break;
            case 't': cfg.num_threads  = atoi(optarg); break;
            case 's': cfg.msg_size     = atoi(optarg); break;
            case 'd': cfg.duration_sec = atoi(optarg); break;
            case 'p': cfg.pipeline     = atoi(optarg); break;
            case 'w': cfg.warmup_sec   = atoi(optarg); break;
            case 'o': csv_path         = optarg; break;
            case 'v': cfg.verbose      = 1; break;
        }
    }

    struct rlimit rl = { .rlim_cur = 65536, .rlim_max = 65536 };
    setrlimit(RLIMIT_NOFILE, &rl);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    FILE *csv_fp = NULL;
    if (csv_path) {
        int need_hdr = access(csv_path, F_OK) != 0;
        csv_fp = fopen(csv_path, "a");
        if (csv_fp && need_hdr)
            fprintf(csv_fp, "label,sock_mode,io_mode,sockets,threads,msg_size,"
                            "qps,tput_mb,avg_us,p50_us,p90_us,p99_us,p999_us,max_us\n");
    }

    const char *mux_name =
#if PLATFORM_LINUX
        "EPOLL";
#else
        "KQUEUE";
#endif

    printf("[client] platform=%s mode=%s io=%s sockets=%d threads=%d msgsize=%d duration=%ds\n",
#if PLATFORM_LINUX
           "Linux",
#else
           "macOS",
#endif
           cfg.sock_mode == MODE_STREAM ? "STREAM" : "DGRAM",
           cfg.io_mode   == IO_BLOCKING    ? "BLOCK"
           : cfg.io_mode == IO_NONBLOCKING ? "NONBLOCK" : mux_name,
           cfg.num_sockets, cfg.num_threads, cfg.msg_size, cfg.duration_sec);
    fflush(stdout);

    int nsocks  = cfg.num_sockets;
    int total   = nsocks * cfg.num_threads;

    client_ctx_t *ctxs = calloc((size_t)total, sizeof(client_ctx_t));
    pthread_t    *tids  = calloc((size_t)total, sizeof(pthread_t));

    for (int i = 0; i < total; i++) {
        hdr_init(&ctxs[i].result.latency);
        ctxs[i].cfg      = &cfg;
        ctxs[i].id       = i;
        ctxs[i].sock_idx = i % nsocks;
        ctxs[i].csv_fp   = csv_fp;
    }

    void *(*fn)(void*) =
        (cfg.sock_mode == MODE_DGRAM)     ? dgram_block_client   :
        (cfg.io_mode   == IO_MUX)         ? stream_mux_client    :
        (cfg.io_mode   == IO_NONBLOCKING) ? stream_nonblock_client:
                                            stream_block_client;

    uint64_t t0 = now_ns();
    for (int i = 0; i < total; i++)
        pthread_create(&tids[i], NULL, fn, &ctxs[i]);
    for (int i = 0; i < total; i++)
        pthread_join(tids[i], NULL);
    double elapsed = (now_ns() - t0) / 1e9;

    /* aggregate */
    bench_result_t agg = {0};
    hdr_init(&agg.latency);
    for (int i = 0; i < total; i++) {
        agg.msgs_sent  += ctxs[i].result.msgs_sent;
        agg.msgs_recv  += ctxs[i].result.msgs_recv;
        agg.bytes_sent += ctxs[i].result.bytes_sent;
        agg.bytes_recv += ctxs[i].result.bytes_recv;
        agg.errors     += ctxs[i].result.errors;
        hdr_merge(&agg.latency, &ctxs[i].result.latency);
    }
    agg.elapsed_sec = elapsed;

    char label[128];
    snprintf(label, sizeof(label), "UDS %s/%s S%d T%d MSG%d",
             cfg.sock_mode == MODE_STREAM ? "STREAM" : "DGRAM",
             cfg.io_mode   == IO_BLOCKING    ? "BLOCK"
             : cfg.io_mode == IO_NONBLOCKING ? "NONBLOCK" : mux_name,
             nsocks, cfg.num_threads, cfg.msg_size);

    print_result(label, &cfg, &agg);
    csv_result(csv_fp, label, &cfg, &agg);

    if (csv_fp) fclose(csv_fp);
    free(ctxs); free(tids);
    return 0;
}
