/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* A small D-Bus client without libdbus, for the static musl binaries (mq_ui, the own player): the system bus over its
 * Unix socket, EXTERNAL authentication, and the wire format (little endian only: the Disc and every PC we test on are).
 * Enough to call methods, receive signals and method calls, and answer them. No file descriptor passing.
 *
 * Writing: a dm_buf collects a body; the dm_w_* functions align as the D-Bus spec says (offsets are body-relative, the
 * body starts 8-aligned in a message). The caller passes the body's signature to dm_send.
 * Reading: dm_read_msgs parses whatever arrived; each message comes with a dm_rd over its body, and dm_r_next walks it
 * value by value (containers give a sub-reader). */
#ifndef DISKOS_DBUSMINI_H
#define DISKOS_DBUSMINI_H
#include <stddef.h>
#include <stdint.h>

enum { DM_CALL = 1, DM_RETURN = 2, DM_ERROR = 3, DM_SIGNAL = 4 };
#define DM_NO_REPLY 0x1            /* header flag: no reply expected */

typedef struct { uint8_t *b; size_t len, cap; int err; } dm_buf;
void dm_buf_init(dm_buf *w);
void dm_buf_free(dm_buf *w);
void dm_w_align(dm_buf *w, size_t a);
void dm_w_u8(dm_buf *w, uint8_t v);
void dm_w_bool(dm_buf *w, int v);
void dm_w_u16(dm_buf *w, uint16_t v);
void dm_w_u32(dm_buf *w, uint32_t v);
void dm_w_i32(dm_buf *w, int32_t v);
void dm_w_i64(dm_buf *w, int64_t v);
void dm_w_str(dm_buf *w, const char *s);     /* 's' and 'o' */
void dm_w_sig(dm_buf *w, const char *s);     /* 'g' */
/* an array: begin returns the place of its length; elem_align is its element type's alignment (8 for structs and
 * dict entries) */
size_t dm_w_array_begin(dm_buf *w, size_t elem_align);
void dm_w_array_end(dm_buf *w, size_t at);
/* the start of a struct or dict entry */
#define dm_w_struct(w) dm_w_align((w), 8)
/* a dict entry {sv} with a string key and a variant holding one basic value of type t ("s", "b", "u", "q", "x", "i", "o") */
void dm_w_dict_sv_str(dm_buf *w, const char *key, const char *val);
void dm_w_dict_sv_bool(dm_buf *w, const char *key, int val);
void dm_w_dict_sv_i64(dm_buf *w, const char *key, int64_t val);
void dm_w_dict_sv_u32(dm_buf *w, const char *key, uint32_t val);

/* ---- reading ---- */
typedef struct dm_rd {
    const uint8_t *b;              /* the body */
    size_t pos, end;               /* body-relative */
    const char *sig;               /* the signature still to read (an array element's, repeated) */
    size_t sigpos, siglen;
    int repeat;                    /* an array: the element signature repeats until end */
    int bad;                       /* malformed data was met */
} dm_rd;
typedef struct {
    char type;                     /* 0 at the end */
    uint64_t u;                    /* y b n q i u x t h (signed ones sign-extended) */
    const char *s; size_t slen;    /* s o g: NUL terminated inside the message */
    dm_rd sub;                     /* a v ( { : the contents */
} dm_val;
/* The next value; 1, or 0 at the end or on malformed data (r->bad set). */
int dm_r_next(dm_rd *r, dm_val *v);
/* Convenience: the next value must be of type t; returns 1 and fills v, else 0. */
int dm_r_expect(dm_rd *r, char t, dm_val *v);
/* In an a{sv} reader: the variant for key (case-sensitive); 1 and *var filled (var->type is the contained type), or 0. */
int dm_r_dict_find(dm_rd dict, const char *key, dm_val *var);

typedef struct {
    int type, flags;
    uint32_t serial, reply_serial;
    const char *path, *iface, *member, *errname, *dest, *sender, *sig;
    dm_rd body;
} dm_msg;
/* Parse one message at buf (n bytes). Returns its total length, 0 when more bytes are needed, -1 when it is malformed or
 * big endian (the caller drops the connection). The strings point into buf. */
long dm_parse(const uint8_t *buf, size_t n, dm_msg *m);

/* ---- the connection ---- */
typedef struct {
    int fd;
    uint32_t serial;
    char unique[64];               /* our bus name (":1.42") after dm_connect */
    uint8_t *rb; size_t rlen, rcap;
} dm_conn;
/* Connect to the system bus: $DBUS_SYSTEM_BUS_ADDRESS (unix:path=...) if set, else /var/run/dbus/system_bus_socket,
 * else /run/dbus/system_bus_socket; authenticate, say Hello (waits at most timeout_ms in all). 0, or -errno
 * (-EPROTO when the bus said no). The socket is non-blocking afterwards. */
int dm_connect(dm_conn *c, const char *socket_path, int timeout_ms);
void dm_close(dm_conn *c);
/* Send a message (path/iface/member/dest/errname may be NULL; sig "" or NULL for no body). Returns its serial, 0 on
 * error (the connection is then better closed). */
uint32_t dm_send(dm_conn *c, int type, int flags, const char *dest, const char *path, const char *iface,
                 const char *member, const char *errname, uint32_t reply_serial, const char *sig, const dm_buf *body);
#define dm_call(c, dest, path, iface, member, sig, body) dm_send((c), DM_CALL, 0, (dest), (path), (iface), (member), NULL, 0, (sig), (body))
/* org.freedesktop.DBus.AddMatch(rule), reply not waited for */
uint32_t dm_add_match(dm_conn *c, const char *rule);
/* Read what the socket has and call fn for every complete message. Returns the number of messages, or -1 when the
 * connection is gone or broken (the caller closes it). */
typedef void (*dm_msg_fn)(void *ctx, const dm_msg *m);
int dm_read_msgs(dm_conn *c, dm_msg_fn fn, void *ctx);
#endif
