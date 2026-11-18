/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hit&Guess: the game's rules (see hitguess_core.h). Defaults by Jo, 2026-11-18. */
#include "hitguess_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const hg_key_t HG_KEYS[HG_S_N] = {
    [HG_S_POOL] = { "pool", 0, 2, 0 },          [HG_S_PICK] = { "pick", 0, 2, 1 },          [HG_S_RNDKNOWN] = { "rndknown", 0, 1, 0 },
    [HG_S_ALONG] = { "along", 0, 1, 1 },        [HG_S_YEAR] = { "year", 0, 1, 1 },          [HG_S_STAGGER] = { "stagger", 0, 1, 1 },
    [HG_S_FORM] = { "form", 0, 1, 0 },          [HG_S_COUNTDOWN] = { "countdown", 0, 1, 0 }, [HG_S_CDEXTRA] = { "cdextra", 0, 600, 0 },
    [HG_S_START] = { "start", 0, 1, 0 },        [HG_S_SNIP] = { "snip", 3, 120, 10 },       [HG_S_REPLAY] = { "replay", 0, 1, 1 },
    [HG_S_SKIP] = { "skip", 0, 3, 1 },          [HG_S_WIN] = { "win", 0, 2, 1 },            [HG_S_WIN_N] = { "win_n", 1, HG_TURNS, 3 },
    [HG_S_REPEAT] = { "repeat", 0, 1, 0 },      [HG_S_LATE] = { "late", 0, 1, 1 },
};

