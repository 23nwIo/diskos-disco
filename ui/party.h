/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Party mode: the guests' link and its rules (party.c). Main thread only. */
#ifndef PARTY_H
#define PARTY_H
#include "remote_core.h"
/* A guest's request (c->guest) or the host's api/party (RMT_CMD_PARTY): 0 done (*body: a JSON answer or NULL),
 * -1 refused with *why. */
int  party_dispatch(const rmt_cmd_t *c, const char **why, char **body);
/* The party link's name while party mode is on, else "". */
const char *party_link_name(void);
/* What the guests' link is for: 0 off, 1 the party, 2 the game Hit&Guess (hitguess.c) */
int  party_mode(void);
/* the volume as guests and players see it (0..100 of the host's maximum), -1 when they may not change it */
int  party_volume_now(void);
/* Who queued a song (queue_item_who): the guest's name, "a guest" without one, "" for the host's own. */
const char *party_guest_name(int who);
/* Where guests' uploads go (absolute; "" while the party or its uploads are off) and the largest file in bytes. */
const char *party_upload_dir(long long *max_bytes);
#endif
