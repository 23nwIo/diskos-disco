/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Public modes (Jo, 2026-11-22): the guests' link and its rules (party.c). One mode at a time, mutually exclusive:
 * Jukebox (guests queue songs; called party mode before) or the game Hit&Guess; more multi-user modes can follow.
 * Main thread only. */
#ifndef PARTY_H
#define PARTY_H
#include "remote_core.h"
/* A guest's request (c->guest) or the host's api/party (RMT_CMD_PARTY): 0 done (*body: a JSON answer or NULL),
 * -1 refused with *why. */
int  party_dispatch(const rmt_cmd_t *c, const char **why, char **body);
/* The guests' link's name while a public mode is on, else "". */
const char *party_link_name(void);
/* The public mode: what the guests' link is for (party.conf "on"; keep the numbers, they are on users' cards) */
enum { PUBLIC_OFF = 0, PUBLIC_JUKEBOX = 1, PUBLIC_HITGUESS = 2 };
int  party_mode(void);
/* The mode's name for people: "Off", "Jukebox", "Hit&Guess" */
const char *party_mode_label(int mode);
/* End the public mode (mode Off; the link's name and every other setting stay): 0 done, -1 not saved (no card). */
int  party_end(void);
/* Settings > Connectivity > Remote Control > Public Mode: the row shows the mode, a tap offers to end it */
extern char g_public_lbl[16];
void public_label_refresh(void);
void public_end_ask(void);
/* the volume as guests and players see it (0..100 of the host's maximum), -1 when they may not change it */
int  party_volume_now(void);
/* Who queued a song (queue_item_who): the guest's name, "a guest" without one, "" for the host's own. */
const char *party_guest_name(int who);
/* Where guests' uploads go (absolute; "" unless the mode is Jukebox with uploads on) and the largest file in bytes. */
const char *party_upload_dir(long long *max_bytes);
#endif
