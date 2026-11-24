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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <limits.h>
#include "sdio.h"
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
static atomic_long g_queue_at;                 /* CLOCK_MONOTONIC s | 1 of the last api/state?q=1 (0 never) */
static char g_party[RMT_PARTY_NAME_MAX + 1];             /* under g_mu */
void rsrv_set_party(const char *name){
    pthread_mutex_lock(&g_mu);
    if(strcmp(g_party, name ? name : "")) snprintf(g_party, sizeof g_party, "%s", name ? name : "");
    pthread_mutex_unlock(&g_mu);
}
static int g_party_game;                                   /* under g_mu */
void rsrv_set_party_game(int on){ pthread_mutex_lock(&g_mu); g_party_game = !!on; pthread_mutex_unlock(&g_mu); }
static char g_up_dir[PATH_MAX];                           /* under g_mu; "" = uploads off */
static long long g_up_max;
static atomic_int g_uploading;                             /* 1 while an upload thread runs */
void rsrv_set_upload(const char *dir, long long max_bytes){
    pthread_mutex_lock(&g_mu);
    snprintf(g_up_dir, sizeof g_up_dir, "%s", dir ? dir : ""); g_up_max = max_bytes;
    pthread_mutex_unlock(&g_mu);
}
int rsrv_queue_watched(void){
    long at = atomic_load(&g_queue_at); if(!at) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec - at <= 5;
}
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

/* ---------------------------------------------------------------- a guest's upload (its own thread) */
#define UP_SPARE (64LL * 1024 * 1024)                      /* what the card keeps free after an upload */
#define UP_STALL_S 20                                      /* no data for this long: the upload is given up */
typedef struct { int c; rmt_cmd_t cmd; long long len, max; char dir[PATH_MAX]; size_t have; char first[REMOTE_REQ_MAX]; } upload_t;
static int mkdir_ok(const char *d){ return mkdir(d, 0755) == 0 || errno == EEXIST ? 0 : -1; }
static void *upload_thread(void *arg){
    upload_t *u = arg; int c = u->c;
    char name[160], path[PATH_MAX], tmp[PATH_MAX]; int fd = -1, lease = 0;
    const char *st = "400 Bad Request", *code = "param", *msg = NULL;
    if(u->len > u->max){ st = "413 Payload Too Large"; code = "size"; msg = "That file is larger than the host allows"; goto out; }
    if(rmt_upload_name(u->cmd.gname, u->cmd.name, name, sizeof name)){ st = "415 Unsupported Media Type"; code = "type";
        msg = "Only music files: " RMT_UPLOAD_EXTS; goto out; }
    if(!(lease = sd_io_begin())){ st = "503 Service Unavailable"; code = "card"; msg = "The SD card is not available right now"; goto out; }
    { char up[PATH_MAX]; const char *sl = strrchr(u->dir, '/');
      snprintf(up, sizeof up, "%.*s", sl ? (int)(sl - u->dir) : 0, u->dir);
      if((up[0] && mkdir_ok(up)) || mkdir_ok(u->dir)){ st = "500 Internal Server Error"; code = "card"; msg = "Couldn't make the upload folder on the SD card"; goto out; } }
    struct statvfs vs;
    if(statvfs(u->dir, &vs) == 0 && (long long)vs.f_bavail * (long long)vs.f_frsize < u->len + UP_SPARE){
        st = "507 Insufficient Storage"; code = "full"; msg = "Not enough room on the SD card"; goto out; }
    const char *dot = strrchr(name, '.');                  /* never overwrite: name-2.ext, name-3.ext, ... */
    int k;
    for(k = 1; k < 100; k++){
        int w = k == 1 ? snprintf(path, sizeof path, "%s/%s", u->dir, name)
                       : snprintf(path, sizeof path, "%s/%.*s-%d%s", u->dir, (int)(dot - name), name, k, dot);
        if(w < 0 || w >= (int)sizeof path){ k = 100; break; }
        struct stat sb; if(lstat(path, &sb) && errno == ENOENT) break;
    }
    if(k == 100){ st = "409 Conflict"; code = "exists"; msg = "That file is already on the card"; goto out; }
    if(snprintf(tmp, sizeof tmp, "%s/.upload.part", u->dir) >= (int)sizeof tmp){ st = "500 Internal Server Error"; code = "card"; msg = "Couldn't write to the SD card"; goto out; }
    if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0){ st = "500 Internal Server Error"; code = "card"; msg = "Couldn't write to the SD card"; goto out; }
    long long got = 0; int stall = 0, werr = 0;
    size_t first = u->have > (size_t)u->len ? (size_t)u->len : u->have;
    if(first && write(fd, u->first, first) != (ssize_t)first) werr = 1;
    got = (long long)first;
    struct timeval tv = { 1, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static char chunk[64 * 1024];
    while(!werr && got < u->len && g_run && stall < UP_STALL_S){
        size_t want = u->len - got < (long long)sizeof chunk ? (size_t)(u->len - got) : sizeof chunk;
        ssize_t r = recv(c, chunk, want, 0);
        if(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)){ stall++; continue; }
        if(r <= 0) break;
        stall = 0;
        for(ssize_t o = 0; o < r; ){ ssize_t w = write(fd, chunk + o, (size_t)(r - o)); if(w <= 0){ werr = 1; break; } o += w; }
        got += r;
    }
    if(!werr && fsync(fd)) werr = 1;
    if(close(fd)) werr = 1;
    fd = -1;
    if(werr || got < u->len || rename(tmp, path)){
        unlink(tmp);
        st = werr ? "500 Internal Server Error" : "400 Bad Request"; code = werr ? "card" : "short";
        msg = werr ? "Couldn't write to the SD card" : "The upload broke off - try again";
        goto out;
    }
    sd_io_end(); lease = 0;
    /* on the card: the main thread puts it into the library and the queue. The path relative to the card goes along. */
    { const char *rel = strstr(path, "/" RMT_UPLOAD_DIR "/");
      if(snprintf(u->cmd.name, sizeof u->cmd.name, "%s", rel ? rel + 1 : path) >= (int)sizeof u->cmd.name) u->cmd.name[0] = 0; }
    struct timeval sv = { 2, 0 }; setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &sv, sizeof sv);
    for(int i = 0; i < 100 && g_run; i++){                 /* the mailbox holds one command: wait for a page's to finish */
        pthread_mutex_lock(&g_mu); int busy = g_box_full; pthread_mutex_unlock(&g_mu);
        if(!busy) break;
        usleep(100 * 1000);
    }
    run_cmd(c, u->cmd);
    msg = NULL; st = NULL;
