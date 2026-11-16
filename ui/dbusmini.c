/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* A small D-Bus client without libdbus (dbusmini.h). Wire format: D-Bus specification, "Message Protocol". */
#include "dbusmini.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DM_MAX_MSG (8u << 20)      /* the spec allows 128 MiB; nothing we talk to sends more than a few KiB */

/* ---- writing ---- */
void dm_buf_init(dm_buf *w){ memset(w, 0, sizeof *w); }
void dm_buf_free(dm_buf *w){ free(w->b); memset(w, 0, sizeof *w); }
static void w_room(dm_buf *w, size_t n){
    if(w->err) return;
    if(w->len + n <= w->cap) return;
    size_t nc = w->cap ? w->cap : 256;
    while(nc < w->len + n) nc *= 2;
    uint8_t *nb = realloc(w->b, nc);
    if(!nb){ w->err = 1; return; }
    w->b = nb; w->cap = nc;
}
static void w_put(dm_buf *w, const void *p, size_t n){
    w_room(w, n);
    if(w->err) return;
    memcpy(w->b + w->len, p, n); w->len += n;
}
void dm_w_align(dm_buf *w, size_t a){
    static const uint8_t z[8] = { 0 };
    size_t pad = (a - (w->len % a)) % a;
    if(pad) w_put(w, z, pad);
}
void dm_w_u8(dm_buf *w, uint8_t v){ w_put(w, &v, 1); }
void dm_w_u16(dm_buf *w, uint16_t v){ dm_w_align(w, 2); uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; w_put(w, b, 2); }
void dm_w_u32(dm_buf *w, uint32_t v){
    dm_w_align(w, 4);
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    w_put(w, b, 4);
}
void dm_w_bool(dm_buf *w, int v){ dm_w_u32(w, v ? 1 : 0); }
void dm_w_i32(dm_buf *w, int32_t v){ dm_w_u32(w, (uint32_t)v); }
void dm_w_i64(dm_buf *w, int64_t v){
    dm_w_align(w, 8);
    uint64_t u = (uint64_t)v; uint8_t b[8];
    for(int i = 0; i < 8; i++) b[i] = (uint8_t)(u >> (8 * i));
    w_put(w, b, 8);
}
void dm_w_str(dm_buf *w, const char *s){
    size_t n = strlen(s);
    dm_w_u32(w, (uint32_t)n);
    w_put(w, s, n + 1);
}
void dm_w_sig(dm_buf *w, const char *s){
    size_t n = strlen(s);
    if(n > 255){ w->err = 1; return; }
    dm_w_u8(w, (uint8_t)n);
    w_put(w, s, n + 1);
}
static void put32_at(dm_buf *w, size_t at, uint32_t n){
    w->b[at] = (uint8_t)n; w->b[at + 1] = (uint8_t)(n >> 8); w->b[at + 2] = (uint8_t)(n >> 16); w->b[at + 3] = (uint8_t)(n >> 24);
}
size_t dm_w_array_begin(dm_buf *w, size_t elem_align){
    dm_w_u32(w, 0);
    size_t at = w->len - 4;
    dm_w_align(w, elem_align);   /* the padding to the first element is not part of the length */
    if(!w->err) put32_at(w, at, (uint32_t)w->len);   /* where the elements start, until dm_w_array_end */
    return at;
}
void dm_w_array_end(dm_buf *w, size_t at){
    if(w->err) return;
    uint32_t start = (uint32_t)w->b[at] | ((uint32_t)w->b[at + 1] << 8) | ((uint32_t)w->b[at + 2] << 16) | ((uint32_t)w->b[at + 3] << 24);
    put32_at(w, at, (uint32_t)(w->len - start));
}
static void dict_key(dm_buf *w, const char *key, const char *sig){
    dm_w_struct(w);
    dm_w_str(w, key);
    dm_w_sig(w, sig);
}
void dm_w_dict_sv_str(dm_buf *w, const char *key, const char *val){ dict_key(w, key, "s"); dm_w_str(w, val); }
void dm_w_dict_sv_bool(dm_buf *w, const char *key, int val){ dict_key(w, key, "b"); dm_w_bool(w, val); }
void dm_w_dict_sv_i64(dm_buf *w, const char *key, int64_t val){ dict_key(w, key, "x"); dm_w_i64(w, val); }
void dm_w_dict_sv_u32(dm_buf *w, const char *key, uint32_t val){ dict_key(w, key, "u"); dm_w_u32(w, val); }

