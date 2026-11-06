/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: request parsing, routing, token and JSON helpers, and the control page. See remote_core.h. */
#include "remote_core.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char B32[] = "abcdefghijklmnopqrstuvwxyz234567";

void rmt_token_encode(const unsigned char *rnd, size_t n, char *out){
    /* 5 bits per character from a bit stream over rnd; REMOTE_TOKEN_LEN * 5 = 100 bits needs 13 bytes */
    size_t bit = 0;
    for(int i = 0; i < REMOTE_TOKEN_LEN; i++){
        unsigned v = 0;
        for(int b = 0; b < 5; b++, bit++){
            size_t byte = bit / 8;
            unsigned x = byte < n ? rnd[byte] : 0;
            v = (v << 1) | ((x >> (7 - bit % 8)) & 1u);
        }
        out[i] = B32[v & 31];
    }
    out[REMOTE_TOKEN_LEN] = 0;
}
int rmt_token_valid(const char *s){
    if(!s || strlen(s) != REMOTE_TOKEN_LEN) return 0;
    for(const char *p = s; *p; p++) if(!strchr(B32, *p)) return 0;
    return 1;
}
int rmt_token_eq(const char *seg, size_t n, const char *token){
    if(!rmt_token_valid(token)) return 0;                      /* never accept an empty or unset token */
    unsigned diff = (unsigned)(n ^ REMOTE_TOKEN_LEN);
    for(size_t i = 0; i < REMOTE_TOKEN_LEN; i++){
        unsigned char c = i < n ? (unsigned char)seg[i] : 0;   /* same work whatever the length */
        diff |= c ^ (unsigned char)token[i];
    }
    return diff == 0;
}

/* ---------------------------------------------------------------- requests */
static long header_content_length(const char *hdr_start, const char *hdr_end){
    /* case-insensitive "\r\ncontent-length:" inside [hdr_start, hdr_end) */
    static const char key[] = "\r\ncontent-length:";
    size_t kl = sizeof key - 1;
    for(const char *p = hdr_start; p + kl <= hdr_end; p++){
        if(strncasecmp(p, key, kl)) continue;
        const char *v = p + kl;
        while(v < hdr_end && (*v == ' ' || *v == '\t')) v++;
        if(v >= hdr_end || !isdigit((unsigned char)*v)) return -2;
        long len = 0;
        while(v < hdr_end && isdigit((unsigned char)*v)){
            if(len > REMOTE_REQ_MAX) return -2;
            len = len * 10 + (*v++ - '0');
        }
        return len;
    }
    return -1;
}
int rmt_req_complete(const char *buf, int n){
    if(n <= 0) return 0;
    const char *he = strstr(buf, "\r\n\r\n");
    if(!he) return n >= REMOTE_REQ_MAX - 1 ? -1 : 0;
    long len = header_content_length(buf, he + 2);
    if(len == -2) return -1;
    if(len < 0) return 1;
    long hlen = (long)(he - buf) + 4;
    if(hlen + len > REMOTE_REQ_MAX - 1) return -1;
    return n - hlen >= len ? 1 : 0;
}
int rmt_req_parse(char *buf, rmt_req_t *r){
    memset(r, 0, sizeof *r);
    char *he = strstr(buf, "\r\n\r\n");
    if(!he) return -1;
    r->content_length = header_content_length(buf, he + 2);
    if(r->content_length == -2) return -1;
    char *body = he + 4;
    *he = 0;
    char *eol = strstr(buf, "\r\n"); if(eol) *eol = 0;          /* request line only */
    char *sp1 = strchr(buf, ' '); if(!sp1) return -1;
    *sp1 = 0;
    char *path = sp1 + 1;
    char *sp2 = strchr(path, ' '); if(!sp2) return -1;
    *sp2 = 0;
    if(strncmp(sp2 + 1, "HTTP/1.", 7)) return -1;
    if(path[0] != '/') return -1;
    char *q = strchr(path, '?');
    if(q){ *q = 0; r->query = q + 1; } else r->query = "";
    r->method = buf; r->path = path;
    if(r->content_length > 0){
        body[r->content_length] = 0;                            /* caller's buffer holds hlen + len (rmt_req_complete) */
        r->body = body;
    } else r->body = "";
    return 0;
}

