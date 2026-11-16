/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "upnp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define AVT "urn:schemas-upnp-org:service:AVTransport:1"

static const char DESC[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
    "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
    "<friendlyName>diskOS player</friendlyName><manufacturer>diskOS</manufacturer><modelName>diskos-player</modelName>"
    "<UDN>uuid:6469736b-6f73-4f70-8c61-796572000001</UDN><serviceList><service>"
    "<serviceType>" AVT "</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
    "<SCPDURL>/AVTransport/scpd.xml</SCPDURL><controlURL>" UP_CTL_PATH "</controlURL>"
    "<eventSubURL>/AVTransport/event</eventSubURL></service></serviceList></device></root>";

/* ---------------------------------------------------------------- pure parts */
static const char *find_ci(const char *h, const char *n, const char *end){
    size_t k = strlen(n);
    for(; h + k <= end; h++) if(!strncasecmp(h, n, k)) return h;
    return NULL;
}
int up_parse(const char *buf, size_t n, char *method, size_t mcap, char *path, size_t pcap, char *action, size_t acap,
             const char **body, size_t *blen){
    const char *end = buf + n, *he = NULL;
    for(const char *p = buf; p + 3 < end; p++) if(!memcmp(p, "\r\n\r\n", 4)){ he = p; break; }
    if(!he) return n >= UP_REQ_MAX ? -1 : 0;
    const char *sp1 = memchr(buf, ' ', (size_t)(he - buf));
    if(!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(he - sp1 - 1));
    if(!sp2) return -1;
    size_t ml = (size_t)(sp1 - buf), pl = (size_t)(sp2 - sp1 - 1);
    if(!ml || ml >= mcap || !pl || pl >= pcap) return -1;
    memcpy(method, buf, ml); method[ml] = 0;
    memcpy(path, sp1 + 1, pl); path[pl] = 0;
    char *q = strchr(path, '?'); if(q) *q = 0;
    long clen = 0;
    const char *cl = find_ci(buf, "\r\nContent-Length:", he);
    if(cl){ clen = strtol(cl + 17, NULL, 10); if(clen < 0 || clen > UP_REQ_MAX) return -1; }
    action[0] = 0;
    const char *sa = find_ci(buf, "\r\nSOAPACTION:", he);
    if(sa){
        const char *v = sa + 13, *le = find_ci(v, "\r\n", he + 2);
        if(!le) le = he;
        const char *h = NULL;
        for(const char *p = v; p < le; p++) if(*p == '#') h = p;
        if(h){
            const char *a = h + 1, *ae = a;
            while(ae < le && *ae != '"' && *ae != ' ' && *ae != '\r') ae++;
            size_t al = (size_t)(ae - a);
            if(al >= acap) return -1;
            memcpy(action, a, al); action[al] = 0;
        }
    }
    const char *b = he + 4;
    if((size_t)(end - b) < (size_t)clen) return 0;
    *body = b; *blen = (size_t)clen;
    return 1;
}

static int decode(const char *s, size_t n, char *out, size_t cap){
    size_t o = 0;
    static const struct { const char *e; char c; } ent[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                                                           { "&quot;", '"' }, { "&apos;", '\'' } };
    for(size_t i = 0; i < n; ){
        char c = s[i]; size_t step = 1;
        if(c == '&') for(size_t k = 0; k < sizeof ent / sizeof ent[0]; k++){
            size_t l = strlen(ent[k].e);
            if(i + l <= n && !memcmp(s + i, ent[k].e, l)){ c = ent[k].c; step = l; break; }
        }
        if(o + 1 >= cap) return -1;
        out[o++] = c; i += step;
    }
    out[o] = 0;
    return 0;
}
int up_xml_value(const char *xml, const char *tag, char *out, size_t cap){
    if(!xml || !cap) return -1;
    size_t tl = strlen(tag);
    for(const char *p = strchr(xml, '<'); p; p = strchr(p + 1, '<')){
        const char *name = p + 1;
        if(*name == '/' || *name == '?' || *name == '!') continue;
        const char *ne = name; while(*ne && *ne != '>' && *ne != ' ' && *ne != '/' && *ne != '\t' && *ne != '\r' && *ne != '\n') ne++;
        const char *local = name; for(const char *c = name; c < ne; c++) if(*c == ':') local = c + 1;
        if((size_t)(ne - local) != tl || strncmp(local, tag, tl)) continue;
        const char *gt = strchr(ne, '>'); if(!gt) return -1;
        if(gt[-1] == '/'){ out[0] = 0; return 0; }                          /* <tag/> */
        const char *v = gt + 1;
        /* the closing tag with the same (prefixed) name */
        char close[80]; size_t nl = (size_t)(ne - name);
        if(nl + 4 > sizeof close) return -1;
        close[0] = '<'; close[1] = '/'; memcpy(close + 2, name, nl); close[nl + 2] = '>'; close[nl + 3] = 0;
        const char *ve = strstr(v, close); if(!ve) return -1;
        return decode(v, (size_t)(ve - v), out, cap);
    }
    return -1;
}
int up_xml_escape(const char *in, char *out, size_t cap){
    size_t o = 0;
    for(; *in; in++){
        const char *r = NULL;
        switch(*in){ case '&': r = "&amp;"; break; case '<': r = "&lt;"; break; case '>': r = "&gt;"; break;
                     case '"': r = "&quot;"; break; case '\'': r = "&apos;"; break; }
        size_t l = r ? strlen(r) : 1;
        if(o + l >= cap) return -1;
        if(r) memcpy(out + o, r, l); else out[o] = *in;
        o += l;
    }
    if(o >= cap) return -1;
    out[o] = 0;
    return 0;
}

static int http(char *out, size_t cap, int status, const char *reason, const char *ctype, const char *body){
    size_t bl = strlen(body);
    int k = snprintf(out, cap, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "Server: Linux UPnP/1.0 diskos-player\r\n\r\n%s", status, reason, ctype, bl, body);
    return k < 0 || (size_t)k >= cap ? 0 : k;
}
int up_reply(const char *method, const char *path, const char *action, const char *body, up_action_fn fn, void *ctx,
             char *out, size_t cap){
    static const char XML[] = "text/xml; charset=\"utf-8\"";
    if(!strcmp(method, "GET") && (!strcmp(path, UP_DESC_PATH) || !strcmp(path, "/"))) return http(out, cap, 200, "OK", XML, DESC);
    if(strcmp(method, "POST") || strcmp(path, UP_CTL_PATH)) return http(out, cap, 404, "Not Found", "text/plain", "not found\n");
    char *args = malloc(8192), *xml = malloc(12288);
    int k = 0;
    if(!args || !xml){ free(args); free(xml); return http(out, cap, 500, "Internal Server Error", "text/plain", "out of memory\n"); }
    args[0] = 0;
    int code = action[0] && fn ? fn(ctx, action, body ? body : "", args, 8192) : 401;   /* 401 invalid action */
    if(!code){
        int w = snprintf(xml, 12288, "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%sResponse xmlns:u=\"" AVT "\">%s"
                         "</u:%sResponse></s:Body></s:Envelope>", action, args, action);
        k = w > 0 && w < 12288 ? http(out, cap, 200, "OK", XML, xml) : 0;
    } else {
        int w = snprintf(xml, 12288, "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
                         "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                         "<errorCode>%d</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>", code);
        k = w > 0 && w < 12288 ? http(out, cap, 500, "Internal Server Error", XML, xml) : 0;
    }
    free(args); free(xml);
    return k;
}

/* ---------------------------------------------------------------- the socket */
int up_listen(int port, up_log_fn log){
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if(fd < 0){ log("upnp: socket: %s", strerror(errno)); return -1; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0){
        log("upnp: listen on 127.0.0.1:%d: %s", port, strerror(errno)); close(fd); return -1;
    }
    log("upnp: renderer on 127.0.0.1:%d", port);
    return fd;
}
static double mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
void up_serve(int lfd, up_action_fn fn, void *ctx, up_log_fn log){
    int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if(c < 0){ if(errno != EAGAIN && errno != EINTR) log("upnp: accept: %s", strerror(errno)); return; }
    char *buf = malloc(UP_REQ_MAX + 1), *out = malloc(UP_REQ_MAX);
    if(!buf || !out){ log("upnp: out of memory"); goto done; }
    size_t n = 0; int r = 0;
    char method[16], path[128], action[64]; const char *body = NULL; size_t blen = 0;
    double end = mono() + 2.0;
    while(!(r = up_parse(buf, n, method, sizeof method, path, sizeof path, action, sizeof action, &body, &blen))){
        int left = (int)((end - mono()) * 1000.0);
        struct pollfd p = { c, POLLIN, 0 };
        if(left <= 0 || poll(&p, 1, left) <= 0){ log("upnp: request timed out"); goto done; }
        ssize_t k = read(c, buf + n, UP_REQ_MAX - n);
        if(k <= 0){ log("upnp: request cut off"); goto done; }
        n += (size_t)k; buf[n] = 0;
    }
    if(r < 0){ log("upnp: bad request"); goto done; }
    buf[n] = 0;
    char save = body[blen]; ((char *)body)[blen] = 0;
    int len = up_reply(method, path, action, body, fn, ctx, out, UP_REQ_MAX);
    ((char *)body)[blen] = save;
    if(action[0]) log("upnp: %s %s %s -> %.12s", method, path, action, out + 9);
    for(int w = 0; w < len; ){
        ssize_t k = write(c, out + w, (size_t)(len - w));
        if(k <= 0) break;
        w += (int)k;
    }
done:
    free(buf); free(out);
    close(c);
}
