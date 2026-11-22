/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Bandcamp, the pure part: see bandcamp_core.h. */
#include "bandcamp_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define JSMN_STATIC                 /* a private strict copy, as radio_core.c */
#define JSMN_STRICT
#include "jsmn.h"

/* ================================================================ URLs */
int bc_url_ok(const char *u){
    if(!u || strncmp(u, "https://", 8)) return 0;
    size_t n = strlen(u);
    if(n <= 8 || n >= BC_URL_LEN || u[8] == '/') return 0;
    for(size_t i = 0; i < n; i++){
        unsigned char c = (unsigned char)u[i];
        if(c <= ' ' || c >= 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' || c == '`') return 0;
    }
    return 1;
}
int bc_url_base(const char *url, char *out, size_t n){
    if(!n) return -1;
    out[0] = 0;
    if(!url || strncmp(url, "https://", 8)) return -1;
    const char *e = url + 8 + strcspn(url + 8, "/?#");
    size_t k = (size_t)(e - url);
    if(k <= 8 || k >= n) return -1;
    memcpy(out, url, k); out[k] = 0;
    return 0;
}
int bc_url_join(const char *base, const char *path, char *out, size_t n){
    if(!n) return -1;
    out[0] = 0;
    if(!path || !path[0]) return -1;
    char b[BC_URL_LEN];
    if(!strncmp(path, "https://", 8)) snprintf(b, sizeof b, "%s", path);
    else if(!strncmp(path, "http://", 7)) snprintf(b, sizeof b, "https://%s", path + 7);   /* Bandcamp serves https */
    else if(path[0] == '/' && path[1] != '/' && base && !strncmp(base, "https://", 8)){
        char h[BC_URL_LEN];
        if(bc_url_base(base, h, sizeof h)) return -1;
        if(snprintf(b, sizeof b, "%s%s", h, path) >= (int)sizeof b) return -1;
    } else return -1;
    b[strcspn(b, "?#")] = 0;
    if(!bc_url_ok(b) || strlen(b) >= n) return -1;
    memcpy(out, b, strlen(b) + 1);
    return 0;
}

/* ================================================================ JSON helpers */
static int hexv(int c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void put_utf8(char *d, size_t n, size_t *o, unsigned cp){
    char b[4]; int k;
    if(cp < 0x80){ b[0] = (char)cp; k = 1; }
    else if(cp < 0x800){ b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if(cp < 0x10000){ b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else if(cp < 0x110000){ b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4; }
    else return;
    if(*o + (size_t)k >= n) return;
    memcpy(d + *o, b, (size_t)k); *o += (size_t)k;
}
/* the string value, unescaped, control characters as spaces; a primitive as it is ("null" -> ""); "" otherwise */
static void tok_str(const char *js, const jsmntok_t *t, char *d, size_t n){
    size_t o = 0;
    if(!n) return;
    if(t->type == JSMN_STRING){
        for(int i = t->start; i < t->end && o + 1 < n; i++){
            char c = js[i];
            if(c != '\\'){ d[o++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c; continue; }
            if(++i >= t->end) break;
            switch(js[i]){
            case 'n': case 't': case 'r': d[o++] = ' '; break;
            case 'b': case 'f': break;
            case 'u': {
                if(i + 4 >= t->end){ i = t->end; break; }
                unsigned cp = 0; int ok = 1;
                for(int k = 1; k <= 4; k++){ int h = hexv(js[i + k]); if(h < 0) ok = 0; cp = cp << 4 | (unsigned)(h < 0 ? 0 : h); }
                i += 4;
                if(!ok) break;
                if(cp >= 0xD800 && cp < 0xDC00 && i + 6 < t->end && js[i + 1] == '\\' && js[i + 2] == 'u'){
                    unsigned lo = 0; ok = 1;
                    for(int k = 3; k <= 6; k++){ int h = hexv(js[i + k]); if(h < 0) ok = 0; lo = lo << 4 | (unsigned)(h < 0 ? 0 : h); }
                    if(ok && lo >= 0xDC00 && lo < 0xE000){ cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    else cp = 0xFFFD;
                } else if(cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                if(cp >= 0x20) put_utf8(d, n, &o, cp);
            } break;
            default: d[o++] = js[i];
            }
        }
    } else if(t->type == JSMN_PRIMITIVE && !(t->end - t->start == 4 && !strncmp(js + t->start, "null", 4))){
        for(int i = t->start; i < t->end && o + 1 < n; i++) d[o++] = js[i];
    }
    d[o] = 0;
}
/* the index just past token i and everything inside it */
static int tok_skip(const jsmntok_t *t, int nt, int i){
    int end = i + 1;
    for(int left = t[i].size; left > 0 && end < nt; left--) end = tok_skip(t, nt, end);
    return end;
}
static int tok_is(const char *js, const jsmntok_t *t, const char *s){
    size_t n = strlen(s);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == n && !strncmp(js + t->start, s, n);
}
/* the value token of key in the object at index obj, -1 none */
static int obj_get(const char *js, const jsmntok_t *t, int nt, int obj, const char *key){
    if(obj < 0 || obj >= nt || t[obj].type != JSMN_OBJECT) return -1;
    int k = obj + 1;
    for(int i = 0; i < t[obj].size && k + 1 < nt; i++){
        if(tok_is(js, &t[k], key)) return k + 1;
        k = tok_skip(t, nt, k + 1);
    }
    return -1;
}
static void get_str(const char *js, const jsmntok_t *t, int nt, int obj, const char *key, char *d, size_t n){
    int v = obj_get(js, t, nt, obj, key);
    if(v < 0){ if(n) d[0] = 0; return; }
    tok_str(js, &t[v], d, n);
}
/* parse js into a malloc'd token array; returns the count, -1 */
static int parse(const char *js, size_t len, jsmntok_t **out){
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, js, len, NULL, 0);
    if(nt <= 0){ *out = NULL; return -1; }
    jsmntok_t *t = malloc(sizeof *t * (size_t)nt);
    if(!t){ *out = NULL; return -1; }
    jsmn_init(&p);
    if(jsmn_parse(&p, js, len, t, (unsigned)nt) != nt){ free(t); *out = NULL; return -1; }
    *out = t;
    return nt;
}
static void trim(char *s){
    size_t n = strlen(s);
    while(n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t k = 0; while(isspace((unsigned char)s[k])) k++;
    if(k) memmove(s, s + k, n - k + 1);
}
/* runs of white space as one space, trimmed */
static void squash(char *s){
    char *w = s; int sp = 0;
    for(char *r = s; *r; r++){
        if(isspace((unsigned char)*r)){ sp = 1; continue; }
        if(sp && w != s) *w++ = ' ';
        sp = 0; *w++ = *r;
    }
    *w = 0;
}

/* ================================================================ search */
int bc_search_body(const char *q, char *out, size_t n){
    size_t o = 0;
    const char *head = "{\"search_text\":\"", *tail = "\",\"search_filter\":\"\",\"full_page\":false,\"fan_id\":null}";
    if(strlen(head) >= n) return -1;
    memcpy(out, head, strlen(head)); o = strlen(head);
    for(const unsigned char *p = (const unsigned char *)(q ? q : ""); *p; p++){
        char e[8]; int k;
        if(*p == '"' || *p == '\\'){ e[0] = '\\'; e[1] = (char)*p; k = 2; }
        else if(*p < 0x20){ k = snprintf(e, sizeof e, "\\u%04x", *p); }
        else { e[0] = (char)*p; k = 1; }
        if(o + (size_t)k >= n) return -1;
        memcpy(out + o, e, (size_t)k); o += (size_t)k;
    }
    if(o + strlen(tail) >= n) return -1;
    memcpy(out + o, tail, strlen(tail) + 1);
    return (int)(o + strlen(tail));
}
int bc_parse_search(const char *json, size_t len, bc_item_t *out, int max){
    jsmntok_t *t; int nt = json ? parse(json, len, &t) : -1;
    if(nt <= 0) return -1;
    int got = -1;
    int a = obj_get(json, t, nt, 0, "auto");
    int r = obj_get(json, t, nt, a, "results");
    if(r >= 0 && t[r].type == JSMN_ARRAY){
        got = 0;
        int k = r + 1;
        for(int i = 0; i < t[r].size && k < nt; i++, k = tok_skip(t, nt, k)){
            if(t[k].type != JSMN_OBJECT || got >= max) continue;
            char type[4]; get_str(json, t, nt, k, "type", type, sizeof type);
            if(strcmp(type, "b") && strcmp(type, "a") && strcmp(type, "t")) continue;   /* fans: no music of their own */
            bc_item_t it; memset(&it, 0, sizeof it);
            it.type = type[0];
            char u[BC_URL_LEN];
            get_str(json, t, nt, k, it.type == 'b' ? "item_url_root" : "item_url_path", u, sizeof u);
            if(bc_url_join(NULL, u, it.url, sizeof it.url)) continue;
            get_str(json, t, nt, k, "name", it.name, sizeof it.name); trim(it.name);
            if(!it.name[0]) continue;
            if(it.type != 'b') get_str(json, t, nt, k, "band_name", it.artist, sizeof it.artist);
            if(it.type == 't') get_str(json, t, nt, k, "album_name", it.album, sizeof it.album);
            get_str(json, t, nt, k, "img", u, sizeof u);
            if(!bc_url_ok(u)) u[0] = 0;
            snprintf(it.art, sizeof it.art, "%s", u);
            out[got++] = it;
        }
    }
    free(t);
    return got;
}

/* ================================================================ HTML */
void bc_entities(char *s){
    char *w = s;
    for(char *r = s; *r; ){
        if(*r != '&'){ *w++ = *r++; continue; }
        char *semi = strchr(r, ';');
        size_t k = semi ? (size_t)(semi - r) : 0;
        if(!semi || k < 2 || k > 9){ *w++ = *r++; continue; }
        unsigned cp = 0; int ok = 1;
        if(r[1] == '#'){
            char *e; long v = (r[2] == 'x' || r[2] == 'X') ? strtol(r + 3, &e, 16) : strtol(r + 2, &e, 10);
            if(e != semi || v <= 0 || v > 0x10FFFF) ok = 0; else cp = (unsigned)v;
        }
        else if(!strncmp(r, "&quot;", 6)) cp = '"';
        else if(!strncmp(r, "&amp;", 5)) cp = '&';
        else if(!strncmp(r, "&lt;", 4)) cp = '<';
        else if(!strncmp(r, "&gt;", 4)) cp = '>';
        else if(!strncmp(r, "&apos;", 6)) cp = '\'';
        else if(!strncmp(r, "&nbsp;", 6)) cp = ' ';
        else ok = 0;
        if(!ok){ *w++ = *r++; continue; }
        size_t o = 0; char b[5];
        put_utf8(b, sizeof b, &o, cp);
        memcpy(w, b, o); w += o;            /* never longer than the entity it replaces */
        r = semi + 1;
    }
    *w = 0;
}
char *bc_html_attr(const char *html, const char *name){
    if(!html || !name) return NULL;
    size_t nl = strlen(name);
    for(const char *p = strstr(html, name); p; p = strstr(p + 1, name)){
        if(p > html && !isspace((unsigned char)p[-1])) continue;
        const char *q = p + nl;
        if(q[0] != '=' || (q[1] != '"' && q[1] != '\'')) continue;
        char quote = q[1];
        const char *v = q + 2, *e = strchr(v, quote);
        if(!e) return NULL;
        char *out = malloc((size_t)(e - v) + 1);
        if(!out) return NULL;
        memcpy(out, v, (size_t)(e - v)); out[e - v] = 0;
        bc_entities(out);
        return out;
    }
    return NULL;
}
static void art_url_size(long long id, int size, char *out, size_t n){
    if(id > 0) snprintf(out, n, "https://f4.bcbits.com/img/a%010lld_%d.jpg", id, size);   /* 2 = 350 px, 10 = 1200 px */
    else if(n) out[0] = 0;
}
static void art_url(long long id, char *out, size_t n){ art_url_size(id, 2, out, n); }
/* "27 Mar 2022 00:00:00 GMT" -> "2022-03-27"; "" when it isn't one */
static void bc_date(const char *in, char *out, size_t n){
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d = 0, y = 0; char m[4] = "";
    if(n) out[0] = 0;
    if(sscanf(in, "%2d %3s %4d", &d, m, &y) != 3 || d < 1 || d > 31 || y < 1000 || y > 2999) return;
    const char *p = strstr(mon, m);
    if(!p || strlen(m) != 3 || (p - mon) % 3) return;
    snprintf(out, n, "%04u-%02u-%02u", (unsigned)y % 10000u, (unsigned)(p - mon) / 3u % 100u + 1u, (unsigned)d % 100u);
}
/* the JSON-LD block's inAlbum name (a track page's album), "" none */
static void ld_album(const char *html, char *out, size_t n){
    if(n) out[0] = 0;
    const char *s = strstr(html, "application/ld+json");
    if(!s || !(s = strchr(s, '>'))) return;
    const char *e = strstr(++s, "</script>");
    if(!e) return;
    jsmntok_t *t; int nt = parse(s, (size_t)(e - s), &t);
    if(nt <= 0) return;
    int a = obj_get(s, t, nt, 0, "inAlbum");
    if(a >= 0) get_str(s, t, nt, a, "name", out, n);
    free(t);
}
int bc_parse_release(const char *html, bc_release_t *r, bc_track_t *out, int max){
    memset(r, 0, sizeof *r);
    char *js = bc_html_attr(html, "data-tralbum");
    if(!js) return -1;
    jsmntok_t *t; int nt = parse(js, strlen(js), &t);
    if(nt <= 0){ free(js); return -1; }
    int got = -1;
    int cur = obj_get(js, t, nt, 0, "current"), ti = obj_get(js, t, nt, 0, "trackinfo");
    char v[BC_URL_LEN];
    get_str(js, t, nt, 0, "url", v, sizeof v);
    if(cur >= 0 && ti >= 0 && t[ti].type == JSMN_ARRAY && !bc_url_join(NULL, v, r->url, sizeof r->url)){
        get_str(js, t, nt, cur, "type", v, sizeof v);
        r->type = !strcmp(v, "track") ? 't' : 'a';
        get_str(js, t, nt, cur, "title", r->title, sizeof r->title); trim(r->title);
        get_str(js, t, nt, 0, "artist", r->artist, sizeof r->artist); trim(r->artist);
        get_str(js, t, nt, 0, "art_id", v, sizeof v);
        art_url(atoll(v), r->art, sizeof r->art);
        art_url_size(atoll(v), 10, r->cover, sizeof r->cover);
        get_str(js, t, nt, cur, "release_date", v, sizeof v); bc_date(v, r->date, sizeof r->date);
        if(!r->date[0]){ get_str(js, t, nt, 0, "album_release_date", v, sizeof v); bc_date(v, r->date, sizeof r->date); }
        get_str(js, t, nt, 0, "freeDownloadPage", v, sizeof v);
        if(bc_url_ok(v)) snprintf(r->free_dl, sizeof r->free_dl, "%s", v);
        if(r->type == 't') ld_album(html, r->album, sizeof r->album);
        else snprintf(r->album, sizeof r->album, "%s", r->title);
        got = 0;
        int k = ti + 1;
        for(int i = 0; i < t[ti].size && k < nt; i++, k = tok_skip(t, nt, k)){
            if(t[k].type != JSMN_OBJECT) continue;
            r->ntracks++;
            if(got >= max) continue;
            bc_track_t *x = &out[got];
            memset(x, 0, sizeof *x);
            get_str(js, t, nt, k, "title", x->title, sizeof x->title); trim(x->title);
            get_str(js, t, nt, k, "artist", x->artist, sizeof x->artist); trim(x->artist);
            if(!x->artist[0]) snprintf(x->artist, sizeof x->artist, "%s", r->artist);
            get_str(js, t, nt, k, "duration", v, sizeof v);
            double d = atof(v); x->ms = d > 0 && d < 86400 ? (long)(d * 1000.0 + 0.5) : 0;
            get_str(js, t, nt, k, "track_num", v, sizeof v); x->num = atoi(v);
            get_str(js, t, nt, k, "title_link", v, sizeof v);
            if(bc_url_join(r->url, v, x->page, sizeof x->page)) x->page[0] = 0;
            int f = obj_get(js, t, nt, k, "file");
            if(f >= 0){ get_str(js, t, nt, f, "mp3-128", v, sizeof v); if(bc_url_ok(v)) snprintf(x->stream, sizeof x->stream, "%s", v); }
            got++;
        }
    }
    free(t); free(js);
    return got;
}
/* "<tag ...>" starting at p: the text up to the matching close "</tag" (no nesting of the same tag), entity-decoded */
static void text_between(const char *p, const char *close, char *out, size_t n){
    if(n) out[0] = 0;
    const char *s = strchr(p, '>');
    if(!s) return;
    const char *e = strstr(++s, close);
    if(!e) return;
    size_t o = 0; int in_tag = 0;
    for(const char *c = s; c < e && o + 1 < n; c++){       /* drop inner tags (<br>, <span>): their text stays */
        if(*c == '<'){ in_tag = 1; if(o + 1 < n) out[o++] = ' '; continue; }
        if(*c == '>'){ in_tag = 0; continue; }
        if(!in_tag) out[o++] = *c;
    }
    out[o] = 0;
    bc_entities(out); squash(out);
}
static int have_url(const bc_item_t *it, int n, const char *u){ for(int i = 0; i < n; i++) if(!strcmp(it[i].url, u)) return 1; return 0; }
int bc_parse_music(const char *html, const char *base, bc_item_t *out, int max, bc_release_t *one){
    char artist[BC_NAME_LEN] = "";
    {   /* the page's own name: <p id="band-name-location"><span class="title">Aphex Twin</span> */
        const char *b = strstr(html, "id=\"band-name-location\"");
        const char *s = b ? strstr(b, "class=\"title\"") : NULL;
        if(s) text_between(s, "</span>", artist, sizeof artist);
    }
    int got = 0;
    for(const char *li = strstr(html, "data-item-id=\""); li && got < max; li = strstr(li + 1, "data-item-id=\"")){
        const char *id = li + 14;
        if(strncmp(id, "album-", 6) && strncmp(id, "track-", 6)) continue;
        const char *end = strstr(li, "</li>");
        if(!end) break;
        bc_item_t it; memset(&it, 0, sizeof it);
        it.type = id[0] == 't' ? 't' : 'a';
        const char *a = strstr(li, "<a href=\"");
        if(!a || a > end) continue;
        char href[BC_URL_LEN]; size_t k = strcspn(a + 9, "\"");
        if(k >= sizeof href) continue;
        memcpy(href, a + 9, k); href[k] = 0; bc_entities(href);
        if(bc_url_join(base, href, it.url, sizeof it.url) || have_url(out, got, it.url)) continue;
        const char *img = strstr(li, "<img ");
        if(img && img < end){
            const char *src = strstr(img, "data-original=\""), *s2 = strstr(img, "src=\"");
            const char *ie = strchr(img, '>');
            const char *use = src && ie && src < ie ? src + 15 : s2 && ie && s2 < ie ? s2 + 5 : NULL;
            if(use){
                size_t m = strcspn(use, "\"");
                if(m < sizeof it.art){ memcpy(it.art, use, m); it.art[m] = 0; if(!bc_url_ok(it.art)) it.art[0] = 0; }
            }
        }
        const char *ti = strstr(li, "class=\"title\"");
        if(!ti || ti > end) continue;
        const char *ov = strstr(ti, "class=\"artist-override\"");
        if(ov && ov < end){
            char full[BC_NAME_LEN];
            text_between(ov, "</span>", it.artist, sizeof it.artist);
            /* the title is the text before the override */
            const char *s = strchr(ti, '>');
            size_t m = s && s < ov ? (size_t)(ov - s - 1) : 0;
            if(m >= sizeof full) m = sizeof full - 1;
            memcpy(full, s ? s + 1 : "", m); full[m] = 0;
            char *lt = strrchr(full, '<'); if(lt) *lt = 0;    /* the "<br><span" before it */
            lt = strrchr(full, '<'); if(lt && strncmp(lt, "<br", 3) == 0) *lt = 0;
            bc_entities(full); squash(full);
            snprintf(it.name, sizeof it.name, "%s", full);
        } else {
            text_between(ti, "</p>", it.name, sizeof it.name);
            snprintf(it.artist, sizeof it.artist, "%s", artist);
        }
        if(!it.name[0]) continue;
        out[got++] = it;
    }
    /* the rest of a long list: <ol ... data-client-items="[{...}]"> */
    char *js = bc_html_attr(html, "data-client-items");
    if(js){
        jsmntok_t *t; int nt = parse(js, strlen(js), &t);
        if(nt > 0 && t[0].type == JSMN_ARRAY){
            int k = 1;
            for(int i = 0; i < t[0].size && k < nt && got < max; i++, k = tok_skip(t, nt, k)){
                if(t[k].type != JSMN_OBJECT) continue;
                bc_item_t it; memset(&it, 0, sizeof it);
                char v[BC_URL_LEN];
                get_str(js, t, nt, k, "type", v, sizeof v);
                if(strcmp(v, "album") && strcmp(v, "track")) continue;
                it.type = v[0];
                get_str(js, t, nt, k, "page_url", v, sizeof v);
                if(bc_url_join(base, v, it.url, sizeof it.url) || have_url(out, got, it.url)) continue;
                get_str(js, t, nt, k, "title", it.name, sizeof it.name); trim(it.name);
                get_str(js, t, nt, k, "artist", it.artist, sizeof it.artist); trim(it.artist);
                if(!it.artist[0]) snprintf(it.artist, sizeof it.artist, "%s", artist);
                get_str(js, t, nt, k, "art_id", v, sizeof v);
                art_url(atoll(v), it.art, sizeof it.art);
                if(it.name[0]) out[got++] = it;
            }
        }
        free(t); free(js);
    }
    if(got > 0) return got;
    bc_release_t r;
    bc_track_t *tmp = malloc(sizeof *tmp);
    int n = tmp ? bc_parse_release(html, &r, tmp, 1) : -1;   /* one release only: its own page */
    free(tmp);
    if(n < 0) return -1;
    if(one) *one = r;
    return 0;
}

/* ================================================================ the own player's list */
static size_t put_clean(char *d, size_t cap, size_t o, const char *s){   /* no tabs or newlines in a field */
    for(; *s && o + 1 < cap; s++) d[o++] = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    return o;
}
int bc_list_format(const bc_release_t *r, const bc_track_t *t, int n, char *out, size_t cap, int *map, size_t *len){
    size_t o = 0; int lines = 0;
    if(!cap) return -1;
    for(int i = 0; i < n; i++){
        if(!t[i].stream[0]) continue;
        char num[24]; int k = snprintf(num, sizeof num, "%ld\t", t[i].ms > 0 ? t[i].ms : 0);
        size_t need = (size_t)k + strlen(t[i].stream) + strlen(t[i].title) + strlen(t[i].artist) + strlen(r->album) + 4;
        if(o + need + 1 > cap) return -1;
        memcpy(out + o, num, (size_t)k); o += (size_t)k;
        o = put_clean(out, cap, o, t[i].stream); out[o++] = '\t';
        o = put_clean(out, cap, o, t[i].title[0] ? t[i].title : "Untitled"); out[o++] = '\t';
        o = put_clean(out, cap, o, t[i].artist); out[o++] = '\t';
        o = put_clean(out, cap, o, r->album); out[o++] = '\n';
        if(map) map[lines] = i;
        lines++;
    }
    out[o] = 0;
    if(len) *len = o;
    return lines;
}

/* ================================================================ downloads */
int bc_safe_name(const char *s, const char *fallback, char *out, size_t n){
    if(!n) return 0;
    size_t o = 0, cap = n - 1 < 100 ? n - 1 : 100;
    while(*s == ' ' || *s == '.') s++;
    for(; *s && o < cap; s++){
        unsigned char c = (unsigned char)*s;
        out[o++] = c < 0x20 || c == 0x7f || strchr("/\\:*?\"<>|", c) ? '_' : (char)c;
    }
    if(((unsigned char)*s & 0xC0) == 0x80){                               /* cut inside a character: it goes */
        while(o && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--;
        if(o) o--;
    }
    while(o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = 0;
    if(!o && fallback && fallback != out) return bc_safe_name(fallback, NULL, out, n);
    return (int)o;
}
int bc_dl_dir(const char *root, const bc_release_t *r, char *out, size_t n){
    char a[104], b[104];
    bc_safe_name(r->artist, "Unknown Artist", a, sizeof a);
    bc_safe_name(r->album[0] ? r->album : r->title, "Unknown Album", b, sizeof b);
    int w = snprintf(out, n, "%s/%s/%s/%s", root, BC_DL_DIR, a, b);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}
int bc_dl_file(const bc_track_t *t, char *out, size_t n){
    char x[104]; bc_safe_name(t->title, "Track", x, sizeof x);
    int w = t->num > 0 ? snprintf(out, n, "%02d %s.mp3", t->num, x) : snprintf(out, n, "%s.mp3", x);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}
size_t bc_id3_skip(const unsigned char *b, size_t len){
    if(len < 10 || memcmp(b, "ID3", 3) || b[3] == 0xFF || b[4] == 0xFF || ((b[6] | b[7] | b[8] | b[9]) & 0x80)) return 0;
    size_t sz = ((size_t)b[6] << 21) | ((size_t)b[7] << 14) | ((size_t)b[8] << 7) | b[9];
    sz += 10 + ((b[5] & 0x10) ? 10 : 0);                                  /* header, and the v2.4 footer */
    return sz <= len ? sz : 0;
}
static void syncsafe(unsigned char *p, size_t v){ p[0] = (v >> 21) & 0x7f; p[1] = (v >> 14) & 0x7f; p[2] = (v >> 7) & 0x7f; p[3] = v & 0x7f; }
/* the header of a frame at o whose len-byte body is already in place after it */
static size_t frame(unsigned char *out, size_t o, const char *id, size_t len){
    memcpy(out + o, id, 4); syncsafe(out + o + 4, len); out[o + 8] = out[o + 9] = 0;
    return o + 10 + len;
}
static size_t text_frame(unsigned char *out, size_t o, const char *id, const char *text){
    size_t l = strlen(text);
    if(!l) return o;
    out[o + 10] = 3;                                                      /* UTF-8 */
    memcpy(out + o + 11, text, l);
    return frame(out, o, id, l + 1);
}
unsigned char *bc_id3_tag(const bc_release_t *r, const bc_track_t *t, int total, const unsigned char *jpg, size_t jlen, size_t *len){
    char trck[24] = "";
    if(t->num > 0) snprintf(trck, sizeof trck, total > 0 ? "%d/%d" : "%d", t->num, total);
    const char *album = r->album[0] ? r->album : r->type == 't' ? "" : r->title;
    size_t cap = 10 + 6 * (11 + BC_NAME_LEN) + 32 + (jpg ? 10 + 32 + jlen : 0);
    unsigned char *out = malloc(cap);
    if(!out) return NULL;
    size_t o = 10;
    o = text_frame(out, o, "TIT2", t->title);
    o = text_frame(out, o, "TPE1", t->artist[0] ? t->artist : r->artist);
    o = text_frame(out, o, "TALB", album);
    o = text_frame(out, o, "TPE2", r->artist);
    o = text_frame(out, o, "TRCK", trck);
    o = text_frame(out, o, "TDRC", r->date);
    if(jpg && jlen){                                                      /* enc 0, "image/jpeg", type 3 (front cover), no description */
        static const unsigned char head[] = "\0image/jpeg\0\3";
        unsigned char *body = out + o + 10;
        memcpy(body, head, sizeof head);                                  /* the string's own NUL is the empty description */
        memcpy(body + sizeof head, jpg, jlen);
        o = frame(out, o, "APIC", sizeof head + jlen);
    }
    memcpy(out, "ID3\4\0\0", 6); syncsafe(out + 6, o - 10);
    *len = o;
    return out;
}

/* ================================================================ Bandcamp's own download */
int bc_parse_dlpage(const char *html, const char *const *prefs, char *enc, size_t en, char *url, size_t un, char *type, size_t tn,
                    long long *bytes){
    *bytes = 0;
    char *js = bc_html_attr(html, "data-blob");
    if(!js) return -1;
    jsmntok_t *t; int nt = parse(js, strlen(js), &t), got = -1;
    if(nt > 0){
        int items = obj_get(js, t, nt, 0, "digital_items");
        int it = items >= 0 && t[items].type == JSMN_ARRAY && t[items].size > 0 ? items + 1 : -1;
        int dl = obj_get(js, t, nt, it, "downloads");
        for(int p = 0; dl >= 0 && prefs[p] && got < 0; p++){
            int f = obj_get(js, t, nt, dl, prefs[p]);
            char v[BC_URL_LEN]; get_str(js, t, nt, f, "url", v, sizeof v);
            if(f >= 0 && bc_url_ok(v)){
                snprintf(url, un, "%s", v); snprintf(enc, en, "%s", prefs[p]);
                get_str(js, t, nt, it, "download_type_str", type, tn);
                if(!type[0]) get_str(js, t, nt, it, "type", type, tn);
                get_str(js, t, nt, f, "size_mb", v, sizeof v);
                char *e = NULL; double x = strtod(v, &e);
                if(e && x > 0) *bytes = (long long)(x * (!strncmp(e, "GB", 2) ? 1e9 : !strncmp(e, "KB", 2) ? 1e3 : 1e6));
                got = 0;
            }
        }
        free(t);
    }
    free(js);
    return got;
}
int bc_stat_url(const char *url, unsigned rnd, char *out, size_t n){
    const char *d = strstr(url, "/download/");
    if(!d || !bc_url_ok(url)) return -1;
    int w = snprintf(out, n, "%.*s/statdownload/%s&.vrs=1&.rand=%u", (int)(d - url), url, d + 10, rnd);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}
int bc_parse_stat(const char *text, char *url, size_t n){
    const char *a = strstr(text, "statResult"), *b = NULL;
    a = strchr(a ? a : text, '{');
    if(!a) return -1;
    int depth = 0, str = 0;                                   /* the object's end: braces outside strings */
    for(const char *c = a; *c && !b; c++){
        if(str){ if(*c == '\\' && c[1]) c++; else if(*c == '"') str = 0; }
        else if(*c == '"') str = 1;
        else if(*c == '{') depth++;
        else if(*c == '}' && --depth == 0) b = c;
    }
    if(!b) return -1;
    size_t len = (size_t)(b - a + 1);
    jsmntok_t *t; int nt = parse(a, len, &t), r = -1;
    if(nt <= 0) return -1;
    char res[16], v[BC_URL_LEN];
    get_str(a, t, nt, 0, "result", res, sizeof res);
    if(!strcmp(res, "ok")){ get_str(a, t, nt, 0, "download_url", v, sizeof v); if(bc_url_ok(v)){ snprintf(url, n, "%s", v); r = 1; } }
    else { get_str(a, t, nt, 0, "retry_url", v, sizeof v); if(bc_url_ok(v)){ snprintf(url, n, "%s", v); r = 0; } }
    free(t);
    return r;
}

/* ================================================================ favourites */
static char *tab_field(char **p){                          /* the next TAB-separated field, NULL at the end */
    if(!*p) return NULL;
    char *f = *p, *t = strchr(f, '\t');
    if(t){ *t = 0; *p = t + 1; } else *p = NULL;
    return f;
}
int bc_fav_find(const bc_item_t *it, int n, const char *url){
    for(int i = 0; i < n; i++) if(!strcmp(it[i].url, url)) return i;
    return -1;
}
int bc_fav_parse(const char *text, bc_item_t *out, int max){
    int n = 0;
    for(const char *line = text; line && *line && n < max; ){
        const char *end = strpbrk(line, "\r\n");
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[3 * BC_NAME_LEN + 2 * BC_URL_LEN + 16];
        if(len > 2 && len < sizeof buf && line[0] != '#'){
            memcpy(buf, line, len); buf[len] = 0;
            char *p = buf, *ty = tab_field(&p), *nm = tab_field(&p), *ar = tab_field(&p), *al = tab_field(&p), *u = tab_field(&p), *art = tab_field(&p);
            if(ty && u && nm && nm[0] && strlen(ty) == 1 && strchr("abt", ty[0]) && bc_url_ok(u) && bc_fav_find(out, n, u) < 0){
                bc_item_t *x = &out[n++]; memset(x, 0, sizeof *x);
                x->type = ty[0];
                snprintf(x->name, sizeof x->name, "%s", nm);
                snprintf(x->artist, sizeof x->artist, "%s", ar ? ar : "");
                snprintf(x->album, sizeof x->album, "%s", al ? al : "");
                snprintf(x->url, sizeof x->url, "%s", u);
                if(art && bc_url_ok(art)) snprintf(x->art, sizeof x->art, "%s", art);
            }
        }
        line = end ? end + 1 : NULL;
    }
    return n;
}
static void clean(char *d, size_t n, const char *s){       /* no TAB or newline inside a field */
    snprintf(d, n, "%s", s);
    for(char *c = d; *c; c++) if(*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
}
int bc_fav_format(const bc_item_t *it, int n, char *out, size_t cap){
    size_t at = 0;
    if(!cap) return -1;
    int k = snprintf(out, cap, "# diskOS Bandcamp favourites: type (b artist, a album, t track), name, artist, album, address, cover\n");
    if(k < 0 || (size_t)k >= cap) return -1;
    at = (size_t)k;
    for(int i = 0; i < n; i++){
        char nm[BC_NAME_LEN], ar[BC_NAME_LEN], al[BC_NAME_LEN];
        clean(nm, sizeof nm, it[i].name); clean(ar, sizeof ar, it[i].artist); clean(al, sizeof al, it[i].album);
        k = snprintf(out + at, cap - at, "%c\t%s\t%s\t%s\t%s\t%s\n", it[i].type, nm, ar, al, it[i].url, it[i].art);
        if(k < 0 || (size_t)k >= cap - at){ out[at] = 0; return -1; }
        at += (size_t)k;
    }
    return (int)at;
}