/* ---- reading ---- */
static size_t sig_one(const char *s, size_t n){
    if(!n) return 0;
    switch(s[0]){
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x': case 't': case 'd': case 'h':
    case 's': case 'o': case 'g': case 'v':
        return 1;
    case 'a': { size_t k = sig_one(s + 1, n - 1); return k ? k + 1 : 0; }
    case '(': case '{': {
        char close = s[0] == '(' ? ')' : '}';
        size_t i = 1;
        while(i < n && s[i] != close){ size_t k = sig_one(s + i, n - i); if(!k) return 0; i += k; }
        return i < n && i > 1 ? i + 1 : 0;
    }
    default: return 0;
    }
}
static size_t align_of(char t){
    switch(t){
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 4;
    }
}
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int dm_r_next(dm_rd *r, dm_val *v){
    memset(v, 0, sizeof *v);
    if(r->bad) return 0;
    if(r->sigpos >= r->siglen){
        if(!r->repeat) return 0;
        r->sigpos = 0;
    }
    if(r->repeat && r->sigpos == 0 && r->pos >= r->end) return 0;
    const char *s = r->sig + r->sigpos;
    size_t k = sig_one(s, r->siglen - r->sigpos);
    if(!k) goto bad;
    char t = s[0];
    size_t a = align_of(t), p = (r->pos + a - 1) & ~(a - 1);
    const uint8_t *b = r->b;
#define NEED(n) do{ if(p > r->end || r->end - p < (size_t)(n)) goto bad; }while(0)
    switch(t){
    case 'y': NEED(1); v->u = b[p]; p += 1; break;
    case 'n': NEED(2); v->u = (uint64_t)(int64_t)(int16_t)(b[p] | (b[p + 1] << 8)); p += 2; break;
    case 'q': NEED(2); v->u = (uint64_t)(b[p] | (b[p + 1] << 8)); p += 2; break;
    case 'i': NEED(4); v->u = (uint64_t)(int64_t)(int32_t)rd32(b + p); p += 4; break;
    case 'b': case 'u': case 'h': NEED(4); v->u = rd32(b + p); p += 4; break;
    case 'x': case 't': case 'd': NEED(8); v->u = (uint64_t)rd32(b + p) | ((uint64_t)rd32(b + p + 4) << 32); p += 8; break;
    case 's': case 'o': {
        NEED(4); uint32_t n = rd32(b + p); p += 4;
        if(n >= r->end - p) goto bad;           /* n bytes and the NUL */
        if(b[p + n] != 0) goto bad;
        v->s = (const char *)b + p; v->slen = n; p += n + 1;
        break;
    }
    case 'g': {
        NEED(1); uint32_t n = b[p]; p += 1;
        if(n >= r->end - p || b[p + n] != 0) goto bad;
        v->s = (const char *)b + p; v->slen = n; p += n + 1;
        break;
    }
    case 'v': {
        NEED(1); uint32_t n = b[p]; p += 1;
        if(n >= r->end - p || b[p + n] != 0 || !n) goto bad;
        const char *vs = (const char *)b + p; p += n + 1;
        if(sig_one(vs, n) != n) goto bad;       /* exactly one complete type */
        dm_rd sub = { b, p, r->end, vs, 0, n, 0, 0 };
        dm_val tmp;
        if(!dm_r_next(&sub, &tmp)) goto bad;
        dm_rd cont = { b, p, sub.pos, vs, 0, n, 0, 0 };
        v->sub = cont; p = sub.pos;
        break;
    }
    case 'a': {
        NEED(4); uint32_t n = rd32(b + p); p += 4;
        size_t ea = align_of(s[1]);
        p = (p + ea - 1) & ~(ea - 1);
        if(p > r->end || n > r->end - p) goto bad;
        dm_rd sub = { b, p, p + n, s + 1, 0, k - 1, 1, 0 };
        v->sub = sub; p += n;
        break;
    }
    case '(': case '{': {
        dm_rd sub = { b, p, r->end, s + 1, 0, k - 2, 0, 0 };
        dm_val tmp;
        while(dm_r_next(&sub, &tmp)){}
        if(sub.bad || sub.sigpos != sub.siglen) goto bad;
        dm_rd cont = { b, p, sub.pos, s + 1, 0, k - 2, 0, 0 };
        v->sub = cont; p = sub.pos;
        break;
    }
    default: goto bad;
    }
#undef NEED
    v->type = t;
    r->sigpos += k;
    r->pos = p;
    return 1;
bad:
    r->bad = 1; v->type = 0;
    return 0;
}

