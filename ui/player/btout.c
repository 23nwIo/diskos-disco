/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "btout.h"
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int bto_pcm_path(const char *mac, char *out, size_t cap){
    if(!mac || strlen(mac) != 17) return -1;
    char d[18];
    for(int i = 0; i < 17; i++){
        char ch = mac[i];
        if(i % 3 == 2){ if(ch != ':') return -1; d[i] = '_'; continue; }
        if(!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))) return -1;
        d[i] = (ch >= 'a' && ch <= 'f') ? (char)(ch - 32) : ch;
    }
    d[17] = 0;
    int n = snprintf(out, cap, "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", d);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

unsigned bto_parse_rate(const char *info){
    static const char *const keys[] = { "Sampling:", "Rate:" };
    for(const char *l = info; l && *l; ){
        while(*l == ' ' || *l == '\t') l++;
        for(unsigned k = 0; k < sizeof keys / sizeof *keys; k++){
            size_t n = strlen(keys[k]);
            if(!strncmp(l, keys[k], n)){
                unsigned long r = strtoul(l + n, NULL, 10);
                if(r >= 8000 && r <= 384000) return (unsigned)r;
            }
        }
        l = strchr(l, '\n');
        if(l) l++;
    }
    return 0;
}

/* run a command, its output (stdout and stderr) into out; the exit status, -1 if it could not run */
static int run(const char *cmd, char *out, size_t cap){
    FILE *p = popen(cmd, "r");
    if(!p) return -1;
    size_t n = 0;
    if(cap) out[0] = 0;
    while(cap && n + 1 < cap){
        size_t r = fread(out + n, 1, cap - 1 - n, p);
        if(r == 0) break;
        n += r;
    }
    if(cap) out[n] = 0;
    char sink[256]; while(fread(sink, 1, sizeof sink, p) > 0){}
    int st = pclose(p);
    return st < 0 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void first_line(char *s){ s[strcspn(s, "\n")] = 0; }

static void upper_word(const char *p, char *out, size_t cap){
    size_t n = 0;
    while(*p == ' ' || *p == '\t') p++;
    while(*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n + 1 < cap){
        char ch = *p++;
        out[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    if(cap) out[n] = 0;
}

void bto_parse_selected(const char *info, char *out, size_t cap){
    static const char *const keys[] = { "Selected codec:", "Codec:" };
    if(cap) out[0] = 0;
    for(unsigned k = 0; info && k < sizeof keys / sizeof *keys; k++){
        for(const char *l = info; *l; ){
            while(*l == ' ' || *l == '\t') l++;
            if(!strncmp(l, keys[k], strlen(keys[k]))){ upper_word(l + strlen(keys[k]), out, cap); return; }
            l = strchr(l, '\n');
            if(!l) break;
            l++;
        }
    }
}

void bto_parse_avail(const char *info, char *out, size_t cap){
    if(cap) out[0] = 0;
    const char *h = info ? strstr(info, "Available codecs:") : NULL;
    if(!h || !cap) return;
    size_t n = 0;
    const char *p = h + 17;
    for(int first = 1; *p; first = 0){
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if(!first && memchr(p, ':', len)) break;
        for(size_t i = 0; i < len;){
            size_t j = i;
            while(j < len && p[j] != ' ' && p[j] != '\t' && p[j] != '\r' && p[j] != ',') j++;
            if(j > i && n + (j - i) + 2 < cap){
                if(n) out[n++] = ' ';
                for(size_t k = i; k < j; k++) out[n++] = (p[k] >= 'a' && p[k] <= 'z') ? (char)(p[k] - 32) : p[k];
                out[n] = 0;
            }
            i = j + 1;
        }
        if(!e) break;
        p = e + 1;
    }
}

int bto_info(const char *mac, unsigned *rate, char *sel, size_t selcap, char *avail, size_t availcap, bto_log_fn log){
    char path[96], cmd[160], out[2048];
    if(rate) *rate = 0;
    if(sel && selcap) sel[0] = 0;
    if(avail && availcap) avail[0] = 0;
    if(bto_pcm_path(mac, path, sizeof path) != 0) return -1;
    snprintf(cmd, sizeof cmd, "bluealsa-cli info %s 2>&1", path);
    int x = run(cmd, out, sizeof out);
    if(x != 0){ first_line(out); log("bt: no link info for %s (%s: %d, %s)", mac, cmd, x, out); return -1; }
    if(rate) *rate = bto_parse_rate(out);
    if(sel) bto_parse_selected(out, sel, selcap);
    if(avail) bto_parse_avail(out, avail, availcap);
    return 0;
}

unsigned bto_rate(const char *mac, bto_log_fn log){
    unsigned r = 0;
    if(bto_info(mac, &r, NULL, 0, NULL, 0, log) == 0 && !r) log("bt: no link rate for %s", mac);
    return r;
}

int bto_volume(const char *mac, int volume, int max, bto_log_fn log){
    char path[96], cmd[176], out[512];
    if(bto_pcm_path(mac, path, sizeof path) != 0 || max <= 0) return -1;
    if(volume < 0) volume = 0;
    if(volume > max) volume = max;
    int v = (volume * 127 + max / 2) / max;
    snprintf(cmd, sizeof cmd, "bluealsa-cli volume %s %d 2>&1", path, v);
    int x = run(cmd, out, sizeof out);
    if(x != 0){ first_line(out); log("bt: %s: %d, %s", cmd, x, out); return -1; }
    return 0;
}

const char *bto_codec_name(int v){
    return v == 1 ? "AAC" : (v >= 2 && v <= 4) ? "LDAC" : v == 5 ? "aptX" : v == 6 ? "aptX-HD" : "SBC";
}

int bto_codec_start(const char *mac, int value, bto_log_fn log){
    char path[96], cmd[320];
    if(bto_pcm_path(mac, path, sizeof path) != 0) return -1;
    snprintf(cmd, sizeof cmd, "echo \"$(date) codec %s (by hand)\"; exec bluealsa-cli codec %s %s",
             bto_codec_name(value), path, bto_codec_name(value));
    pid_t p = fork();
    if(p < 0){ log("bt: codec %s: fork: %s", bto_codec_name(value), strerror(errno)); return -1; }
    if(p == 0){
        int fd = open(ENG_RUN_DIR "/bt.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if(fd >= 0){ dup2(fd, 1); dup2(fd, 2); if(fd > 2) close(fd); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    log("bt: codec %s for %s asked (pid %d)", bto_codec_name(value), mac, (int)p);
    return (int)p;
}

void bto_codec(const char *mac, int value, bto_log_fn log){
    char path[96], cmd[256];
    if(bto_pcm_path(mac, path, sizeof path) != 0) return;
    const char *name = value == 1 ? "AAC" : (value >= 2 && value <= 4) ? "LDAC" : "SBC";
    snprintf(cmd, sizeof cmd, "(echo \"$(date) codec %s\"; bluealsa-cli codec %s %s) >> %s/bt.log 2>&1 &",
             name, path, name, ENG_RUN_DIR);
    int st = system(cmd);
    log("bt: codec %s for %s asked%s", name, mac, st == 0 ? "" : " (could not start bluealsa-cli)");
}
