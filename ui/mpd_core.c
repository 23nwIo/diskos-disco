/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD protocol server: the pure parts. See mpd_core.h. */
#include "mpd_core.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const EV_NAMES[MPD_EV_COUNT] = { "database", "update", "stored_playlist", "playlist", "player", "mixer", "output", "options" };
const char *mpd_event_name(int bit){ return bit >= 0 && bit < MPD_EV_COUNT ? EV_NAMES[bit] : ""; }
int mpd_event_bit(const char *name){
    for(int i = 0; i < MPD_EV_COUNT; i++) if(!strcmp(name, EV_NAMES[i])) return 1 << i;
    return 0;
}

/* ---------------------------------------------------------------- lines */
int mpd_tokenize(char *line, char **argv, int max){
    int argc = 0; char *p = line;
    for(;;){
        while(*p == ' ' || *p == '\t') p++;
        if(!*p) break;
        if(argc >= max) return -2;
        if(*p == '"'){
            char *w = ++p, *start = p;
            for(;;){
                if(!*p) return -1;                                   /* unterminated */
                if(*p == '\\'){ if(!p[1]) return -1; *w++ = p[1]; p += 2; continue; }
                if(*p == '"'){ p++; break; }
                *w++ = *p++;
            }
            if(*p && *p != ' ' && *p != '\t') return -1;              /* "abc"def */
            char *after = p;
            *w = 0;
            argv[argc++] = start;
            p = after;
        } else {
            argv[argc++] = p;
            while(*p && *p != ' ' && *p != '\t'){
                if(*p == '"') return -1;                            /* a quote inside a bare word */
                p++;
            }
            if(*p) *p++ = 0;
        }
    }
    return argc;
}
void mpd_buf_quoted(rmt_buf_t *b, const char *s){
    rmt_buf_put(b, "\"");
    char tmp[2] = { 0, 0 };
    for(; s && *s; s++){
        if(*s == '"' || *s == '\\') rmt_buf_put(b, "\\");
        tmp[0] = *s; rmt_buf_put(b, tmp);
    }
    rmt_buf_put(b, "\"");
}
void mpd_ack(rmt_buf_t *out, int code, int index, const char *cmd, const char *msg){
    rmt_buf_printf(out, "ACK [%d@%d] {%s} ", code, index, cmd ? cmd : "");
    char line[512]; snprintf(line, sizeof line, "%s", msg ? msg : "");
    for(char *c = line; *c; c++) if(*c == '\n' || *c == '\r') *c = ' ';
    rmt_buf_put(out, line); rmt_buf_put(out, "\n");
}
void mpd_kv(rmt_buf_t *out, const char *key, const char *value){
    rmt_buf_put(out, key); rmt_buf_put(out, ": ");
    if(value && strpbrk(value, "\r\n")){
        size_t n = strlen(value); char *c = malloc(n + 1);
        if(!c){ out->oom = 1; return; }
        for(size_t i = 0; i <= n; i++) c[i] = value[i] == '\n' || value[i] == '\r' ? ' ' : value[i];
        rmt_buf_put(out, c); free(c);
    } else rmt_buf_put(out, value ? value : "");
    rmt_buf_put(out, "\n");
}

