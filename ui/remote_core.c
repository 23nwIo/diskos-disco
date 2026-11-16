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
    { "songs", RMT_L_SONGS }, { "folder", RMT_L_FOLDER }, { "radio", RMT_L_RADIO }, { "radio_search", RMT_L_RADIO_HITS },
};
static const struct { const char *name; rmt_item_t i; } ITEMS[] = {
    { "album", RMT_I_ALBUM }, { "artist", RMT_I_ARTIST }, { "playlist", RMT_I_PLAYLIST }, { "favorites", RMT_I_FAVORITES },
    { "song", RMT_I_SONG }, { "file", RMT_I_FILE }, { "folder", RMT_I_FOLDER }, { "all", RMT_I_ALL }, { "radio", RMT_I_RADIO }, { "radio_hit", RMT_I_RADIO_HIT }, { "radio_now", RMT_I_RADIO_NOW },
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
    case RMT_I_RADIO_NOW: break;                              /* no parameters: whatever plays */
    case RMT_I_FOLDER: {                                      /* path "" or none = the whole card */
        int pr = param_str(r, "path", c->name, sizeof c->name);
        if(pr == -1) c->name[0] = 0;
        if(pr == -2 || !rmt_relpath_ok(c->name)) return -1;
        break; }
    case RMT_I_RADIO: case RMT_I_RADIO_HIT: {
        if(c->pos <= 0) return -1;
        int u = param_str(r, "url", c->name, sizeof c->name);
        if(u == -2 || (u == -1 && it == RMT_I_RADIO_HIT)) return -1;   /* search results change under you: say which */
        if(u == -1) c->name[0] = 0;
        break; }
    default: break;
    }
    return 1;
}