out:
    if(fd >= 0){ close(fd); unlink(tmp); }
    if(lease) sd_io_end();
    if(st){
        if(msg) fprintf(stderr, "remote: upload refused (%s): %s\n", code, msg);
        reply_json_err(c, st, code, msg ? msg : "Missing or invalid value");
    }
    shutdown(c, SHUT_WR); close(c);
    free(u);
    atomic_store(&g_uploading, 0);
    return NULL;
}
/* 1: c went to an upload thread (it closes c), 0: not an upload, -1: answered here (refused) */
static int start_upload(int c, const char *buf, size_t n){
    rmt_cmd_t cmd; long long len = 0; size_t hl = 0;
    char party[sizeof g_party], dir[PATH_MAX]; long long max;
    pthread_mutex_lock(&g_mu); memcpy(party, g_party, sizeof party); memcpy(dir, g_up_dir, sizeof dir); max = g_up_max; pthread_mutex_unlock(&g_mu);
    if(!party[0]) return 0;
    int r = rmt_upload_head(buf, n, party, &cmd, &len, &hl);
    if(r == 0) return 0;
    if(r < 0){ reply_json_err(c, "400 Bad Request", "param", "Missing or invalid value"); return -1; }
    if(!dir[0]){ reply_json_err(c, "403 Forbidden", "off", "The host doesn't take uploads at this party"); return -1; }
    int idle = 0;
    if(!atomic_compare_exchange_strong(&g_uploading, &idle, 1)){
        reply_json_err(c, "503 Service Unavailable", "busy", "Someone else is uploading - try again in a moment"); return -1; }
    upload_t *u = calloc(1, sizeof *u);
    if(!u){ atomic_store(&g_uploading, 0); reply_json_err(c, "500 Internal Server Error", "internal", "Out of memory"); return -1; }
    u->c = c; u->cmd = cmd; u->len = len; u->max = max;
    snprintf(u->dir, sizeof u->dir, "%s", dir);
    u->have = n - hl; memcpy(u->first, buf + hl, u->have);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    int e = pthread_create(&th, &at, upload_thread, u);
    pthread_attr_destroy(&at);
    if(e){ free(u); atomic_store(&g_uploading, 0); reply_json_err(c, "503 Service Unavailable", "busy", "Couldn't start the upload"); return -1; }
    return 1;
}

