/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "mpdcli.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int read_line(int fd, char *b, size_t cap){
    size_t i = 0;
    for(;;){
        char c;
        ssize_t r = read(fd, &c, 1);
        if(r < 0 && errno == EINTR) continue;
        if(r == 0) errno = ECONNRESET;           /* closed; a timeout leaves EAGAIN (engine.c tells them apart) */
        if(r != 1) return -1;
        if(c == '\n') break;
        if(i + 1 < cap) b[i++] = c;
    }
    b[i] = 0;
    return (int)i;
}

int mc_connect(const char *path, int timeout_ms){
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if(strlen(path) >= sizeof a.sun_path){ close(fd); errno = ENAMETOOLONG; return -1; }
    strcpy(a.sun_path, path);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if(connect(fd, (struct sockaddr *)&a, sizeof a) != 0){ int e = errno; close(fd); errno = e; return -1; }
    char g[128];
    if(read_line(fd, g, sizeof g) < 0 || strncmp(g, "OK MPD", 6)){ close(fd); errno = EPROTO; return -1; }
    return fd;
}

static int send_all(int fd, const char *s, size_t n){
    while(n){
        ssize_t w = write(fd, s, n);
        if(w < 0 && errno == EINTR) continue;
        if(w <= 0) return -1;
        s += w; n -= (size_t)w;
    }
    return 0;
}

int mc_cmd(int fd, const char *cmd, char *out, size_t cap){
    if(cap) out[0] = 0;
    if(send_all(fd, cmd, strlen(cmd)) || send_all(fd, "\n", 1)) return -1;
    size_t len = 0;
    char line[1024];
    for(;;){
        if(read_line(fd, line, sizeof line) < 0) return -1;
        if(!strcmp(line, "OK")) return 0;
        int ack = !strncmp(line, "ACK ", 4);
        size_t l = strlen(line);
        if(cap && len + l + 2 <= cap){ memcpy(out + len, line, l); len += l; out[len++] = '\n'; out[len] = 0; }
        if(ack) return 1;
    }
}

int mc_idle_begin(int fd){
    static const char c[] = "idle player options mixer\n";
    return send_all(fd, c, sizeof c - 1);
}

int mc_idle_end(int fd){
    int ev = 0;
    char line[256];
    for(;;){
        if(read_line(fd, line, sizeof line) < 0) return -1;
        if(!strcmp(line, "OK")) return ev;
        if(!strncmp(line, "ACK ", 4)) return -1;
        if(!strncmp(line, "changed: ", 9)){
            const char *w = line + 9;
            ev |= !strcmp(w, "player") ? MC_EV_PLAYER : !strcmp(w, "options") ? MC_EV_OPTIONS
                : !strcmp(w, "mixer") ? MC_EV_MIXER : MC_EV_OTHER;
        }
    }
}

int mc_get(const char *resp, const char *key, char *val, size_t cap){
    size_t kl = strlen(key);
    for(const char *p = resp; p && *p; ){
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if(ll > kl + 1 && !strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' '){
            size_t vl = ll - kl - 2;
            if(cap){ if(vl >= cap) vl = cap - 1; memcpy(val, p + kl + 2, vl); val[vl] = 0; }
            return 1;
        }
        p = e ? e + 1 : NULL;
    }
    return 0;
}

int mc_quote(char *out, size_t cap, const char *arg){
    size_t n = 0;
    if(cap < 3) return -1;
    out[n++] = '"';
    for(; *arg; arg++){
        if(*arg == '"' || *arg == '\\'){ if(n + 1 >= cap) return -1; out[n++] = '\\'; }
        if(n + 1 >= cap) return -1;
        out[n++] = *arg;
    }
    if(n + 2 > cap) return -1;
    out[n++] = '"'; out[n] = 0;
    return (int)n;
}