int dm_r_expect(dm_rd *r, char t, dm_val *v){ return dm_r_next(r, v) && v->type == t; }

int dm_r_dict_find(dm_rd dict, const char *key, dm_val *var){
    dm_val e;
    while(dm_r_next(&dict, &e)){
        if(e.type != '{') return 0;
        dm_rd en = e.sub; dm_val k, v;
        if(!dm_r_next(&en, &k) || k.type != 's' || !dm_r_next(&en, &v) || v.type != 'v') continue;
        if(strcmp(k.s, key)) continue;
        dm_rd vr = v.sub;
        return dm_r_next(&vr, var);
    }
    return 0;
}

long dm_parse(const uint8_t *buf, size_t n, dm_msg *m){
    memset(m, 0, sizeof *m);
    if(n < 16) return 0;
    if(buf[0] != 'l' || buf[3] != 1) return -1;
    uint32_t blen = rd32(buf + 4), flen = rd32(buf + 12);
    if(blen > DM_MAX_MSG || flen > DM_MAX_MSG) return -1;
    size_t hend = 16 + (size_t)flen, bstart = (hend + 7) & ~(size_t)7, total = bstart + blen;
    if(total > DM_MAX_MSG) return -1;
    if(n < total) return 0;
    m->type = buf[1]; m->flags = buf[2]; m->serial = rd32(buf + 8);
    dm_rd h = { buf, 16, hend, "(yv)", 0, 4, 1, 0 };
    dm_val f;
    while(dm_r_next(&h, &f)){
        dm_rd fr = f.sub; dm_val code, var, val;
        if(!dm_r_next(&fr, &code) || !dm_r_next(&fr, &var)) return -1;
        dm_rd vr = var.sub;
        if(!dm_r_next(&vr, &val)) return -1;
        switch(code.u){
        case 1: if(val.type == 'o') m->path = val.s; break;
        case 2: if(val.type == 's') m->iface = val.s; break;
        case 3: if(val.type == 's') m->member = val.s; break;
        case 4: if(val.type == 's') m->errname = val.s; break;
        case 5: if(val.type == 'u') m->reply_serial = (uint32_t)val.u; break;
        case 6: if(val.type == 's') m->dest = val.s; break;
        case 7: if(val.type == 's') m->sender = val.s; break;
        case 8: if(val.type == 'g') m->sig = val.s; break;
        default: break;
        }
    }
    if(h.bad) return -1;
    dm_rd body = { buf + bstart, 0, blen, m->sig ? m->sig : "", 0, m->sig ? strlen(m->sig) : 0, 0, 0 };
    m->body = body;
    return (long)total;
}

/* ---- the connection ---- */
static long ms_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

static int write_all(int fd, const uint8_t *p, size_t n, int timeout_ms){
    long until = ms_now() + timeout_ms;
    while(n){
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if(w > 0){ p += w; n -= (size_t)w; continue; }
        if(w < 0 && errno == EINTR) continue;
        if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
            long left = until - ms_now();
            if(left <= 0) return -ETIMEDOUT;
            struct pollfd pf = { fd, POLLOUT, 0 };
            poll(&pf, 1, (int)left);
            continue;
        }
        return w < 0 ? -errno : -EPIPE;
    }
    return 0;
}

