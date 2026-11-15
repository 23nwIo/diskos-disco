/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "proto.h"
#include <stdio.h>
#include <string.h>

static int hexval(char c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static long hex4(const char *p){
    long v = 0;
    for(int i = 0; i < 4; i++){ int h = hexval(p[i]); if(h < 0) return -1; v = v * 16 + h; }
    return v;
}

int pp_parse(const char *buf, size_t n, pp_frame_t *f){
    if(!buf || n < 8) return -1;
    long tag = hex4(buf), len = hex4(buf + 4);
    if(tag < 0 || len < 8 || (size_t)len > n) return -1;
    f->tag = (unsigned)tag;
    f->data = buf + 8;
    f->dlen = (int)len - 8;
    return 0;
}

int pp_class1(const pp_frame_t *f){
    if(f->dlen != 4) return -1;
    long v = hex4(f->data);
    return v < 0 ? -1 : (int)v;
}

static int fit(int r, size_t cap){ return (r < 0 || (size_t)r >= cap) ? -1 : r; }

int pp_fmt_a1(char *out, size_t cap, unsigned long ms){
    return fit(snprintf(out, cap, "a1030010%08lx", ms & 0xffffffffUL), cap);
}
int pp_fmt_a714(char *out, size_t cap, int volume){
    if(volume < 0) volume = 0;
    if(volume > 0xff) volume = 0xff;
    return fit(snprintf(out, cap, "a714000C00%02x", volume), cap);
}
int pp_fmt_a607(char *out, size_t cap, int mode){
    return fit(snprintf(out, cap, "a607000C%04x", mode & 0xffff), cap);
}
int pp_fmt_a2(char *out, size_t cap, const char *json){
    size_t jl = strlen(json);
    if(jl + 8 > 0xffff || jl + 8 >= cap) return -1;
    /* "a202" as ui/mqfeed.c sends it; the stock player's exact tag is checked against the protocol capture */
    return fit(snprintf(out, cap, "a202%04x%s", (unsigned)(jl + 8), json), cap);
}

int pp_json_escape(char *out, size_t cap, size_t len, const char *s){
    size_t o = len;
    for(const unsigned char *p = (const unsigned char *)s; *p; p++){
        char tmp[8]; size_t k;
        if(*p == '"' || *p == '\\'){ tmp[0] = '\\'; tmp[1] = (char)*p; k = 2; }
        else if(*p < 0x20){ k = (size_t)snprintf(tmp, sizeof tmp, "\\u%04x", *p); }
        else { tmp[0] = (char)*p; k = 1; }
        if(o + k >= cap){ if(len < cap) out[len] = 0; return -1; }
        memcpy(out + o, tmp, k); o += k;
    }
    out[o] = 0;
    return (int)o;
}
