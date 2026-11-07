/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: the HTTP server thread. See remote_srv.h. */
#include "remote_srv.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_t g_th;
static int g_live, g_fd = -1, g_port;
static volatile int g_run;
static char g_token[REMOTE_TOKEN_LEN + 1];

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;                 /* CLOCK_MONOTONIC, set up in rsrv_start */
static int g_cv_ready;
static rmt_state_t g_state;                   /* guarded by g_mu */
static int g_have_state;
/* one-slot mailbox, guarded by g_mu */
static int g_box_full, g_box_taken, g_box_done, g_box_orphan, g_box_ok;
static rmt_cmd_t g_box_cmd;
static char g_box_msg[96];
static char *g_box_body;                     /* a JSON answer from the main thread (malloc'd; the server frees it) */
static size_t g_box_len;                     /* its length when binary (0 = a NUL-terminated JSON string) */
static const char *g_box_ctype;              /* its type when binary */
static void (*g_notify)(void);
void rsrv_set_notify(void (*cb)(void)){ g_notify = cb; }

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- main-thread side */
void rsrv_publish(const rmt_state_t *s){
    pthread_mutex_lock(&g_mu);
    g_state = *s; g_have_state = 1;
    pthread_mutex_unlock(&g_mu);
}
int rsrv_take(rmt_cmd_t *out){
    int r = 0;
    pthread_mutex_lock(&g_mu);
    if(g_box_full && !g_box_taken){ *out = g_box_cmd; g_box_taken = 1; r = 1; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
static void done_any(int ok, const char *message, char *body, size_t len, const char *ctype){
    pthread_mutex_lock(&g_mu);
    if(g_box_full && g_box_taken){
        g_box_ok = ok; g_box_done = 1; g_box_len = len; g_box_ctype = ctype;
        snprintf(g_box_msg, sizeof g_box_msg, "%s", message ? message : "");
        free(g_box_body); g_box_body = body; body = NULL;
        if(g_box_orphan){                                  /* its request gave up: free the slot */
            g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
            free(g_box_body); g_box_body = NULL;
        }
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_mu);
    free(body);                                            /* nobody waiting for it */
}
void rsrv_done_body(int ok, const char *message, char *body){ done_any(ok, message, body, 0, NULL); }
void rsrv_done_blob(char *body, size_t len, const char *ctype){ done_any(1, NULL, body, len, ctype); }
void rsrv_done(int ok, const char *message){ rsrv_done_body(ok, message, NULL); }

/* ---------------------------------------------------------------- HTTP */
static void send_all(int c, const char *p, size_t n){
    while(n){ ssize_t w = send(c, p, n, MSG_NOSIGNAL); if(w <= 0) return; p += w; n -= (size_t)w; }
}
static void reply_n(int c, const char *status, const char *ctype, const char *body, size_t bl, const char *extra_hdr){
    char hdr[512];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
        "%sConnection: close\r\n\r\n", status, ctype, bl, extra_hdr ? extra_hdr : "");
    if(h < 0 || h >= (int)sizeof hdr) return;
    send_all(c, hdr, (size_t)h); send_all(c, body, bl);
}
static void reply(int c, const char *status, const char *ctype, const char *body, const char *extra_hdr){
    reply_n(c, status, ctype, body, strlen(body), extra_hdr);
}
static void reply_json_err(int c, const char *status, const char *code, const char *message){
    char b[256], m[160];
    rmt_json_escape(m, sizeof m, message);
    snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\",\"message\":\"%s\"}", code, m);
    reply(c, status, "application/json", b, NULL);
}

