/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: request parsing, routing, token and JSON helpers, and the control page. See remote_core.h. */
#include "remote_core.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char B32[] = "abcdefghijklmnopqrstuvwxyz234567";

void rc_token_encode(const unsigned char *rnd, size_t n, char *out){
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
int rc_token_valid(const char *s){
    if(!s || strlen(s) != REMOTE_TOKEN_LEN) return 0;
    for(const char *p = s; *p; p++) if(!strchr(B32, *p)) return 0;
    return 1;
}
int rc_token_eq(const char *seg, size_t n, const char *token){
    if(!rc_token_valid(token)) return 0;                      /* never accept an empty or unset token */
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
int rc_req_complete(const char *buf, int n){
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
int rc_req_parse(char *buf, rc_req_t *r){
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
        body[r->content_length] = 0;                            /* caller's buffer holds hlen + len (rc_req_complete) */
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
static int param(const rc_req_t *r, const char *name, long *out){
    int a = param_long(r->query, name, out);
    if(a != -1) return a;
    return param_long(r->body, name, out);
}

static const struct { const char *name; rc_cmd_kind_t kind; } CMDS[] = {
    { "play_pause", RC_CMD_PLAY_PAUSE }, { "play", RC_CMD_PLAY }, { "pause", RC_CMD_PAUSE },
    { "next", RC_CMD_NEXT }, { "previous", RC_CMD_PREV },
    { "volume", RC_CMD_VOLUME }, { "seek", RC_CMD_SEEK }, { "play_mode", RC_CMD_PLAY_MODE },
};
const char *rc_cmd_name(rc_cmd_kind_t k){
    if(k == RC_CMD_VOLUME_STEP) return "volume";
    for(size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++) if(CMDS[i].kind == k) return CMDS[i].name;
    return "none";
}

rc_route_t rc_route(const rc_req_t *r, const char *token){
    rc_route_t rt = { RC_R_NOT_FOUND, { RC_CMD_NONE, 0 } };
    if(!r || !r->path || r->path[0] != '/') return rt;
    const char *seg = r->path + 1;
    const char *slash = strchr(seg, '/');
    size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
    if(!rc_token_eq(seg, sl, token)) return rt;
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if(!slash){ rt.kind = get ? RC_R_REDIRECT : RC_R_BAD_METHOD; return rt; }
    const char *rest = slash;                                    /* "/" | "/api/..." */
    if(!strcmp(rest, "/")){ rt.kind = get ? RC_R_PAGE : RC_R_BAD_METHOD; return rt; }
    if(strncmp(rest, "/api/", 5)) return rt;
    const char *ep = rest + 5;
    if(!strcmp(ep, "state")){ rt.kind = get ? RC_R_STATE : RC_R_BAD_METHOD; return rt; }
    for(size_t i = 0; i < sizeof CMDS / sizeof CMDS[0]; i++){
        if(strcmp(ep, CMDS[i].name)) continue;
        if(!post){ rt.kind = RC_R_BAD_METHOD; return rt; }
        rt.cmd.kind = CMDS[i].kind; rt.kind = RC_R_CMD;
        long v = 0; int pr;
        switch(CMDS[i].kind){
        case RC_CMD_VOLUME:
            if((pr = param(r, "value", &v)) == 0){
                if(v < 0 || v > 120) rt.kind = RC_R_BAD_PARAM;
            } else if(pr == -1 && (pr = param(r, "step", &v)) == 0){
                rt.cmd.kind = RC_CMD_VOLUME_STEP;
                if(v < -120 || v > 120) rt.kind = RC_R_BAD_PARAM;
            } else rt.kind = RC_R_BAD_PARAM;
            break;
        case RC_CMD_SEEK:
            if(param(r, "position_ms", &v) || v < 0 || v > 24L * 3600 * 1000) rt.kind = RC_R_BAD_PARAM;
            break;
        case RC_CMD_PLAY_MODE:
            if(param(r, "mode", &v) || v < 0 || v > 4) rt.kind = RC_R_BAD_PARAM;
            break;
        default: break;
        }
        rt.cmd.arg = v;
        if(rt.kind == RC_R_BAD_PARAM) rt.cmd.kind = RC_CMD_NONE;
        return rt;
    }
    return rt;
}

/* ---------------------------------------------------------------- JSON */
int rc_json_escape(char *dst, size_t cap, const char *src){
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
int rc_state_json(char *dst, size_t cap, const rc_state_t *s){
    char t[sizeof s->title * 6], a[sizeof s->artist * 6], al[sizeof s->album * 6];   /* worst case: every byte \u00XX */
    if(rc_json_escape(t, sizeof t, s->title) < 0 || rc_json_escape(a, sizeof a, s->artist) < 0 ||
       rc_json_escape(al, sizeof al, s->album) < 0){ if(cap) dst[0] = 0; return -1; }
    int n = snprintf(dst, cap,
        "{\"ok\":true,\"has_track\":%s,\"playing\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
        "\"position_ms\":%ld,\"duration_ms\":%ld,\"volume\":%d,\"volume_max\":%d,\"play_mode\":%d,"
        "\"source\":\"%s\",\"controllable\":%s}",
        s->has_track ? "true" : "false", s->playing ? "true" : "false", t, a, al,
        s->position_ms < 0 ? 0 : s->position_ms, s->duration_ms < 0 ? 0 : s->duration_ms,
        s->volume, s->volume_max, s->play_mode, s->source ? s->source : "other", s->controllable ? "true" : "false");
    if(n < 0 || (size_t)n >= cap){ if(cap) dst[0] = 0; return -1; }
    return n;
}

/* ---------------------------------------------------------------- the control page */
/* One self-contained page: relative URLs (it is served at /<token>/), polls api/state once a second while visible. */
const char RC_PAGE_HTML[] =
"<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<meta name=referrer content=no-referrer><title>diskOS Remote</title>"
"<style>"
":root{--bg:#111;--fg:#eee;--mut:#999;--card:#1d1d1d;--acc:#4da3ff}"
"@media(prefers-color-scheme:light){:root{--bg:#f4f4f4;--fg:#111;--mut:#666;--card:#fff;--acc:#0a66d1}}"
"*{box-sizing:border-box}body{margin:0;font:16px/1.4 -apple-system,system-ui,sans-serif;background:var(--bg);color:var(--fg)}"
"main{max-width:28em;margin:0 auto;padding:1.2em 1em}"
"h1{font-size:1em;color:var(--mut);font-weight:600;margin:0 0 1em}"
".card{background:var(--card);border-radius:14px;padding:1em;margin-bottom:1em}"
"#t{font-size:1.3em;font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
"#a,#al{color:var(--mut);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
"input[type=range]{width:100%;accent-color:var(--acc)}"
".row{display:flex;justify-content:space-between;color:var(--mut);font-size:.85em}"
".tr{display:flex;gap:.6em;justify-content:center;margin:.6em 0}"
"button{flex:1;border:0;border-radius:12px;padding:.8em 0;font-size:1.3em;background:var(--card);color:var(--fg)}"
"button.pp{background:var(--acc);color:#fff}"
"select{width:100%;padding:.6em;border-radius:10px;font-size:1em;background:var(--card);color:var(--fg);border:1px solid var(--mut)}"
"#msg{min-height:1.4em;color:#e66;font-size:.9em}label{font-size:.85em;color:var(--mut)}"
"</style><main><h1>diskOS Remote</h1>"
"<div class=card><div id=t>-</div><div id=a></div><div id=al></div>"
"<input id=pos type=range min=0 max=1 value=0 aria-label=Position><div class=row><span id=pc>0:00</span><span id=du>0:00</span></div></div>"
"<div class=tr><button id=pv aria-label=Previous>&#9198;</button><button id=pp class=pp aria-label='Play or pause'>&#9199;</button>"
"<button id=nx aria-label=Next>&#9197;</button></div>"
"<div class=card><label for=vol>Volume <span id=vv></span></label><input id=vol type=range min=0 max=120 value=0>"
"<label for=pm>Play mode</label><select id=pm><option value=0>Sequential<option value=1>Shuffle<option value=2>Repeat one"
"<option value=3>Repeat all<option value=4>Single</select></div><div id=msg></div></main>"
"<script>"
"var $=function(i){return document.getElementById(i)},drag={},busy=0;"
"function fm(ms){var s=Math.floor(ms/1000);return Math.floor(s/60)+':'+('0'+s%60).slice(-2)}"
"function cmd(c,b){return fetch('api/'+c,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''})"
".then(function(r){return r.json().then(function(j){$('msg').textContent=j.ok?'':(j.message||'Not possible right now');setTimeout(poll,300)})})"
".catch(function(){$('msg').textContent='Player not reachable'})}"
"function poll(){if(document.hidden||busy)return;busy=1;fetch('api/state',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){busy=0;"
"$('t').textContent=s.has_track?(s.title||'-'):'Nothing playing';$('a').textContent=s.artist;$('al').textContent=s.album;"
"$('pp').innerHTML=s.playing?'&#9208;':'&#9654;';"
"if(!drag.pos){$('pos').max=Math.max(1,s.duration_ms);$('pos').value=s.position_ms}"
"$('pc').textContent=fm(s.position_ms);$('du').textContent=fm(s.duration_ms);"
"if(!drag.vol){$('vol').max=s.volume_max;$('vol').value=s.volume}$('vv').textContent=s.volume;"
"if(s.play_mode>=0&&document.activeElement!==$('pm'))$('pm').value=s.play_mode;"
"if(!s.controllable)$('msg').textContent=s.source==='other'?'The player is in another mode (e.g. USB or Bluetooth)':''"
"}).catch(function(){busy=0;$('msg').textContent='Player not reachable'})}"
"$('pp').onclick=function(){cmd('play_pause')};$('nx').onclick=function(){cmd('next')};$('pv').onclick=function(){cmd('previous')};"
"['pos','vol'].forEach(function(i){var e=$(i);e.oninput=function(){drag[i]=1;if(i==='vol')$('vv').textContent=e.value};"
"e.onchange=function(){drag[i]=0;cmd(i==='pos'?'seek':'volume',(i==='pos'?'position_ms=':'value=')+e.value)}});"
"$('pm').onchange=function(){cmd('play_mode','mode='+$('pm').value)};"
"setInterval(poll,1000);document.addEventListener('visibilitychange',poll);poll();"
"</script></html>";