/* ---------------------------------------------------------------- arguments */
int mpd_parse_int(const char *s, long lo, long hi, long *out){
    if(!s || !*s) return -1;
    char *end; errno = 0; long v = strtol(s, &end, 10);
    if(errno || *end || v < lo || v > hi) return -1;
    *out = v; return 0;
}
int mpd_parse_bool(const char *s, int *out){
    if(!s || (strcmp(s, "0") && strcmp(s, "1"))) return -1;
    *out = s[0] == '1'; return 0;
}
int mpd_parse_range(const char *s, long *start, long *end){
    if(!s || !*s) return -1;
    const char *c = strchr(s, ':');
    if(!c){ long v; if(mpd_parse_int(s, 0, INT_MAX - 1, &v)) return -1; *start = v; *end = v + 1; return 0; }
    char a[24]; size_t al = (size_t)(c - s); if(al == 0 || al >= sizeof a) return -1;
    memcpy(a, s, al); a[al] = 0;
    long v; if(mpd_parse_int(a, 0, INT_MAX, &v)) return -1;
    *start = v;
    if(!c[1]){ *end = -1; return 0; }
    long e; if(mpd_parse_int(c + 1, 0, INT_MAX, &e) || e < v) return -1;
    *end = e; return 0;
}
int mpd_parse_time(const char *s, long *ms, int *relative){
    if(!s || !*s) return -1;
    int rel = 0;
    if(*s == '+'){ rel = 1; s++; } else if(*s == '-'){ rel = -1; s++; }
    if(!isdigit((unsigned char)*s) && !(*s == '.' && isdigit((unsigned char)s[1]))) return -1;
    char *end; errno = 0; double v = strtod(s, &end);
    if(errno || *end || v < 0 || v > 2e6) return -1;   /* ms must fit a 32-bit long (MIPS) */
    *ms = (long)(v * 1000.0 + 0.5);
    if(relative) *relative = rel; else if(rel) return -1;
    return 0;
}

/* ---------------------------------------------------------------- modes, volume */
void mpd_flags_from_mode(int mode, int *random, int *repeat, int *single){
    *random = mode == 1; *repeat = mode == 2 || mode == 3; *single = mode == 2 || mode == 4;
}
int mpd_mode_from_flags(int random, int repeat, int single){
    if(random) return repeat || single ? -1 : 1;
    if(repeat) return single ? 2 : 3;
    return single ? 4 : 0;
}
int mpd_vol_to_mpd(int v, int vmax){
    if(v < 0 || vmax <= 0) return -1;
    if(v > vmax) v = vmax;
    return (v * 100 + vmax / 2) / vmax;
}
int mpd_vol_from_mpd(int p, int vmax){
    if(p < 0) p = 0;
    if(p > 100) p = 100;
    return (p * vmax + 50) / 100;
}

