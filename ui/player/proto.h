/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Frames of the UI <-> player protocol (docs/PLAYER_PROTOCOL.md): ASCII text, TAG (4 hex) + LEN (4 hex, length of the
 * whole frame in characters) + data, no terminator. Pure functions, no I/O, so ui/tests can drive them on the host. */
#ifndef DISKOS_PLAYER_PROTO_H
#define DISKOS_PLAYER_PROTO_H
#include <stddef.h>

#define PP_MAX_FRAME 8192          /* msgsize of both queues */

typedef struct {
    unsigned tag;                  /* e.g. 0x0715 */
    const char *data;              /* the characters after TAG+LEN (not terminated) */
    int dlen;
} pp_frame_t;

/* Split a received frame. 0 on success; -1 if it is shorter than 8 characters, TAG or LEN is not hex, or LEN does not
 * match the received length (LEN larger than n, or smaller than 8). A LEN smaller than n is accepted and the rest is
 * ignored (the queue hands over exactly one frame, so that does not happen with the UI). */
int pp_parse(const char *buf, size_t n, pp_frame_t *f);

/* The value of a class 1 frame (TTTT000Cvvvv). -1 if the frame is not exactly 4 hex characters of data. */
int pp_class1(const pp_frame_t *f);

/* Formatters. Each writes a frame plus a terminating NUL into out and returns the frame length (without the NUL), or
 * -1 if it does not fit. */
int pp_fmt_a1(char *out, size_t cap, unsigned long ms);                  /* a1030010<ms 8h> position */
int pp_fmt_a714(char *out, size_t cap, int volume);                      /* a714000C00<VV> volume 0..120 */
int pp_fmt_a607(char *out, size_t cap, int mode);                        /* a607000C<mode 4h> work mode */
int pp_fmt_a2(char *out, size_t cap, const char *json);                  /* a202 LEN <json> state */

/* Append s to out as a JSON string body (without the quotes), escaping " \ and control characters. Returns the new
 * length, or -1 if it does not fit (out stays terminated at its old length). */
int pp_json_escape(char *out, size_t cap, size_t len, const char *s);
#endif
