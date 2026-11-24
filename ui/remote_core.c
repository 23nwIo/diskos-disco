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
/* Hit&Guess corrections: a = artist, t = title (absent or empty: unchanged), y = year (absent or 0: unchanged),
 * x = 1 leave the song out of the game from now on. 0, or -1 when a value is bad. */
static int rmt_fix_params(const rmt_req_t *r, rmt_cmd_t *c){
    long y = 0, x = 0;
    int pa = param_str(r, "a", c->name, HG_TXT_MAX), pt = param_str(r, "t", c->text, HG_TXT_MAX), py = param(r, "y", &y), px = param(r, "x", &x);
    if(pa == -2 || pt == -2 || py == -2 || px == -2) return -1;
    if(pa == -1) c->name[0] = 0;
    if(pt == -1) c->text[0] = 0;
    if(py == -1) y = 0;
    if(px == -1) x = 0;
    if((y && (y < 1000 || y > 2999)) || x < 0 || x > 1) return -1;
    c->offset = y; c->item = (int)x;
    return 0;
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
    if(k == RMT_CMD_BANDCAMP) return "bandcamp";
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
    if(k == RMT_CMD_LIST_END) return "list_end";
    if(k == RMT_CMD_PARTY_STATE) return "party_state";
    if(k == RMT_CMD_PARTY) return "party";
    if(k == RMT_CMD_PARTY_RADIO) return "party_radio";
    if(k == RMT_CMD_LOOK) return "look";
    if(k == RMT_CMD_GAME) return "game";
    if(k == RMT_CMD_GAME_SEARCH) return "game_search";
    if(k == RMT_CMD_GAME_ADMIN) return "game_admin";
    for(size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) if(CMDS[i].kind == k) return CMDS[i].name;
    return "none";
}