/* ---------------------------------------------------------------- tags, filters */
static const char *const TAG_NAMES[MPD_TAG_COUNT] = { "any", "file", "base", "Title", "Artist", "Album", "AlbumArtist", "Genre", "Track", "Disc", "Date", "Name" };
const char *mpd_tag_name(int tag){ return tag >= 0 && tag < MPD_TAG_COUNT ? TAG_NAMES[tag] : ""; }
int mpd_tag_from_name(const char *n){
    if(!n) return -1;
    for(int i = 0; i < MPD_TAG_COUNT; i++) if(!strcasecmp(n, TAG_NAMES[i])) return i;
    if(!strcasecmp(n, "ArtistSort") || !strcasecmp(n, "Performer") || !strcasecmp(n, "Composer")) return MPD_TAG_ARTIST;
    if(!strcasecmp(n, "AlbumArtistSort")) return MPD_TAG_ALBUMARTIST;
    if(!strcasecmp(n, "AlbumSort")) return MPD_TAG_ALBUM;
    if(!strcasecmp(n, "TitleSort")) return MPD_TAG_TITLE;
    if(!strcasecmp(n, "OriginalDate")) return MPD_TAG_DATE;
    return -1;
}
static const char *skip_ws(const char *p){ while(*p == ' ' || *p == '\t') p++; return p; }
/* a quoted string in an expression: '...' or "..." with backslash escapes */
static const char *expr_string(const char *p, char *out, size_t cap){
    char q = *p;
    if(q != '"' && q != '\'') return NULL;
    p++; size_t n = 0;
    while(*p && *p != q){
        if(*p == '\\'){ p++; if(!*p) return NULL; }
        if(n + 1 >= cap) return NULL;
        out[n++] = *p++;
    }
    if(*p != q) return NULL;
    out[n] = 0; return p + 1;
}
static const char *expr_word(const char *p, char *out, size_t cap){
    size_t n = 0;
    while(*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-')){ if(n + 1 >= cap) return NULL; out[n++] = *p++; }
    if(!n) return NULL;
    out[n] = 0; return p;
}
/* one parenthesised expression: (TAG OP 'v') | (base 'v') | (!EXPR) | (EXPR AND EXPR ...) */
static const char *expr_parse(mpd_filter_t *f, const char *p, int neg, int depth, const char **err){
    if(depth > 8){ *err = "Expression too deep"; return NULL; }
    p = skip_ws(p);
    if(*p != '('){ *err = "Expected ("; return NULL; }
    p = skip_ws(p + 1);
    if(*p == '!'){
        p = expr_parse(f, p + 1, !neg, depth + 1, err); if(!p) return NULL;
        p = skip_ws(p);
        if(*p != ')'){ *err = "Expected )"; return NULL; }
        return p + 1;
    }
    if(*p == '('){
        if(neg){ *err = "Negating a combined expression is not supported"; return NULL; }   /* !(a AND b) = OR: no OR here */
        for(;;){
            p = expr_parse(f, p, 0, depth + 1, err); if(!p) return NULL;
            p = skip_ws(p);
            if(*p == ')') return p + 1;
            char w[8]; const char *q = expr_word(p, w, sizeof w);
            if(!q || strcmp(w, "AND")){ *err = "Only AND is supported"; return NULL; }
            p = skip_ws(q);
        }
    }
    char tag[32]; const char *q = expr_word(p, tag, sizeof tag);
    if(!q){ *err = "Expected a tag"; return NULL; }
    if(f->n >= MPD_FILTER_MAX){ *err = "Too many terms"; return NULL; }
    mpd_term_t *t = &f->t[f->n];
    memset(t, 0, sizeof *t);
    t->neg = neg;
    if(!strcasecmp(tag, "base")){
        t->tag = MPD_TAG_BASE; t->op = MPD_OP_STARTS;
        q = expr_string(skip_ws(q), t->value, sizeof t->value);
        if(!q){ *err = "Bad string"; return NULL; }
    } else if(!strcasecmp(tag, "modified-since") || !strcasecmp(tag, "added-since") || !strcasecmp(tag, "AudioFormat")
              || !strcasecmp(tag, "prio")){
        *err = "Unsupported filter"; return NULL;
    } else {
        int tg = mpd_tag_from_name(tag);
        if(tg < 0){ *err = "Unknown filter type"; return NULL; }
        t->tag = tg;
        q = skip_ws(q);
        char op[16]; size_t on = 0;
        if(*q == '=' || *q == '!'){ while(*q && strchr("=!~", *q) && on < 3) op[on++] = *q++; op[on] = 0; }
        else { const char *e = expr_word(q, op, sizeof op); if(!e){ *err = "Expected an operator"; return NULL; } q = e; }
        if(!strcmp(op, "==")) t->op = MPD_OP_EQ;
        else if(!strcmp(op, "!=")) t->op = MPD_OP_NE;
        else if(!strcmp(op, "contains")) t->op = MPD_OP_CONTAINS;
        else if(!strcmp(op, "starts_with")) t->op = MPD_OP_STARTS;
        else if(!strcmp(op, "=~") || !strcmp(op, "!~")){ *err = "Regular expressions are not supported"; return NULL; }
        else { *err = "Unknown operator"; return NULL; }
        q = expr_string(skip_ws(q), t->value, sizeof t->value);
        if(!q){ *err = "Bad string"; return NULL; }
    }
    f->n++;
    q = skip_ws(q);
    if(*q != ')'){ *err = "Expected )"; return NULL; }
    return q + 1;
}
int mpd_filter_parse(mpd_filter_t *f, int argc, char **argv, int fold, const char **err){
    memset(f, 0, sizeof *f);
    f->fold = fold;
    *err = NULL;
    if(argc <= 0){ *err = "Filter missing"; return -1; }
    if(argc == 1 && argv[0][0] == '('){
        const char *e = expr_parse(f, argv[0], 0, 0, err);
        if(!e) return -1;
        if(*skip_ws(e)){ *err = "Trailing text after the expression"; return -1; }
        return 0;
    }
    if(argc % 2){ *err = "Incorrect number of filter arguments"; return -1; }
    for(int i = 0; i < argc; i += 2){
        if(f->n >= MPD_FILTER_MAX){ *err = "Too many terms"; return -1; }
        mpd_term_t *t = &f->t[f->n];
        memset(t, 0, sizeof *t);
        int tg = mpd_tag_from_name(argv[i]);
        if(tg < 0){ *err = "Unknown filter type"; return -1; }
        t->tag = tg;
        t->op = tg == MPD_TAG_BASE ? MPD_OP_STARTS : fold ? MPD_OP_CONTAINS : MPD_OP_EQ;
        if(strlen(argv[i + 1]) >= sizeof t->value){ *err = "Value too long"; return -1; }
        snprintf(t->value, sizeof t->value, "%s", argv[i + 1]);
        f->n++;
    }
    return 0;
}
static int fold_eq(const char *a, const char *b, int fold){ return fold ? !strcasecmp(a, b) : !strcmp(a, b); }
static int has_sub(const char *hay, const char *needle, int fold){
    if(!*needle) return 1;
    if(!fold) return strstr(hay, needle) != NULL;
    size_t nl = strlen(needle);
    for(; *hay; hay++) if(!strncasecmp(hay, needle, nl)) return 1;
    return 0;
}
static int starts(const char *s, const char *p, int fold){ size_t n = strlen(p); return fold ? !strncasecmp(s, p, n) : !strncmp(s, p, n); }
static int term_value(const mpd_term_t *t, const char *v, int fold){
    switch(t->op){
    case MPD_OP_EQ: return fold_eq(v, t->value, fold);
    case MPD_OP_NE: return !fold_eq(v, t->value, fold);
    case MPD_OP_CONTAINS: return has_sub(v, t->value, fold);
    case MPD_OP_STARTS: return starts(v, t->value, fold);
    }
    return 0;
}
static int term_match(const mpd_term_t *t, const mpd_tags_t *s, int fold){
    if(t->tag == MPD_TAG_BASE){                              /* a directory: the song is inside it */
        const char *file = s->v[MPD_TAG_FILE] ? s->v[MPD_TAG_FILE] : "";
        size_t n = strlen(t->value);
        while(n && t->value[n - 1] == '/') n--;
        if(n == 0) return 1;
        return !strncmp(file, t->value, n) && file[n] == '/';
    }
    if(t->tag == MPD_TAG_ANY){
        if(t->op == MPD_OP_NE){                               /* != on "any": no tag equals the value */
            for(int i = MPD_TAG_FILE; i < MPD_TAG_COUNT; i++) if(i != MPD_TAG_BASE && s->v[i] && fold_eq(s->v[i], t->value, fold)) return 0;
            return 1;
        }
        for(int i = MPD_TAG_FILE; i < MPD_TAG_COUNT; i++) if(i != MPD_TAG_BASE && s->v[i] && term_value(t, s->v[i], fold)) return 1;
        return 0;
    }
    const char *v = s->v[t->tag] ? s->v[t->tag] : "";
    return term_value(t, v, fold);
}
int mpd_filter_match(const mpd_filter_t *f, const mpd_tags_t *s){
    for(int i = 0; i < f->n; i++){
        int m = term_match(&f->t[i], s, f->fold);
        if(f->t[i].neg) m = !m;
        if(!m) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- sessions */
void mpd_sess_init(mpd_sess_t *s, int authed){ memset(s, 0, sizeof *s); s->authed = authed; }
void mpd_sess_free(mpd_sess_t *s){ free(s->batch.p); memset(&s->batch, 0, sizeof s->batch); }
static void batch_reset(mpd_sess_t *s){
    free(s->batch.p); memset(&s->batch, 0, sizeof s->batch);
    s->list = 0; s->nlist = 0; s->list_bad = 0;
}

const char *const MPD_SESSION_COMMANDS[] = { "binarylimit", "close", "command_list_begin", "command_list_end", "command_list_ok_begin",
    "idle", "noidle", "password", "ping", "tagtypes", NULL };
static const char *const TAGTYPES[] = { "Artist", "Album", "AlbumArtist", "Title", "Track", "Name", "Genre", "Date", "Disc", NULL };
/* without the password only these */
static int allowed_unauthed(const char *cmd){
    return !strcmp(cmd, "password") || !strcmp(cmd, "ping") || !strcmp(cmd, "close") || !strcmp(cmd, "commands") || !strcmp(cmd, "notcommands");
}
static int password_ok(const char *given, const char *password){
    if(!password) return 1;
    return rmt_token_eq(given, strlen(given), password);
}
/* Commands that never need the UI thread. 1 = handled (answer lines or an ACK code in *code), 0 = not ours. */
static int session_cmd(mpd_sess_t *s, int argc, char **argv, const char *password, rmt_buf_t *out, int *code, const char **msg){
    const char *c = argv[0];
    *code = 0;
    if(!strcmp(c, "ping")) return 1;
    if(!strcmp(c, "password")){
        if(argc != 2){ *code = MPD_ACK_ARG; *msg = "wrong number of arguments for \"password\""; return 1; }
        if(!password_ok(argv[1], password)){ *code = MPD_ACK_PASSWORD; *msg = "incorrect password"; return 1; }
        s->authed = 1; return 1;
    }
    if(!strcmp(c, "binarylimit")){
        long v; if(argc != 2 || mpd_parse_int(argv[1], 64, 1 << 24, &v)){ *code = MPD_ACK_ARG; *msg = "Bad binary limit"; }
        return 1;
    }
    if(!strcmp(c, "tagtypes")){
        if(argc == 1){ for(int i = 0; TAGTYPES[i]; i++) mpd_kv(out, "tagtype", TAGTYPES[i]); return 1; }
        if(!strcmp(argv[1], "clear") || !strcmp(argv[1], "all") || !strcmp(argv[1], "enable") || !strcmp(argv[1], "disable") || !strcmp(argv[1], "reset"))
            return 1;                                          /* accepted; every answer carries all our tags */
        *code = MPD_ACK_ARG; *msg = "Unknown sub command"; return 1;
    }
    return 0;
}
int mpd_sess_line(mpd_sess_t *s, char *line, const char *password, rmt_buf_t *out){
    size_t ln = strlen(line);
    if(ln && line[ln - 1] == '\r') line[--ln] = 0;           /* telnet-style clients */
    if(s->idle){
        if(!strcmp(line, "noidle")) return MPD_A_NOIDLE;
        return MPD_A_CLOSE;                                  /* MPD drops a client that talks during idle */
    }
    if(s->list){
        if(!strcmp(line, "command_list_end")) return MPD_A_RUN;
        if(s->list_bad) return MPD_A_NONE;
        if(s->nlist >= MPD_LIST_MAX || s->batch.n + ln + 1 > (1u << 20)){ s->list_bad = 1; return MPD_A_NONE; }
        rmt_buf_put(&s->batch, line); rmt_buf_put(&s->batch, "\n");
        if(s->batch.oom) s->list_bad = 1;
        s->nlist++;
        return MPD_A_NONE;
    }
    if(!strcmp(line, "command_list_begin") || !strcmp(line, "command_list_ok_begin")){
        batch_reset(s);
        s->list = line[13] == 'o' ? 2 : 1;
        return MPD_A_NONE;
    }
    char copy[MPD_LINE_MAX]; snprintf(copy, sizeof copy, "%s", line);
    char *argv[MPD_ARGS_MAX];
    int argc = mpd_tokenize(copy, argv, MPD_ARGS_MAX);
    if(argc < 0){ mpd_ack(out, MPD_ACK_ARG, 0, "", argc == -2 ? "Too many arguments" : "Invalid unquoted character"); return MPD_A_REPLY; }
    if(argc == 0){ mpd_ack(out, MPD_ACK_UNKNOWN, 0, "", "No command given"); return MPD_A_REPLY; }
    if(!strcmp(argv[0], "close")) return MPD_A_CLOSE;
    if(!strcmp(argv[0], "noidle")) return MPD_A_NONE;          /* not idle: ignored, as MPD does */
    if(!s->authed && !allowed_unauthed(argv[0])){
        char m[128]; snprintf(m, sizeof m, "you don't have permission for \"%.60s\"", argv[0]);
        mpd_ack(out, MPD_ACK_PERMISSION, 0, argv[0], m); return MPD_A_REPLY;
    }
    if(!strcmp(argv[0], "idle")){
        unsigned mask = 0;
        for(int i = 1; i < argc; i++){
            int b = mpd_event_bit(argv[i]);
            if(!b){ mpd_ack(out, MPD_ACK_ARG, 0, "idle", "Unrecognized idle event"); return MPD_A_REPLY; }
            mask |= (unsigned)b;
        }
        s->idle_mask = mask ? mask : MPD_EV_ALL; s->idle = 1;
        return MPD_A_IDLE;
    }
    int code; const char *msg = NULL;
    if(session_cmd(s, argc, argv, password, out, &code, &msg)){
        if(code) mpd_ack(out, code, 0, argv[0], msg); else rmt_buf_put(out, "OK\n");
        return MPD_A_REPLY;
    }
    batch_reset(s);
    rmt_buf_put(&s->batch, line); rmt_buf_put(&s->batch, "\n");
    if(s->batch.oom){ batch_reset(s); mpd_ack(out, MPD_ACK_SYSTEM, 0, argv[0], "Out of memory"); return MPD_A_REPLY; }
    return MPD_A_RUN;
}
void mpd_run_batch(mpd_sess_t *s, const char *password, mpd_handler_t h, void *ud, rmt_buf_t *out){
    int list = s->list;
    if(s->list_bad){ mpd_ack(out, MPD_ACK_SYSTEM, s->nlist, "", "Command list too long"); batch_reset(s); return; }
    char *text = s->batch.p ? s->batch.p : (char *)"";
    int idx = 0, failed = 0;
    for(char *line = text; *line && !failed; idx++){
        char *nl = strchr(line, '\n'); if(nl) *nl = 0;
        char *argv[MPD_ARGS_MAX];
        int argc = mpd_tokenize(line, argv, MPD_ARGS_MAX);
        int code = 0; const char *msg = NULL; const char *cmd = argc > 0 ? argv[0] : "";
        if(argc < 0){ code = MPD_ACK_ARG; msg = argc == -2 ? "Too many arguments" : "Invalid unquoted character"; }
        else if(argc == 0){ code = MPD_ACK_UNKNOWN; msg = "No command given"; }
        else if(!s->authed && !allowed_unauthed(cmd)){ code = MPD_ACK_PERMISSION; msg = "you don't have permission"; }
        else if(!strcmp(cmd, "idle") || !strcmp(cmd, "noidle") || !strcmp(cmd, "close") || !strncmp(cmd, "command_list", 12)){
            code = MPD_ACK_UNKNOWN; msg = "not allowed in a command list";
        }
        else if(!session_cmd(s, argc, argv, password, out, &code, &msg)){
            code = h(ud, argc, argv, out, &msg, s);
            if(code && !msg) msg = "Failed";
        }
        if(code){ mpd_ack(out, code, list ? idx : 0, cmd, msg); failed = 1; break; }
        if(list == 2) rmt_buf_put(out, "list_OK\n");
        if(out->n > MPD_OUT_MAX || out->oom){ out->n = 0; out->oom = 0; if(out->p) out->p[0] = 0; mpd_ack(out, MPD_ACK_SYSTEM, list ? idx : 0, cmd, "Answer too large"); failed = 1; break; }
        if(!nl) break;
        line = nl + 1;
    }
    if(!failed) rmt_buf_put(out, "OK\n");
    batch_reset(s);
}