/* name=value in an urlencoded list; numbers only. 0 ok (*out set), -1 absent, -2 malformed */
static int param_long(const char *list, const char *name, long *out){
    size_t nl = strlen(name);
    const char *p = list;
    while(p && *p){
        if(!strncmp(p, name, nl) && p[nl] == '='){
            const char *v = p + nl + 1;
            char num[24]; size_t k = 0;
            if(!strncasecmp(v, "%2B", 3)){ v += 3; }                /* "+5" as typed: %2B */
            else if(*v == '+') v++;                                  /* "+5" unencoded ('+' would mean a space) */
            while(*v && *v != '&' && k < sizeof num - 1) num[k++] = *v++;
            num[k] = 0;
            if(!k || (*v && *v != '&')) return -2;
            char *end; errno = 0;
            long x = strtol(num, &end, 10);
            if(errno || *end || end == num) return -2;
            *out = x; return 0;
        }
        p = strchr(p, '&'); if(p) p++;
    }
    return -1;
}
static int param(const rmt_req_t *r, const char *name, long *out){
    int a = param_long(r->query, name, out);
    if(a != -1) return a;
    return param_long(r->body, name, out);
}
static int hexv(int c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* name=value, url-decoded ('+' = space, %XX). 0 ok, -1 absent, -2 malformed / too long / a control byte */
static int param_str_list(const char *list, const char *name, char *out, size_t cap){
    size_t nl = strlen(name);
    const char *p = list;
    while(p && *p){
        if(!strncmp(p, name, nl) && p[nl] == '='){
            const char *v = p + nl + 1; size_t k = 0;
            while(*v && *v != '&'){
                int c = (unsigned char)*v;
                if(c == '+'){ c = ' '; v++; }
                else if(c == '%'){
                    int h = hexv(v[1]), l = h < 0 ? -1 : hexv(v[2]);
                    if(h < 0 || l < 0) return -2;
                    c = h * 16 + l; v += 3;
                } else v++;
                if(c < 0x20 || c == 0x7f) return -2;                 /* never NUL or control bytes into a name or path */
                if(k + 1 >= cap) return -2;
                out[k++] = (char)c;
            }
            out[k] = 0;
            return 0;
        }
        p = strchr(p, '&'); if(p) p++;
    }
    return -1;
}
static int param_str(const rmt_req_t *r, const char *name, char *out, size_t cap){
    int a = param_str_list(r->query, name, out, cap);
    if(a != -1) return a;
    return param_str_list(r->body, name, out, cap);
}
int rmt_relpath_ok(const char *p){
    if(!p) return 0;
    if(!*p) return 1;
    if(*p == '/') return 0;
    const char *seg = p;
    for(;;){
        const char *e = strchr(seg, '/');
        size_t n = e ? (size_t)(e - seg) : strlen(seg);
        if(n == 0) return 0;                                         /* "a//b" or a trailing '/' */
        if((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
        for(size_t i = 0; i < n; i++) if((unsigned char)seg[i] < 0x20 || seg[i] == 0x7f) return 0;
        if(!e) return 1;
        seg = e + 1;
    }
}
int rmt_match(const char *hay, const char *needle){
    if(!needle || !*needle) return 1;
    return hay && strcasestr(hay, needle) != NULL;
}

/* ---------------------------------------------------------------- library routes */
static const struct { const char *name; rmt_list_t l; } LISTS[] = {
    { "albums", RMT_L_ALBUMS }, { "album", RMT_L_ALBUM }, { "artists", RMT_L_ARTISTS }, { "artist", RMT_L_ARTIST },
    { "playlists", RMT_L_PLAYLISTS }, { "playlist", RMT_L_PLAYLIST }, { "favorites", RMT_L_FAVORITES },
    { "songs", RMT_L_SONGS }, { "folder", RMT_L_FOLDER }, { "radio", RMT_L_RADIO },
};
static const struct { const char *name; rmt_item_t i; } ITEMS[] = {
    { "album", RMT_I_ALBUM }, { "artist", RMT_I_ARTIST }, { "playlist", RMT_I_PLAYLIST }, { "favorites", RMT_I_FAVORITES },
    { "song", RMT_I_SONG }, { "file", RMT_I_FILE }, { "all", RMT_I_ALL }, { "radio", RMT_I_RADIO },
};
/* optional number: absent = def; out of [lo, hi] or malformed = -1 */
static int opt_num(const rmt_req_t *r, const char *name, long lo, long hi, long def, long *out){
    long v; int pr = param(r, name, &v);
    if(pr == -1){ *out = def; return 0; }
    if(pr || v < lo || v > hi) return -1;
    *out = v; return 0;
}
static int route_list(const rmt_req_t *r, const char *which, rmt_cmd_t *c){
    int l = 0;
    for(size_t i = 0; i < sizeof LISTS / sizeof LISTS[0]; i++) if(!strcmp(which, LISTS[i].name)) l = LISTS[i].l;
    if(!l) return 0;
    c->kind = RMT_CMD_BROWSE; c->sub = l;
    if(opt_num(r, "offset", 0, 1000000, 0, &c->offset) || opt_num(r, "limit", 1, RMT_LIMIT_MAX, RMT_LIMIT_DEF, &c->limit)) return -1;
    int q = param_str(r, "q", c->q, sizeof c->q); if(q == -2) return -1;
    if(l == RMT_L_ALBUM || l == RMT_L_ARTIST){ if(param_str(r, "name", c->name, sizeof c->name) || !c->name[0]) return -1; }
    if(l == RMT_L_PLAYLIST){ if(param(r, "id", &c->id) || c->id <= 0) return -1; }
    if(l == RMT_L_FOLDER){
        int pr = param_str(r, "path", c->name, sizeof c->name);
        if(pr == -2 || !rmt_relpath_ok(pr == -1 ? "" : c->name)) return -1;
        if(pr == -1) c->name[0] = 0;
    }
    return 1;
}
static int route_item(const rmt_req_t *r, rmt_cmd_t *c){
    char kind[16];
    if(param_str(r, "kind", kind, sizeof kind)) return -1;
    int it = 0;
    for(size_t i = 0; i < sizeof ITEMS / sizeof ITEMS[0]; i++) if(!strcmp(kind, ITEMS[i].name)) it = ITEMS[i].i;
    if(!it) return -1;
    c->kind = RMT_CMD_PLAY_ITEM; c->sub = it;
    if(opt_num(r, "song_id", 1, 0x7fffffffL, 0, &c->song_id) || opt_num(r, "pos", 1, 1000000, 0, &c->pos)) return -1;
    switch(it){
    case RMT_I_ALBUM: case RMT_I_ARTIST:
        if(param_str(r, "name", c->name, sizeof c->name) || !c->name[0]) return -1;
        break;
    case RMT_I_PLAYLIST: if(param(r, "id", &c->id) || c->id <= 0) return -1; break;
    case RMT_I_SONG: if(c->song_id <= 0) return -1; break;
    case RMT_I_FILE:
        if(param_str(r, "path", c->name, sizeof c->name) || !c->name[0] || !rmt_relpath_ok(c->name)) return -1;
        break;
    case RMT_I_RADIO: {
        if(c->pos <= 0) return -1;
        int u = param_str(r, "url", c->name, sizeof c->name);
        if(u == -2) return -1;
        if(u == -1) c->name[0] = 0;
        break; }
    default: break;
    }
    return 1;
}

static const struct { const char *name; rmt_cmd_kind_t kind; } CMDS[] = {
    { "play_pause", RMT_CMD_PLAY_PAUSE }, { "play", RMT_CMD_PLAY }, { "pause", RMT_CMD_PAUSE },
    { "next", RMT_CMD_NEXT }, { "previous", RMT_CMD_PREV },
    { "volume", RMT_CMD_VOLUME }, { "seek", RMT_CMD_SEEK }, { "play_mode", RMT_CMD_PLAY_MODE },
    { "radio_stop", RMT_CMD_RADIO_STOP },
};
const char *rmt_cmd_name(rmt_cmd_kind_t k){
    if(k == RMT_CMD_VOLUME_STEP) return "volume";
    if(k == RMT_CMD_BROWSE) return "library";
    if(k == RMT_CMD_PLAY_ITEM) return "play_item";
    for(size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) if(CMDS[i].kind == k) return CMDS[i].name;
    return "none";
}

rmt_route_t rmt_route(const rmt_req_t *r, const char *token){
    rmt_route_t rt; memset(&rt, 0, sizeof rt); rt.kind = RMT_R_NOT_FOUND;
    if(!r || !r->path || r->path[0] != '/') return rt;
    const char *seg = r->path + 1;
    const char *slash = strchr(seg, '/');
    size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
    if(!rmt_token_eq(seg, sl, token)) return rt;
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if(!slash){ rt.kind = get ? RMT_R_REDIRECT : RMT_R_BAD_METHOD; return rt; }
    const char *rest = slash;                                    /* "/" | "/api/..." */
    if(!strcmp(rest, "/")){ rt.kind = get ? RMT_R_PAGE : RMT_R_BAD_METHOD; return rt; }
    if(strncmp(rest, "/api/", 5)) return rt;
    const char *ep = rest + 5;
    if(!strcmp(ep, "state")){ rt.kind = get ? RMT_R_STATE : RMT_R_BAD_METHOD; return rt; }
    if(!strncmp(ep, "library/", 8)){
        int k = route_list(r, ep + 8, &rt.cmd);
        if(k == 0){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
        rt.kind = !get ? RMT_R_BAD_METHOD : k < 0 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind != RMT_R_CMD) memset(&rt.cmd, 0, sizeof rt.cmd);
        return rt;
    }
    if(!strcmp(ep, "play_item")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        rt.kind = route_item(r, &rt.cmd) < 0 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind != RMT_R_CMD) memset(&rt.cmd, 0, sizeof rt.cmd);
        return rt;
    }
    for(size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++){
        if(strcmp(ep, CMDS[i].name)) continue;
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        rt.cmd.kind = CMDS[i].kind; rt.kind = RMT_R_CMD;
        long v = 0; int pr;
        switch(CMDS[i].kind){
        case RMT_CMD_VOLUME:
            if((pr = param(r, "value", &v)) == 0){
                if(v < 0 || v > 120) rt.kind = RMT_R_BAD_PARAM;
            } else if(pr == -1 && (pr = param(r, "step", &v)) == 0){
                rt.cmd.kind = RMT_CMD_VOLUME_STEP;
                if(v < -120 || v > 120) rt.kind = RMT_R_BAD_PARAM;
            } else rt.kind = RMT_R_BAD_PARAM;
            break;
        case RMT_CMD_SEEK:
            if(param(r, "position_ms", &v) || v < 0 || v > 24L * 3600 * 1000) rt.kind = RMT_R_BAD_PARAM;
            break;
        case RMT_CMD_PLAY_MODE:
            if(param(r, "mode", &v) || v < 0 || v > 4) rt.kind = RMT_R_BAD_PARAM;
            break;
        default: break;
        }
        rt.cmd.arg = v;
        if(rt.kind == RMT_R_BAD_PARAM) rt.cmd.kind = RMT_CMD_NONE;
        return rt;
    }
    return rt;
}

/* ---------------------------------------------------------------- JSON */
int rmt_json_escape(char *dst, size_t cap, const char *src){
    if(!dst || cap == 0) return -1;
    size_t o = 0;
    for(const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++){
        char tmp[8]; const char *add; size_t al;
        if(*p == '"'){ add = "\\\""; al = 2; }
        else if(*p == '\\'){ add = "\\\\"; al = 2; }
        else if(*p == '\n'){ add = "\\n"; al = 2; }
        else if(*p == '\r'){ add = "\\r"; al = 2; }
        else if(*p == '\t'){ add = "\\t"; al = 2; }
        else if(*p < 0x20 || *p == 0x7f){ snprintf(tmp, sizeof tmp, "\\u%04x", *p); add = tmp; al = 6; }
        else if(*p == '<'){ add = "\\u003c"; al = 6; }          /* never "</script>"-shaped, whatever the tag says */
        else { tmp[0] = (char)*p; add = tmp; al = 1; }
        if(o + al >= cap){ dst[o] = 0; return -1; }
        memcpy(dst + o, add, al); o += al;
    }
    dst[o] = 0;
    return (int)o;
}
int rmt_state_json(char *dst, size_t cap, const rmt_state_t *s){
    char t[sizeof s->title * 6], a[sizeof s->artist * 6], al[sizeof s->album * 6];   /* worst case: every byte \u00XX */
    char st[sizeof s->station * 6], re[sizeof s->radio_error * 6];
    if(rmt_json_escape(t, sizeof t, s->title) < 0 || rmt_json_escape(a, sizeof a, s->artist) < 0 ||
       rmt_json_escape(al, sizeof al, s->album) < 0 || rmt_json_escape(st, sizeof st, s->station) < 0 ||
       rmt_json_escape(re, sizeof re, s->radio_error) < 0){ if(cap) dst[0] = 0; return -1; }
    int n = snprintf(dst, cap,
        "{\"ok\":true,\"has_track\":%s,\"playing\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
        "\"position_ms\":%ld,\"duration_ms\":%ld,\"volume\":%d,\"volume_max\":%d,\"play_mode\":%d,"
        "\"source\":\"%s\",\"controllable\":%s,\"radio_state\":\"%s\",\"station\":\"%s\",\"radio_error\":\"%s\"}",
        s->has_track ? "true" : "false", s->playing ? "true" : "false", t, a, al,
        s->position_ms < 0 ? 0 : s->position_ms, s->duration_ms < 0 ? 0 : s->duration_ms,
        s->volume, s->volume_max, s->play_mode, s->source ? s->source : "other", s->controllable ? "true" : "false",
        s->radio_state ? s->radio_state : "off", st, re);
    if(n < 0 || (size_t)n >= cap){ if(cap) dst[0] = 0; return -1; }
    return n;
}

/* ---------------------------------------------------------------- growing JSON buffer */
static int buf_room(rmt_buf_t *b, size_t add){
    if(b->oom) return 0;
    if(b->n + add + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 1024;
    while(nc < b->n + add + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if(!np){ b->oom = 1; return 0; }
    b->p = np; b->cap = nc; return 1;
}
void rmt_buf_put(rmt_buf_t *b, const char *s){
    size_t l = strlen(s);
    if(!buf_room(b, l)) return;
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
void rmt_buf_printf(rmt_buf_t *b, const char *fmt, ...){
    char tmp[256]; va_list ap;
    va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if(n < 0) return;
    if((size_t)n < sizeof tmp){ rmt_buf_put(b, tmp); return; }
    if(!buf_room(b, (size_t)n)) return;
    va_start(ap, fmt); vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap); va_end(ap);
    b->n += (size_t)n;
}
void rmt_buf_str(rmt_buf_t *b, const char *s){
    size_t l = strlen(s ? s : "");
    if(!buf_room(b, l * 6 + 2)) return;
    b->p[b->n++] = '"';
    int w = rmt_json_escape(b->p + b->n, b->cap - b->n, s);
    if(w < 0){ b->oom = 1; return; }
    b->n += (size_t)w;
    b->p[b->n++] = '"'; b->p[b->n] = 0;
}
char *rmt_buf_take(rmt_buf_t *b){
    char *p = b->oom ? NULL : b->p;
    if(b->oom) free(b->p);
    if(p && !b->n){ p[0] = 0; }
    b->p = NULL; b->n = b->cap = 0; b->oom = 0;
    return p;
}

/* ---------------------------------------------------------------- the control page */
/* One self-contained page: relative URLs (it is served at /<token>/), polls api/state once a second while visible. */
const char RMT_PAGE_HTML[] =
"<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>\n"
"<meta name=referrer content=no-referrer><title>diskOS Remote</title>\n"
"<style>\n"
":root{--bg:#111;--fg:#eee;--mut:#999;--card:#1d1d1d;--line:#2a2a2a;--acc:#4da3ff}\n"
"@media(prefers-color-scheme:light){:root{--bg:#f4f4f4;--fg:#111;--mut:#666;--card:#fff;--line:#e4e4e4;--acc:#0a66d1}}\n"
"*{box-sizing:border-box}body{margin:0;font:16px/1.4 -apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg)}\n"
"main{max-width:30em;margin:0 auto;padding:1em 1em 4em}\n"
"h1{font-size:1em;color:var(--mut);font-weight:600;margin:0}\n"
".top{display:flex;justify-content:space-between;align-items:center;margin-bottom:.8em}\n"
".seg{display:flex;gap:.4em;overflow-x:auto;margin-bottom:.8em}\n"
".seg button{flex:none;padding:.45em .9em;font-size:.95em;border-radius:99px;background:var(--card);color:var(--fg);border:1px solid var(--line)}\n"
".seg button.on{background:var(--acc);color:#fff;border-color:var(--acc)}\n"
".card{background:var(--card);border-radius:14px;padding:1em;margin-bottom:1em}\n"
"#t{font-size:1.3em;font-weight:700}\n"
"#t,#a,#al,.r b,.r small{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;display:block}\n"
"#a,#al,.r small{color:var(--mut)}\n"
"input[type=range]{width:100%;accent-color:var(--acc)}\n"
".row{display:flex;justify-content:space-between;color:var(--mut);font-size:.85em}\n"
".tr{display:flex;gap:.6em;margin:0 0 1em}\n"
".tr button{flex:1;border:0;border-radius:12px;padding:.8em 0;font-size:1.3em;background:var(--card);color:var(--fg)}\n"
".tr button:disabled,input:disabled{opacity:.35}\n"
".tr button.pp{background:var(--acc);color:#fff}\n"
"select,input[type=search]{width:100%;padding:.6em;border-radius:10px;font-size:1em;background:var(--card);color:var(--fg);border:1px solid var(--line)}\n"
"#msg{position:fixed;left:50%;bottom:1em;transform:translateX(-50%);max-width:90%;background:var(--card);border:1px solid var(--line);padding:.5em 1em;border-radius:99px;font-size:.9em;box-shadow:0 2px 12px #0004}#msg:empty{display:none}#msg.err{color:#e66}label{font-size:.85em;color:var(--mut)}\n"
".hd{display:flex;align-items:center;gap:.5em;margin:.8em 0 .4em}.hd b{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".hd button,.more{border:0;border-radius:10px;padding:.5em .8em;background:var(--card);color:var(--fg);font-size:.95em}\n"
".hd .pa{background:var(--acc);color:#fff}\n"
"#list{background:var(--card);border-radius:14px;overflow:hidden}\n"
".r{display:block;width:100%;text-align:left;border:0;border-bottom:1px solid var(--line);background:none;color:var(--fg);padding:.65em 1em;font-size:1em}\n"
".r b{font-weight:500}.r small{font-size:.85em}\n"
".more{display:block;margin:.6em auto}\n"
".hide{display:none}\n"
"</style><main><div class=top><h1>diskOS Remote</h1></div>\n"
"<div class=seg id=tabs><button data-v=np class=on>Now playing</button><button data-v=albums>Albums</button><button data-v=artists>Artists</button>\n"
"<button data-v=playlists>Playlists</button><button data-v=favorites>Favourites</button><button data-v=songs>Songs</button><button data-v=folder>Folders</button><button data-v=radio>Radio</button></div>\n"
"<div id=np><div class=card><div id=t>-</div><div id=a></div><div id=al></div>\n"
"<input id=pos type=range min=0 max=1 value=0 aria-label=Position><div class=row><span id=pc>0:00</span><span id=du>0:00</span></div></div>\n"
"<button id=rs class='more hide'>Stop radio</button>\n"
"<div class=tr><button id=pv aria-label=Previous>&#9198;</button><button id=pp class=pp aria-label='Play or pause'>&#9199;</button>\n"
"<button id=nx aria-label=Next>&#9197;</button></div>\n"
"<div class=card><label for=vol>Volume <span id=vv></span></label><input id=vol type=range min=0 max=120 value=0>\n"
"<div id=pmw><label for=pm>Play mode</label><select id=pm><option value=0>Sequential<option value=1>Shuffle<option value=2>Repeat one\n"
"<option value=3>Repeat all<option value=4>Single</select></div></div></div>\n"
"<div id=lib class=hide><input id=q type=search placeholder=Search aria-label=Search>\n"
"<div class=hd><button id=bk aria-label=Back>&#8249;</button><b id=ti></b><button id=pa class=pa>Play all</button></div>\n"
"<div id=list></div><button id=mo class='more hide'>Load more</button></div>\n"
"<div id=msg></div></main>\n"
"<script>\n"
"var $=function(i){return document.getElementById(i)},drag={},busy=0,V='np',st=[],items=[],tot=0;\n"
"function fm(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}\n"
"function enc(o){return Object.keys(o).map(function(k){return k+'='+encodeURIComponent(o[k])}).join('&')}\n"
"var mt;function say(t,ok){var m=$('msg');m.textContent=t||'';m.className=ok?'':'err';clearTimeout(mt);if(ok)mt=setTimeout(function(){say('')},2500)}\n"
"function cmd(c,b){return fetch('api/'+c,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''})\n"
".then(function(r){return r.json().then(function(j){say(j.ok?'':(j.message||'Not possible right now'));setTimeout(poll,300);return j.ok})})\n"
".catch(function(){say('Player not reachable')})}\n"
"function play(o){cmd('play_item',enc(o)).then(function(ok){if(ok)say('Playing',1)})}\n"
"function poll(){if(document.hidden||busy||V!=='np')return;busy=1;fetch('api/state',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){busy=0;\n"
"$('t').textContent=s.has_track?(s.title||'-'):'Nothing playing';$('a').textContent=s.artist;$('al').textContent=s.album;\n"
"var ra=s.source==='radio';if(ra){$('t').textContent=s.station||'Web radio';$('a').textContent=s.radio_state==='starting'?'Starting…':s.radio_state==='error'?(s.radio_error||'Radio error'):'Web radio';$('al').textContent=''}\n"
"$('rs').classList.toggle('hide',!ra);['pv','nx'].forEach(function(i){$(i).disabled=ra});$('pos').classList.toggle('hide',ra);$('pc').parentNode.classList.toggle('hide',ra);$('pmw').classList.toggle('hide',ra||s.play_mode<0);\n"
"$('pp').innerHTML=s.playing?'&#9208;':'&#9654;';\n"
"if(!drag.pos){$('pos').max=Math.max(1,s.duration_ms);$('pos').value=s.position_ms}\n"
"$('pc').textContent=fm(s.position_ms);$('du').textContent=fm(s.duration_ms);\n"
"if(!drag.vol){$('vol').max=s.volume_max;$('vol').value=s.volume}$('vv').textContent=s.volume;\n"
"if(s.play_mode>=0&&document.activeElement!==$('pm'))$('pm').value=s.play_mode;\n"
"if(!s.controllable)say(s.source==='other'?'The player is in another mode (e.g. USB or Bluetooth)':'')\n"
"}).catch(function(){busy=0;say('Player not reachable')})}\n"
"$('pp').onclick=function(){cmd('play_pause')};$('nx').onclick=function(){cmd('next')};$('pv').onclick=function(){cmd('previous')};\n"
"['pos','vol'].forEach(function(i){var e=$(i);e.oninput=function(){drag[i]=1;if(i==='vol')$('vv').textContent=e.value};\n"
"e.onchange=function(){drag[i]=0;cmd(i==='pos'?'seek':'volume',(i==='pos'?'position_ms=':'value=')+e.value)}});\n"
"$('rs').onclick=function(){cmd('radio_stop')};$('pm').onchange=function(){cmd('play_mode','mode='+$('pm').value)};\n"
"/* library: st = stack of views {l:list, p:params, t:title, play:item for Play all, row:function(item)->[title,sub,action]} */\n"
"var ROOT={albums:['Albums',function(x){return[x.name,x.artist+' · '+x.count,function(){open('album',{name:x.name},x.name,{kind:'album',name:x.name},function(s){return song(s,{kind:'album',name:x.name,song_id:s.id})})}]}],\n"
"artists:['Artists',function(x){return[x.name,'',function(){open('artist',{name:x.name},x.name,{kind:'artist',name:x.name},function(s){return song(s,{kind:'artist',name:x.name,song_id:s.id})})}]}],\n"
"playlists:['Playlists',function(x){return[x.name,'',function(){open('playlist',{id:x.id},x.name,{kind:'playlist',id:x.id},function(s){return song(s,{kind:'playlist',id:x.id,pos:s.pos})})}]}],\n"
"favorites:['Favourites',function(s){return song(s,{kind:'favorites',pos:s.pos})},{kind:'favorites'}],\n"
"songs:['Songs',function(s){return song(s,{kind:'song',song_id:s.id})},{kind:'all'}],\n"
"folder:['Folders',null],\n"
"radio:['Radio favourites',function(x){return[x.name,[x.codec,x.bitrate?x.bitrate+' kbps':'',x.country].filter(Boolean).join(' · '),function(){play({kind:'radio',pos:x.pos,url:x.url})}]}]};\n"
"function song(s,o){return[s.title,s.artist+(s.album?' · '+s.album:''),function(){play(o)}]}\n"
"function frow(path){return function(x){var p=path?path+'/'+x.name:x.name;return[(x.dir?'📁 ':'')+x.name,'',x.dir?function(){open('folder',{path:p},x.name,null,frow(p))}:function(){play({kind:'file',path:p})}]}}\n"
"function open(l,p,t,pl,row){st.push({l:l,p:p,t:t,play:pl,row:row});$('q').value='';load(0)}\n"
"function cur(){return st[st.length-1]}\n"
"function load(off){var v=cur(),p={};for(var k in v.p)p[k]=v.p[k];if($('q').value)p.q=$('q').value;p.offset=off;p.limit=100;\n"
"$('ti').textContent=v.t;$('bk').style.visibility=st.length>1?'visible':'hidden';$('pa').classList.toggle('hide',!v.play);\n"
"if(!off){items=[];$('list').textContent='Loading…'}\n"
"fetch('api/library/'+v.l+'?'+enc(p),{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){\n"
"if(v!==cur())return;if(!j.ok){$('list').textContent='';say(j.message||'Not possible right now');return}say('');\n"
"if(!off)$('list').textContent='';tot=j.total;j.items.forEach(function(x){items.push(x);var r=v.row(x),b=document.createElement('button');b.className='r';\n"
"var t=document.createElement('b');t.textContent=r[0];b.appendChild(t);if(r[1]){var s=document.createElement('small');s.textContent=r[1];b.appendChild(s)}\n"
"b.onclick=r[2];$('list').appendChild(b)});\n"
"if(!tot)$('list').textContent=$('q').value?'No matches':v.l==='radio'?'No favourites yet - hold a station in the Radio app on the Disc to save it':'Empty';$('mo').classList.toggle('hide',items.length>=tot)}).catch(function(){say('Player not reachable')})}\n"
"function tab(v){V=v;[].forEach.call($('tabs').children,function(b){b.classList.toggle('on',b.dataset.v===v)});\n"
"$('np').classList.toggle('hide',v!=='np');$('lib').classList.toggle('hide',v==='np');say('');\n"
"if(v==='np'){poll();return}var R=ROOT[v];st=[];open(v,v==='folder'?{path:''}:{},R[0],R[2]||null,v==='folder'?frow(''):R[1])}\n"
"[].forEach.call($('tabs').children,function(b){b.onclick=function(){tab(b.dataset.v)}});\n"
"$('bk').onclick=function(){if(st.length>1){st.pop();$('q').value='';load(0)}};\n"
"$('pa').onclick=function(){if(cur().play)play(cur().play)};$('mo').onclick=function(){load(items.length)};\n"
"var qt;$('q').oninput=function(){clearTimeout(qt);qt=setTimeout(function(){load(0)},300)};\n"
"setInterval(poll,1000);document.addEventListener('visibilitychange',poll);poll();\n"
"</script></html>";