void hg_reset(hg_game_t *g, const int *set){
    memset(g, 0, sizeof *g);
    for(int k = 0; k < HG_S_N; k++) g->set[k] = set ? set[k] : HG_KEYS[k].def;
    g->phase = HG_LOBBY; g->cur = -1; g->first = -1;
}
void hg_again(hg_game_t *g){
    hg_game_t *n = malloc(sizeof *n);
    if(!n) return;
    hg_reset(n, g->set);
    for(int i = 0; i < g->np; i++){                       /* who still plays, in the same order */
        if(!g->p[i].in) continue;
        hg_player_t *p = &n->p[n->np++];
        snprintf(p->gid, sizeof p->gid, "%s", g->p[i].gid); snprintf(p->name, sizeof p->name, "%s", g->p[i].name);
        p->in = 1;
    }
    memcpy(g, n, sizeof *g);
    free(n);
}
int hg_find(const hg_game_t *g, const char *gid){
    if(!gid || !gid[0]) return -1;
    for(int i = 0; i < g->np; i++) if(!strcmp(g->p[i].gid, gid)) return i;
    return -1;
}
int hg_join(hg_game_t *g, const char *gid, const char *name, const char **why){
    int i = hg_find(g, gid);
    char nm[HG_NAME_MAX + 1]; snprintf(nm, sizeof nm, "%s", name ? name : "");
    for(char *s = nm; *s; s++) if((unsigned char)*s < 0x20) *s = ' ';
    while(nm[0] == ' ') memmove(nm, nm + 1, strlen(nm));
    for(size_t n = strlen(nm); n && nm[n - 1] == ' '; ) nm[--n] = 0;
    if(!nm[0]){ *why = "Enter your name first"; return -1; }
    for(int k = 0; k < g->np; k++)
        if(k != i && g->p[k].in && !strcasecmp(g->p[k].name, nm)){ *why = "Someone already plays with that name - choose another"; return -1; }
    if(i >= 0){                                           /* back again (another tab, a reload): same player */
        if(!g->p[i].in){
            if(g->phase != HG_LOBBY && !g->set[HG_S_LATE] && g->phase != HG_END){ *why = "This game is running - joining later is off"; return -1; }
            g->p[i].in = 1;
        }
        snprintf(g->p[i].name, sizeof g->p[i].name, "%s", nm);
        return i;
    }
    if(g->phase == HG_END){ *why = "This game is over - wait for the next one"; return -1; }
    if(g->phase != HG_LOBBY && !g->set[HG_S_LATE]){ *why = "This game is running - joining later is off"; return -1; }
    if(!gid || !gid[0] || g->np >= HG_PLAYERS){ *why = "The game is full"; return -1; }
    hg_player_t *p = &g->p[g->np];
    memset(p, 0, sizeof *p);
    snprintf(p->gid, sizeof p->gid, "%s", gid); snprintf(p->name, sizeof p->name, "%s", nm);
    p->in = 1; p->joined = g->nt;
    return g->np++;
}
int hg_after(const hg_game_t *g, int i){
    for(int k = 1; k <= g->np; k++){
        int j = ((i < 0 ? -1 : i) + k) % g->np;
        if(j < 0) j += g->np;
        if(g->p[j].in) return j;
    }
    return -1;
}
static int playing(const hg_game_t *g){ int n = 0; for(int i = 0; i < g->np; i++) n += g->p[i].in; return n; }
int hg_start(hg_game_t *g, const char **why){
    if(g->phase != HG_LOBBY){ *why = "The game runs already"; return -1; }
    if(!playing(g)){ *why = "Nobody plays yet - open the game's link on the phones first"; return -1; }
    g->cur = g->first = hg_after(g, -1);
    g->phase = HG_PICK;
    return 0;
}
int hg_year_of(const char *date){
    if(!date || strlen(date) < 4) return 0;
    for(int i = 0; i < 4; i++) if(date[i] < '0' || date[i] > '9') return 0;
    int y = atoi(date);
    return y >= 1000 && y <= 2999 ? y : 0;
}
int hg_fix_year(hg_game_t *g, int k, int year){
    if(k < 0 || k >= g->nt || year < 1000 || year > 2999) return -1;
    hg_turn_t *t = &g->t[k];
    snprintf(t->date, sizeof t->date, "%d", year);         /* the library's day and month may be as wrong as its year */
    if(g->set[HG_S_YEAR]) t->year = year;
    return 0;
}
void hg_song(hg_game_t *g, int song, int chance, const char *title, const char *artist, const char *album, const char *date){
    if(g->nt >= HG_TURNS || g->cur < 0) return;
    hg_turn_t *t = &g->t[g->nt++];
    memset(t, 0, sizeof *t);
    t->song = song; t->chance = chance; t->player = g->cur;
    snprintf(t->title, sizeof t->title, "%s", title ? title : ""); snprintf(t->artist, sizeof t->artist, "%s", artist ? artist : "");
    snprintf(t->album, sizeof t->album, "%s", album ? album : ""); snprintf(t->date, sizeof t->date, "%s", date ? date : "");
    t->year = g->set[HG_S_YEAR] ? hg_year_of(date) : 0;
    for(int i = 0; i < g->np; i++){ g->p[i].tipped = 0; g->p[i].tip_a[0] = g->p[i].tip_t[0] = 0; g->p[i].tip_y = 0; }
    g->phase = HG_PLAY;
}
static hg_turn_t *turn(hg_game_t *g){ return g->nt && g->phase >= HG_PLAY && g->phase <= HG_REVEAL ? &g->t[g->nt - 1] : NULL; }
int hg_knows(const hg_game_t *g, int i){
    if(g->phase == HG_REVEAL || g->phase == HG_END) return 1;
    if(g->phase != HG_PLAY || i != g->cur || !g->nt) return 0;
    return !g->t[g->nt - 1].chance || g->set[HG_S_RNDKNOWN];
}
int hg_guesses(const hg_game_t *g, int i){
    if(i < 0 || i >= g->np || !g->p[i].in) return 0;
    if(i != g->cur) return 1;
    return g->phase == HG_PLAY && !hg_knows(g, i) && g->set[HG_S_ALONG];
}
int hg_tip(hg_game_t *g, int i, const char *artist, const char *title, int year, const char **why){
    if(g->phase != HG_PLAY){ *why = "Guessing is over for this song"; return -1; }
    if(!hg_guesses(g, i)){ *why = "You know this song - the others guess"; return -1; }
    hg_player_t *p = &g->p[i];
    snprintf(p->tip_a, sizeof p->tip_a, "%s", artist ? artist : ""); snprintf(p->tip_t, sizeof p->tip_t, "%s", title ? title : "");
    p->tip_y = year > 0 && year < 10000 ? year : 0;
    p->tipped = 1;
    return 0;
}
int hg_all_tipped(const hg_game_t *g){
    int any = 0;
    for(int i = 0; i < g->np; i++) if(hg_guesses(g, i)){ any = 1; if(!g->p[i].tipped) return 0; }
    return any;
}
static int year_marks(int guess, int year, int stagger){
    if(!guess || !year) return 0;
    int d = guess > year ? guess - year : year - guess;
    return d == 0 ? HG_M_YEAR : stagger && d <= 2 ? HG_M_NEAR : 0;
}
void hg_reveal(hg_game_t *g){
    hg_turn_t *t = turn(g);
    if(!t || g->phase != HG_PLAY) return;
    if(g->set[HG_S_FORM])
        for(int i = 0; i < g->np; i++){
            const hg_player_t *p = &g->p[i];
            if(!p->tipped || t->mark[i]) continue;         /* A's own ticks made before the reveal stay */
            int st = g->set[HG_S_STAGGER], ma = hg_match(p->tip_a, t->artist), mt = hg_match(p->tip_t, t->title);
            int m = (ma == 2 ? HG_M_ARTIST : ma && st ? HG_M_ANEAR : 0) | (mt == 2 ? HG_M_TITLE : mt && st ? HG_M_TNEAR : 0)
                  | year_marks(p->tip_y, t->year, st);
            t->mark[i] = (unsigned char)m;
        }
    g->phase = HG_REVEAL;
}
void hg_mark(hg_game_t *g, int i, int marks){
    hg_turn_t *t = turn(g);
    if(!t || (g->phase != HG_REVEAL && g->phase != HG_PLAY) || i < 0 || i >= g->np) return;
    marks &= HG_M_ARTIST | HG_M_TITLE | HG_M_YEAR | HG_M_NEAR | HG_M_ANEAR | HG_M_TNEAR;
    if(!t->year) marks &= ~(HG_M_YEAR | HG_M_NEAR);
    if(!g->set[HG_S_STAGGER]) marks &= ~(HG_M_NEAR | HG_M_ANEAR | HG_M_TNEAR);
    if(marks & HG_M_YEAR) marks &= ~HG_M_NEAR;           /* right wins over almost */
    if(marks & HG_M_ARTIST) marks &= ~HG_M_ANEAR;
    if(marks & HG_M_TITLE) marks &= ~HG_M_TNEAR;
    t->mark[i] = (unsigned char)marks;
}
static int half_a(int m){ return m & HG_M_ARTIST ? 2 : m & HG_M_ANEAR ? 1 : 0; }
static int half_t(int m){ return m & HG_M_TITLE ? 2 : m & HG_M_TNEAR ? 1 : 0; }
static int half_y(int m){ return m & HG_M_YEAR ? 2 : m & HG_M_NEAR ? 1 : 0; }
int hg_points(int m){ return half_a(m) + half_t(m) + half_y(m); }
const char *hg_pts_str(int h, char *buf, int cap){ snprintf(buf, (size_t)cap, h % 2 ? "%d.5" : "%d", h / 2); return buf; }
int hg_total(const hg_player_t *p){ return p->pa + p->pt + p->py; }
static int over(const hg_game_t *g){                   /* the setup's goal reached? */
    int n = g->set[HG_S_WIN_N];
    if(g->set[HG_S_WIN] == 0) return g->nt >= n;
    if(g->set[HG_S_WIN] == 1) return g->rounds >= n;
    for(int i = 0; i < g->np; i++) if(hg_total(&g->p[i]) >= 2 * n) return 1;   /* half points */
    return 0;
}
/* the turn passes on; a round is complete when it comes back to (or past) who began it */
static int next_turn(hg_game_t *g, int pool_left){
    int was = g->cur, nx = hg_after(g, was);
    if(nx < 0){ hg_end(g, 0); return g->phase; }
    for(int k = was; ; ){                                  /* passing the place of who began (even if they left) */
        k = (k + 1) % g->np;
        if(k == g->first){ g->rounds++; break; }
        if(k == nx) break;
    }
    g->cur = nx;
    if(over(g)){ hg_end(g, 0); return g->phase; }
    if(pool_left == 0 || g->nt >= HG_TURNS){ hg_end(g, 1); return g->phase; }
    g->phase = HG_PICK;
    return g->phase;
}
int hg_confirm(hg_game_t *g, int pool_left){
    hg_turn_t *t = turn(g);
    if(!t || g->phase != HG_REVEAL) return g->phase;
    for(int i = 0; i < g->np; i++){
        int m = t->mark[i];
        g->p[i].pa += half_a(m); g->p[i].pt += half_t(m); g->p[i].py += half_y(m);
    }
    return next_turn(g, pool_left);
}
void hg_skip(hg_game_t *g, int pool_left){
    if(g->phase == HG_LOBBY || g->phase == HG_END) return;
    hg_turn_t *t = turn(g);
    if(t){ t->skipped = 1; memset(t->mark, 0, sizeof t->mark); }
    next_turn(g, pool_left);
}
void hg_remove(hg_game_t *g, int i, int pool_left){
    if(i < 0 || i >= g->np || !g->p[i].in) return;
    if(g->phase != HG_LOBBY && i == g->cur){ hg_skip(g, pool_left); }
    g->p[i].in = 0;
    if(g->phase != HG_LOBBY && g->phase != HG_END && !playing(g)) hg_end(g, 2);
    if(g->phase == HG_LOBBY){                                /* nothing played yet: gone for good */
        memmove(&g->p[i], &g->p[i + 1], sizeof g->p[0] * (size_t)(g->np - i - 1));
        g->np--;
    }
}
void hg_end(hg_game_t *g, int why){ g->phase = HG_END; g->ended_why = why; g->next_song = 0; }
int hg_may_stop(const hg_game_t *g, int i){
    if(g->phase != HG_REVEAL || i < 0) return 0;
    switch(g->set[HG_S_SKIP]){
    case 1: return i == g->cur;
    case 2: return i == g->cur || i == hg_after(g, g->cur);
    case 3: return g->p[i].in;
    default: return 0;
    }
}
static const hg_game_t *rank_g;
static int rank_cmp(const void *a, const void *b){
    const hg_player_t *x = &rank_g->p[*(const int *)a], *y = &rank_g->p[*(const int *)b];
    int d = hg_total(y) - hg_total(x);
    if(d) return d;
    return *(const int *)a - *(const int *)b;               /* a tie keeps the join order */
}
int hg_rank(const hg_game_t *g, int *idx, int *place){
    int n = 0;
    for(int i = 0; i < g->np; i++) idx[n++] = i;
    rank_g = g; qsort(idx, (size_t)n, sizeof *idx, rank_cmp);
    for(int k = 0; k < n; k++)
        place[k] = k && hg_total(&g->p[idx[k]]) == hg_total(&g->p[idx[k - 1]]) ? place[k - 1] : k + 1;
    return n;
}

