/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD protocol server: the network thread. See mpd_srv.h. */
#include "mpd_srv.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    char in[MPD_LINE_MAX + 2]; int nin;
    int overlong;                         /* skipping the rest of a line that was too long */
    mpd_sess_t s;
    unsigned seen[MPD_EV_COUNT];          /* the event counters this client has been told about */
    time_t last;                          /* last line received */
} client_t;

static pthread_t g_th;
static int g_live, g_fd = -1, g_port, g_wake[2] = { -1, -1 };
static volatile int g_run;
static char g_pass[REMOTE_TOKEN_LEN + 1]; static int g_has_pass;
static client_t g_cl[MPD_CLIENTS]; static int g_ncl;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv; static int g_cv_ready;
static unsigned g_ev[MPD_EV_COUNT];                     /* guarded by g_mu */
/* one-slot mailbox, guarded by g_mu */
static int g_box_full, g_box_taken, g_box_done, g_box_orphan;
static mpd_sess_t g_box_sess;                           /* the job's own copy of the session (owns its batch) */
static rmt_buf_t g_box_out;
static void (*g_notify)(void);
static mpd_handler_t g_handler; static void *g_handler_ud;
void msrv_set_notify(void (*cb)(void)){ g_notify = cb; }
void msrv_set_handler(mpd_handler_t h, void *ud){ g_handler = h; g_handler_ud = ud; }
int  msrv_clients(void){ return g_live ? g_ncl : 0; }

static void wake_thread(void){ if(g_wake[1] >= 0){ char c = 1; ssize_t w = write(g_wake[1], &c, 1); (void)w; } }

