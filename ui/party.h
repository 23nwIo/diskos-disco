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
/* Who queued a song (queue_item_who): the guest's name, "a guest" without one, "" for the host's own. */
const char *party_guest_name(int who);
#endif
