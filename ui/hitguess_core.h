/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hit&Guess (Jo, 2026-11-18): the game's rules without the Disc - players, turns, phases, points, the end and the
 * ranking. Pure C (no LVGL, no player, no clock: callers pass the time in ms), so the host tests drive it directly.
 * hitguess.c plays the songs, keeps the settings on the SD card, answers the phones and writes the log.
 *
 * A game: players join in the LOBBY with their phone (a name; their browser's random id tells them apart). The host
 * starts it. Then the players take turns in the order they joined: the player whose turn it is (A) PICKs a song (or
 * lets chance pick), it PLAYs and everybody guesses artist, title and - when the song has a date and the setup counts
 * years - the year. A REVEALs the song to everybody, ticks what each player got right and confirms; then the next
 * player (B) has the turn, who may already choose while A is still ticking. The game ENDs after a number of songs, of
 * rounds or at a score, or when no song is left. */
#ifndef HITGUESS_CORE_H
#define HITGUESS_CORE_H
#include <stdint.h>

#define HG_PLAYERS   32
#define HG_TURNS     400           /* songs per game at most */
#define HG_NAME_MAX  24            /* a player's name (bytes) */
#define HG_GID_MAX   24            /* the browser's random id */
#define HG_TIP_MAX   96            /* a typed guess */
#define HG_TXT_MAX   160           /* a song's title / artist / album as the game keeps it */

/* The setup (hitguess.conf on the SD card; api/game_admin): */
enum { HG_S_POOL,      /* 0 the whole library, 1 a folder (and its subfolders), 2 a playlist */
       HG_S_PICK,      /* 0 the player chooses, 1 chooses with 3 random suggestions, 2 only chance */
       HG_S_RNDKNOWN,  /* 1: a song chance picked is shown to the player whose turn it is */
       HG_S_ALONG,     /* 1: that player guesses along when they don't know the song */
       HG_S_YEAR,      /* 1: the year counts (songs with a date) */
       HG_S_STAGGER,   /* 1: almost right is half a point (artist, title similar; year off by 1..2); 0: right only */
       HG_S_FORM,      /* 1: players type their guesses (matched for the ticks); 0: they say them aloud */
       HG_S_COUNTDOWN, /* 1: guessing ends when the song (or its part) has played plus HG_S_CDEXTRA seconds */
       HG_S_CDEXTRA,   /* seconds */
       HG_S_START,     /* 0 songs play from the start, 1 a part of HG_S_SNIP seconds from a random place */
       HG_S_SNIP,      /* seconds */
       HG_S_REPLAY,    /* 1: the player whose turn it is may play the part again */
       HG_S_SKIP,      /* who may stop the song once it is revealed: 0 nobody, 1 that player, 2 that player or the next, 3 all */
       HG_S_WIN,       /* 0 a number of songs, 1 of rounds, 2 a score */
       HG_S_WIN_N,
       HG_S_REPEAT,    /* 1: a song may come up again (0: the game ends when every song has played) */
       HG_S_LATE,      /* 1: players may join a game that runs */
       HG_S_N };
typedef struct { const char *key; int min, max, def; } hg_key_t;
extern const hg_key_t HG_KEYS[HG_S_N];

enum { HG_LOBBY = 0, HG_PICK, HG_PLAY, HG_REVEAL, HG_END };
/* what a player got right in a turn (hg_turn_t.mark) */
#define HG_M_ARTIST 1
#define HG_M_TITLE  2
#define HG_M_YEAR   4              /* the exact year */
#define HG_M_NEAR   8              /* off by 1..2 years (HG_S_STAGGER) */
#define HG_M_ANEAR  16             /* the artist almost right (HG_S_STAGGER) */
#define HG_M_TNEAR  32             /* the title almost right (HG_S_STAGGER) */
/* Points (Jo, 2026-11-18): right 1 each (artist, title, year), almost right half a point. Kept in HALF points
 * (hg_points, hg_total, pa/pt/py); hg_pts_str writes them for people ("3", "3.5"). */

typedef struct {
    char gid[HG_GID_MAX + 1], name[HG_NAME_MAX + 1];
    int  in;                       /* 1 plays, 0 left or removed */
    int  pa, pt, py;               /* half points by artist, title, year */
    int  joined;                   /* the number of songs played before they joined */
    char tip_a[HG_TIP_MAX], tip_t[HG_TIP_MAX]; int tip_y, tipped;   /* this turn's typed guess */
} hg_player_t;
typedef struct {
    int  song;                     /* SONG.ID */
    char title[HG_TXT_MAX], artist[HG_TXT_MAX], album[HG_TXT_MAX], date[11];
    int  player;                   /* whose turn it was */
    int  chance;                   /* 1: chance picked it */
    int  year;                     /* the year that counted, 0 none */
    unsigned char mark[HG_PLAYERS];
    int  skipped;                  /* the host skipped the turn: no points */
} hg_turn_t;
typedef struct {
    int  set[HG_S_N];
    int  phase;
    hg_player_t p[HG_PLAYERS]; int np;
    hg_turn_t t[HG_TURNS]; int nt;  /* t[nt-1] is the current song from PLAY on */
    int  cur;                      /* whose turn (player index), -1 none */
    int  rounds;                   /* completed rounds */
    int  first;                    /* who began the round running now */
    /* B's early choice (made while A's song is revealed): */
    int  next_song, next_chance;
    int  ended_why;                /* 0 songs/rounds/score reached, 1 no song left, 2 the host ended it */
} hg_game_t;

/* a new game with this setup: no players, the lobby */
void hg_reset(hg_game_t *g, const int *set);
/* the same players, points back to 0, back to the lobby */
void hg_again(hg_game_t *g);
/* A phone joins or comes back: its index, or -1 with *why (late joining off, full, no name). A known gid keeps its
 * player (and takes the new name). */
int  hg_join(hg_game_t *g, const char *gid, const char *name, const char **why);
int  hg_find(const hg_game_t *g, const char *gid);
/* The host starts the game: -1 with *why when nobody plays yet */
int  hg_start(hg_game_t *g, const char **why);
/* the next playing player after i (wraps), -1 none */
int  hg_after(const hg_game_t *g, int i);
/* A song starts for player cur: a new turn (PLAY); title etc. as the library has them */
void hg_song(hg_game_t *g, int song, int chance, const char *title, const char *artist, const char *album, const char *date);
/* does the turn count a year (the setup's year on and the song has a date)? the year, else 0 */
int  hg_year_of(const char *date);
/* may player i see the song before it is revealed? */
int  hg_knows(const hg_game_t *g, int i);
/* may player i guess this turn? */
int  hg_guesses(const hg_game_t *g, int i);
/* a typed guess (HG_S_FORM): 0, or -1 with *why */
int  hg_tip(hg_game_t *g, int i, const char *artist, const char *title, int year, const char **why);
/* every guessing player has typed a guess */
int  hg_all_tipped(const hg_game_t *g);
/* PLAY -> REVEAL; with typed guesses the ticks start as the matches */
void hg_reveal(hg_game_t *g);
/* the ticks for player i (HG_M_*, only what the turn allows) */
void hg_mark(hg_game_t *g, int i, int marks);
/* the half points one mark set is worth */
int  hg_points(int marks);
/* half points as text: "3", "3.5" */
const char *hg_pts_str(int halves, char *buf, int cap);
/* A confirms: the points count, then the next turn (PICK) or the END. pool_left: songs that can still come up
 * (-1 unknown). Returns the new phase. */
int  hg_confirm(hg_game_t *g, int pool_left);
/* the host: skip the current turn (no points), remove a player, end the game */
void hg_skip(hg_game_t *g, int pool_left);
void hg_remove(hg_game_t *g, int i, int pool_left);
void hg_end(hg_game_t *g, int why);
/* may player i stop the revealed song (HG_S_SKIP)? */
int  hg_may_stop(const hg_game_t *g, int i);
/* the ranking: indices of every player who ever played, best first (a tie shares the place, in join order);
 * returns the count. place[k] = the shared place (1-based) of rank k. */
int  hg_rank(const hg_game_t *g, int *idx, int *place);
int  hg_total(const hg_player_t *p);
/* a typed guess against the truth: 2 right (case, accents of Latin letters, "the", brackets and punctuation don't
 * matter), 1 almost (one typo per five letters, or a good part of it: "Take on" for "Take On Me"), 0 wrong */
int  hg_match(const char *guess, const char *truth);
#endif
