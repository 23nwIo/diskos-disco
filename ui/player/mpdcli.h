/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* A small client for MPD's text protocol over its local socket (docs/MPD_CORE.md). Blocking with a timeout for
 * commands; a second connection sits in "idle" so MPD's changes arrive as events on a descriptor the main loop polls.
 * The parsing helpers are pure, for ui/tests. */
#ifndef DISKOS_PLAYER_MPDCLI_H
#define DISKOS_PLAYER_MPDCLI_H
#include <stddef.h>

#define MC_RESP_MAX 16384

/* Connect to the socket and read the greeting. The descriptor, or -1 (errno set). timeout_ms applies to every read. */
int mc_connect(const char *sock_path, int timeout_ms);

/* Send one command line (without newline) and read the answer up to "OK" or "ACK ...". The answer's lines (without the
 * final OK) go to out, NUL terminated. 0 = OK, 1 = ACK (the ACK line is in out), -1 = I/O error or timeout. */
int mc_cmd(int fd, const char *cmd, char *out, size_t cap);

/* Idle connection: send "idle player options mixer" (mc_idle_begin); when the descriptor becomes readable, read the
 * answer with mc_idle_end, which returns a bitmask of MC_EV_* (0 if nothing), or -1 on error. */
enum { MC_EV_PLAYER = 1, MC_EV_OPTIONS = 2, MC_EV_MIXER = 4, MC_EV_OTHER = 8 };
int mc_idle_begin(int fd);
int mc_idle_end(int fd);

/* The value of "key: value" in an answer, copied to val (NUL terminated); 1 if found, 0 if not. */
int mc_get(const char *resp, const char *key, char *val, size_t cap);

/* Quote an argument for a command line: "..." with \ and " escaped. Returns the length, or -1 if it does not fit. */
int mc_quote(char *out, size_t cap, const char *arg);
#endif
