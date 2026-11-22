/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Bandcamp without an account: see bandcamp.h and docs/BANDCAMP.md.
 *
 * Threads: one worker (started on first use) runs one download at a time with wget (HTTPS, no shell); results are
 * handed over under g_mu and picked up by bandcamp_tick on the main thread, which also switches the source and talks
 * to the player. A newer request of a kind replaces a waiting one of that kind; generations drop stale answers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bandcamp.h"
#include "command_spawn.h"
#include "ipc.h"
#include "own_player.h"
#include "remote_core.h"     /* rmt_buf_t: the JSON for the Remote Control page */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int  ui_get_source_mode(void);
int  ui_set_source_mode(int mode);
void ui_toast(const char *msg);
void ui_loop_wake(void);

#ifndef BC_OPL_RUNNING
#define BC_OPL_RUNNING opl_running   /* the host test has no own player process */
#else
int BC_OPL_RUNNING(void);
#endif
#ifndef BC_TMP_DIR
#define BC_TMP_DIR "/tmp"
#endif
#define BC_TMP_SEARCH BC_TMP_DIR "/bandcamp_search.json"
#define BC_TMP_PAGE   BC_TMP_DIR "/bandcamp_page.html"
#define BC_TMP_PLAY   BC_TMP_DIR "/bandcamp_play.html"
#define BC_SEARCH_URL "https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic"
#define BC_PAGE_MAX   (4 << 20)      /* a release page is ~300 KB, a long artist page ~1 MB */
#define BC_FRESH_S    600            /* a page opened this recently still has good stream links (they last ~24 h) */
#define BC_RETRY_S    60             /* fresh links for the same track at most once a minute (ad14) */

/* ================================================================ shared state (g_mu) */
enum { J_SEARCH = 1, J_OPEN = 2, J_PLAY = 4 };
enum { S_IDLE, S_BUSY, S_DONE, S_FAIL };
enum { P_OFF, P_STARTING, P_PLAYING, P_ERROR };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_worker, g_jobs;                     /* started; the J_* waiting */
static unsigned g_wake_bump;                     /* the worker finished something: bandcamp_tick looks */

/* search */
static char g_sq[96], g_job_sq[96];
static int g_sstate = S_IDLE, g_nhits;
static bc_item_t g_hits[BC_MAX_HITS];
static unsigned g_sgen;
/* the page opened: an artist's releases or a release's tracks */
static char g_purl[BC_URL_LEN], g_job_purl[BC_URL_LEN];
static int g_pstate = S_IDLE, g_pkind;           /* 'b' artist (g_rel), 'r' release (g_page_rel + g_page_t) */
static bc_item_t *g_rel; static int g_nrel;
static bc_release_t g_page_rel;
static bc_track_t *g_page_t; static int g_page_n;
static time_t g_page_at;                         /* when the release page was fetched (its stream links' age) */
static char g_artist_name[BC_NAME_LEN];
static unsigned g_pgen;
/* playing */
static char g_job_play_url[BC_URL_LEN]; static int g_job_play_idx, g_job_play_retry;
static unsigned g_play_gen;                      /* bumped by every play/stop: a stale download is dropped */
static int g_play_ready;                         /* the worker wrote OPL_BC_LIST: the main thread starts it */
static int g_play_line;                          /* the list line to start */
static int g_pstate_play = P_OFF;
static char g_err[128];
static bc_release_t g_now;                       /* the release playing */
static bc_track_t *g_now_t; static int g_now_n;
static int g_map[BC_MAX_TRACKS], g_lines;        /* list line -> track index */
static time_t g_retry_at; static int g_retry_line = -1;

