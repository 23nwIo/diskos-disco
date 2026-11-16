/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "bookmarks.h"
#include "sdstate.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct { char *book, *member; long pos, updated; int completed; } bm_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static bm_t *g_b; static int g_n, g_cap;
static int g_have;                                  /* g_b is the card's file as of g_st (or "no file": g_none) */
static int g_none;
static struct stat g_st;
#define BM_FILE_MAX (4u * 1024 * 1024)

static void clear(void){
    for(int i = 0; i < g_n; i++){ free(g_b[i].book); free(g_b[i].member); }
    g_n = 0;
}
static int find(const char *book){
    for(int i = 0; i < g_n; i++) if(g_b[i].book && !strcmp(g_b[i].book, book)) return i;
    return -1;
}
static int add(const char *book, const char *member, long pos, long updated, int completed){
    int i = find(book);
    if(i < 0){
        if(g_n == g_cap){
            int c = g_cap ? g_cap * 2 : 32; bm_t *nb = realloc(g_b, (size_t)c * sizeof *nb);
            if(!nb) return -1;
            g_b = nb; g_cap = c;
        }
        char *b = strdup(book); if(!b) return -1;
        i = g_n++; g_b[i].book = b; g_b[i].member = NULL;
    }
    char *m = strdup(member ? member : book); if(!m) return -1;
    free(g_b[i].member); g_b[i].member = m;
    g_b[i].pos = pos < 0 ? 0 : pos; g_b[i].updated = updated; g_b[i].completed = completed ? 1 : 0;
    return 0;
}

/* ---- the file ---- */
static void unescape(char *s){
    char *o = s;
    for(; *s; s++){
        if(*s == '\\' && s[1]){ s++; *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s; }
        else *o++ = *s;
    }
    *o = 0;
}
static void parse(char *b){
    for(char *line = b, *next; line && *line; line = next){
        next = strchr(line, '\n'); if(next) *next++ = 0;
        if(line[0] == '#' || !line[0]) continue;
        char *f[5]; int k = 0;
        for(char *p = line; k < 5; ){
            f[k++] = p;
            char *t = strchr(p, '\t'); if(!t) break;
            *t = 0; p = t + 1;
        }
        if(k < 5) continue;
        unescape(f[0]); unescape(f[1]);
        if(f[0][0]) add(f[0], f[1], atol(f[2]), atol(f[3]), atoi(f[4]));
    }
}
static int same_file(const struct stat *a, const struct stat *b){
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size && a->st_mtime == b->st_mtime;
}
/* inside a lease: g_b is the card's file now; 0 or -1 */
static int load(void){
    char p[512]; struct stat st;
    if(sds_path(p, sizeof p, BM_FILE) != 0) return -1;
    if(stat(p, &st) != 0){
        if(errno != ENOENT) return -1;
        if(!g_have || !g_none){ clear(); g_none = 1; }
        g_have = 1;
        return 0;
    }
    if(g_have && !g_none && same_file(&st, &g_st)) return 0;
    char *b; size_t n;
    if(sds_read(BM_FILE, &b, &n, BM_FILE_MAX) != 1) return -1;
    clear(); parse(b); free(b);
    g_st = st; g_have = 1; g_none = 0;
    return 0;
}
static void put_esc(char **o, const char *s){
    for(; *s; s++){
        if(*s == '\t'){ *(*o)++ = '\\'; *(*o)++ = 't'; }
        else if(*s == '\n'){ *(*o)++ = '\\'; *(*o)++ = 'n'; }
        else if(*s == '\\'){ *(*o)++ = '\\'; *(*o)++ = '\\'; }
        else *(*o)++ = *s;
    }
}
/* inside a lease: write g_b; 0 or -1 */
static int save(void){
    size_t cap = 128;
    for(int i = 0; i < g_n; i++) cap += 2 * (strlen(g_b[i].book) + strlen(g_b[i].member)) + 64;
    char *b = malloc(cap), *o = b;
    if(!b) return -1;
    o += sprintf(o, "# diskOS audiobook bookmarks: book, member, position ms, updated, completed\n");
    for(int i = 0; i < g_n; i++){
        put_esc(&o, g_b[i].book); *o++ = '\t'; put_esc(&o, g_b[i].member);
        o += sprintf(o, "\t%ld\t%ld\t%d\n", g_b[i].pos, g_b[i].updated, g_b[i].completed);
    }
    int r = sds_write(BM_FILE, b, (size_t)(o - b));
    free(b);
    if(r != 0){ g_have = 0; return -1; }            /* read it again next time: the card has what it has */
    char p[512];
    if(sds_path(p, sizeof p, BM_FILE) == 0 && stat(p, &g_st) == 0){ g_none = 0; g_have = 1; }
    else g_have = 0;
    return 0;
}

