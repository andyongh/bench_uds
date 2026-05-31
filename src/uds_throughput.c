/*
 * uds_throughput.c — Pure throughput test (no RTT measurement)
 *
 * Server floods data, client just sinks it (or vice versa).
 * Use this for max bandwidth measurement.
 *
 * Build: gcc -O3 -o uds_throughput uds_throughput.c -lpthread -lm
 */
#define _GNU_SOURCE
#include "../include/uds_bench.h"
#include <inttypes.h>
#include <getopt.h>

volatile sig_atomic_t g_stop = 0;
static _Atomic uint64_t g_bytes_sent = 0;
static _Atomic uint64_t g_msgs_sent  = 0;
static _Atomic uint64_t g_bytes_recv = 0;
static _Atomic uint64_t g_msgs_recv  = 0;

static void sig_handler(int s) { (void)s; g_stop = 1; }

/* ─── Sender thread ─────────────────────────────────────────── */
typedef struct {
    int   fd;
    int   msg_size;
    int   duration_sec;
} thr_arg_t;

static void *sender_thread(void *arg) {
    thr_arg_t *a = arg;
    uint8_t *buf = malloc(a->msg_size);
    memset(buf, 0x42, a->msg_size);

    uint64_t t_end = now_ns() + (uint64_t)a->duration_sec * 1000000000ULL;
    while (!g_stop && now_ns() < t_end) {
        ssize_t s = send(a->fd, buf, a->msg_size, MSG_NOSIGNAL);
        if (s > 0) {
            atomic_fetch_add(&g_bytes_sent, s);
            atomic_fetch_add(&g_msgs_sent,  1);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            sched_yield();
        } else {
            break;
        }
    }
    free(buf);
    return NULL;
}

/* ─── Receiver thread ───────────────────────────────────────── */
static void *receiver_thread(void *arg) {
    thr_arg_t *a = arg;
    uint8_t *buf = malloc(a->msg_size * 4);
    uint64_t t_end = now_ns() + (uint64_t)a->duration_sec * 1000000000ULL;
    while (!g_stop && now_ns() < t_end) {
        ssize_t r = recv(a->fd, buf, a->msg_size * 4, 0);
        if (r > 0) {
            atomic_fetch_add(&g_bytes_recv, r);
            atomic_fetch_add(&g_msgs_recv,  r / a->msg_size);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            sched_yield();
        } else {
            break;
        }
    }
    free(buf);
    return NULL;
}

/* ─── Reporter thread ───────────────────────────────────────── */
typedef struct {
    int duration_sec;
    int interval_sec;
    int msg_size;
} reporter_arg_t;