/* ---------------------------------------------------------------- UI-thread side */
void msrv_events(unsigned mask){
    if(!mask) return;
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < MPD_EV_COUNT; i++) if(mask & (1u << i)) g_ev[i]++;
    pthread_mutex_unlock(&g_mu);
    wake_thread();
}
int msrv_pump(void){
    mpd_sess_t job;
    pthread_mutex_lock(&g_mu);
    if(!g_box_full || g_box_taken){ pthread_mutex_unlock(&g_mu); return 0; }
    job = g_box_sess; memset(&g_box_sess.batch, 0, sizeof g_box_sess.batch);   /* the job owns the lines now */
    g_box_taken = 1;
    pthread_mutex_unlock(&g_mu);
    rmt_buf_t out = { 0 };
    if(g_handler) mpd_run_batch(&job, g_has_pass ? g_pass : NULL, g_handler, g_handler_ud, &out);
    else { mpd_sess_free(&job); mpd_ack(&out, MPD_ACK_SYSTEM, 0, "", "Not ready"); }
    mpd_sess_free(&job);
    pthread_mutex_lock(&g_mu);
    if(g_box_orphan){                                   /* its client gave up: free the slot */
        g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
        free(out.p);
    } else {
        g_box_out = out; g_box_sess.authed = job.authed;   /* a password inside a list */
        g_box_done = 1;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    return 1;
}

/* ---------------------------------------------------------------- network side */
static int send_all(int fd, const char *p, size_t n){
    while(n){
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
static int send_buf(client_t *c, rmt_buf_t *b){
    int r = 0;
    if(b->oom) r = send_all(c->fd, "ACK [52@0] {} Out of memory\n", 28);
    else if(b->n) r = send_all(c->fd, b->p, b->n);
    free(b->p); memset(b, 0, sizeof *b);
    return r;
}
/* Hand the client's batch to the UI thread and wait for its answer. -1: drop the client. */
static int run_batch(client_t *c){
    rmt_buf_t out = { 0 };
    pthread_mutex_lock(&g_mu);
    while(g_box_full && g_run){                         /* only this thread fills the box: a leftover is an orphan */
        struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl); dl.tv_nsec += 50000000L;
        if(dl.tv_nsec >= 1000000000L){ dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_cv, &g_mu, &dl);
    }
    if(!g_run){ pthread_mutex_unlock(&g_mu); return -1; }
    g_box_sess = c->s; memset(&c->s.batch, 0, sizeof c->s.batch);     /* the box owns the lines */
    c->s.list = 0; c->s.nlist = 0; c->s.list_bad = 0;
    g_box_full = 1; g_box_taken = g_box_done = g_box_orphan = 0;
    if(g_notify) g_notify();
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += MSRV_CMD_WAIT_MS / 1000; dl.tv_nsec += (long)(MSRV_CMD_WAIT_MS % 1000) * 1000000L;
    if(dl.tv_nsec >= 1000000000L){ dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    while(!g_box_done && g_run){
        if(pthread_cond_timedwait(&g_cv, &g_mu, &dl) == ETIMEDOUT) break;
    }
    if(!g_box_done){
        if(g_box_taken) g_box_orphan = 1;              /* the UI thread runs it; msrv_pump frees the slot */
        else { mpd_sess_free(&g_box_sess); g_box_full = 0; }   /* never picked up: withdraw it, it must not run late */
        pthread_mutex_unlock(&g_mu);
        mpd_ack(&out, MPD_ACK_SYSTEM, 0, "", "The player did not answer in time");
        return send_buf(c, &out);
    }
    out = g_box_out; memset(&g_box_out, 0, sizeof g_box_out);
    c->s.authed = g_box_sess.authed;
    mpd_sess_free(&g_box_sess);
    g_box_full = g_box_taken = g_box_done = 0;
    pthread_mutex_unlock(&g_mu);
    return send_buf(c, &out);
}
/* The events this client waits for that it has not been told about: answer them and leave idle. 1 = answered. */
static int idle_answer(client_t *c, int force){
    unsigned now[MPD_EV_COUNT], pending = 0;
    pthread_mutex_lock(&g_mu); memcpy(now, g_ev, sizeof now); pthread_mutex_unlock(&g_mu);
    for(int i = 0; i < MPD_EV_COUNT; i++) if(now[i] != c->seen[i] && (c->s.idle_mask & (1u << i))) pending |= 1u << i;
    if(!pending && !force) return 0;
    rmt_buf_t out = { 0 };
    for(int i = 0; i < MPD_EV_COUNT; i++) if(pending & (1u << i)){ mpd_kv(&out, "changed", mpd_event_name(i)); c->seen[i] = now[i]; }
    rmt_buf_put(&out, "OK\n");
    c->s.idle = 0; c->s.idle_mask = 0;
    c->last = time(NULL);
    if(send_buf(c, &out)) return -1;
    return 1;
}
static void drop(int i){
    close(g_cl[i].fd);
    mpd_sess_free(&g_cl[i].s);
    g_cl[i] = g_cl[--g_ncl];
}
/* One line from client i. -1: drop it. */
static int line_in(client_t *c, char *line){
    rmt_buf_t out = { 0 };
    c->last = time(NULL);
    int a = mpd_sess_line(&c->s, line, g_has_pass ? g_pass : NULL, &out);
    switch(a){
    case MPD_A_REPLY: return send_buf(c, &out);
    case MPD_A_RUN: free(out.p); return run_batch(c);
    case MPD_A_IDLE: free(out.p); return idle_answer(c, 0) < 0 ? -1 : 0;
    case MPD_A_NOIDLE: free(out.p); return idle_answer(c, 1) < 0 ? -1 : 0;
    case MPD_A_CLOSE: free(out.p); return -1;
    default: free(out.p); return 0;
    }
}
static int client_read(client_t *c){
    char buf[2048];
    ssize_t r = recv(c->fd, buf, sizeof buf, 0);
    if(r < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    if(r <= 0) return -1;
    for(ssize_t i = 0; i < r; i++){
        char ch = buf[i];
        if(ch == '\n'){
            if(c->overlong){
                c->overlong = 0; c->nin = 0;
                rmt_buf_t out = { 0 }; mpd_ack(&out, MPD_ACK_ARG, 0, "", "Line too long");
                if(send_buf(c, &out)) return -1;
                continue;
            }
            c->in[c->nin] = 0; c->nin = 0;
            if(line_in(c, c->in)) return -1;
            continue;
        }
        if(c->overlong) continue;
        if(!ch){ c->overlong = 1; continue; }                     /* NUL: refuse the line */
        if(c->nin >= MPD_LINE_MAX){ c->overlong = 1; continue; }
        c->in[c->nin++] = ch;
    }
    return 0;
}
static void *worker(void *arg){
    (void)arg;
    while(g_run){
        struct pollfd pf[MPD_CLIENTS + 2];
        pf[0] = (struct pollfd){ g_fd, POLLIN, 0 };
        pf[1] = (struct pollfd){ g_wake[0], POLLIN, 0 };
        for(int i = 0; i < g_ncl; i++) pf[2 + i] = (struct pollfd){ g_cl[i].fd, POLLIN, 0 };
        int nfds = 2 + g_ncl;
        int pr = poll(pf, (nfds_t)nfds, 1000);
        if(!g_run) break;
        if(pr < 0) continue;
        if(pf[1].revents & POLLIN){ char b[64]; while(read(g_wake[0], b, sizeof b) > 0){} }
        /* clients first (their slots shift when one is dropped: walk backwards) */
        int ncl = g_ncl;
        for(int i = ncl - 1; i >= 0; i--){
            if(i >= g_ncl) continue;
            short re = pf[2 + i].revents;
            if(re & (POLLERR | POLLHUP | POLLNVAL)){ drop(i); continue; }
            if((re & POLLIN) && client_read(&g_cl[i])){ drop(i); continue; }
        }
        time_t now = time(NULL);
        for(int i = g_ncl - 1; i >= 0; i--){
            client_t *c = &g_cl[i];
            if(c->s.idle){ if(idle_answer(c, 0) < 0) drop(i); }
            else if(now - c->last > MSRV_IDLE_TIMEOUT) drop(i);
        }
        if(pf[0].revents & POLLIN){
            int fd = accept(g_fd, NULL, NULL);
            if(fd >= 0){
                if(g_ncl >= MPD_CLIENTS){ close(fd); continue; }
                int fl = fcntl(fd, F_GETFD); if(fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
                struct timeval tv = { 5, 0 };
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                client_t *c = &g_cl[g_ncl];
                memset(c, 0, sizeof *c);
                c->fd = fd; c->last = time(NULL);
                mpd_sess_init(&c->s, !g_has_pass);
                pthread_mutex_lock(&g_mu); memcpy(c->seen, g_ev, sizeof c->seen); pthread_mutex_unlock(&g_mu);
                if(send_all(fd, "OK MPD " MPD_PROTO_VERSION "\n", strlen("OK MPD " MPD_PROTO_VERSION "\n"))){ close(fd); continue; }
                g_ncl++;
            }
        }
    }
    while(g_ncl) drop(g_ncl - 1);
    return NULL;
}

int msrv_start(const char *bind_ip, int port, const char *password){
    if(g_live || !bind_ip) return -1;
    if(password && !rmt_token_valid(password)) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
    if(inet_pton(AF_INET, bind_ip, &a.sin_addr) != 1 || a.sin_addr.s_addr == htonl(INADDR_ANY)) return -1;   /* never every interface */
    if(!g_cv_ready){
        pthread_condattr_t ca; pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&g_cv, &ca); pthread_condattr_destroy(&ca);
        g_cv_ready = 1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if(fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if(bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 8)){ close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if(pipe(g_wake)){ close(fd); g_wake[0] = g_wake[1] = -1; return -1; }
    for(int i = 0; i < 2; i++){ fcntl(g_wake[i], F_SETFL, O_NONBLOCK); fcntl(g_wake[i], F_SETFD, FD_CLOEXEC); }
    socklen_t al = sizeof a;
    g_port = getsockname(fd, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : port;
    g_has_pass = password != NULL;
    snprintf(g_pass, sizeof g_pass, "%s", password ? password : "");
    pthread_mutex_lock(&g_mu);
    g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
    pthread_mutex_unlock(&g_mu);
    g_ncl = 0;
    g_fd = fd; g_run = 1;
    if(pthread_create(&g_th, NULL, worker, NULL)){
        close(fd); g_fd = -1; g_run = 0;
        close(g_wake[0]); close(g_wake[1]); g_wake[0] = g_wake[1] = -1;
        return -1;
    }
    g_live = 1;
    return 0;
}
void msrv_stop(void){
    if(!g_live) return;
    g_run = 0;
    wake_thread();
    pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    pthread_join(g_th, NULL);
    close(g_fd); g_fd = -1; g_live = 0;
    close(g_wake[0]); close(g_wake[1]); g_wake[0] = g_wake[1] = -1;
    pthread_mutex_lock(&g_mu);
    if(g_box_full && !g_box_taken){ mpd_sess_free(&g_box_sess); g_box_full = 0; }
    else if(g_box_full) g_box_orphan = 1;               /* a running job frees itself */
    free(g_box_out.p); memset(&g_box_out, 0, sizeof g_box_out);
    g_box_done = 0;
    pthread_mutex_unlock(&g_mu);
    memset(g_pass, 0, sizeof g_pass);
}
int msrv_running(void){ return g_live; }
int msrv_port(void){ return g_live ? g_port : 0; }