/* ---------------------------------------------------------------- matching a typed guess */
/* a Latin letter with an accent (UTF-8 U+00C0..U+00FF) -> its plain letter(s), "" = not a letter */
static const char *fold(unsigned cp){
    static const char *const T[64] = {
        "a","a","a","a","ae","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","oe","","o","u","u","u","ue","y","","ss",
        "a","a","a","a","ae","a","ae","c","e","e","e","e","i","i","i","i","d","n","o","o","o","o","oe","","o","u","u","u","ue","y","","y" };
    return cp >= 0xC0 && cp <= 0xFF ? T[cp - 0xC0] : NULL;
}
/* lower-case letters and digits only; "the " at the start, "(...)" / "[...]" and " - ..." (remix, live) dropped;
 * "&" = "and" */
static void norm(const char *s, char *out, size_t cap, int plain){   /* plain: ä ö ü -> a o u (else ae oe ue) */
    size_t o = 0; int depth = 0;
    char tmp[HG_TXT_MAX * 2]; size_t to = 0;
    for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && to + 4 < sizeof tmp; p++){
        if(*p == '(' || *p == '['){ depth++; continue; }
        if(*p == ')' || *p == ']'){ if(depth) depth--; continue; }
        if(depth) continue;
        if(p[0] == ' ' && p[1] == '-' && p[2] == ' ' && to) break;   /* "Song - 2011 Remaster" */
        if(*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF){
            const char *f = fold(0xC0u + (p[1] - 0x80u)); p++;
            if(f) for(int k = 0; f[k] && to + 1 < sizeof tmp && !(plain && k == 1 && f[1] == 'e'); k++) tmp[to++] = f[k];
            continue;
        }
        if(*p == '&'){ memcpy(tmp + to, " and ", 5); to += 5; continue; }
        tmp[to++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    }
    tmp[to] = 0;
    const char *q = tmp;
    while(*q == ' ') q++;
    if(!strncmp(q, "the ", 4)) q += 4;
    for(; *q && o + 1 < cap; q++) if((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || (unsigned char)*q >= 0x80) out[o++] = *q;
    out[o] = 0;
}
static int lev(const char *a, const char *b){              /* edit distance, both short (normalised) */
    size_t n = strlen(a), m = strlen(b);
    if(n > 120 || m > 120) return 99;
    int row[121];
    for(size_t j = 0; j <= m; j++) row[j] = (int)j;
    for(size_t i = 1; i <= n; i++){
        int prev = row[0]; row[0] = (int)i;
        for(size_t j = 1; j <= m; j++){
            int cur = row[j], c = prev + (a[i - 1] != b[j - 1]);
            if(row[j] + 1 < c) c = row[j] + 1;
            if(row[j - 1] + 1 < c) c = row[j - 1] + 1;
            row[j] = c; prev = cur;
        }
    }
    return row[m];
}
static int match1(const char *a, const char *b){
    if(!a[0] || !b[0]) return 0;
    if(!strcmp(a, b)) return 2;
    int n = (int)strlen(b), na = (int)strlen(a);
    if(lev(a, b) <= (n < 5 ? 0 : n / 5)) return 1;
    /* a good part: at least 4 letters and half of the truth, at its start or end ("Take on" / "Take On Me") */
    if(na >= 4 && 2 * na >= n && na < n && (!strncmp(b, a, (size_t)na) || !strcmp(b + n - na, a))) return 1;
    return 0;
}
int hg_match(const char *guess, const char *truth){     /* the best of: umlauts as ae/oe/ue, or as a/o/u */
    char a[2][HG_TXT_MAX * 2], b[2][HG_TXT_MAX * 2]; int best = 0;
    for(int k = 0; k < 2; k++){ norm(guess, a[k], sizeof a[k], k); norm(truth, b[k], sizeof b[k], k); }
    for(int i = 0; i < 2; i++) for(int j = 0; j < 2; j++){ int m = match1(a[i], b[j]); if(m > best) best = m; }
    return best;
}