/* everything after /<token>: slash is "/..." or NULL (the bare /<token>) */
static rmt_route_t route_rest(const rmt_req_t *r, const char *slash){
    rmt_route_t rt; memset(&rt, 0, sizeof rt); rt.kind = RMT_R_NOT_FOUND;
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if(!slash){ rt.kind = get ? RMT_R_REDIRECT : RMT_R_BAD_METHOD; return rt; }
    const char *rest = slash;                                    /* "/" | "/api/..." */
    if(!strcmp(rest, "/")){ rt.kind = get ? RMT_R_PAGE : RMT_R_BAD_METHOD; return rt; }
    if(!strcmp(rest, "/system")){ rt.kind = get ? RMT_R_SYS_PAGE : RMT_R_BAD_METHOD; return rt; }
    if(!strcmp(rest, "/hitguess")){ rt.kind = get ? RMT_R_GAME_PAGE : RMT_R_BAD_METHOD; return rt; }
    if(strncmp(rest, "/api/", 5)) return rt;
    const char *ep = rest + 5;
    if(!strcmp(ep, "state")){ long q = 0; rt.kind = get ? RMT_R_STATE : RMT_R_BAD_METHOD; rt.cmd.arg = param(r, "q", &q) == 0 && q == 1; return rt; }
    if(!strcmp(ep, "list_end")){
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        long m = -1, pos = 0, pl = 0, fl = 0; int pp = param(r, "pos", &pos), pu = param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name);
        int ppl = param(r, "pl", &pl), pfl = param(r, "flags", &fl);
        if(param(r, "mode", &m) || m < 0 || m > 3 || pp == -2 || pu == -2 || (pp == 0 && (pos < 1 || pu != 0))
           || ppl == -2 || pfl == -2 || (ppl == 0) != (pfl == 0) || (ppl == 0 && (m != 3 || pl < 1 || fl < 0 || fl > 3))){
            memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_LIST_END; rt.cmd.arg = m; rt.cmd.pos = pp == 0 ? pos : 0;
        if(ppl == 0){ rt.cmd.id = pl; rt.cmd.offset = fl; rt.cmd.item = 1; }   /* the playlist for mode 3, with Repeat 1 / Shuffle 2 */
        if(pp != 0) rt.cmd.name[0] = 0;
        return rt;
    }
    if(!strcmp(ep, "game_admin")){
        if(get){ rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_GAME_ADMIN; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        rt.cmd.kind = RMT_CMD_GAME_ADMIN; rt.cmd.arg = 1;
        char act[16]; int pa = param_str(r, "action", act, sizeof act);
        if(pa == 0){
            static const char *const A[] = { "start", "skip", "remove", "end", "again", "reset" };
            rt.kind = RMT_R_BAD_PARAM;
            for(int i = 0; i < 6; i++) if(!strcmp(act, A[i])) rt.cmd.sub = RMT_G_START + i;
            if(!strcmp(act, "fixsong")){                     /* k = the turn (0 = the first song; default the one now) */
                long k = -1; int pk = param(r, "k", &k);
                if(pk == -2 || (pk == 0 && (k < 0 || k >= HG_TURNS)) || rmt_fix_params(r, &rt.cmd)){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
                rt.cmd.sub = RMT_G_FIXSONG; rt.cmd.pos = k; rt.kind = RMT_R_CMD; return rt;
            }
            if(!rt.cmd.sub){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
            long p = -1;
            if(rt.cmd.sub == RMT_G_REMOVE && (param(r, "p", &p) || p < 0 || p >= HG_PLAYERS)){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
            rt.cmd.pos = p; rt.kind = RMT_R_CMD; return rt;
        }
        int any = 0, bad = pa == -2;
        for(int k = 0; k < HG_S_N; k++){
            long v; int pr = param(r, HG_KEYS[k].key, &v);
            rt.cmd.game[k] = -1;
            if(pr == -1) continue;
            if(pr || v < HG_KEYS[k].min || v > HG_KEYS[k].max) bad = 1;
            else { rt.cmd.game[k] = (int)v; any = 1; }
        }
        rt.cmd.id = -1;
        int pd = param_str(r, "dir", rt.cmd.name, sizeof rt.cmd.name);
        if(pd == 0){ if(!rmt_relpath_ok(rt.cmd.name)) bad = 1; else { any = 1; rt.cmd.item = 1; } }
        else if(pd == -2) bad = 1;
        long pl; int pp = param(r, "pl", &pl);
        if(pp == 0){ if(pl < 0) bad = 1; else { rt.cmd.id = pl; any = 1; } }
        else if(pp == -2) bad = 1;
        if(bad || !any){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.kind = RMT_R_CMD; rt.cmd.sub = RMT_G_SETUP;
        return rt;
    }
    if(!strcmp(ep, "look")){                                       /* POST: l=<line> for each line of the file */
        if(get){ rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_LOOK; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        size_t o = 0; int bad = 0;
        for(const char *p = r->body; p && *p && !bad; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL){
            char v[160];
            if(strncmp(p, "l=", 2)) continue;
            int pr = param_str_list(p, "l", v, sizeof v);
            if(pr == -1) v[0] = 0; else if(pr){ bad = 1; break; }
            int w = snprintf(rt.cmd.text + o, sizeof rt.cmd.text - o, "%s\n", v);
            if(w < 0 || (size_t)w >= sizeof rt.cmd.text - o) bad = 1; else o += (size_t)w;
        }
        rmt_look_t lk;
        if(bad || rmt_look_parse(rt.cmd.text, &lk)){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_LOOK; rt.cmd.arg = 1;
        return rt;
    }
    if(!strcmp(ep, "party")){
        if(get){ rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_PARTY; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        int any = 0, bad = 0;
        for(int k = 0; k < RMT_P_N; k++){
            long v; int pr = param(r, RMT_PARTY_KEYS[k].key, &v);
            rt.cmd.party[k] = -1;
            if(pr == -1) continue;
            if(pr || v < RMT_PARTY_KEYS[k].min || v > RMT_PARTY_KEYS[k].max) bad = 1;
            else { rt.cmd.party[k] = (int)v; any = 1; }
        }
        int pn = param_str(r, "name", rt.cmd.name, RMT_PARTY_NAME_MAX + 1);
        if(pn == 0){ if(!rmt_party_name_ok(rt.cmd.name)) bad = 1; else any = 1; }
        else if(pn == -2) bad = 1;
        else rt.cmd.name[0] = 0;
        if(bad || !any){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
        rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_PARTY; rt.cmd.arg = 1;
        return rt;
    }
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
            } else if(ok && !strcmp(k, "file")){
                rt.cmd.item = RMT_I_FILE;
                ok = !param_str(r, "path", rt.cmd.name, sizeof rt.cmd.name) && rt.cmd.name[0] && rmt_relpath_ok(rt.cmd.name);
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
        rt.kind = pr || (strcmp(a, "start") && strcmp(a, "full") && strcmp(a, "stop")) ? RMT_R_BAD_PARAM : RMT_R_CMD;
        if(rt.kind == RMT_R_CMD){ rt.cmd.kind = RMT_CMD_RESCAN; rt.cmd.arg = !strcmp(a, "stop") ? 0 : !strcmp(a, "full") ? 2 : 1; }
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
    if(!strcmp(ep, "bandcamp")){
        rt.cmd.kind = RMT_CMD_BANDCAMP;
        if(get){ long f = 0; rt.kind = RMT_R_CMD; rt.cmd.sub = param(r, "favs", &f) == 0 && f ? RMT_BC_FAVS : RMT_BC_STATE; return rt; }
        if(!post){ rt.kind = RMT_R_BAD_METHOD; return rt; }
        char a[16]; int ok = 0;
        rt.kind = RMT_R_BAD_PARAM;
        if(!param_str(r, "action", a, sizeof a)){
            if(!strcmp(a, "search")){ rt.cmd.sub = RMT_BC_SEARCH; ok = !param_str(r, "q", rt.cmd.q, sizeof rt.cmd.q) && rt.cmd.q[0]; }
            else if(!strcmp(a, "open")){ rt.cmd.sub = RMT_BC_OPEN; ok = !param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name) && !strncmp(rt.cmd.name, "https://", 8); }
            else if(!strcmp(a, "play") || !strcmp(a, "download")){
                long t = -1; int pt = param(r, "track", &t);
                rt.cmd.sub = a[0] == 'p' ? RMT_BC_PLAY : RMT_BC_DOWNLOAD; rt.cmd.pos = pt == 0 ? t : -1;
                ok = !param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name) && !strncmp(rt.cmd.name, "https://", 8)
                     && pt != -2 && (pt != 0 || (t >= 0 && t < 10000));
            }
            else if(!strcmp(a, "stop")){ rt.cmd.sub = RMT_BC_STOP; ok = 1; }
            else if(!strcmp(a, "stop_downloads")){ rt.cmd.sub = RMT_BC_DL_STOP; ok = 1; }
            else if(!strcmp(a, "fav_add")){                 /* text = the favourite as a line of bandcamp_favorites.txt */
                char ty[4], nm[BC_FAV_FIELD], ar[BC_FAV_FIELD], al[BC_FAV_FIELD], art[512];
                rt.cmd.sub = RMT_BC_FAV_ADD;
                ok = !param_str(r, "type", ty, sizeof ty) && strlen(ty) == 1 && strchr("abt", ty[0]) && !param_str(r, "name", nm, sizeof nm) && nm[0]
                     && param_str(r, "artist", ar, sizeof ar) != -2 && param_str(r, "album", al, sizeof al) != -2 && param_str(r, "art", art, sizeof art) != -2
                     && !param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name) && !strncmp(rt.cmd.name, "https://", 8);
                if(ok){
                    if(param_str(r, "artist", ar, sizeof ar)) ar[0] = 0;
                    if(param_str(r, "album", al, sizeof al)) al[0] = 0;
                    if(param_str(r, "art", art, sizeof art)) art[0] = 0;
                    for(char *f[] = { nm, ar, al, art, NULL }, **x = f; *x; x++) for(char *c = *x; *c; c++) if(*c == '\t') *c = ' ';
                    snprintf(rt.cmd.text, sizeof rt.cmd.text, "%c\t%s\t%s\t%s\t%s\t%s", ty[0], nm, ar, al, rt.cmd.name, art);
                }
            }
            else if(!strcmp(a, "fav_remove")){ rt.cmd.sub = RMT_BC_FAV_REMOVE; ok = !param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name) && rt.cmd.name[0]; }
            else if(!strcmp(a, "quality")){ long q = -1; rt.cmd.sub = RMT_BC_QUALITY; ok = param(r, "q", &q) == 0 && q >= 0 && q <= 3; rt.cmd.pos = q; }
        }
        if(!ok){ memset(&rt.cmd, 0, sizeof rt.cmd); return rt; }
        rt.kind = RMT_R_CMD;
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
        char k[8];
        if(add && !param_str(r, "kind", k, sizeof k) && !strcmp(k, "stream")){   /* a stream URL of your own (radio.c checks it) */
            int ok = !param_str(r, "name", rt.cmd.q, sizeof rt.cmd.q) && rt.cmd.q[0] && !param_str(r, "url", rt.cmd.name, sizeof rt.cmd.name)
                     && (!strncmp(rt.cmd.name, "http://", 7) || !strncmp(rt.cmd.name, "https://", 8));
            if(!ok){ memset(&rt.cmd, 0, sizeof rt.cmd); rt.kind = RMT_R_BAD_PARAM; return rt; }
            rt.kind = RMT_R_CMD; rt.cmd.kind = RMT_CMD_RADIO_FAV; rt.cmd.sub = RMT_I_STREAM; rt.cmd.arg = 1;
            return rt;
        }
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
rmt_route_t rmt_route(const rmt_req_t *r, const char *token){
    rmt_route_t rt; memset(&rt, 0, sizeof rt); rt.kind = RMT_R_NOT_FOUND;
    if(!r || !r->path || r->path[0] != '/') return rt;
    const char *seg = r->path + 1;
    const char *slash = strchr(seg, '/');
    size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
    if(!rmt_token_eq(seg, sl, token)) return rt;
    return route_rest(r, slash);
}

/* ---------------------------------------------------------------- party mode (the guests' link) */
const rmt_party_key_t RMT_PARTY_KEYS[RMT_P_N] = {
    [RMT_P_ON] = { "on", 0, 2, 0 },          [RMT_P_VOL] = { "vol", 0, 1, 1 },        [RMT_P_MAXVOL] = { "maxvol", 1, 100, 100 },
    [RMT_P_NP] = { "np", 0, 1, 1 },          [RMT_P_QUEUE] = { "queue", 0, 1, 1 },    [RMT_P_SKIP] = { "skip", 0, 1, 0 },
    [RMT_P_TOTAL] = { "total", 1, 50, 3 },   [RMT_P_CONSEC] = { "consec", 1, 10, 1 }, [RMT_P_REPEAT] = { "repeat", 0, 1, 0 },
    [RMT_P_RADIO] = { "radio", 0, 1, 0 },    [RMT_P_NAMES] = { "names", 0, 1, 1 },
    [RMT_P_UPLOAD] = { "upload", 0, 1, 0 },  [RMT_P_UPMAX] = { "upmax", 1, 2000, 100 },   /* MB */
    [RMT_P_BANDCAMP] = { "bandcamp", 0, 1, 0 },   /* guests pick songs from Bandcamp (saved to the card, then queued) */
};
static void name_part(char *out, size_t cap, size_t *o, const char *s, size_t n){
    for(size_t i = 0; i < n && *o + 1 < cap; i++){
        unsigned char c = (unsigned char)s[i];
        int ok = isalnum(c) || c >= 0x80 || strchr("-_.()+,'&", c);
        char w = ok ? (char)c : '_';
        if(w == '_' && *o && out[*o - 1] == '_') continue;         /* one '_' for a run of them */
        out[(*o)++] = w;
    }
    out[*o] = 0;
}
int rmt_upload_name(const char *guest, const char *file, char *out, size_t cap){
    if(!out || cap < 16) return -1;
    out[0] = 0;
    if(!file) return -1;
    const char *b = file;                                          /* a browser may send a whole path */
    for(const char *p = file; *p; p++) if(*p == '/' || *p == '\\') b = p + 1;
    const char *dot = strrchr(b, '.');
    if(!dot || dot == b || !dot[1] || strlen(dot + 1) > 4) return -1;
    char ext[8]; size_t el = strlen(dot + 1);
    for(size_t i = 0; i <= el; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    const char *hit = strstr(" " RMT_UPLOAD_EXTS " ", ext);
    if(!hit || hit[-1] != ' ' || hit[el] != ' ') return -1;
    char g[RMT_GNAME_MAX + 1]; size_t go = 0;
    name_part(g, sizeof g, &go, guest && guest[0] ? guest : "guest", strlen(guest && guest[0] ? guest : "guest"));
    while(go && (g[go - 1] == '_' || g[go - 1] == '.')) g[--go] = 0;
    size_t o = 0, room = cap < 128 ? cap : 128;
    name_part(out, room - el - 1, &o, go ? g : "guest", go ? go : 5);
    if(o + 1 < room - el - 1) out[o++] = '_';
    size_t start = o;
    name_part(out, room - el - 1, &o, b, (size_t)(dot - b));
    while(o > start && (out[o - 1] == '_' || out[o - 1] == '.')) o--;
    if(o == start) return -1;                                      /* nothing left of the name */
    out[o++] = '.'; memcpy(out + o, ext, el + 1);
    if(out[0] == '.') out[0] = '_';
    return 0;
}
const char RMT_LOOK_DEFAULT[] =
    "# diskOS Remote Control: the favourite symbols and colours each browser can choose from\n"
    "# symbol=<one symbol>   (emoji keep their own colours)\n"
    "symbol=\xe2\x9d\xa4\xef\xb8\x8f\nsymbol=\xe2\xad\x90\nsymbol=\xe2\x98\xad\nsymbol=\xf0\x96\xa4\x90\nsymbol=\xf0\x90\x95\xa3\n"
    "# color=<name> #<dark theme> [#<light theme>]\n"
    "color=Red #ff4d5e #d61f3b\ncolor=Yellow #ffcc33 #c99700\ncolor=Orange #ff9f43 #d35f00\ncolor=Pink #ff7eb6 #d6337a\n"
    "color=Purple #b48cff #7a4fd6\ncolor=Blue #4da3ff #0a66d1\ncolor=Green #3ccf7a #1a8f4c\ncolor=White #f2f2f2 #8a8a8a\n"
    "color=Black #000000\n";
static int hex_ok(const char *h){
    if(strlen(h) != 7 || h[0] != '#') return 0;
    for(int i = 1; i < 7; i++) if(!isxdigit((unsigned char)h[i])) return 0;
    return 1;
}
int rmt_look_parse(const char *text, rmt_look_t *lk){
    memset(lk, 0, sizeof *lk);
    int ln = 0;
    for(const char *l = text ? text : ""; *l; ){
        const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
        char line[160]; ln++;
        if(n >= sizeof line) return ln;
        memcpy(line, l, n); line[n] = 0;
        l += n + (e ? 1 : 0);
        while(n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        char *p = line; while(*p == ' ' || *p == '\t') p++;
        if(!*p || *p == '#') continue;
        for(char *q = p; *q; q++) if((unsigned char)*q < 0x20 || *q == '<' || *q == '"' || *q == '\\') return ln;
        if(!strncmp(p, "symbol=", 7)){
            const char *v = p + 7;
            if(!v[0] || strlen(v) >= sizeof lk->sym[0] || strchr(v, ' ') || lk->nsym >= RMT_LOOK_SYMS) return ln;
            snprintf(lk->sym[lk->nsym++], sizeof lk->sym[0], "%s", v);
        } else if(!strncmp(p, "color=", 6)){
            char *v = p + 6, *h1 = strchr(v, '#');
            if(!h1 || h1 == v || lk->ncol >= RMT_LOOK_COLS) return ln;
            char *ne = h1; while(ne > v && ne[-1] == ' ') ne--;
            if(ne == v || (size_t)(ne - v) >= sizeof lk->col[0].name) return ln;
            char d[16] = "", li[16] = "", extra[4] = "";
            if(sscanf(h1, "%15s %15s %3s", d, li, extra) == 3 || !hex_ok(d) || (li[0] && !hex_ok(li))) return ln;
            snprintf(lk->col[lk->ncol].name, sizeof lk->col[0].name, "%.*s", (int)(ne - v), v);
            snprintf(lk->col[lk->ncol].dark, 8, "%.7s", d); snprintf(lk->col[lk->ncol].light, 8, "%.7s", li[0] ? li : d);
            for(int i = 0; i < lk->ncol; i++) if(!strcmp(lk->col[i].name, lk->col[lk->ncol].name)) return ln;   /* names pick them */
            lk->ncol++;
        } else return ln;
    }
    return lk->nsym && lk->ncol ? 0 : ln + 1;
}
void rmt_look_json(rmt_buf_t *b, const rmt_look_t *lk){
    rmt_buf_put(b, "\"symbols\":[");
    for(int i = 0; i < lk->nsym; i++){ if(i) rmt_buf_put(b, ","); rmt_buf_str(b, lk->sym[i]); }
    rmt_buf_put(b, "],\"colors\":[");
    for(int i = 0; i < lk->ncol; i++){
        rmt_buf_put(b, i ? ",{\"name\":" : "{\"name\":"); rmt_buf_str(b, lk->col[i].name);
        rmt_buf_printf(b, ",\"dark\":\"%s\",\"light\":\"%s\"}", lk->col[i].dark, lk->col[i].light);
    }
    rmt_buf_put(b, "]");
}
int rmt_party_name_ok(const char *s){
    size_t n = s ? strlen(s) : 0;
    if(n < 2 || n > RMT_PARTY_NAME_MAX || !strcasecmp(s, "api") || !strcasecmp(s, "system")) return 0;
    for(size_t i = 0; i < n; i++) if(!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return 0;
    return 1;
}
int rmt_party_place(const int *who, int n, int g, int consec, int now_who){
    int r = 1, at = 0, others = 0;
    for(int i = 0; i < n; i++){ if(who[i] == g) r++; else others = 1; }
    if(!others) return n;                                          /* only g's songs (or none): nobody to take turns with */
    for(int i = 0; i < n; i++){
        int round = 1;
        if(who[i]) for(int j = 0; j < i; j++) if(who[j] == who[i]) round++;
        if(!who[i] || round <= r) at = i + 1;
    }
    int run = 1;
    for(int i = at - 1; i >= 0 && who[i] == g; i--) run++;
    if(run == at + 1) run += now_who == g;                         /* everything before it is g's: the song playing counts */
    for(int i = at; i < n && who[i] == g; i++) run++;
    return run > consec ? -1 : at;
}
static int guest_id_ok(const char *g){
    size_t n = strlen(g);
    if(n < 8 || n > RMT_GID_MAX) return 0;
    for(size_t i = 0; i < n; i++) if(!isalnum((unsigned char)g[i])) return 0;
    return 1;
}
int rmt_upload_head(const char *buf, size_t n, const char *party, rmt_cmd_t *cmd, long long *len, size_t *hlen){
    const char *he = buf ? strstr(buf, "\r\n\r\n") : NULL;
    if(!he || !rmt_party_name_ok(party)) return 0;
    char want[RMT_PARTY_NAME_MAX + 32];
    int wl = snprintf(want, sizeof want, "POST /%s/api/upload?", party);
    if(strncmp(buf, want, (size_t)wl)) return 0;
    memset(cmd, 0, sizeof *cmd);
    char head[REMOTE_REQ_MAX];
    size_t hl = (size_t)(he - buf) + 4;
    if(hl >= sizeof head || hl > n) return -1;
    memcpy(head, buf, hl); head[hl] = 0;
    char *eol = strstr(head, "\r\n"); *eol = 0;                   /* the request line */
    char *q = head + wl, *sp = strchr(q, ' ');
    if(!sp || strncmp(sp + 1, "HTTP/1.", 7)) return -1;
    *sp = 0;
    long long cl = -1;                                             /* Content-Length, case-insensitive; no chunks */
    for(char *l = eol + 2; *l; ){
        char *e = strstr(l, "\r\n"); if(e) *e = 0;
        if(!strncasecmp(l, "content-length:", 15)){
            char *v = l + 15, *end; while(*v == ' ' || *v == '\t') v++;
            errno = 0; cl = strtoll(v, &end, 10);
            if(errno || end == v || cl < 0) return -1;
        }
        if(!strncasecmp(l, "transfer-encoding:", 18)) return -1;
        if(!e) break;
        l = e + 2;
    }
    if(cl <= 0) return -1;
    rmt_req_t r; memset(&r, 0, sizeof r); r.method = "POST"; r.path = ""; r.query = q; r.body = "";
    if(param_str(&r, "g", cmd->gid, sizeof cmd->gid) || !guest_id_ok(cmd->gid)
       || param_str(&r, "f", cmd->name, sizeof cmd->name) || !cmd->name[0]){ memset(cmd, 0, sizeof *cmd); return -1; }
    int pn = param_str(&r, "n", cmd->gname, sizeof cmd->gname);
    if(pn == -2){ memset(cmd, 0, sizeof *cmd); return -1; }
    if(pn == -1) cmd->gname[0] = 0;
    cmd->kind = RMT_CMD_PARTY_UPLOAD; cmd->guest = 1;
    *len = cl; *hlen = hl;
    return 1;
}
rmt_route_t rmt_route_party(const rmt_req_t *r, const char *party){
    rmt_route_t rt; memset(&rt, 0, sizeof rt); rt.kind = RMT_R_NOT_FOUND;
    if(!r || !r->path || r->path[0] != '/' || !rmt_party_name_ok(party)) return rt;
    const char *seg = r->path + 1, *slash = strchr(seg, '/');
    size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
    if(sl != strlen(party) || strncmp(seg, party, sl)) return rt;
    rmt_route_t a;
    if(slash && !strcmp(slash, "/api/stations")){                  /* the guest's own stations, for the host to see */
        memset(&a, 0, sizeof a);
        if(strcmp(r->method, "POST")) a.kind = RMT_R_BAD_METHOD;
        else {
            a.kind = RMT_R_CMD; a.cmd.kind = RMT_CMD_PARTY_STATIONS;
            size_t o = 0;
            for(const char *p = r->body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL){
                char v[700], *sp;                                  /* s=<url> <name> */
                if(strncmp(p, "s=", 2) || param_str_list(p, "s", v, sizeof v) || !(sp = strchr(v, ' ')) || !sp[1]) continue;
                *sp = 0;
                if(strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) continue;
                int w = snprintf(a.cmd.text + o, sizeof a.cmd.text - o, "%s\t%s\n", sp + 1, v);
                if(w < 0 || (size_t)w >= sizeof a.cmd.text - o){ a.cmd.text[o] = 0; break; }   /* the rest doesn't fit */
                o += (size_t)w;
            }
        }
    } else if(slash && !strcmp(slash, "/api/radio")){                     /* the guests' own: a station by name and URL */
        memset(&a, 0, sizeof a);
        long now = 0; int pn = param_str(r, "name", a.cmd.q, sizeof a.cmd.q), pu = param_str(r, "url", a.cmd.name, sizeof a.cmd.name);
        int pw = param(r, "now", &now);
        int ok = pn == 0 && pu == 0 && a.cmd.q[0] && (!strncmp(a.cmd.name, "http://", 7) || !strncmp(a.cmd.name, "https://", 8)) && pw != -2 && now >= 0 && now <= 1;
        if(strcmp(r->method, "POST")) a.kind = RMT_R_BAD_METHOD;
        else if(!ok) a.kind = RMT_R_BAD_PARAM;
        else { a.kind = RMT_R_CMD; a.cmd.kind = RMT_CMD_PARTY_RADIO; a.cmd.arg = now; }
        if(a.kind != RMT_R_CMD) memset(&a.cmd, 0, sizeof a.cmd);
    } else if(slash && (!strcmp(slash, "/api/game") || !strcmp(slash, "/api/game_search"))){   /* a game's players */
        memset(&a, 0, sizeof a);
        int get = !strcmp(r->method, "GET"), search = !strcmp(slash, "/api/game_search");
        a.kind = RMT_R_CMD; a.cmd.kind = search ? RMT_CMD_GAME_SEARCH : RMT_CMD_GAME;
        if(search){
            long off = 0, lim = 50; int po = param(r, "offset", &off), pl = param(r, "limit", &lim), pq = param_str(r, "q", a.cmd.q, sizeof a.cmd.q);
            if(!get) a.kind = RMT_R_BAD_METHOD;
            else if(po == -2 || pl == -2 || pq == -2 || off < 0 || lim < 1 || lim > 100) a.kind = RMT_R_BAD_PARAM;
            if(pq == -1) a.cmd.q[0] = 0;
            a.cmd.offset = off; a.cmd.limit = lim;
        } else if(!get){
            static const char *const A[] = { "join", "pick", "random", "reroll", "reveal", "mark", "confirm", "replay", "stop", "tip", "leave" };
            char act[16]; long v;
            if(strcmp(r->method, "POST")) a.kind = RMT_R_BAD_METHOD;
            else if(param_str(r, "action", act, sizeof act)) a.kind = RMT_R_BAD_PARAM;
            else {
                for(int i = 0; i < 11; i++) if(!strcmp(act, A[i])) a.cmd.sub = RMT_G_JOIN + i;
                if(!strcmp(act, "fixsong")){ a.cmd.sub = RMT_G_FIXSONG; if(rmt_fix_params(r, &a.cmd)) a.kind = RMT_R_BAD_PARAM; }
                if(!a.cmd.sub) a.kind = RMT_R_BAD_PARAM;
                if(a.cmd.sub == RMT_G_PICK){ if(param(r, "song_id", &v) || v < 1) a.kind = RMT_R_BAD_PARAM; else a.cmd.song_id = v; }
                if(a.cmd.sub == RMT_G_MARK){
                    long m; if(param(r, "p", &v) || v < 0 || v >= HG_PLAYERS || param(r, "m", &m) || m < 0 || m > 63) a.kind = RMT_R_BAD_PARAM;
                    else { a.cmd.pos = v; a.cmd.arg = m; }
                }
                if(a.cmd.sub == RMT_G_TIP){
                    int pa = param_str(r, "a", a.cmd.q, sizeof a.cmd.q), pt = param_str(r, "t", a.cmd.text, HG_TIP_MAX), py = param(r, "y", &v);
                    if(pa == -2 || pt == -2 || py == -2 || (py == 0 && (v < 0 || v > 9999))) a.kind = RMT_R_BAD_PARAM;
                    if(pa == -1) a.cmd.q[0] = 0;
                    if(pt == -1) a.cmd.text[0] = 0;
                    a.cmd.arg = py == 0 ? v : 0;
                }
            }
        }
        if(a.kind != RMT_R_CMD){ rmt_route_kind_t k = a.kind; memset(&a.cmd, 0, sizeof a.cmd); a.kind = k; }
    } else a = route_rest(r, slash);
    int ok = 0;
    switch(a.kind){
    case RMT_R_REDIRECT: case RMT_R_BAD_METHOD: case RMT_R_BAD_PARAM: ok = 1; break;
    case RMT_R_PAGE: a.kind = RMT_R_PARTY_PAGE; ok = 1; break;
    case RMT_R_STATE: a.kind = RMT_R_CMD; memset(&a.cmd, 0, sizeof a.cmd); a.cmd.kind = RMT_CMD_PARTY_STATE; ok = 1; break;
    case RMT_R_CMD:
        switch(a.cmd.kind){
        case RMT_CMD_COVER: case RMT_CMD_NEXT: case RMT_CMD_RADIO_SEARCH: case RMT_CMD_PARTY_RADIO: case RMT_CMD_PARTY_STATIONS: ok = 1; break;
        case RMT_CMD_LOOK: ok = !a.cmd.arg; break;                 /* the choices, not changing them */
        case RMT_CMD_GAME: case RMT_CMD_GAME_SEARCH: ok = 1; break;
        case RMT_CMD_VOLUME:                                       /* the guests' scale: 100 = the host's maximum */
            ok = 1; if(a.cmd.arg > 100){ memset(&a.cmd, 0, sizeof a.cmd); a.kind = RMT_R_BAD_PARAM; }
            break;
        case RMT_CMD_BANDCAMP:                                     /* search, open a page, pick a track (party.c) */
            ok = a.cmd.sub == RMT_BC_STATE || a.cmd.sub == RMT_BC_SEARCH || a.cmd.sub == RMT_BC_OPEN || (a.cmd.sub == RMT_BC_DOWNLOAD && a.cmd.pos >= 0);
            break;
        case RMT_CMD_QUEUE: ok = a.cmd.sub == RMT_Q_LIST || (a.cmd.sub == RMT_Q_ADD && (a.cmd.item == RMT_I_SONG || a.cmd.item == RMT_I_FILE)); break;
        case RMT_CMD_BROWSE:
            ok = a.cmd.sub != RMT_L_RADIO && a.cmd.sub != RMT_L_FAVORITES;   /* the whole library, not the host's favourites (guests keep their own) */
            break;
        default: break;
        }
        break;
    default: break;
    }
    if(!ok) return rt;                                             /* 404, like a path that doesn't exist */
    a.cmd.guest = 1;
    if(a.kind == RMT_R_CMD){
        int pg = param_str(r, "g", a.cmd.gid, sizeof a.cmd.gid), pn = param_str(r, "n", a.cmd.gname, sizeof a.cmd.gname);
        if(pg == -1) a.cmd.gid[0] = 0;
        if(pn == -1) a.cmd.gname[0] = 0;
        int need = (a.cmd.kind == RMT_CMD_QUEUE && a.cmd.sub == RMT_Q_ADD) || a.cmd.kind == RMT_CMD_PARTY_STATIONS   /* adding counts against the guest's songs */
                   || a.cmd.kind == RMT_CMD_BANDCAMP                                                                  /* their own search and picks */
                   || a.cmd.kind == RMT_CMD_GAME || a.cmd.kind == RMT_CMD_GAME_SEARCH;                                  /* a player is their id */
        if(pg == -2 || pn == -2 || (pg == 0 && !guest_id_ok(a.cmd.gid)) || (need && pg != 0)){
            memset(&a.cmd, 0, sizeof a.cmd); a.kind = RMT_R_BAD_PARAM; a.cmd.guest = 1; }
    }
    return a;
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
    char le[sizeof s->end_station * 12 + 96] = "null", les[sizeof s->end_station * 6], lep[sizeof s->end_playlist * 6];
    if(s->end_mode >= 0 && rmt_json_escape(les, sizeof les, s->end_station) >= 0 && rmt_json_escape(lep, sizeof lep, s->end_playlist) >= 0)
        snprintf(le, sizeof le, "{\"mode\":%d,\"station\":\"%s\",\"playlist\":\"%s\",\"pl_id\":%ld,\"pl_flags\":%d}", s->end_mode, les, lep, s->end_pl_id, s->end_pl_flags);
    for(int i = 0; i < 5; i++) if(s->out_codecs & (1 << i)) snprintf(cl + strlen(cl), sizeof cl - strlen(cl), "%s%d", cl[0] ? "," : "", i);
    if(s->out_own)
        snprintf(ou, sizeof ou, "{\"aux\":%s,\"bt\":%s,\"vol_aux\":%d,\"vol_bt\":%d,\"bt_ready\":%s,\"auto\":%d,\"jack\":%d,\"bal\":%d,\"codec\":%d,\"codecs\":[%s],\"switching\":%s}",
                 s->out_aux ? "true" : "false", s->out_bt ? "true" : "false", s->out_vol_aux, s->out_vol_bt, s->out_bt_ready ? "true" : "false",
                 s->out_auto, s->out_jack, s->out_bal, s->out_codec, cl, s->out_switching ? "true" : "false");
    int n = snprintf(dst, cap,
        "{\"ok\":true,\"has_track\":%s,\"playing\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
        "\"position_ms\":%ld,\"duration_ms\":%ld,\"volume\":%d,\"volume_max\":%d,\"balance\":%d,\"queue_ver\":%u,\"play_mode\":%d,\"work_mode\":%d,"
        "\"source\":\"%s\",\"controllable\":%s,\"radio_state\":\"%s\",\"station\":\"%s\",\"station_url\":\"%s\",\"stream_title\":\"%s\",\"bt_codec\":\"%s\",\"bt_device\":\"%s\",\"station_fav\":%s,\"radio_error\":\"%s\","
        "\"format\":\"%s\",\"kbps\":%d,\"bits\":%d,\"sample_rate\":%d,\"channels\":%d,\"favorite\":%s,\"art\":\"%s\",\"eq\":%d,\"eq_name\":\"%s\",\"outputs\":%s,\"list_end\":%s}",
        s->has_track ? "true" : "false", s->playing ? "true" : "false", t, a, al,
        s->position_ms < 0 ? 0 : s->position_ms, s->duration_ms < 0 ? 0 : s->duration_ms,
        s->volume, s->volume_max, s->balance, s->queue_ver, s->play_mode, s->work_mode, s->source ? s->source : "other", s->controllable ? "true" : "false",
        s->radio_state ? s->radio_state : "off", st, su, sti, bc, bd, s->station_fav < 0 ? "null" : s->station_fav ? "true" : "false", re,
        fm, s->kbps, s->bits, s->sample_rate, s->channels, s->favorite < 0 ? "null" : s->favorite ? "true" : "false", ar, s->eq, eqn, ou, le);
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
