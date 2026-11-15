/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "btout.h"
#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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

unsigned bto_rate(const char *mac, bto_log_fn log){
    char path[96], cmd[160], out[2048];
    if(bto_pcm_path(mac, path, sizeof path) != 0) return 0;
    snprintf(cmd, sizeof cmd, "bluealsa-cli info %s 2>&1", path);
    int x = run(cmd, out, sizeof out);
    unsigned r = x == 0 ? bto_parse_rate(out) : 0;
    if(!r){ first_line(out); log("bt: no link rate for %s (%s: %d, %s)", mac, cmd, x, out); }
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

void bto_codec(const char *mac, int value, bto_log_fn log){
    char path[96], cmd[256];
    if(bto_pcm_path(mac, path, sizeof path) != 0) return;
    const char *name = value == 1 ? "AAC" : (value >= 2 && value <= 4) ? "LDAC" : "SBC";
    snprintf(cmd, sizeof cmd, "(echo \"$(date) codec %s\"; bluealsa-cli codec %s %s) >> %s/bt.log 2>&1 &",
             name, path, name, ENG_RUN_DIR);
    int st = system(cmd);
    log("bt: codec %s for %s asked%s", name, mac, st == 0 ? "" : " (could not start bluealsa-cli)");
}