uint32_t dm_send(dm_conn *c, int type, int flags, const char *dest, const char *path, const char *iface,
                 const char *member, const char *errname, uint32_t reply_serial, const char *sig, const dm_buf *body){
    if(!c || c->fd < 0 || (body && body->err)) return 0;
    uint32_t serial = ++c->serial;
    if(!serial) serial = ++c->serial;
    dm_buf h; dm_buf_init(&h);
    dm_w_u8(&h, 'l'); dm_w_u8(&h, (uint8_t)type); dm_w_u8(&h, (uint8_t)flags); dm_w_u8(&h, 1);
    dm_w_u32(&h, body ? (uint32_t)body->len : 0);
    dm_w_u32(&h, serial);
    size_t at = dm_w_array_begin(&h, 8);
    struct { uint8_t code; const char *sig; const char *val; } f[] = {
        { 1, "o", path }, { 2, "s", iface }, { 3, "s", member }, { 4, "s", errname }, { 6, "s", dest },
    };
    for(unsigned i = 0; i < sizeof f / sizeof *f; i++){
        if(!f[i].val) continue;
        dm_w_struct(&h); dm_w_u8(&h, f[i].code); dm_w_sig(&h, f[i].sig); dm_w_str(&h, f[i].val);
    }
    if(reply_serial){ dm_w_struct(&h); dm_w_u8(&h, 5); dm_w_sig(&h, "u"); dm_w_u32(&h, reply_serial); }
    if(sig && *sig && body && body->len){ dm_w_struct(&h); dm_w_u8(&h, 8); dm_w_sig(&h, "g"); dm_w_sig(&h, sig); }
    dm_w_array_end(&h, at);
    dm_w_align(&h, 8);
    int r = h.err ? -ENOMEM : write_all(c->fd, h.b, h.len, 1000);
    if(!r && body && body->len) r = write_all(c->fd, body->b, body->len, 1000);
    dm_buf_free(&h);
    return r ? 0 : serial;
}

uint32_t dm_add_match(dm_conn *c, const char *rule){
    dm_buf b; dm_buf_init(&b); dm_w_str(&b, rule);
    uint32_t s = dm_call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s", &b);
    dm_buf_free(&b);
    return s;
}