static void run_cmd(int c, rmt_cmd_t cmd){
    pthread_mutex_lock(&g_mu);
    if(g_box_full){ pthread_mutex_unlock(&g_mu); reply_json_err(c, "503 Service Unavailable", "busy", "Another command is still running"); return; }
    g_box_cmd = cmd; g_box_full = 1; g_box_taken = g_box_done = g_box_orphan = 0; g_box_msg[0] = 0;
    if(g_notify) g_notify();                              /* wake a resting main loop (a pipe write: safe under the lock) */
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += RSRV_CMD_WAIT_MS / 1000; dl.tv_nsec += (long)(RSRV_CMD_WAIT_MS % 1000) * 1000000L;
    if(dl.tv_nsec >= 1000000000L){ dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    while(!g_box_done && g_run){
        if(pthread_cond_timedwait(&g_cv, &g_mu, &dl) == ETIMEDOUT) break;
    }
    if(!g_box_done){
        if(g_box_taken) g_box_orphan = 1;                 /* the main thread runs it; its rsrv_done frees the slot */
        else g_box_full = 0;                              /* never picked up: withdraw it, it must not run late */
        pthread_mutex_unlock(&g_mu);
        reply_json_err(c, "504 Gateway Timeout", "timeout", "The player did not answer in time");
        return;
    }
    int ok = g_box_ok; char msg[96]; snprintf(msg, sizeof msg, "%s", g_box_msg);
    char *body = g_box_body; g_box_body = NULL;
    size_t blen = g_box_len; const char *bct = g_box_ctype;
    g_box_full = g_box_taken = g_box_done = 0;
    pthread_mutex_unlock(&g_mu);
    if(ok && body && blen) reply_n(c, "200 OK", bct ? bct : "application/octet-stream", body, blen, NULL);
    else if(ok) reply(c, "200 OK", "application/json", body ? body : "{\"ok\":true}", NULL);
    else reply_json_err(c, "409 Conflict", "refused", msg[0] ? msg : "Not possible right now");
    free(body);
}

static void handle(int c){
    struct timeval tv = { 2, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    static char buf[REMOTE_REQ_MAX];                     /* one client at a time: the server thread's own buffer */
    int n = 0, st = 0; long long deadline = now_ms() + 6000;
    buf[0] = 0;
    while(g_run && now_ms() < deadline){
        int r = (int)recv(c, buf + n, sizeof buf - 1 - (size_t)n, 0);
        if(r <= 0) break;
        n += r; buf[n] = 0;
        if((st = rmt_req_complete(buf, n)) != 0) break;
    }
    if(st != 1){ if(st < 0) reply(c, "400 Bad Request", "text/plain", "bad request", NULL); return; }
    rmt_req_t rq;
    if(rmt_req_parse(buf, &rq)){ reply(c, "400 Bad Request", "text/plain", "bad request", NULL); return; }
    rmt_route_t rt = rmt_route(&rq, g_token);
    switch(rt.kind){
    case RMT_R_REDIRECT: {
        char loc[64]; snprintf(loc, sizeof loc, "Location: /%s/\r\n", g_token);
        reply(c, "301 Moved Permanently", "text/plain", "", loc); break; }
    case RMT_R_PAGE:
        reply(c, "200 OK", "text/html; charset=utf-8", RMT_PAGE_HTML,
              "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self'; frame-ancestors 'none'\r\n");
        break;
    case RMT_R_STATE: {
        rmt_state_t s; int have;
        if(g_notify) g_notify();                          /* a fresher snapshot for the next poll */
        pthread_mutex_lock(&g_mu); s = g_state; have = g_have_state; pthread_mutex_unlock(&g_mu);
        if(!have){ reply_json_err(c, "503 Service Unavailable", "starting", "Starting up"); break; }
        static char js[14000];   /* rmt_state_json's worst case (every string fully escaped) */
        if(rmt_state_json(js, sizeof js, &s) < 0){ reply_json_err(c, "500 Internal Server Error", "internal", "State too large"); break; }
        reply(c, "200 OK", "application/json", js, NULL);
        break; }
    case RMT_R_CMD: run_cmd(c, rt.cmd); break;
    case RMT_R_BAD_METHOD: reply_json_err(c, "405 Method Not Allowed", "method", "Wrong method"); break;
    case RMT_R_BAD_PARAM: reply_json_err(c, "400 Bad Request", "param", "Missing or invalid value"); break;
    default: reply(c, "404 Not Found", "text/plain", "not found", NULL); break;
    }
}

static void *worker(void *arg){
    (void)arg;
    while(g_run){
        struct pollfd pf = { g_fd, POLLIN, 0 };
        if(poll(&pf, 1, 500) <= 0) continue;
        int c = accept(g_fd, NULL, NULL);
        if(c < 0) continue;
        handle(c);
        shutdown(c, SHUT_WR);
        close(c);
    }
    return NULL;
}

int rsrv_start(const char *bind_ip, int port, const char *token){
    if(g_live) return -1;
    if(!rmt_token_valid(token) || !bind_ip) return -1;
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
    if(bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)){ close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);                       /* accept() never blocks after poll() */
    socklen_t al = sizeof a;
    g_port = getsockname(fd, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : port;
    snprintf(g_token, sizeof g_token, "%s", token);
    pthread_mutex_lock(&g_mu);
    g_have_state = 0; g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
    pthread_mutex_unlock(&g_mu);
    g_fd = fd; g_run = 1;
    if(pthread_create(&g_th, NULL, worker, NULL)){ close(fd); g_fd = -1; g_run = 0; return -1; }
    g_live = 1;
    return 0;
}
void rsrv_stop(void){
    if(!g_live) return;
    g_run = 0;
    pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);   /* a waiting command gives up */
    pthread_join(g_th, NULL);
    close(g_fd); g_fd = -1; g_live = 0;
    pthread_mutex_lock(&g_mu);
    g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
    free(g_box_body); g_box_body = NULL;
    pthread_mutex_unlock(&g_mu);
    memset(g_token, 0, sizeof g_token);
}
int rsrv_running(void){ return g_live; }
int rsrv_port(void){ return g_live ? g_port : 0; }