static int handle(int c){
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
        if(n > 5 && !strncmp(buf, "POST ", 5) && strstr(buf, "\r\n\r\n")){      /* a guest's upload: its body goes straight to the card */
            int u = start_upload(c, buf, (size_t)n);
            if(u) return u > 0;
        }
        if((st = rmt_req_complete(buf, n)) != 0) break;
    }
    if(st != 1){ if(st < 0) reply(c, "400 Bad Request", "text/plain", "bad request", NULL); return 0; }
    rmt_req_t rq;
    if(rmt_req_parse(buf, &rq)){ reply(c, "400 Bad Request", "text/plain", "bad request", NULL); return 0; }
    rmt_route_t rt = rmt_route(&rq, g_token);
    char party[sizeof g_party];
    if(rt.kind == RMT_R_NOT_FOUND){                       /* not the host's link: the guests' one, while the party is on */
        pthread_mutex_lock(&g_mu); memcpy(party, g_party, sizeof party); pthread_mutex_unlock(&g_mu);
        if(party[0]) rt = rmt_route_party(&rq, party);
    }
    switch(rt.kind){
    case RMT_R_REDIRECT: {
        char loc[64]; snprintf(loc, sizeof loc, "Location: /%s/\r\n", rt.cmd.guest ? party : g_token);
        reply(c, "301 Moved Permanently", "text/plain", "", loc); break; }
    case RMT_R_PAGE: case RMT_R_SYS_PAGE: case RMT_R_PARTY_PAGE: case RMT_R_GAME_PAGE: {
        int game; pthread_mutex_lock(&g_mu); game = g_party_game; pthread_mutex_unlock(&g_mu);
        reply(c, "200 OK", "text/html; charset=utf-8", rt.kind == RMT_R_PAGE ? RMT_PAGE_HTML : rt.kind == RMT_R_SYS_PAGE ? RMT_SYS_HTML
              : rt.kind == RMT_R_GAME_PAGE ? RMT_HGADMIN_HTML : game ? RMT_HG_HTML : RMT_PARTY_HTML,
              "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' https://f4.bcbits.com; frame-ancestors 'none'\r\n");
        break; }
    case RMT_R_STATE: {
        rmt_state_t s; int have;
        if(rt.cmd.arg){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); atomic_store(&g_queue_at, (long)ts.tv_sec | 1); }   /* q=1 */
        if(g_notify) g_notify();                          /* a fresher snapshot for the next poll */
        pthread_mutex_lock(&g_mu); s = g_state; have = g_have_state; pthread_mutex_unlock(&g_mu);
        if(!have){ reply_json_err(c, "503 Service Unavailable", "starting", "Starting up"); break; }
        static char js[14000];   /* rmt_state_json's worst case (every string fully escaped) */
        if(rmt_state_json(js, sizeof js, &s) < 0){ reply_json_err(c, "500 Internal Server Error", "internal", "State too large"); break; }
        reply(c, "200 OK", "application/json", js, NULL);
        break; }
    case RMT_R_CMD: run_cmd(c, rt.cmd); break;
    case RMT_R_BAD_METHOD: reply_json_err(c, "405 Method Not Allowed", "method", "Wrong method"); break;
    case RMT_R_BAD_PARAM: reply_json_err(c, "400 Bad Request", "param", rt.cmd.kind == RMT_CMD_LOOK && rt.cmd.text[0] ? rt.cmd.text : "Missing or invalid value"); break;
    default: reply(c, "404 Not Found", "text/plain", "not found", NULL); break;
    }
    return 0;
}

static void *worker(void *arg){
    (void)arg;
    while(g_run){
        struct pollfd pf = { g_fd, POLLIN, 0 };
        if(poll(&pf, 1, 500) <= 0) continue;
        int c = accept(g_fd, NULL, NULL);
        if(c < 0) continue;
        if(handle(c)) continue;                           /* an upload thread has it now */
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
    for(int i = 0; i < 50 && atomic_load(&g_uploading); i++) usleep(100 * 1000);   /* it sees g_run within a second */
    close(g_fd); g_fd = -1; g_live = 0;
    pthread_mutex_lock(&g_mu);
    g_box_full = g_box_taken = g_box_done = g_box_orphan = 0;
    free(g_box_body); g_box_body = NULL;
    pthread_mutex_unlock(&g_mu);
    memset(g_token, 0, sizeof g_token);
}
int rsrv_running(void){ return g_live; }
int rsrv_port(void){ return g_live ? g_port : 0; }