static void *reporter_thread(void *arg) {
    reporter_arg_t *r = arg;
    uint64_t prev_bytes = 0;
    uint64_t prev_msgs  = 0;
    uint64_t t0         = now_ns();

    for (int sec = 1; sec <= r->duration_sec && !g_stop; sec++) {
        sleep(r->interval_sec);
        uint64_t cur_bytes = atomic_load(&g_bytes_recv);
        uint64_t cur_msgs  = atomic_load(&g_msgs_recv);
        uint64_t elapsed   = now_ns() - t0;
        double dt = r->interval_sec;

        printf("[%3ds] QPS=%9.0f  Tput=%8.2f MB/s  Total=%.2f MB  Msgs=%" PRIu64 "\n",
               sec,
               (cur_msgs - prev_msgs) / dt,
               (cur_bytes - prev_bytes) / dt / 1e6,
               cur_bytes / 1e6,
               cur_msgs);
        fflush(stdout);
        prev_bytes = cur_bytes;
        prev_msgs  = cur_msgs;
        (void)elapsed;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int msg_size    = 4096;
    int duration    = 10;
    int num_senders = 4;
    int sock_type   = SOCK_STREAM;
    int is_server   = 0;

    int opt;
    while ((opt = getopt(argc, argv, "s:d:n:DSv")) != -1) {
        switch (opt) {
            case 's': msg_size    = atoi(optarg); break;
            case 'd': duration    = atoi(optarg); break;
            case 'n': num_senders = atoi(optarg); break;
            case 'D': sock_type   = SOCK_DGRAM;   break;
            case 'S': is_server   = 1;             break;
            case 'v': break;
        }
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    char path[256];
    snprintf(path, sizeof(path), "%s.tput", UDS_PATH_BASE);

    if (is_server) {
        /* ── SERVER: creates socket, accepts, spawns receivers ─ */
        int lfd = socket(AF_UNIX, sock_type, 0);
        set_sockbuf(lfd, 1 << 21);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
        unlink(path);
        bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
        if (sock_type == SOCK_STREAM) listen(lfd, 128);

        printf("[server] listening on %s  type=%s  msg=%d  duration=%ds\n",
               path, sock_type == SOCK_STREAM ? "STREAM" : "DGRAM",
               msg_size, duration);
        fflush(stdout);

        reporter_arg_t rarg = { .duration_sec=duration, .interval_sec=1, .msg_size=msg_size };
        pthread_t rtid;
        pthread_create(&rtid, NULL, reporter_thread, &rarg);

        pthread_t *tids = calloc(num_senders, sizeof(pthread_t));
        thr_arg_t *args = calloc(num_senders, sizeof(thr_arg_t));

        for (int i = 0; i < num_senders; i++) {
            int cfd = sock_type == SOCK_STREAM
                    ? accept(lfd, NULL, NULL)
                    : lfd;  /* dgram: reuse same fd */
            set_sockbuf(cfd, 1 << 21);
            args[i] = (thr_arg_t){ .fd=cfd, .msg_size=msg_size, .duration_sec=duration };
            pthread_create(&tids[i], NULL, receiver_thread, &args[i]);
        }

        for (int i = 0; i < num_senders; i++) pthread_join(tids[i], NULL);
        pthread_join(rtid, NULL);

        uint64_t tot_bytes = atomic_load(&g_bytes_recv);
        uint64_t tot_msgs  = atomic_load(&g_msgs_recv);
        printf("\n[SERVER FINAL] bytes_recv=%" PRIu64 " msgs=%" PRIu64
               " tput=%.2f MB/s qps=%.0f\n",
               tot_bytes, tot_msgs,
               tot_bytes / (double)duration / 1e6,
               tot_msgs  / (double)duration);

        free(tids); free(args);
        close(lfd); unlink(path);
    } else {
        /* ── CLIENT: connects, spawns senders ─────────────────── */
        printf("[client] connecting to %s  senders=%d  msg=%d  duration=%ds\n",
               path, num_senders, msg_size, duration);
        fflush(stdout);

        pthread_t *tids = calloc(num_senders, sizeof(pthread_t));
        thr_arg_t *args = calloc(num_senders, sizeof(thr_arg_t));

        for (int i = 0; i < num_senders; i++) {
            int fd = socket(AF_UNIX, sock_type, 0);
            set_sockbuf(fd, 1 << 21);

            /* dgram: bind self */
            if (sock_type == SOCK_DGRAM) {
                char sp[256];
                snprintf(sp, sizeof(sp), "%s_tput_c%d_%d", UDS_PATH_BASE, getpid(), i);
                unlink(sp);
                struct sockaddr_un sa = { .sun_family = AF_UNIX };
                snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sp);
                bind(fd, (struct sockaddr*)&sa, sizeof(sa));
            }

            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
            for (int r = 0; r < 50; r++) {
                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
                usleep(100000);
            }
            args[i] = (thr_arg_t){ .fd=fd, .msg_size=msg_size, .duration_sec=duration };
            pthread_create(&tids[i], NULL, sender_thread, &args[i]);
        }

        reporter_arg_t rarg = { .duration_sec=duration, .interval_sec=1, .msg_size=msg_size };
        pthread_t rtid;
        pthread_create(&rtid, NULL, reporter_thread, &rarg);

        for (int i = 0; i < num_senders; i++) pthread_join(tids[i], NULL);
        pthread_join(rtid, NULL);

        uint64_t tot_bytes = atomic_load(&g_bytes_sent);
        uint64_t tot_msgs  = atomic_load(&g_msgs_sent);
        printf("\n[CLIENT FINAL] bytes_sent=%" PRIu64 " msgs=%" PRIu64
               " tput=%.2f MB/s qps=%.0f\n",
               tot_bytes, tot_msgs,
               tot_bytes / (double)duration / 1e6,
               tot_msgs  / (double)duration);

        free(tids); free(args);
    }
    return 0;
}