static const struct { const char *name; rmt_edit_t e; } EDITS[] = {
    { "playlist_create", RMT_E_PL_CREATE }, { "playlist_rename", RMT_E_PL_RENAME }, { "playlist_delete", RMT_E_PL_DELETE },
    { "playlist_add", RMT_E_PL_ADD }, { "playlist_remove", RMT_E_PL_REMOVE }, { "favorite", RMT_E_FAV },
};
static int pl_name(const rmt_req_t *r, rmt_cmd_t *c){       /* 1..RMT_PL_NAME_MAX bytes, not only blanks */
    if(param_str(r, "name", c->name, RMT_PL_NAME_MAX + 1)) return -1;
    for(const char *p = c->name; *p; p++) if(*p != ' ') return 0;
    return -1;
}
static int route_edit(const rmt_req_t *r, rmt_edit_t e, rmt_cmd_t *c){
    c->kind = RMT_CMD_EDIT; c->sub = e;
    long v;
    if(e != RMT_E_PL_CREATE && e != RMT_E_FAV && (param(r, "id", &c->id) || c->id <= 0)) return -1;
    switch(e){
    case RMT_E_PL_CREATE: case RMT_E_PL_RENAME: return pl_name(r, c);
    case RMT_E_PL_DELETE: return 0;
    case RMT_E_PL_ADD: {
        char kind[16];
        if(param_str(r, "kind", kind, sizeof kind)) return -1;
        if(!strcmp(kind, "song")){ c->item = RMT_I_SONG; return param(r, "song_id", &c->song_id) || c->song_id <= 0 ? -1 : 0; }
        c->item = !strcmp(kind, "album") ? RMT_I_ALBUM : !strcmp(kind, "artist") ? RMT_I_ARTIST : 0;
        if(!c->item || param_str(r, "name", c->name, sizeof c->name) || !c->name[0]) return -1;
        return 0; }
    case RMT_E_PL_REMOVE:
        if(param(r, "pos", &c->pos) || c->pos <= 0 || param(r, "song_id", &c->song_id) || c->song_id <= 0) return -1;
        return 0;
    case RMT_E_FAV:
        if(param(r, "on", &v) || (v != 0 && v != 1)) return -1;
        c->arg = v;
        return opt_num(r, "song_id", 1, 0x7fffffffL, 0, &c->song_id);
    }
    return -1;
}
static const struct { const char *name; rmt_cmd_kind_t kind; } CMDS[] = {
    { "play_pause", RMT_CMD_PLAY_PAUSE }, { "play", RMT_CMD_PLAY }, { "pause", RMT_CMD_PAUSE },
    { "next", RMT_CMD_NEXT }, { "previous", RMT_CMD_PREV },
    { "volume", RMT_CMD_VOLUME }, { "seek", RMT_CMD_SEEK }, { "play_mode", RMT_CMD_PLAY_MODE },
    { "balance", RMT_CMD_BALANCE },
    { "radio_stop", RMT_CMD_RADIO_STOP },
};
const char *rmt_cmd_name(rmt_cmd_kind_t k){
    if(k == RMT_CMD_VOLUME_STEP) return "volume";
    if(k == RMT_CMD_BROWSE) return "library";
    if(k == RMT_CMD_PLAY_ITEM) return "play_item";
    if(k == RMT_CMD_RADIO_SEARCH) return "radio_search";
    if(k == RMT_CMD_COVER) return "cover";
    if(k == RMT_CMD_STATUS) return "status";
    if(k == RMT_CMD_EDIT) return "edit";
    if(k == RMT_CMD_RESCAN) return "rescan";
    if(k == RMT_CMD_WORK_MODE) return "work_mode";
    if(k == RMT_CMD_REBOOT) return "reboot";
    if(k == RMT_CMD_BT_LIST || k == RMT_CMD_BT) return "bluetooth";
    if(k == RMT_CMD_EQ_LIST || k == RMT_CMD_EQ) return "eq";
    if(k == RMT_CMD_RADIO_FAV) return "radio_fav";
    if(k == RMT_CMD_SCREEN) return "screen";
    if(k == RMT_CMD_TOUCH) return "touch";
    if(k == RMT_CMD_OUTPUTS) return "outputs";
    if(k == RMT_CMD_QUEUE) return "queue";
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
    if(!strcmp(rest, "/system")){ rt.kind = get ? RMT_R_SYS_PAGE : RMT_R_BAD_METHOD; return rt; }
    if(strncmp(rest, "/api/", 5)) return rt;
    const char *ep = rest + 5;
    if(!strcmp(ep, "state")){ long q = 0; rt.kind = get ? RMT_R_STATE : RMT_R_BAD_METHOD; rt.cmd.arg = param(r, "q", &q) == 0 && q == 1; return rt; }
    if(!strcmp(ep, "queue")){
        if(get){
            long l = 100; int pr = param(r, "limit", &l);
            if(pr == -2 || l < 1 || l > 500){ rt.kind = RMT_R_BAD_PARAM; return rt; }
            rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_QUEUE; rt.cmd.sub = RMT_Q_LIST; rt.cmd.limit = l; return rt;
        }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        static const char *const A[] = { "jump", "remove", "move", "clear", "add" };
        char a[8], k[8]; int act = 0, ok; long ver = 0, i = 0, b = 0, to = 0;
        if(!param_str(r, "action", a, sizeof a)) for(int j = 0; j < 5; j++) if(!strcmp(a, A[j])) act = j + RMT_Q_JUMP;
        if(act == RMT_Q_ADD){
            ok = !param_str(r, "kind", k, sizeof k);
            if(ok && !strcmp(k, "song")){ rt.cmd.item = RMT_I_SONG; ok = !param(r, "song_id", &rt.cmd.song_id) && rt.cmd.song_id > 0; }
            else if(ok && (!strcmp(k, "album") || !strcmp(k, "artist"))){
                rt.cmd.item = k[1] == 'l' ? RMT_I_ALBUM : RMT_I_ARTIST;
                ok = !param_str(r, "name", rt.cmd.name, sizeof rt.cmd.name) && rt.cmd.name[0];
            } else if(ok && !strcmp(k, "next")){ rt.cmd.item = -1; ok = !param(r, "b", &rt.cmd.id) && rt.cmd.id > 0; }
            else ok = 0;
            if(!ok){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
            rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_QUEUE; rt.cmd.sub = act; return rt;
        }
        ok = act && !param(r, "ver", &ver) && ver > 0;
        if(ok && act == RMT_Q_JUMP){
            ok = !param_str(r, "kind", k, sizeof k);
            if(ok && !strcmp(k, "queue")){ rt.cmd.item = 1; ok = !param(r, "i", &i) && i >= 0 && i < 1000; }
            else if(ok && !strcmp(k, "next")){ rt.cmd.item = 2; ok = !param(r, "b", &b) && b > 0; }
            else ok = 0;
        }
        if(ok && (act == RMT_Q_REMOVE || act == RMT_Q_MOVE)) ok = !param(r, "i", &i) && i >= 0 && i < 1000;
        if(ok && act == RMT_Q_MOVE) ok = !param(r, "to", &to) && to >= 0 && to < 1000;
        if(!ok){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_QUEUE; rt.cmd.sub = act; rt.cmd.arg = ver; rt.cmd.pos = i; rt.cmd.id = b; rt.cmd.offset = to;
        return rt;
    }
    if(!strcmp(ep, "bluetooth") || !strcmp(ep, "bt_device")){
        int dev = ep[1] == 't';
        if(!dev && get){ rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_BT_LIST; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        static const char *const A[] = { "on", "off", "refresh", "search", "connect", "disconnect", "forget", "rename", "codec" };
        char a[12]; int k = 0;
        if(!param_str(r, "action", a, sizeof a)) for(int i = 0; i < 9; i++) if(!strcmp(a, A[i])) k = i + 1;
        int ok = k && (dev ? k >= RMT_BT_CONNECT && k <= RMT_BT_RENAME : k < RMT_BT_CONNECT || k == RMT_BT_CODEC);
        if(ok && dev){
            ok = !param_str(r, "mac", rt.cmd.q, sizeof rt.cmd.q) && strlen(rt.cmd.q) == 17;
            for(int i = 0; ok && i < 17; i++) ok = i % 3 == 2 ? rt.cmd.q[i] == ':' : isxdigit((unsigned char)rt.cmd.q[i]) != 0;
            if(ok && k == RMT_BT_RENAME){ int pr = param_str(r, "name", rt.cmd.name, 48); ok = pr == 0 || pr == -1; if(pr == -1) rt.cmd.name[0] = 0; }
        }
        if(ok && k == RMT_BT_CODEC){ long v = -1; ok = !param(r, "codec", &v) && v >= 0 && v <= 9; rt.cmd.arg = v; }
        if(!ok){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.cmd.kind = RMT_CMD_BT; rt.cmd.sub = k; rt.kind = RMT_R_CMD;
        return rt;
    }
    if(!strcmp(ep, "work_mode")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        long v; rt.kind = param(r, "mode", &v) || v < 0 || v > 5 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind == RMT_R_CMD){ rt.cmd.kind = RMT_CMD_WORK_MODE; rt.cmd.arg = v; }
        return rt;
    }
    if(!strcmp(ep, "reboot")){                                 /* confirm=1: a stray request can't restart the Disc */
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        long v; rt.kind = param(r, "confirm", &v) || v != 1 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind == RMT_R_CMD) rt.cmd.kind = RMT_CMD_REBOOT;
        return rt;
    }
    if(!strcmp(ep, "rescan")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        char a[8]; int pr = param_str(r, "action", a, sizeof a);
        rt.kind = pr || (strcmp(a, "start") && strcmp(a, "stop")) ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind == RMT_R_CMD){ rt.cmd.kind = RMT_CMD_RESCAN; rt.cmd.arg = !strcmp(a, "start"); }
        return rt;
    }
    if(!strcmp(ep, "status")){ rt.kind = get ? RMT_R_CMD : RMT_R_BAD_METHOD; if(get) rt.cmd.kind = RMT_CMD_STATUS; return rt; }
    if(!strcmp(ep, "eq")){
        if(get){ rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_EQ_LIST; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        long v; rt.kind = param(r, "preset", &v) || v < 0 || v > 20 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind == RMT_R_CMD){ rt.cmd.kind = RMT_CMD_EQ; rt.cmd.arg = v; }
        return rt;
    }
    if(!strcmp(ep, "screen")){ rt.kind = get ? RMT_R_CMD : RMT_R_BAD_METHOD; if(get) rt.cmd.kind = RMT_CMD_SCREEN; return rt; }
    if(!strcmp(ep, "touch")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        long x, y, x2, y2, ms = 100; int bad = param(r, "x", &x) || param(r, "y", &y);
        if(!bad){
            int p2 = param(r, "x2", &x2), p3 = param(r, "y2", &y2), p4 = param(r, "ms", &ms);
            if(p2 == -1) x2 = x;
            if(p3 == -1) y2 = y;
            if(p4 == -1) ms = 100;
            bad = p2 == -2 || p3 == -2 || p4 == -2;
        }
        if(!bad) bad = x < 0 || y < 0 || x2 < 0 || y2 < 0 || x >= RMT_SCREEN_PX || y >= RMT_SCREEN_PX ||
                       x2 >= RMT_SCREEN_PX || y2 >= RMT_SCREEN_PX || ms < 0 || ms > RMT_TOUCH_MS_MAX;
        rt.kind = bad ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(!bad){ rt.cmd.kind = RMT_CMD_TOUCH; rt.cmd.pos = x; rt.cmd.id = y; rt.cmd.offset = x2; rt.cmd.limit = y2; rt.cmd.arg = ms; }
        return rt;
    }
    if(!strcmp(ep, "cover")){ rt.kind = get ? RMT_R_CMD : RMT_R_BAD_METHOD; if(get) rt.cmd.kind = RMT_CMD_COVER; return rt; }
    if(!strncmp(ep, "library/", 8)){
        int k = route_list(r, ep + 8, &rt.cmd);
        if(k == 0){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
        rt.kind = !get ? RMT_R_BAD_METHOD : k < 0 ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind != RMT_R_CMD) memset(&rt.cmd, 0, sizeof rt.cmd);
        return rt;
    }
    if(!strcmp(ep, "radio_search")){                             /* starts a download on the Disc: POST */
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        rt.cmd.kind = RMT_CMD_RADIO_SEARCH; rt.kind = RMT_R_CMD;
        if(param_str(r, "q", rt.cmd.q, sizeof rt.cmd.q) == -2){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; }
        return rt;
    }
    if(!strcmp(ep, "radio_fav_add") || !strcmp(ep, "radio_fav_remove")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        int add = ep[10] == 'a';
        rt.kind = route_item(r, &rt.cmd) < 0 || (rt.cmd.sub == RMT_I_RADIO_NOW ? 0 : (rt.cmd.sub != RMT_I_RADIO && rt.cmd.sub != RMT_I_RADIO_HIT) || !rt.cmd.name[0]) ? RMT_R_BAD_PARAM : RMT_R_CMD;
        rt.cmd.kind = RMT_CMD_RADIO_FAV; rt.cmd.arg = add;
        if(rt.kind != RMT_R_CMD) memset(&rt.cmd, 0, sizeof rt.cmd);
        return rt;
    }
    for(size_t i = 0; i < sizeof EDITS / sizeof EDITS[0]; i++){
        if(strcmp(ep, EDITS[i].name)) continue;
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        rt.kind = route_edit(r, EDITS[i].e, &rt.cmd) ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind != RMT_R_CMD) memset(&rt.cmd, 0, sizeof rt.cmd);
        return rt;
    }
    if(!strcmp(ep, "outputs")){
        static const struct { const char *k; int sub; long max; } O[] = {
            { "aux", RMT_OUT_AUX, 1 }, { "bt", RMT_OUT_BT, 1 }, { "vol_aux", RMT_OUT_VOL_AUX, 120 },
            { "vol_bt", RMT_OUT_VOL_BT, 120 }, { "auto", RMT_OUT_AUTO, 2 }, { "codec", RMT_OUT_CODEC, 4 } };
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        int found = 0; long v = 0;
        for(size_t k = 0; k < sizeof O / sizeof O[0]; k++){
            long x; int pr = param(r, O[k].k, &x);
            if(pr == -1) continue;                                 /* not given */
            if(pr || x < 0 || x > O[k].max || found++){ found = 2; break; }
            rt.cmd.sub = O[k].sub; v = x;
        }
        if(found != 1){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.cmd.kind = RMT_CMD_OUTPUTS; rt.cmd.arg = v; rt.kind = RMT_R_CMD;
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
        case RMT_CMD_BALANCE:
            if(param(r, "value", &v) || v < -10 || v > 10) rt.kind = RMT_R_BAD_PARAM;
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
    char bd[sizeof s->bt_device * 6], bc[sizeof s->bt_codec * 6], su[sizeof s->station_url * 6], sti[sizeof s->stream_title * 6], st[sizeof s->station * 6], re[sizeof s->radio_error * 6], fm[sizeof s->format * 6], ar[sizeof s->art * 6], eqn[sizeof s->eq_name * 6];
    if(rmt_json_escape(t, sizeof t, s->title) < 0 || rmt_json_escape(a, sizeof a, s->artist) < 0 ||
       rmt_json_escape(al, sizeof al, s->album) < 0 || rmt_json_escape(st, sizeof st, s->station) < 0 || rmt_json_escape(su, sizeof su, s->station_url) < 0 || rmt_json_escape(bc, sizeof bc, s->bt_codec) < 0 || rmt_json_escape(bd, sizeof bd, s->bt_device) < 0 || rmt_json_escape(sti, sizeof sti, s->stream_title) < 0 ||
       rmt_json_escape(re, sizeof re, s->radio_error) < 0 || rmt_json_escape(fm, sizeof fm, s->format) < 0 ||
       rmt_json_escape(ar, sizeof ar, s->art) < 0 || rmt_json_escape(eqn, sizeof eqn, s->eq_name) < 0){ if(cap) dst[0] = 0; return -1; }
    char ou[300] = "null", cl[32] = "";
    for(int i = 0; i < 5; i++) if(s->out_codecs & (1 << i)) snprintf(cl + strlen(cl), sizeof cl - strlen(cl), "%s%d", cl[0] ? "," : "", i);
    if(s->out_own)
        snprintf(ou, sizeof ou, "{\"aux\":%s,\"bt\":%s,\"vol_aux\":%d,\"vol_bt\":%d,\"bt_ready\":%s,\"auto\":%d,\"jack\":%d,\"bal\":%d,\"codec\":%d,\"codecs\":[%s],\"switching\":%s}",
                 s->out_aux ? "true" : "false", s->out_bt ? "true" : "false", s->out_vol_aux, s->out_vol_bt, s->out_bt_ready ? "true" : "false",
                 s->out_auto, s->out_jack, s->out_bal, s->out_codec, cl, s->out_switching ? "true" : "false");
    int n = snprintf(dst, cap,
        "{\"ok\":true,\"has_track\":%s,\"playing\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
        "\"position_ms\":%ld,\"duration_ms\":%ld,\"volume\":%d,\"volume_max\":%d,\"balance\":%d,\"queue_ver\":%u,\"play_mode\":%d,\"work_mode\":%d,"
        "\"source\":\"%s\",\"controllable\":%s,\"radio_state\":\"%s\",\"station\":\"%s\",\"station_url\":\"%s\",\"stream_title\":\"%s\",\"bt_codec\":\"%s\",\"bt_device\":\"%s\",\"station_fav\":%s,\"radio_error\":\"%s\","
        "\"format\":\"%s\",\"kbps\":%d,\"bits\":%d,\"sample_rate\":%d,\"channels\":%d,\"favorite\":%s,\"art\":\"%s\",\"eq\":%d,\"eq_name\":\"%s\",\"outputs\":%s}",
        s->has_track ? "true" : "false", s->playing ? "true" : "false", t, a, al,
        s->position_ms < 0 ? 0 : s->position_ms, s->duration_ms < 0 ? 0 : s->duration_ms,
        s->volume, s->volume_max, s->balance, s->queue_ver, s->play_mode, s->work_mode, s->source ? s->source : "other", s->controllable ? "true" : "false",
        s->radio_state ? s->radio_state : "off", st, su, sti, bc, bd, s->station_fav < 0 ? "null" : s->station_fav ? "true" : "false", re,
        fm, s->kbps, s->bits, s->sample_rate, s->channels, s->favorite < 0 ? "null" : s->favorite ? "true" : "false", ar, s->eq, eqn, ou);
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

/* ---------------------------------------------------------------- the pages */
/* RMT_PAGE_HTML and RMT_SYS_HTML live in remote_page.c, generated from ui/remote_page/ (ui/tools/gen_remote_page.py). */
