/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Web radio, the pure part: see radio_core.h. */
#include "radio_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define JSMN_STATIC                 /* a private strict copy; ipc.c keeps its own lenient one */
#define JSMN_STRICT
#include "jsmn.h"

static void cpy(char *d, size_t n, const char *s){ if(n){ snprintf(d, n, "%s", s ? s : ""); } }

int rc_url_ok(const char *url){
    if(!url) return 0;
    size_t skip = !strncasecmp(url, "http://", 7) ? 7 : !strncasecmp(url, "https://", 8) ? 8 : 0;
    if(!skip) return 0;
    size_t len = strlen(url);
    if(len >= RC_URL_LEN || len <= skip) return 0;
    if(url[skip] == '/' || url[skip] == ':' || url[skip] == '?') return 0;        /* no host */
    for(const unsigned char *p = (const unsigned char *)url; *p; p++)
        if(*p <= 0x20 || *p >= 0x7f || *p == '"' || *p == '\'' || *p == '<' || *p == '>' || *p == '\\' || *p == '`') return 0;
    return 1;
}

void rc_detail(const rc_station_t *s, char *out, size_t n){
    if(!n) return;
    out[0] = 0;
    char br[24] = "";
    if(s->bitrate > 0) snprintf(br, sizeof br, "%d kbps", s->bitrate);
    const char *part[3] = { s->codec, br, s->country };
    size_t at = 0;
    for(int i = 0; i < 3; i++){
        if(!part[i][0]) continue;
        int k = snprintf(out + at, n - at, "%s%s", at ? " \xC2\xB7 " : "", part[i]);   /* U+00B7 middle dot */
        if(k < 0 || (size_t)k >= n - at){ out[at] = 0; return; }
        at += (size_t)k;
    }
}