/* ---- the calls ---- */
int bm_state(int *n){
    if(n) *n = 0;
    if(!sds_begin()) return -1;
    pthread_mutex_lock(&g_mu);
    int r = load() == 0 ? (g_none ? 0 : 1) : -1;
    if(r == 1 && n) *n = g_n;
    pthread_mutex_unlock(&g_mu);
    sds_end();
    return r;
}
int bm_get(const char *book, char *member, int member_cap, long *position_ms, int *completed, long *updated){
    if(completed) *completed = 0;
    if(updated) *updated = 0;
    if(!book) return -1;
    if(!sds_begin()) return -1;
    pthread_mutex_lock(&g_mu);
    int r = -1;
    if(load() == 0){
        int i = find(book);
        r = i >= 0;
        if(i >= 0){
            if(member && member_cap > 0) snprintf(member, (size_t)member_cap, "%s", g_b[i].member);
            if(position_ms) *position_ms = g_b[i].pos;
            if(completed) *completed = g_b[i].completed;
            if(updated) *updated = g_b[i].updated;
        }
    }
    pthread_mutex_unlock(&g_mu);
    sds_end();
    return r;
}
int bm_put_many(int n, const char *const *book, const char *const *member, const long *pos, const int *completed,
                const long *updated){
    if(n <= 0) return 1;
    if(!sds_begin()) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = load() == 0;
    for(int i = 0; ok && i < n; i++)
        if(!book[i] || !book[i][0] || add(book[i], member ? member[i] : NULL, pos[i], updated ? updated[i] : (long)time(NULL),
                                          completed ? completed[i] : 0) != 0) ok = 0;
    if(ok) ok = save() == 0;
    else g_have = 0;                                /* a half-applied change: read the card again next time */
    pthread_mutex_unlock(&g_mu);
    sds_end();
    return ok;
}
int bm_put(const char *book, const char *member, long position_ms, int completed, long updated){
    if(!book || !book[0]) return 0;
    return bm_put_many(1, &book, &member, &position_ms, &completed, &updated);
}
static int under(const char *p, const char *oldp, size_t on){
    return !strncmp(p, oldp, on) && (p[on] == 0 || p[on] == '/');
}
static char *moved(const char *p, size_t on, const char *newp){
    size_t a = strlen(newp), b = strlen(p + on);
    char *r = malloc(a + b + 1);
    if(r){ memcpy(r, newp, a); memcpy(r + a, p + on, b + 1); }
    return r;
}
int bm_moved(const char *oldp, const char *newp){
    if(!oldp || !newp || !oldp[0]) return 0;
    if(!sds_begin()) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = load() == 0, hit = 0;
    size_t on = strlen(oldp);
    for(int i = 0; ok && i < g_n; i++){
        if(!g_b[i].book) continue;
        if(under(g_b[i].member, oldp, on)){
            char *m = moved(g_b[i].member, on, newp); if(!m){ ok = 0; break; }
            free(g_b[i].member); g_b[i].member = m; hit = 1;
        }
        if(under(g_b[i].book, oldp, on)){
            char *b = moved(g_b[i].book, on, newp); if(!b){ ok = 0; break; }
            int j = find(b);                          /* a bookmark already there for the new path: the moved one wins */
            if(j >= 0 && j != i){ free(g_b[j].book); free(g_b[j].member); g_b[j].book = g_b[j].member = NULL; }
            free(g_b[i].book); g_b[i].book = b; hit = 1;
        }
    }
    int k = 0;                                        /* drop the ones a move replaced */
    for(int i = 0; i < g_n; i++) if(g_b[i].book) g_b[k++] = g_b[i];
    g_n = k;
    if(ok && hit) ok = save() == 0;
    else if(!ok) g_have = 0;
    pthread_mutex_unlock(&g_mu);
    sds_end();
    return ok;
}
void bm_reset(void){
    pthread_mutex_lock(&g_mu);
    clear(); g_have = 0; g_none = 0;
    pthread_mutex_unlock(&g_mu);
}