/* ================================================================ downloads (worker) */
static char *slurp(const char *path, size_t max, size_t *len){
    FILE *f = fopen(path, "rb"); if(!f) return NULL;
    char *b = malloc(max + 1); size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if(!b) return NULL;
    b[n] = 0; if(len) *len = n;
    return b;
}
/* GET url (or POST body as JSON) into path; the text, malloc'd; NULL on failure. wget does DNS, HTTPS and redirects. */
static char *fetch(const char *url, const char *post, const char *path, size_t *len){
    unlink(path);
    char pd[400];
    int rc;
    if(post){
        snprintf(pd, sizeof pd, "--post-data=%s", post);
        char *argv[] = { "wget", "-q", "-T", "15", "-t", "1", "--header=Content-Type: application/json", pd, "-O", (char *)path, (char *)url, NULL };
        rc = command_wait(argv, 25000);
    } else {
        char *argv[] = { "wget", "-q", "-T", "15", "-t", "1", "-O", (char *)path, (char *)url, NULL };
        rc = command_wait(argv, 25000);
    }
    char *b = rc == 0 ? slurp(path, BC_PAGE_MAX, len) : NULL;
    if(!b) fprintf(stderr, "bandcamp: %s failed (wget %d)\n", url, rc);
    unlink(path);
    return b;
}
static void job_search(const char *q, unsigned gen){
    char body[300]; size_t len = 0;
    char *js = bc_search_body(q, body, sizeof body) > 0 ? fetch(BC_SEARCH_URL, body, BC_TMP_SEARCH, &len) : NULL;
    static bc_item_t hits[BC_MAX_HITS];
    int n = js ? bc_parse_search(js, len, hits, BC_MAX_HITS) : -1;
    free(js);
    pthread_mutex_lock(&g_mu);
    if(gen == g_sgen){
        if(n >= 0){ memcpy(g_hits, hits, sizeof hits[0] * (size_t)n); g_nhits = n; g_sstate = S_DONE; }
        else { g_nhits = 0; g_sstate = S_FAIL; }
        g_wake_bump++;
    }
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "bandcamp: search \"%s\": %d results\n", q, n);
}
static int is_release_url(const char *u){ return strstr(u, "/album/") || strstr(u, "/track/"); }
static void job_open(const char *url, unsigned gen){
    char page[BC_URL_LEN + 8];
    int release = is_release_url(url);
    if(release) snprintf(page, sizeof page, "%s", url);
    else { char base[BC_URL_LEN]; if(bc_url_base(url, base, sizeof base)){ base[0] = 0; } snprintf(page, sizeof page, "%s/music", base); }
    char *h = fetch(page, NULL, BC_TMP_PAGE, NULL);
    bc_item_t *rel = NULL; bc_track_t *t = NULL; int n = -1, kind = 0; bc_release_t r; memset(&r, 0, sizeof r);
    char artist[BC_NAME_LEN] = "";
    if(h){
        if(!release){
            rel = calloc(BC_MAX_REL, sizeof *rel);
            if(rel){ char base[BC_URL_LEN]; bc_url_base(url, base, sizeof base); n = bc_parse_music(h, base, rel, BC_MAX_REL, &r); }
            if(n > 0){ kind = 'b'; snprintf(artist, sizeof artist, "%s", rel[0].artist); }
            else if(n == 0){ free(rel); rel = NULL; release = 1; }      /* one release: /music showed its page */
        }
        if(release){
            t = calloc(BC_MAX_TRACKS, sizeof *t);
            n = t ? bc_parse_release(h, &r, t, BC_MAX_TRACKS) : -1;
            if(n >= 0) kind = 'r';
        }
        free(h);
    }
    pthread_mutex_lock(&g_mu);
    if(gen == g_pgen){
        free(g_rel); free(g_page_t); g_rel = NULL; g_page_t = NULL; g_nrel = g_page_n = 0;
        if(kind == 'b'){ g_rel = rel; g_nrel = n; rel = NULL; snprintf(g_artist_name, sizeof g_artist_name, "%s", artist); }
        if(kind == 'r'){ g_page_rel = r; g_page_t = t; g_page_n = n; t = NULL; g_page_at = time(NULL); }
        g_pkind = kind; g_pstate = kind ? S_DONE : S_FAIL;
        g_wake_bump++;
    }
    pthread_mutex_unlock(&g_mu);
    free(rel); free(t);
    fprintf(stderr, "bandcamp: open %s: %s, %d\n", url, kind == 'b' ? "releases" : kind == 'r' ? "tracks" : "failed", n);
}
/* write OPL_BC_LIST atomically; 0 ok */
static int write_list(const char *text, size_t len){
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", OPL_BC_LIST);
    FILE *f = fopen(tmp, "we");
    if(!f){ fprintf(stderr, "bandcamp: %s: %s\n", tmp, strerror(errno)); return -1; }
    int ok = fwrite(text, 1, len, f) == len;
    if(fclose(f) != 0) ok = 0;
    if(!ok || rename(tmp, OPL_BC_LIST) != 0){ fprintf(stderr, "bandcamp: write %s: %s\n", OPL_BC_LIST, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}
static void job_play(const char *url, int idx, int retry, unsigned gen){
    bc_release_t r; bc_track_t *t = calloc(BC_MAX_TRACKS, sizeof *t);
    int n = -1;
    const char *err = NULL;
    pthread_mutex_lock(&g_mu);   /* the page just opened is fresh enough, unless the player said its links failed */
    int cached = !retry && t && g_pkind == 'r' && g_page_t && !strcmp(g_page_rel.url, url) && time(NULL) - g_page_at < BC_FRESH_S;
    if(cached){ r = g_page_rel; n = g_page_n; memcpy(t, g_page_t, sizeof *t * (size_t)n); }
    pthread_mutex_unlock(&g_mu);
    if(!cached && t){
        char *h = fetch(url, NULL, BC_TMP_PLAY, NULL);
        n = h ? bc_parse_release(h, &r, t, BC_MAX_TRACKS) : -1;
        free(h);
        if(!h) err = "Couldn't reach Bandcamp";
        else if(n < 0) err = "Bandcamp sent a page without music";
    }
    if(!t) err = "Out of memory";
    char *text = NULL; size_t len = 0; int map[BC_MAX_TRACKS], lines = -1, line = 0;
    if(!err){
        size_t cap = (size_t)n * 1200 + 64;
        text = malloc(cap);
        lines = text ? bc_list_format(&r, t, n, text, cap, map, &len) : -1;
        if(lines < 0) err = "Out of memory";
        else if(lines == 0) err = "This release can't be streamed";
        else {
            for(int k = 0; k < lines; k++) if(map[k] == idx){ line = k; break; }   /* idx -1 or not streamable: the first */
            if(idx >= 0 && idx < n && !t[idx].stream[0]) err = "That track can't be streamed";
        }
    }
    pthread_mutex_lock(&g_mu);
    if(gen == g_play_gen){
        if(!err && write_list(text, len) != 0) err = "Couldn't prepare the list";
        if(err){ g_pstate_play = P_ERROR; snprintf(g_err, sizeof g_err, "%s", err); g_play_ready = -1; }
        else {
            free(g_now_t); g_now_t = t; t = NULL; g_now_n = n; g_now = r;
            memcpy(g_map, map, sizeof map[0] * (size_t)lines); g_lines = lines;
            g_play_line = line; g_play_ready = 1;
        }
        g_wake_bump++;
    }
    pthread_mutex_unlock(&g_mu);
    free(t); free(text);
    fprintf(stderr, "bandcamp: play %s from track %d%s: %s\n", url, idx, retry ? " (fresh links)" : "", err ? err : "ready");
}
static void *worker(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_mu);
        while(!g_jobs) pthread_cond_wait(&g_cv, &g_mu);
        /* playing first: someone is waiting to hear it */
        int job = (g_jobs & J_PLAY) ? J_PLAY : (g_jobs & J_OPEN) ? J_OPEN : J_SEARCH;
        g_jobs &= ~job;
        char s[BC_URL_LEN]; int idx = g_job_play_idx, retry = g_job_play_retry; unsigned gen;
        if(job == J_SEARCH){ snprintf(s, sizeof s, "%s", g_job_sq); gen = g_sgen; }
        else if(job == J_OPEN){ snprintf(s, sizeof s, "%s", g_job_purl); gen = g_pgen; }
        else { snprintf(s, sizeof s, "%s", g_job_play_url); gen = g_play_gen; }
        pthread_mutex_unlock(&g_mu);
        if(job == J_SEARCH) job_search(s, gen);
        else if(job == J_OPEN) job_open(s, gen);
        else job_play(s, idx, retry, gen);
        ui_loop_wake();
    }
    return NULL;
}
static void post_job(int job){                          /* caller holds g_mu */
    if(!g_worker){
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if(pthread_create(&t, &a, worker, NULL) == 0) g_worker = 1;
        pthread_attr_destroy(&a);
    }
    g_jobs |= job; pthread_cond_signal(&g_cv);
}