static int fill(dm_conn *c){       /* read what is there; 1 got bytes, 0 nothing now, -1 gone */
    for(;;){
        if(c->rcap - c->rlen < 4096){
            if(c->rcap >= DM_MAX_MSG + 4096) return -1;
            size_t nc = c->rcap ? c->rcap * 2 : 16384;
            uint8_t *nb = realloc(c->rb, nc);
            if(!nb) return -1;
            c->rb = nb; c->rcap = nc;
        }
        ssize_t n = recv(c->fd, c->rb + c->rlen, c->rcap - c->rlen, 0);
        if(n > 0){ c->rlen += (size_t)n; return 1; }
        if(n == 0) return -1;
        if(errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
}

int dm_read_msgs(dm_conn *c, dm_msg_fn fn, void *ctx){
    if(!c || c->fd < 0) return -1;
    int count = 0, gone = 0;
    for(;;){
        int f = fill(c);
        if(f < 0){ gone = 1; break; }
        if(f == 0) break;
    }
    size_t off = 0;
    for(;;){
        dm_msg m;
        long t = dm_parse(c->rb + off, c->rlen - off, &m);
        if(t < 0) return -1;
        if(t == 0) break;
        if(fn) fn(ctx, &m);
        count++;
        off += (size_t)t;
        if(c->fd < 0) return -1;   /* fn closed it */
    }
    if(off){ memmove(c->rb, c->rb + off, c->rlen - off); c->rlen -= off; }
    return gone ? -1 : count;
}

static int addr_from_env(char *out, size_t cap){
    const char *e = getenv("DBUS_SYSTEM_BUS_ADDRESS");
    if(!e) return 0;
    const char *p = strstr(e, "unix:path=");
    if(!p) return 0;
    p += 10;
    size_t n = strcspn(p, ",;");
    if(!n || n >= cap) return 0;
    memcpy(out, p, n); out[n] = 0;
    return 1;
}

static int read_line(int fd, char *out, size_t cap, long until){
    size_t n = 0;
    while(n + 1 < cap){
        long left = until - ms_now();
        if(left <= 0) return -ETIMEDOUT;
        struct pollfd pf = { fd, POLLIN, 0 };
        if(poll(&pf, 1, (int)left) <= 0) continue;
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if(r == 0) return -ECONNRESET;
        if(r < 0){ if(errno == EINTR || errno == EAGAIN) continue; return -errno; }
        out[n++] = ch;
        if(n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n'){ out[n - 2] = 0; return 0; }
    }
    return -EPROTO;
}

struct hello_wait { uint32_t serial; int done; dm_conn *c; };
static void hello_cb(void *ctx, const dm_msg *m){
    struct hello_wait *h = ctx;
    if(m->reply_serial != h->serial) return;
    if(m->type == DM_RETURN){
        dm_rd b = m->body; dm_val v;
        if(dm_r_expect(&b, 's', &v)) snprintf(h->c->unique, sizeof h->c->unique, "%s", v.s);
        h->done = 1;
    } else if(m->type == DM_ERROR) h->done = -1;
}

int dm_connect(dm_conn *c, const char *socket_path, int timeout_ms){
    memset(c, 0, sizeof *c);
    c->fd = -1;
    long until = ms_now() + timeout_ms;
    char envp[108];
    const char *cands[3]; int nc = 0;
    if(socket_path) cands[nc++] = socket_path;
    else {
        if(addr_from_env(envp, sizeof envp)) cands[nc++] = envp;
        cands[nc++] = "/var/run/dbus/system_bus_socket";
        cands[nc++] = "/run/dbus/system_bus_socket";
    }
    int err = -ENOENT;
    for(int i = 0; i < nc && c->fd < 0; i++){
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(fd < 0) return -errno;
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa); sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", cands[i]);
        if(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0){ c->fd = fd; break; }
        err = -errno;
        close(fd);
    }
    if(c->fd < 0) return err;
    /* EXTERNAL: our uid, as a decimal string, hex encoded */
    char uid[16], hex[40], line[256];
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    for(size_t i = 0; uid[i]; i++) snprintf(hex + 2 * i, 3, "%02x", (unsigned char)uid[i]);
    snprintf(line, sizeof line, "AUTH EXTERNAL %s\r\n", hex);
    int r = write_all(c->fd, (const uint8_t *)"", 1, 1000);
    if(!r) r = write_all(c->fd, (const uint8_t *)line, strlen(line), 1000);
    if(!r) r = read_line(c->fd, line, sizeof line, until);
    if(!r && strncmp(line, "OK ", 3)) r = -EPROTO;
    if(!r) r = write_all(c->fd, (const uint8_t *)"BEGIN\r\n", 7, 1000);
    if(r){ dm_close(c); return r; }
    int fl = fcntl(c->fd, F_GETFL);
    fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    struct hello_wait h = { 0, 0, c };
    h.serial = dm_call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL);
    if(!h.serial){ dm_close(c); return -EIO; }
    while(!h.done){
        long left = until - ms_now();
        if(left <= 0){ dm_close(c); return -ETIMEDOUT; }
        struct pollfd pf = { c->fd, POLLIN, 0 };
        poll(&pf, 1, (int)left);
        if(dm_read_msgs(c, hello_cb, &h) < 0){ dm_close(c); return -ECONNRESET; }
    }
    if(h.done < 0){ dm_close(c); return -EPROTO; }
    return 0;
}

void dm_close(dm_conn *c){
    if(c->fd >= 0) close(c->fd);
    c->fd = -1;
    free(c->rb); c->rb = NULL; c->rlen = c->rcap = 0;
}