void rc_urlencode(const char *in, char *out, size_t n){
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if(!n) return;
    for(const unsigned char *p = (const unsigned char *)in; *p; p++){
        if(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~'){
            if(o + 1 >= n) break;
            out[o++] = (char)*p;
        } else {
            if(o + 3 >= n) break;
            out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

int rc_xml_escape(const char *in, char *out, size_t n){
    size_t o = 0;
    if(!n) return -1;
    for(; *in; in++){
        const char *r = NULL;
        switch(*in){
            case '&': r = "&amp;"; break;  case '<': r = "&lt;"; break;  case '>': r = "&gt;"; break;
            case '"': r = "&quot;"; break; case '\'': r = "&apos;"; break;
        }
        size_t l = r ? strlen(r) : 1;
        if(o + l >= n){ out[o] = 0; return -1; }
        if(r) memcpy(out + o, r, l); else out[o] = *in;
        o += l;
    }
    out[o] = 0;
    return 0;
}

const char *rc_mime(const char *codec, const char *url){
    const char *c = codec ? codec : "";
    if(!strcasecmp(c, "MP3")) return "audio/mpeg";
    if(!strncasecmp(c, "AAC", 3)) return "audio/aac";            /* AAC, AAC+ */
    if(!strcasecmp(c, "OGG") || !strcasecmp(c, "OPUS") || !strcasecmp(c, "VORBIS")) return "audio/ogg";
    if(!strcasecmp(c, "FLAC")) return "audio/flac";
    const char *u = url ? url : "";
    if(strcasestr(u, ".ogg") || strcasestr(u, "opus") || strcasestr(u, ".oga")) return "audio/ogg";
    if(strcasestr(u, "aac")) return "audio/aac";
    if(strcasestr(u, "flac")) return "audio/flac";
    return "audio/mpeg";
}

/* ---------------------------------------------------------------- favourites file */
static char *field(char **p){                       /* next TAB-separated field of a line, NULL at the end */
    if(!*p) return NULL;
    char *f = *p, *t = strchr(f, '\t');
    if(t){ *t = 0; *p = t + 1; } else *p = NULL;
    return f;
}
int rc_fav_parse(const char *text, rc_station_t *out, int max){
    int n = 0;
    if(!text) return 0;
    const char *line = text;
    while(*line && n < max){
        const char *end = strpbrk(line, "\r\n");
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[RC_NAME_LEN + RC_URL_LEN + 128];
        if(len > 0 && len < sizeof buf && line[0] != '#'){
            memcpy(buf, line, len); buf[len] = 0;
            char *p = buf, *nm = field(&p), *url = field(&p), *codec = field(&p), *br = field(&p), *country = field(&p);
            if(nm && url && nm[0] && rc_url_ok(url) && strlen(nm) < RC_NAME_LEN){
                rc_station_t *s = &out[n]; memset(s, 0, sizeof *s);
                cpy(s->name, sizeof s->name, nm); cpy(s->url, sizeof s->url, url);
                cpy(s->codec, sizeof s->codec, codec); cpy(s->country, sizeof s->country, country);
                s->bitrate = br ? atoi(br) : 0; if(s->bitrate < 0 || s->bitrate > 100000) s->bitrate = 0;
                if(rc_fav_find(out, n, s->url) < 0) n++;           /* no duplicates */
            }
        }
        if(!end) break;
        line = end + 1;
    }
    return n;
}
static void clean_field(char *d, size_t n, const char *s){       /* no TAB/newline inside a field */
    cpy(d, n, s);
    for(char *p = d; *p; p++) if(*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
}
int rc_fav_format(const rc_station_t *s, int n, char *out, size_t cap){
    size_t at = 0;
    if(!cap) return -1;
    out[0] = 0;
    for(int i = 0; i < n; i++){
        char nm[RC_NAME_LEN], codec[16], country[48];
        clean_field(nm, sizeof nm, s[i].name); clean_field(codec, sizeof codec, s[i].codec);
        clean_field(country, sizeof country, s[i].country);
        int k = snprintf(out + at, cap - at, "%s\t%s\t%s\t%d\t%s\n", nm, s[i].url, codec, s[i].bitrate, country);
        if(k < 0 || (size_t)k >= cap - at){ out[at] = 0; return -1; }
        at += (size_t)k;
    }
    return (int)at;
}
int rc_fav_find(const rc_station_t *s, int n, const char *url){
    for(int i = 0; i < n; i++) if(!strcmp(s[i].url, url)) return i;
    return -1;
}

/* ---------------------------------------------------------------- radio-browser.info JSON */
static int tok_skip(const jsmntok_t *t, int i, int nt){          /* index after token i and everything inside it */
    if(i >= nt) return nt;
    int n = t[i].size, j = i + 1;
    if(t[i].type == JSMN_OBJECT){ for(int k = 0; k < n && j < nt; k++){ j++; j = tok_skip(t, j, nt); } }   /* key, value */
    else if(t[i].type == JSMN_ARRAY){ for(int k = 0; k < n && j < nt; k++) j = tok_skip(t, j, nt); }
    return j;
}
static int hexv(int c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void put_utf8(char *d, size_t n, size_t *o, unsigned cp){
    char b[4]; int k;
    if(cp < 0x80){ b[0] = (char)cp; k = 1; }
    else if(cp < 0x800){ b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if(cp < 0x10000){ b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4; }
    if(*o + (size_t)k >= n) return;
    memcpy(d + *o, b, (size_t)k); *o += (size_t)k;
}
static void tok_str(const char *js, const jsmntok_t *t, char *d, size_t n){   /* unescaped string value; "" otherwise */
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
                    if(i + 4 >= t->end) { i = t->end; break; }
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
    } else if(t->type == JSMN_PRIMITIVE){
        for(int i = t->start; i < t->end && o + 1 < n; i++) d[o++] = js[i];
    }
    d[o] = 0;
}
static void trim(char *s){
    size_t n = strlen(s);
    while(n && (s[n - 1] == ' ')) s[--n] = 0;
    size_t k = 0; while(s[k] == ' ') k++;
    if(k) memmove(s, s + k, n - k + 1);
}
int rc_parse_stations(const char *json, size_t len, rc_station_t *out, int max){
    if(!json) return -1;
    int cap = 16384;
    jsmntok_t *t = malloc(sizeof *t * (size_t)cap);
    if(!t) return -1;
    jsmn_parser p; jsmn_init(&p);
    int nt = jsmn_parse(&p, json, len, t, (unsigned)cap);
    if(nt < 1 || t[0].type != JSMN_ARRAY){ free(t); return -1; }
    int n = 0, i = 1;
    for(int e = 0; e < t[0].size && i < nt && n < max; e++){
        int next = tok_skip(t, i, nt);
        if(t[i].type == JSMN_OBJECT){
            rc_station_t s; memset(&s, 0, sizeof s);
            char url[RC_URL_LEN] = "", resolved[RC_URL_LEN] = "", br[16] = "", lastok[8] = "";
            int j = i + 1;
            for(int k = 0; k < t[i].size && j + 1 < nt; k++){
                const jsmntok_t *key = &t[j], *val = &t[j + 1];
                int kl = key->end - key->start;
                const char *ks = json + key->start;
#define KEY(lit) (kl == (int)sizeof(lit) - 1 && !memcmp(ks, lit, sizeof(lit) - 1))
                if(KEY("name")) tok_str(json, val, s.name, sizeof s.name);
                else if(KEY("url")) tok_str(json, val, url, sizeof url);
                else if(KEY("url_resolved")) tok_str(json, val, resolved, sizeof resolved);
                else if(KEY("codec")) tok_str(json, val, s.codec, sizeof s.codec);
                else if(KEY("country")) tok_str(json, val, s.country, sizeof s.country);
                else if(KEY("bitrate")) tok_str(json, val, br, sizeof br);
                else if(KEY("lastcheckok")) tok_str(json, val, lastok, sizeof lastok);
#undef KEY
                j = tok_skip(t, j + 1, nt);
            }
            trim(s.name); trim(s.codec); trim(s.country);
            if(!strcasecmp(s.codec, "UNKNOWN")) s.codec[0] = 0;
            s.bitrate = atoi(br); if(s.bitrate < 0 || s.bitrate > 100000) s.bitrate = 0;
            const char *u = rc_url_ok(resolved) ? resolved : url;
            if(s.name[0] && rc_url_ok(u) && strcmp(lastok, "0") && rc_fav_find(out, n, u) < 0){
                cpy(s.url, sizeof s.url, u);
                out[n++] = s;
            }
        }
        i = next;
    }
    free(t);
    return n;
}

/* ---------------------------------------------------------------- UPnP */
int rc_split_http(const char *url, char *host, size_t hn, int *port, char *path, size_t pn){
    if(!url || strncasecmp(url, "http://", 7)) return -1;
    const char *h = url + 7, *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if(!hl || hl >= hn) return -1;
    memcpy(host, h, hl); host[hl] = 0;
    *port = 80;
    char *c = strchr(host, ':');
    if(c){ *c = 0; char *e; long v = strtol(c + 1, &e, 10); if(*e || v <= 0 || v > 65535) return -1; *port = (int)v; }
    if(!host[0]) return -1;
    if(snprintf(path, pn, "%s", slash ? slash : "/") >= (int)pn) return -1;
    return 0;
}
static const char *ci_find(const char *hay, const char *needle){ return strcasestr(hay, needle); }
int rc_xml_value(const char *xml, const char *tag, char *out, size_t n){
    char open[64], close[64];
    if(!n) return -1;
    out[0] = 0;
    snprintf(open, sizeof open, "<%s>", tag); snprintf(close, sizeof close, "</%s>", tag);
    const char *a = ci_find(xml, open); if(!a) return -1;
    a += strlen(open);
    const char *b = ci_find(a, close); if(!b) return -1;
    size_t o = 0;
    for(const char *p = a; p < b && o + 1 < n; ){
        static const struct { const char *e; char c; } ent[] = { {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''} };
        int hit = 0;
        if(*p == '&') for(size_t k = 0; k < sizeof ent / sizeof ent[0]; k++){
            size_t l = strlen(ent[k].e);
            if((size_t)(b - p) >= l && !strncmp(p, ent[k].e, l)){ out[o++] = ent[k].c; p += l; hit = 1; break; }
        }
        if(!hit){ out[o++] = *p++; }
    }
    out[o] = 0;
    return 0;
}
int rc_desc_control_url(const char *desc, const char *desc_url, char *out, size_t n){
    if(!desc || !n) return -1;
    const char *svc = ci_find(desc, "urn:schemas-upnp-org:service:AVTransport:");
    if(!svc) return -1;
    char ctl[RC_URL_LEN];
    if(rc_xml_value(svc, "controlURL", ctl, sizeof ctl)) return -1;
    trim(ctl);
    if(!ctl[0]) return -1;
    if(!strncasecmp(ctl, "http://", 7)) return snprintf(out, n, "%s", ctl) < (int)n ? 0 : -1;
    char host[64], path[256]; int port;
    if(rc_split_http(desc_url, host, sizeof host, &port, path, sizeof path)) return -1;
    return snprintf(out, n, "http://%s:%d%s%s", host, port, ctl[0] == '/' ? "" : "/", ctl) < (int)n ? 0 : -1;
}
int rc_soap_body(const char *action, const char *args, char *out, size_t n){
    int k = snprintf(out, n,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"><InstanceID>0</InstanceID>%s</u:%s></s:Body></s:Envelope>",
        action, args ? args : "", action);
    return (k < 0 || (size_t)k >= n) ? -1 : 0;
}
int rc_set_uri_args(const rc_station_t *s, char *out, size_t n){
    char eurl[RC_URL_LEN * 2], ename[RC_NAME_LEN * 6], det[128], edet[128 * 6], didl[4096], edidl[8192];
    char detail[128];
    rc_detail(s, detail, sizeof detail);
    snprintf(det, sizeof det, "%s", detail[0] ? detail : "Web radio");
    if(rc_xml_escape(s->url, eurl, sizeof eurl) || rc_xml_escape(s->name, ename, sizeof ename) || rc_xml_escape(det, edet, sizeof edet)) return -1;
    int k = snprintf(didl, sizeof didl,
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\"><item id=\"radio\" parentID=\"0\" restricted=\"1\">"
        "<dc:title>%s</dc:title><dc:creator>Web radio</dc:creator><upnp:artist>Web radio</upnp:artist><upnp:album>%s</upnp:album>"
        "<upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
        "<res protocolInfo=\"http-get:*:%s:*\">%s</res></item></DIDL-Lite>",
        ename, edet, rc_mime(s->codec, s->url), eurl);
    if(k < 0 || (size_t)k >= sizeof didl) return -1;
    if(rc_xml_escape(didl, edidl, sizeof edidl)) return -1;
    k = snprintf(out, n, "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>", eurl, edidl);
    return (k < 0 || (size_t)k >= n) ? -1 : 0;
}

/* ================================================================ ICY in-stream metadata */
int rc_icy_head_end(const char *buf, size_t n){
    for(size_t i = 0; i + 1 < n; i++){
        if(buf[i] == '\n' && buf[i + 1] == '\n') return (int)(i + 2);
        if(i + 3 < n && buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return (int)(i + 4);
    }
    return -1;
}
long rc_icy_metaint(const char *head, size_t n){
    static const char key[] = "icy-metaint:";
    const size_t kl = sizeof key - 1;
    for(size_t i = 0; i + kl <= n; i++){
        if((i && head[i - 1] != '\n') || strncasecmp(head + i, key, kl)) continue;
        size_t j = i + kl; long v = 0; int digits = 0;
        while(j < n && (head[j] == ' ' || head[j] == '\t')) j++;
        while(j < n && head[j] >= '0' && head[j] <= '9' && digits < 7){ v = v * 10 + (head[j] - '0'); j++; digits++; }
        return (digits && v > 0 && v <= RC_ICY_METAINT_MAX) ? v : -1;
    }
    return -1;
}
static int utf8_valid(const unsigned char *s, size_t n){
    for(size_t i = 0; i < n;){
        unsigned c = s[i]; size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if(!k || i + k > n || (k == 2 && c < 0xC2)) return 0;
        for(size_t j = 1; j < k; j++) if((s[i + j] & 0xC0) != 0x80) return 0;
        i += k;
    }
    return 1;
}
int rc_icy_title(const char *meta, size_t n, char *out, size_t on){
    static const char key[] = "StreamTitle='";
    const size_t kl = sizeof key - 1;
    if(!on) return -1;
    out[0] = 0;
    size_t len = 0; while(len < n && meta[len]) len++;    /* the block is NUL-padded */
    const char *s = NULL;
    for(size_t i = 0; i + kl <= len; i++) if(!strncmp(meta + i, key, kl)){ s = meta + i + kl; break; }
    if(!s) return -1;
    size_t rest = len - (size_t)(s - meta), vl = rest;
    for(size_t i = 0; i + 1 < rest; i++) if(s[i] == '\'' && s[i + 1] == ';'){ vl = i; break; }   /* titles may hold ' */
    if(vl == rest && vl && s[vl - 1] == '\'') vl--;
    const unsigned char *u = (const unsigned char *)s;
    int utf8 = utf8_valid(u, vl);
    size_t o = 0;
    for(size_t i = 0; i < vl && o + 1 < on; i++){
        unsigned c = u[i];
        if(c < 0x20 || c == 0x7F){ if(o && out[o - 1] != ' ') out[o++] = ' '; continue; }
        if(utf8 || c < 0x80){ out[o++] = (char)c; continue; }
        if(c < 0xA0) continue;                              /* Latin-1 C1 controls */
        if(o + 2 >= on) break;
        out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63));   /* Latin-1 -> UTF-8 */
    }
    out[o] = 0;
    if(utf8 && o){                                          /* never end on a cut UTF-8 sequence */
        size_t b = o; while(b && ((unsigned char)out[b - 1] & 0xC0) == 0x80) b--;
        if(b){ unsigned c = (unsigned char)out[b - 1]; size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
               if(o - (b - 1) < need) out[b - 1] = 0; }
    }
    trim(out);
    return 0;
}
int rc_icy_split(const char *t, char *artist, size_t an, char *title, size_t tn){
    const char *d = t ? strstr(t, " - ") : NULL;
    if(!d || d == t || !d[3]){ cpy(artist, an, ""); cpy(title, tn, t); return 0; }
    size_t al = (size_t)(d - t);
    if(an){ if(al >= an) al = an - 1; memcpy(artist, t, al); artist[al] = 0; trim(artist); }
    cpy(title, tn, d + 3); trim(title);
    return artist[0] && title[0];
}

/* ================================================================ Ogg (Vorbis / Opus) comments */
static unsigned le32(const unsigned char *p){ return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
size_t rc_ogg_payload(const unsigned char *in, size_t n, unsigned char *out, size_t on){
    size_t i = 0, o = 0; int have = 0; unsigned serial = 0;
    while(i + 27 <= n && o < on){
        if(memcmp(in + i, "OggS", 4)){ i++; continue; }    /* resync (a page may be cut at the start) */
        unsigned segs = in[i + 26];
        if(i + 27 + segs > n) break;
        size_t body = 0; for(unsigned k = 0; k < segs; k++) body += in[i + 27 + k];
        unsigned sn = le32(in + i + 14);
        size_t at = i + 27 + segs;
        if(!have){ serial = sn; have = 1; }
        if(sn == serial){
            size_t take = body; if(at + take > n) take = n - at;
            if(take > on - o) take = on - o;
            memcpy(out + o, in + at, take); o += take;
        }
        i = at + body;
    }
    return o;
}
static void field_val(const unsigned char *c, size_t cl, size_t kl, char *out, size_t on){
    size_t vl = cl - kl; char tmp[512]; if(vl >= sizeof tmp) vl = sizeof tmp - 1;
    memcpy(tmp, c + kl, vl); tmp[vl] = 0;
    char q[600]; snprintf(q, sizeof q, "StreamTitle='%s';", tmp);  /* the same cleaning as an ICY title */
    rc_icy_title(q, strlen(q), out, on);
}
int rc_ogg_tags(const unsigned char *p, size_t n, char *artist, size_t an, char *title, size_t tn){
    if(an) artist[0] = 0;
    if(tn) title[0] = 0;
    size_t i = 0, start = 0; int found = 0;
    for(; i + 8 <= n; i++){
        if(!memcmp(p + i, "\x03vorbis", 7)){ start = i + 7; found = 1; break; }
        if(!memcmp(p + i, "OpusTags", 8)){ start = i + 8; found = 1; break; }
    }
    if(!found || start + 4 > n) return -1;
    size_t at = start, vl = le32(p + at); at += 4;
    if(vl > n - at) return -1;
    at += vl;
    if(at + 4 > n) return -1;
    unsigned cnt = le32(p + at); at += 4;
    for(unsigned k = 0; k < cnt && at + 4 <= n; k++){
        size_t cl = le32(p + at); at += 4;
        if(cl > n - at) break;                              /* cut (e.g. embedded cover art past our window) */
        const unsigned char *c = p + at; at += cl;
        if(cl > 6 && !strncasecmp((const char *)c, "TITLE=", 6) && tn && !title[0]) field_val(c, cl, 6, title, tn);
        else if(cl > 7 && !strncasecmp((const char *)c, "ARTIST=", 7) && an && !artist[0]) field_val(c, cl, 7, artist, an);
    }
    return (tn && title[0]) || (an && artist[0]) ? 0 : -1;
}