/* ================================================================ main thread */
int bandcamp_active(void){ return ui_get_source_mode() == BANDCAMP_SOURCE; }
const char *bandcamp_state(void){
    pthread_mutex_lock(&g_mu); int s = g_pstate_play; pthread_mutex_unlock(&g_mu);
    return s == P_STARTING ? "starting" : s == P_PLAYING ? "playing" : s == P_ERROR ? "error" : "off";
}
const char *bandcamp_last_error(void){
    static char e[sizeof g_err];
    pthread_mutex_lock(&g_mu); snprintf(e, sizeof e, "%s", g_err); pthread_mutex_unlock(&g_mu);
    return e;
}
int bandcamp_search(const char *q, const char **why){
    char t[sizeof g_sq]; snprintf(t, sizeof t, "%s", q ? q : "");
    size_t n = strlen(t); while(n && t[n - 1] == ' ') t[--n] = 0;
    if(!t[0]){ *why = "Type what to search for"; return -1; }
    pthread_mutex_lock(&g_mu);
    snprintf(g_sq, sizeof g_sq, "%s", t); snprintf(g_job_sq, sizeof g_job_sq, "%s", t);
    g_sgen++; g_sstate = S_BUSY; g_nhits = 0;
    post_job(J_SEARCH);
    pthread_mutex_unlock(&g_mu);
    return 0;
}
int bandcamp_open(const char *url, const char **why){
    char u[BC_URL_LEN];
    if(bc_url_join(NULL, url, u, sizeof u)){ *why = "That is not a Bandcamp address"; return -1; }
    pthread_mutex_lock(&g_mu);
    snprintf(g_purl, sizeof g_purl, "%s", u); snprintf(g_job_purl, sizeof g_job_purl, "%s", u);
    g_pgen++; g_pstate = S_BUSY;
    post_job(J_OPEN);
    pthread_mutex_unlock(&g_mu);
    return 0;
}
static void play_job(const char *url, int idx, int retry){   /* caller holds g_mu */
    snprintf(g_job_play_url, sizeof g_job_play_url, "%s", url);
    g_job_play_idx = idx; g_job_play_retry = retry;
    g_play_gen++; g_play_ready = 0; g_pstate_play = P_STARTING; g_err[0] = 0;
    post_job(J_PLAY);
}
int bandcamp_play(const char *url, int idx, const char **why){
    char u[BC_URL_LEN];
    if(bc_url_join(NULL, url, u, sizeof u) || !is_release_url(u)){ *why = "That is not a Bandcamp album or track"; return -1; }
    if(idx < -1 || idx >= BC_MAX_TRACKS){ *why = "No such track"; return -1; }
    pthread_mutex_lock(&g_mu);
    play_job(u, idx, 0);
    g_retry_line = -1;
    pthread_mutex_unlock(&g_mu);
    return 0;
}
static void send_line(int line){
    char f[24]; snprintf(f, sizeof f, "0d14000C%04X", (unsigned)line & 0xFFFFu);
    if(ipc_send_cmd(f) < 0){
        pthread_mutex_lock(&g_mu); g_pstate_play = P_ERROR; snprintf(g_err, sizeof g_err, "The player didn't take the list"); pthread_mutex_unlock(&g_mu);
        return;
    }
    fprintf(stderr, "bandcamp: playing list line %d\n", line);
}
void bandcamp_stop(void){
    pthread_mutex_lock(&g_mu); g_play_gen++; g_play_ready = 0; g_pstate_play = P_OFF; g_err[0] = 0; pthread_mutex_unlock(&g_mu);
    if(bandcamp_active()) ui_set_source_mode(0);
}
void bandcamp_mode_leaving(void){
    pthread_mutex_lock(&g_mu);
    if(g_pstate_play != P_OFF) fprintf(stderr, "bandcamp: another source takes over\n");
    g_play_gen++; g_play_ready = 0; g_pstate_play = P_OFF;
    pthread_mutex_unlock(&g_mu);
}
static int now_line(void){                     /* the list line playing: the track frame's pos_id is line + 1; -1 */
    track_state_t st; ipc_get_state(&st);
    return st.have_track && st.pos_id > 0 && st.pos_id <= BC_MAX_TRACKS ? st.pos_id - 1 : -1;
}
void bandcamp_restart(void){
    if(!bandcamp_active()) return;
    int l = now_line();
    pthread_mutex_lock(&g_mu); if(l >= g_lines) l = -1; pthread_mutex_unlock(&g_mu);
    send_line(l >= 0 ? l : 0);
}
void bandcamp_link_failed(int line){
    if(!bandcamp_active()) return;
    pthread_mutex_lock(&g_mu);
    time_t now = time(NULL);
    if(line == g_retry_line && now - g_retry_at < BC_RETRY_S){   /* fresh links didn't help: the track can't play */
        g_pstate_play = P_ERROR; snprintf(g_err, sizeof g_err, "Bandcamp didn't send this track");
        pthread_mutex_unlock(&g_mu);
        ui_toast("Bandcamp didn't send this track");
        return;
    }
    g_retry_line = line; g_retry_at = now;
    int idx = line >= 0 && line < g_lines ? g_map[line] : -1;
    char url[BC_URL_LEN]; snprintf(url, sizeof url, "%s", g_now.url);
    if(url[0]){ play_job(url, idx, 1); fprintf(stderr, "bandcamp: line %d didn't play - fetching fresh links\n", line); }
    pthread_mutex_unlock(&g_mu);
}
void bandcamp_tick(void){
    static unsigned seen;
    pthread_mutex_lock(&g_mu);
    if(g_wake_bump == seen){ pthread_mutex_unlock(&g_mu); return; }
    seen = g_wake_bump;
    int ready = g_play_ready, line = g_play_line; g_play_ready = 0;
    char err[sizeof g_err]; snprintf(err, sizeof err, "%s", g_err);
    pthread_mutex_unlock(&g_mu);
    if(ready < 0){ ui_toast(err[0] ? err : "Bandcamp didn't play"); return; }
    if(ready != 1) return;
    if(!BC_OPL_RUNNING()){
        pthread_mutex_lock(&g_mu); g_pstate_play = P_ERROR; snprintf(g_err, sizeof g_err, "Bandcamp needs the diskOS player"); pthread_mutex_unlock(&g_mu);
        ui_toast("Bandcamp needs the diskOS player");
        return;
    }
    if(!bandcamp_active() && ui_set_source_mode(BANDCAMP_SOURCE) != 0){   /* the reason was toasted */
        pthread_mutex_lock(&g_mu); g_pstate_play = P_ERROR; snprintf(g_err, sizeof g_err, "Couldn't switch to Bandcamp"); pthread_mutex_unlock(&g_mu);
        return;
    }
    pthread_mutex_lock(&g_mu); g_pstate_play = P_PLAYING; pthread_mutex_unlock(&g_mu);
    send_line(line);
}

