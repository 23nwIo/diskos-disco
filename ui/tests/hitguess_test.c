/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hit&Guess rules (hitguess_core.c): joining, turns and rounds, who knows and guesses, ticks and points, the end,
 * the ranking and matching typed guesses. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../hitguess_core.h"
static hg_game_t g;
static const char *why;
#define OK(x) do { if(!(x)){ fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while(0)
static void play(int marks0, int marks1, int marks2, int left){
    hg_song(&g, 100 + g.nt, 0, "Song", "Band", "Album", "1985-06-01");
    hg_reveal(&g);
    hg_mark(&g, 0, marks0); hg_mark(&g, 1, marks1); hg_mark(&g, 2, marks2);
    hg_confirm(&g, left);
}
int main(void){
    int set[HG_S_N]; for(int k = 0; k < HG_S_N; k++) set[k] = HG_KEYS[k].def;
    OK(set[HG_S_PICK] == 1 && set[HG_S_YEAR] == 1 && set[HG_S_STAGGER] == 1 && set[HG_S_COUNTDOWN] == 0 && set[HG_S_CDEXTRA] == 0
       && set[HG_S_ALONG] == 1 && set[HG_S_START] == 0 && set[HG_S_SKIP] == 1 && set[HG_S_REPEAT] == 0 && set[HG_S_LATE] == 1
       && set[HG_S_FORM] == 0 && set[HG_S_REPLAY] == 1);               /* Jo's defaults */
    set[HG_S_WIN] = 1; set[HG_S_WIN_N] = 2;                           /* two rounds */
    hg_reset(&g, set);
    OK(hg_start(&g, &why) == -1);
    OK(hg_join(&g, "aaaaaaaa", "  Anna ", &why) == 0 && !strcmp(g.p[0].name, "Anna"));
    OK(hg_join(&g, "bbbbbbbb", "anna", &why) == -1);                  /* the same name */
    OK(hg_join(&g, "bbbbbbbb", "", &why) == -1);
    OK(hg_join(&g, "bbbbbbbb", "Ben", &why) == 1);
    OK(hg_join(&g, "aaaaaaaa", "Anni", &why) == 0 && !strcmp(g.p[0].name, "Anni"));   /* back: same player, new name */
    OK(hg_start(&g, &why) == 0 && g.phase == HG_PICK && g.cur == 0);
    /* Anni picks: she knows the song, Ben guesses; reveal, ticks, points */
    hg_song(&g, 7, 0, "Take On Me", "a-ha", "Hunting High and Low", "1985-06-01");
    OK(g.phase == HG_PLAY && g.t[0].year == 1985 && hg_knows(&g, 0) && !hg_knows(&g, 1) && !hg_guesses(&g, 0) && hg_guesses(&g, 1));
    OK(hg_join(&g, "cccccccc", "Cleo", &why) == 2 && g.p[2].joined == 1);   /* joins late (allowed by default) */
    hg_reveal(&g);
    OK(hg_knows(&g, 1) && g.phase == HG_REVEAL);
    OK(hg_may_stop(&g, 0) && !hg_may_stop(&g, 1));                    /* skip: only that player (default) */
    hg_mark(&g, 1, HG_M_ARTIST | HG_M_YEAR | HG_M_NEAR);             /* exact wins over near */
    hg_mark(&g, 2, HG_M_TITLE | HG_M_NEAR);
    OK(hg_confirm(&g, -1) == HG_PICK && g.cur == 1 && g.rounds == 0);
    OK(g.p[1].pa == 2 && g.p[1].py == 2 && g.p[2].pt == 2 && g.p[2].py == 1 && hg_total(&g.p[1]) == 4);   /* half points: 2 + 2, 2 + 1 */
    hg_mark(&g, 0, 0); { char b[16]; OK(!strcmp(hg_pts_str(3, b, 16), "1.5") && !strcmp(hg_pts_str(4, b, 16), "2") && hg_points(HG_M_ANEAR | HG_M_TITLE | HG_M_NEAR) == 4); }
    /* a song without a date: the year can't be ticked */
    hg_song(&g, 8, 1, "Song", "Band", "", "");
    OK(g.t[1].year == 0 && !hg_knows(&g, 1) && hg_guesses(&g, 1));     /* chance: Ben doesn't know it and guesses along */
    hg_reveal(&g); hg_mark(&g, 0, HG_M_YEAR | HG_M_TITLE);
    OK(g.t[1].mark[0] == HG_M_TITLE);
    OK(hg_confirm(&g, -1) == HG_PICK && g.cur == 2);
    play(0, 0, 0, -1);                                                /* Cleo's turn: round 1 complete */
    OK(g.rounds == 1 && g.cur == 0 && g.phase == HG_PICK);
    /* the host removes Ben while it is his turn next: his turn is skipped */
    play(0, 0, 0, -1); OK(g.cur == 1);
    hg_remove(&g, 1, -1);
    OK(g.cur == 2 && !g.p[1].in && g.phase == HG_PICK);
    play(0, 0, 0, -1);
    OK(g.phase == HG_END && g.rounds == 2 && g.ended_why == 0);       /* two rounds */
    int idx[HG_PLAYERS], place[HG_PLAYERS];
    OK(hg_rank(&g, idx, place) == 3 && idx[0] == 1 && place[0] == 1 && idx[1] == 2 && idx[2] == 0 && place[1] == 2 && place[2] == 3);   /* Ben 3, Cleo 2, Anni 1 */
    /* no song left ends it; a score as the goal */
    set[HG_S_WIN] = 2; set[HG_S_WIN_N] = 4; hg_reset(&g, set);
    hg_join(&g, "aaaaaaaa", "A", &why); hg_join(&g, "bbbbbbbb", "B", &why); hg_join(&g, "cccccccc", "C", &why); hg_start(&g, &why);
    play(0, HG_M_ARTIST | HG_M_TITLE, 0, -1); OK(g.phase == HG_PICK);
    play(HG_M_YEAR, 0, 0, -1); OK(g.phase == HG_PICK);
    play(0, HG_M_ARTIST | HG_M_TITLE, 0, -1); OK(g.phase == HG_END && g.ended_why == 0);   /* B: 4 points */
    hg_again(&g);
    OK(g.phase == HG_LOBBY && g.np == 3 && hg_total(&g.p[1]) == 0 && g.nt == 0);
    hg_start(&g, &why); play(0, 0, 0, 0); OK(g.phase == HG_END && g.ended_why == 1);
    /* joining later off */
    set[HG_S_LATE] = 0; hg_reset(&g, set); hg_join(&g, "aaaaaaaa", "A", &why); hg_start(&g, &why);
    OK(hg_join(&g, "bbbbbbbb", "B", &why) == -1 && hg_join(&g, "aaaaaaaa", "A", &why) == 0);
    /* typed guesses */
    set[HG_S_FORM] = 1; set[HG_S_LATE] = 1; hg_reset(&g, set);
    hg_join(&g, "aaaaaaaa", "A", &why); hg_join(&g, "bbbbbbbb", "B", &why); hg_join(&g, "cccccccc", "C", &why); hg_start(&g, &why);
    hg_song(&g, 1, 0, "Don't Stop Me Now (2011 Remaster)", "Queen", "Jazz", "1978-10-26");
    OK(hg_tip(&g, 0, "x", "y", 1, &why) == -1);                      /* A knows it */
    OK(hg_tip(&g, 1, "queen", "dont stop me now", 1979, &why) == 0 && !hg_all_tipped(&g));
    OK(hg_tip(&g, 2, "The Queen", "Dont stop me know", 1990, &why) == 0 && hg_all_tipped(&g));
    hg_reveal(&g);
    OK(g.t[0].mark[1] == (HG_M_ARTIST | HG_M_TITLE | HG_M_NEAR) && g.t[0].mark[2] == (HG_M_ARTIST | HG_M_TNEAR));   /* "know": almost */
    hg_mark(&g, 2, HG_M_TITLE | HG_M_TNEAR | HG_M_ANEAR); OK(g.t[0].mark[2] == (HG_M_TITLE | HG_M_ANEAR));   /* right wins over almost */
    OK(hg_tip(&g, 1, "a", "b", 1978, &why) == -1);                   /* too late */
    OK(hg_match("Beyonce", "Beyonc\xc3\xa9") == 2 && hg_match("Motorhead", "Mot\xc3\xb6rhead") == 2 && hg_match("Simon & Garfunkel", "Simon and Garfunkel") == 2
       && !hg_match("ABBA", "Queen") && !hg_match("", "Queen") && hg_match("Sweet Child of Mine", "Sweet Child O' Mine") == 1
       && hg_match("Bohemian Rapsody", "Bohemian Rhapsody") == 1 && hg_match("Smells like teen spirit", "Smells Like Teen Spirit - Live") == 2
       && hg_match("Take on", "Take On Me") == 1 && hg_match("Me", "Take On Me") == 0 && hg_match("Bohemian", "Bohemian Rhapsody") == 1);
    /* almost right off: only right counts */
    set[HG_S_STAGGER] = 0; hg_reset(&g, set);
    hg_join(&g, "aaaaaaaa", "A", &why); hg_join(&g, "bbbbbbbb", "B", &why); hg_start(&g, &why);
    hg_song(&g, 1, 0, "Africa", "Toto", "Toto IV", "1982");
    hg_tip(&g, 1, "Totto", "Africa", 1983, &why); hg_reveal(&g);
    OK(g.t[0].mark[1] == HG_M_TITLE);
    hg_mark(&g, 1, HG_M_ANEAR | HG_M_TITLE | HG_M_NEAR); OK(g.t[0].mark[1] == HG_M_TITLE);
    OK(hg_year_of("1985-06-01") == 1985 && hg_year_of("") == 0 && hg_year_of("85") == 0);
    puts("hitguess_test: ok");
    return 0;
}
