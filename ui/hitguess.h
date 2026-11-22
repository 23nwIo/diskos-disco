/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hit&Guess on the Disc (hitguess.c): the game of hitguess_core.c with the library, the player, the phones and the
 * host's page. Runs while the guests' link is in game mode (public mode Hit&Guess, party_mode() PUBLIC_HITGUESS). Main thread only. */
#ifndef HITGUESS_H
#define HITGUESS_H
#include "remote_core.h"
/* RMT_CMD_GAME / RMT_CMD_GAME_SEARCH (a player, c->gid) and RMT_CMD_GAME_ADMIN (the host): 0 with *body (JSON) or
 * -1 with *why */
int  hitguess_dispatch(const rmt_cmd_t *c, const char **why, char **body);
/* may this phone fetch the cover of the song playing (revealed, or the player who chose it)? */
int  hitguess_cover_ok(const char *gid);
/* main loop (remote_pump): stops a part or a song at its end, the countdown */
void hitguess_tick(void);
#define HITGUESS_LOG_DIR "/tmp/hitguess"
#endif