/* ================================================================ JSON for the Remote Control page */
static void item_json(rmt_buf_t *b, const bc_item_t *it){
    rmt_buf_printf(b, "{\"type\":\"%c\",\"name\":", it->type); rmt_buf_str(b, it->name);
    rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, it->artist);
    rmt_buf_put(b, ",\"album\":"); rmt_buf_str(b, it->album);
    rmt_buf_put(b, ",\"url\":"); rmt_buf_str(b, it->url);
    rmt_buf_put(b, ",\"art\":"); rmt_buf_str(b, it->art);
    rmt_buf_put(b, "}");
}
static void release_json(rmt_buf_t *b, const bc_release_t *r, const bc_track_t *t, int n){
    rmt_buf_printf(b, "\"release\":{\"type\":\"%c\",\"title\":", r->type ? r->type : 'a'); rmt_buf_str(b, r->title);
    rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, r->artist);
    rmt_buf_put(b, ",\"album\":"); rmt_buf_str(b, r->album);
    rmt_buf_put(b, ",\"url\":"); rmt_buf_str(b, r->url);
    rmt_buf_put(b, ",\"art\":"); rmt_buf_str(b, r->art);
    rmt_buf_put(b, ",\"tracks\":[");
    for(int i = 0; i < n; i++){
        rmt_buf_printf(b, "%s{\"title\":", i ? "," : ""); rmt_buf_str(b, t[i].title);
        rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, t[i].artist);
        rmt_buf_printf(b, ",\"ms\":%ld,\"num\":%d,\"plays\":%s}", t[i].ms, t[i].num, t[i].stream[0] ? "true" : "false");
    }
    rmt_buf_put(b, "]}");
}
static const char *sname(int s){ return s == S_BUSY ? "busy" : s == S_DONE ? "done" : s == S_FAIL ? "failed" : "idle"; }
char *bandcamp_json(void){
    rmt_buf_t b = { 0 };
    int line = bandcamp_active() ? now_line() : -1;
    pthread_mutex_lock(&g_mu);
    rmt_buf_printf(&b, "{\"ok\":true,\"own_player\":%s,\"search\":{\"state\":\"%s\",\"q\":", BC_OPL_RUNNING() ? "true" : "false", sname(g_sstate));
    rmt_buf_str(&b, g_sq);
    rmt_buf_put(&b, ",\"items\":[");
    for(int i = 0; i < g_nhits; i++){ if(i) rmt_buf_put(&b, ","); item_json(&b, &g_hits[i]); }
    rmt_buf_printf(&b, "]},\"page\":{\"state\":\"%s\",\"url\":", sname(g_pstate));
    rmt_buf_str(&b, g_purl);
    if(g_pstate == S_DONE && g_pkind == 'b'){
        rmt_buf_put(&b, ",\"kind\":\"artist\",\"name\":"); rmt_buf_str(&b, g_artist_name);
        rmt_buf_put(&b, ",\"items\":[");
        for(int i = 0; i < g_nrel; i++){ if(i) rmt_buf_put(&b, ","); item_json(&b, &g_rel[i]); }
        rmt_buf_put(&b, "]");
    } else if(g_pstate == S_DONE && g_pkind == 'r'){
        rmt_buf_put(&b, ",\"kind\":\"release\","); release_json(&b, &g_page_rel, g_page_t, g_page_n);
    }
    int st = g_pstate_play;
    rmt_buf_printf(&b, "},\"now\":{\"state\":\"%s\",\"active\":%s,\"error\":",
                   st == P_STARTING ? "starting" : st == P_PLAYING ? "playing" : st == P_ERROR ? "error" : "off",
                   bandcamp_active() ? "true" : "false");
    rmt_buf_str(&b, g_err);
    if(g_now_t && st != P_OFF){
        rmt_buf_printf(&b, ",\"track\":%d,", line >= 0 && line < g_lines ? g_map[line] : -1);
        release_json(&b, &g_now, g_now_t, g_now_n);
    }
    rmt_buf_put(&b, "}}");
    pthread_mutex_unlock(&g_mu);
    return rmt_buf_take(&b);
}
