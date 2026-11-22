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
#include "scanner.h"
#include "sdio.h"
#include "config.h"
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/statvfs.h>
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
#define BC_TMP_DLPAGE BC_TMP_DIR "/bandcamp_dl.html"
#define BC_TMP_COVER  BC_TMP_DIR "/bandcamp_cover.jpg"
#ifndef BC_CARD_ROOT
#define BC_CARD_ROOT  "/tmp/sdcard"  /* folderbrowser.h FOLDERBROWSER_ROOT */
#endif
#ifndef BC_CARD_MOUNTED
#include "system.h"
#define BC_CARD_MOUNTED() sys_is_mounted("/proc/mounts", BC_CARD_ROOT)
#else
int BC_CARD_MOUNTED(void);
#endif
#define BC_COVER_MAX  (8 << 20)      /* diskos-artdec's limit for a picture */
#define BC_DL_SPARE   (64LL << 20)   /* what the card keeps free after a download (as the party uploads) */
#define BC_DL_JOBS    24             /* downloads remembered: waiting, running and finished */
#define BC_DL_TRACK_S 900            /* one track's download may take this long */
#define BC_SEARCH_URL "https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic"
#define BC_PAGE_MAX   (4 << 20)      /* a release page is ~300 KB, a long artist page ~1 MB */
#define BC_FRESH_S    600            /* a page opened this recently still has good stream links (they last ~24 h) */
#define BC_RETRY_S    60             /* fresh links for the same track at most once a minute (ad14) */

/* ================================================================ shared state (g_mu) */
enum { J_SEARCH = 1, J_OPEN = 2, J_PLAY = 4 };
enum { S_IDLE = BC_IDLE, S_BUSY = BC_BUSY, S_DONE = BC_DONE, S_FAIL = BC_FAILED };
enum { P_OFF, P_STARTING, P_PLAYING, P_ERROR };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_worker, g_jobs;                     /* started; the J_* waiting */
static unsigned g_wake_bump;                     /* the worker finished something: bandcamp_tick looks */

/* What a search found and the page opened: the host's (view 0: the Remote page and the Disc) and one each for up to
 * BC_GUEST_VIEWS Jukebox guests (party.c, Jo 2026-11-22), so a guest's search doesn't replace the host's. */
#define BC_VIEWS (1 + BC_GUEST_VIEWS)
typedef struct {
    int who; time_t used;                        /* the guest (0 the host); when last asked for */
    int jobs;                                    /* J_SEARCH | J_OPEN waiting */
    char sq[96], job_sq[96];                     /* search */
    int sstate, nhits;
    bc_item_t hits[BC_MAX_HITS];
    unsigned sgen;
    char purl[BC_URL_LEN], job_purl[BC_URL_LEN]; /* the page opened: an artist's releases or a release's tracks */
    int pstate, pkind;                           /* 'b' artist (rel), 'r' release (page_rel + page_t) */
    bc_item_t *rel; int nrel;
    bc_release_t page_rel;
    bc_track_t *page_t; int page_n;
    time_t page_at;                              /* when the release page was fetched (its stream links' age) */
    char artist_name[BC_NAME_LEN];
    unsigned pgen;
} view_t;
static view_t g_host_view;
static view_t *g_view[BC_VIEWS] = { &g_host_view };   /* guests' on first use */
static int g_view_rr;                            /* the worker serves the guests in turn */
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
static int g_next_track = -1;                    /* the track index that plays next (bandcamp_tick), -1 none */

/* downloads to the card: their own worker, so a long album doesn't hold up searching and playing */
enum { D_WAIT = BC_DL_WAITING, D_RUN = BC_DL_SAVING, D_DONE = BC_DL_SAVED, D_FAIL = BC_DL_FAILED, D_CANCEL = BC_DL_STOPPED };
typedef struct {
    char url[BC_URL_LEN]; int idx;          /* the release, the track (-1 all) */
    char title[BC_NAME_LEN], artist[BC_NAME_LEN];
    int state, done, total;                 /* tracks saved (or already there) of total */
    int q;                                  /* BC_Q_* */
    char err[96];
    unsigned id;
    int who;                                /* a Jukebox guest's pick (party.c): queued for them once saved; 0 the host's */
    int qstate;                             /* a pick: 0 not yet, 1 queued, -1 not queued (qerr) */
    char qerr[96];
} dl_job_t;
static dl_job_t g_dl[BC_DL_JOBS]; static int g_ndl;
static unsigned g_dl_id;
static int g_dl_worker, g_dl_cancel;
static pthread_cond_t g_dl_cv = PTHREAD_COND_INITIALIZER;
#define BC_LIB_MAX 256
typedef struct { char *path; unsigned job; int who; } lib_t;
static lib_t g_lib[BC_LIB_MAX]; static int g_nlib;  /* saved files the main thread puts into the library (and a pick into the queue) */

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
static void job_search(int vi, const char *q, unsigned gen){
    char body[300]; size_t len = 0;
    char *js = bc_search_body(q, body, sizeof body) > 0 ? fetch(BC_SEARCH_URL, body, BC_TMP_SEARCH, &len) : NULL;
    static bc_item_t hits[BC_MAX_HITS];
    int n = js ? bc_parse_search(js, len, hits, BC_MAX_HITS) : -1;
    free(js);
    pthread_mutex_lock(&g_mu);
    view_t *v = g_view[vi];
    if(gen == v->sgen){
        if(n >= 0){ memcpy(v->hits, hits, sizeof hits[0] * (size_t)n); v->nhits = n; v->sstate = S_DONE; }
        else { v->nhits = 0; v->sstate = S_FAIL; }
        g_wake_bump++;
    }
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "bandcamp: search \"%s\"%s: %d results\n", q, vi ? " (a guest)" : "", n);
}
static int is_release_url(const char *u){ return strstr(u, "/album/") || strstr(u, "/track/"); }
static void job_open(int vi, const char *url, unsigned gen){
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
    view_t *v = g_view[vi];
    if(gen == v->pgen){
        free(v->rel); free(v->page_t); v->rel = NULL; v->page_t = NULL; v->nrel = v->page_n = 0;
        if(kind == 'b'){ v->rel = rel; v->nrel = n; rel = NULL; snprintf(v->artist_name, sizeof v->artist_name, "%s", artist); }
        if(kind == 'r'){ v->page_rel = r; v->page_t = t; v->page_n = n; t = NULL; v->page_at = time(NULL); }
        v->pkind = kind; v->pstate = kind ? S_DONE : S_FAIL;
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
/* Tracks of the release saved on the card before (Jo, 2026-11-22): played from there instead of the stream. Worker. */
static int local_tracks(const bc_release_t *r, bc_track_t *t, int n){
    char dir[PATH_MAX];
    if(bc_dl_dir(BC_CARD_ROOT, r, dir, sizeof dir) || !BC_CARD_MOUNTED() || !sd_io_begin()) return 0;
    int k = 0; DIR *d = opendir(dir); struct dirent *e;
    while(d && (e = readdir(d)))
        for(int i = 0; i < n; i++){
            if(t[i].stream[0] == '/' || !bc_local_match(e->d_name, &t[i])) continue;
            char p[PATH_MAX + 300];
            int w = snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if(w > 0 && (size_t)w < sizeof t[i].stream){ memcpy(t[i].stream, p, (size_t)w + 1); k++; }
            break;
        }
    if(d) closedir(d);
    sd_io_end();
    return k;
}
static void job_play(const char *url, int idx, int retry, unsigned gen){
    bc_release_t r; bc_track_t *t = calloc(BC_MAX_TRACKS, sizeof *t);
    int n = -1;
    const char *err = NULL;
    pthread_mutex_lock(&g_mu);   /* the page just opened is fresh enough, unless the player said its links failed */
    const view_t *v = &g_host_view;
    int cached = !retry && t && v->pkind == 'r' && v->page_t && !strcmp(v->page_rel.url, url) && time(NULL) - v->page_at < BC_FRESH_S;
    if(cached){ r = v->page_rel; n = v->page_n; memcpy(t, v->page_t, sizeof *t * (size_t)n); }
    pthread_mutex_unlock(&g_mu);
    if(!cached && t){
        char *h = fetch(url, NULL, BC_TMP_PLAY, NULL);
        n = h ? bc_parse_release(h, &r, t, BC_MAX_TRACKS) : -1;
        free(h);
        if(!h) err = "Couldn't reach Bandcamp";
        else if(n < 0) err = "Bandcamp sent a page without music";
    }
    if(!t) err = "Out of memory";
    int local = err ? 0 : local_tracks(&r, t, n);
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
    fprintf(stderr, "bandcamp: play %s from track %d%s: %s (%d from the card)\n", url, idx, retry ? " (fresh links)" : "", err ? err : "ready", local);
}
static void *worker(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_mu);
        int vi = -1;
        for(;;){
            if(g_jobs & J_PLAY) break;                          /* playing first: someone is waiting to hear it */
            if(g_host_view.jobs){ vi = 0; break; }               /* then the host, then the guests in turn */
            for(int k = 1; k < BC_VIEWS && vi < 0; k++){ int i = 1 + (g_view_rr + k - 1) % BC_GUEST_VIEWS; if(g_view[i] && g_view[i]->jobs) vi = i; }
            if(vi >= 0){ g_view_rr = vi; break; }
            pthread_cond_wait(&g_cv, &g_mu);
        }
        int job;
        char s[BC_URL_LEN]; int idx = g_job_play_idx, retry = g_job_play_retry; unsigned gen;
        if(vi < 0){ job = J_PLAY; g_jobs &= ~J_PLAY; snprintf(s, sizeof s, "%s", g_job_play_url); gen = g_play_gen; }
        else {
            view_t *v = g_view[vi];
            job = (v->jobs & J_OPEN) ? J_OPEN : J_SEARCH;
            v->jobs &= ~job;
            if(job == J_SEARCH){ snprintf(s, sizeof s, "%s", v->job_sq); gen = v->sgen; }
            else { snprintf(s, sizeof s, "%s", v->job_purl); gen = v->pgen; }
        }
        pthread_mutex_unlock(&g_mu);
        if(job == J_SEARCH) job_search(vi, s, gen);
        else if(job == J_OPEN) job_open(vi, s, gen);
        else job_play(s, idx, retry, gen);
        ui_loop_wake();
    }
    return NULL;
}
static void post_job(view_t *v, int job){              /* caller holds g_mu; v NULL: J_PLAY */
    if(!g_worker){
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if(pthread_create(&t, &a, worker, NULL) == 0) g_worker = 1;
        pthread_attr_destroy(&a);
    }
    if(v) v->jobs |= job; else g_jobs |= job;
    pthread_cond_signal(&g_cv);
}

/* ================================================================ saving to the card (download worker) */
static int mkdir_ok(const char *d){ return mkdir(d, 0755) == 0 || errno == EEXIST ? 0 : -1; }
/* every folder of path below the card's root */
static int mkdirs(const char *path){
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s", path);
    for(char *s = p + strlen(BC_CARD_ROOT) + 1; (s = strchr(s, '/')); s++){ *s = 0; if(mkdir_ok(p)) return -1; *s = '/'; }
    return mkdir_ok(p);
}
static void dl_set(unsigned id, int state, int done, const char *err){   /* the job's progress, for the page */
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_ndl; i++) if(g_dl[i].id == id){
        if(state >= 0) g_dl[i].state = state;
        if(done >= 0) g_dl[i].done = done;
        if(err) snprintf(g_dl[i].err, sizeof g_dl[i].err, "%s", err);
    }
    g_wake_bump++;
    pthread_mutex_unlock(&g_mu);
    ui_loop_wake();
}
static void lib_push(const dl_job_t *j, const char *path){
    pthread_mutex_lock(&g_mu);
    if(g_nlib < BC_LIB_MAX){ lib_t *l = &g_lib[g_nlib]; l->path = strdup(path); l->job = j->id; l->who = j->who; if(l->path) g_nlib++; }
    pthread_mutex_unlock(&g_mu);
}
static int room_for(const char *dir, long long bytes){
    struct statvfs vs;
    return statvfs(dir, &vs) != 0 || (long long)vs.f_bavail * (long long)vs.f_frsize >= bytes + BC_DL_SPARE;
}
/* src (the MP3 as Bandcamp sent it) -> dst with our tag in front of its audio; atomically (dst.part, rename) */
static const char *write_tagged(const char *src, const char *dst, const unsigned char *tag, size_t tlen){
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if(in < 0) return "Couldn't read the download";
    struct stat sb; unsigned char h[10];
    if(fstat(in, &sb) || read(in, h, 10) != 10){ close(in); return "Bandcamp sent an empty file"; }
    size_t skip = bc_id3_skip(h, (size_t)sb.st_size);                  /* its own tag (if any) makes way for ours */
    if(lseek(in, (off_t)skip, SEEK_SET) < 0){ close(in); return "Couldn't read the download"; }
    char tmp[PATH_MAX + 200]; snprintf(tmp, sizeof tmp, "%s.part", dst);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(out < 0){ close(in); return "Couldn't write to the SD card"; }
    int bad = write(out, tag, tlen) != (ssize_t)tlen;
    static char buf[64 * 1024]; ssize_t r;
    while(!bad && (r = read(in, buf, sizeof buf)) > 0)
        for(ssize_t o = 0; o < r; ){ ssize_t w = write(out, buf + o, (size_t)(r - o)); if(w <= 0){ bad = 1; break; } o += w; }
    if(!bad && fsync(out)) bad = 1;
    if(close(out)) bad = 1;
    close(in);
    if(bad || rename(tmp, dst)){ unlink(tmp); return "Couldn't write to the SD card"; }
    return NULL;
}
/* ---- the best quality: Bandcamp's own download (free / name-your-price releases; bandcamp_core.h steps 1-3) */
static const char *const Q_PREFS[][6] = {
    [BC_Q_BEST] = { "flac", "mp3-v0", "mp3-320", "vorbis", "aac-hi", NULL },
    [BC_Q_V0]   = { "mp3-v0", "mp3-320", NULL },
    [BC_Q_320]  = { "mp3-320", "mp3-v0", NULL },
};
static const char *enc_ext(const char *enc){
    return !strcmp(enc, "flac") ? ".flac" : !strncmp(enc, "mp3", 3) ? ".mp3" : !strcmp(enc, "vorbis") ? ".ogg" : ".m4a";
}
static int audio_name(const char *n){
    const char *d = strrchr(n, '.');
    return d && n[0] != '.' && (!strcasecmp(d, ".flac") || !strcasecmp(d, ".mp3") || !strcasecmp(d, ".ogg") || !strcasecmp(d, ".m4a"));
}
#define BC_DL_FILE_S 3600                     /* a whole album in FLAC may take this long */
/* free_dl -> the file's address (follows retry_url, Bandcamp's first answer is "expired"); 0 ok */
static int best_url(const char *free_dl, int q, char *url, size_t un, char *enc, size_t en, char *type, size_t tn, long long *bytes){
    char *h = fetch(free_dl, NULL, BC_TMP_DLPAGE, NULL);
    int r = h ? bc_parse_dlpage(h, Q_PREFS[q], enc, en, url, un, type, tn, bytes) : -1;
    free(h);
    if(r) return -1;
    for(int k = 1; k <= 5; k++){
        char su[BC_URL_LEN + 40];
        if(bc_stat_url(url, (unsigned)time(NULL) % 100000u + (unsigned)k, su, sizeof su)) return -1;
        char *st = fetch(su, NULL, BC_TMP_DLPAGE, NULL);
        int ok = st ? bc_parse_stat(st, url, un) : -1;
        free(st);
        if(ok == 1) return 0;
        if(ok < 0) return -1;
    }
    return -1;
}
/* NULL: saved in the best quality (*done files); else why not (the caller saves the stream instead) */
static const char *job_best(const dl_job_t *j, int q, const bc_release_t *r, const bc_track_t *t, int n, const char *dir, int *done){
    char free_dl[BC_URL_LEN];
    snprintf(free_dl, sizeof free_dl, "%s", r->free_dl);
    if(j->idx >= 0 && r->type != 't'){                        /* one track of an album: its own page's download */
        free_dl[0] = 0;
        if(j->idx < n && t[j->idx].page[0]){
            bc_release_t tr; bc_track_t *tt = calloc(4, sizeof *tt);
            char *h = tt ? fetch(t[j->idx].page, NULL, BC_TMP_DLPAGE, NULL) : NULL;
            if(h && bc_parse_release(h, &tr, tt, 4) >= 0) snprintf(free_dl, sizeof free_dl, "%s", tr.free_dl);
            free(h); free(tt);
        }
    }
    if(!free_dl[0]) return "not a free download";
    char url[BC_URL_LEN], enc[16], type[16]; long long bytes = 0;
    if(best_url(free_dl, q, url, sizeof url, enc, sizeof enc, type, sizeof type, &bytes)) return "Bandcamp didn't give the download";
    int album = !strcmp(type, "album");
    long long need = 0;
    for(int i = 0; i < n; i++) need += (j->idx < 0 || i == j->idx) ? (t[i].ms / 1000 + 1) * (!strcmp(enc, "flac") ? 110000 : 40000) : 0;
    if(bytes > need) need = bytes;                            /* the size the download page names */
    if(album) need *= 2;                                      /* the ZIP and its files, for a moment */
    if(!sd_io_begin()) return "The SD card is not available right now";
    const char *err = NULL;
    char part[PATH_MAX + 40]; snprintf(part, sizeof part, "%s/.bandcamp-download", dir);
    if(!room_for(dir, need)) err = "Not enough room on the SD card";
    if(!err){
        unlink(part);
        char *argv[] = { "wget", "-q", "-T", "30", "-t", "2", "-O", part, url, NULL };
        int rc = command_wait(argv, BC_DL_FILE_S * 1000);
        if(rc) { err = "Bandcamp didn't send the download"; fprintf(stderr, "bandcamp: %s: wget %d\n", url, rc); }
    }
    if(!err && album){
        /* the ZIP into the folder: the tracks (Bandcamp's names and tags) and the cover; nothing there is overwritten */
        DIR *d = opendir(dir); char **had = NULL; int nhad = 0;
        struct dirent *e;
        while(d && (e = readdir(d))) if(audio_name(e->d_name)){ char **g = realloc(had, sizeof *had * (size_t)(nhad + 1)); if(g){ had = g; had[nhad++] = strdup(e->d_name); } }
        if(d) closedir(d);
        char *argv[] = { "unzip", "-n", "-q", part, "-d", (char *)dir, NULL };
        int rc = command_wait(argv, 600000);
        if(rc){ err = "Couldn't unpack the download"; fprintf(stderr, "bandcamp: unzip %s: %d\n", part, rc); }
        d = opendir(dir);
        while(d && (e = readdir(d))){
            if(!audio_name(e->d_name)) continue;
            int old = 0; for(int i = 0; i < nhad && !old; i++) old = had[i] && !strcmp(had[i], e->d_name);
            char p[PATH_MAX + 300]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            lib_push(j, p);
            if(!old) (*done)++;
        }
        if(d) closedir(d);
        for(int i = 0; i < nhad; i++) free(had[i]);
        free(had);
        if(!err && !*done) err = "The download had no music";
    } else if(!err){
        char name[160], path[PATH_MAX + 168];
        bc_track_t x = j->idx >= 0 && j->idx < n ? t[j->idx] : t[0];
        if(bc_dl_file(&x, name, sizeof name)) err = "The names are too long for the card";
        else {
            char *dot = strrchr(name, '.'); if(dot) snprintf(dot, sizeof name - (size_t)(dot - name), "%s", enc_ext(enc));
            snprintf(path, sizeof path, "%s/%s", dir, name);
            struct stat sb;
            if(stat(path, &sb) == 0) fprintf(stderr, "bandcamp: %s is already on the card\n", path);
            else if(rename(part, path)) err = "Couldn't write to the SD card";
            if(!err){ lib_push(j, path); (*done)++; }
        }
    }
    unlink(part);
    sd_io_end();
    if(!err) fprintf(stderr, "bandcamp: saved %s in %s (%d files)\n", r->title, enc, *done);
    return err;
}
/* The release playing now (caller holds g_mu): is it this one? */
static int playing_release(const bc_release_t *r, const char *url){
    return g_now_t && g_pstate_play != P_OFF && g_now.url[0] && (!strcmp(g_now.url, r->url) || !strcmp(g_now.url, url));
}
/* Track i of the release playing now is saved: the list plays it from the card from now on (the player reads the list
 * again before its next track). Only tracks that are in the list already, so its lines stay the same. */
static void now_saved(const bc_release_t *r, const char *url, int i, const char *path){
    pthread_mutex_lock(&g_mu);
    char *text = NULL; size_t len = 0; int lines = -1;
    if(playing_release(r, url) && i < g_now_n && g_now_t[i].stream[0] && g_now_t[i].stream[0] != '/' && strlen(path) < sizeof g_now_t[i].stream){
        snprintf(g_now_t[i].stream, sizeof g_now_t[i].stream, "%s", path);
        size_t cap = (size_t)g_now_n * 1200 + 64;
        if((text = malloc(cap))) lines = bc_list_format(&g_now, g_now_t, g_now_n, text, cap, NULL, &len);
        if(lines != g_lines || write_list(text, len)) fprintf(stderr, "bandcamp: the list couldn't take %s\n", path);
    }
    pthread_mutex_unlock(&g_mu);
    free(text);
}
static void job_download(const dl_job_t *j){
    bc_release_t r; bc_track_t *t = calloc(BC_MAX_TRACKS, sizeof *t);
    char *h = t ? fetch(j->url, NULL, BC_TMP_DLPAGE, NULL) : NULL;
    int n = h ? bc_parse_release(h, &r, t, BC_MAX_TRACKS) : -1;
    free(h);
    const char *err = !t ? "Out of memory" : n < 0 ? "Couldn't reach Bandcamp" : NULL;
    int first = j->idx >= 0 ? j->idx : 0, last = j->idx >= 0 ? j->idx + 1 : n, want = 0;
    if(!err && j->idx >= n) err = "No such track";
    if(!err){ for(int i = first; i < last; i++) if(t[i].stream[0]) want++; if(!want) err = "Bandcamp doesn't stream this - nothing to save"; }
    if(!err){
        pthread_mutex_lock(&g_mu);
        for(int i = 0; i < g_ndl; i++) if(g_dl[i].id == j->id){
            snprintf(g_dl[i].title, sizeof g_dl[i].title, "%s", j->idx >= 0 ? t[j->idx].title : r.title);
            snprintf(g_dl[i].artist, sizeof g_dl[i].artist, "%s", r.artist);
            g_dl[i].total = want;
        }
        pthread_mutex_unlock(&g_mu);
    }
    /* the cover: cover.jpg in the album's folder and inside every file */
    size_t jlen = 0; unsigned char *jpg = NULL;
    if(!err && r.cover[0]){
        unlink(BC_TMP_COVER);
        char *argv[] = { "wget", "-q", "-T", "15", "-t", "1", "-O", BC_TMP_COVER, r.cover, NULL };
        if(command_wait(argv, 30000) == 0) jpg = (unsigned char *)slurp(BC_TMP_COVER, BC_COVER_MAX, &jlen);
        unlink(BC_TMP_COVER);
        if(jpg && (jlen < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8)){ free(jpg); jpg = NULL; jlen = 0; }   /* not a JPEG */
        if(!jpg) fprintf(stderr, "bandcamp: no cover from %s\n", r.cover);
    }
    char dir[PATH_MAX] = "";
    if(!err && bc_dl_dir(BC_CARD_ROOT, &r, dir, sizeof dir)) err = "The names are too long for the card";
    if(!err){
        if(!sd_io_begin()) err = "The SD card is not available right now";
        else {
            if(mkdirs(dir)) err = "Couldn't make the download folder on the SD card";
            else if(jpg){
                char cp[PATH_MAX + 16]; snprintf(cp, sizeof cp, "%s/cover.jpg", dir);
                struct stat sb;
                if(stat(cp, &sb) != 0){
                    FILE *f = fopen(cp, "we");
                    int ok = f && fwrite(jpg, 1, jlen, f) == jlen;
                    if(f && fclose(f)) ok = 0;
                    if(!ok){ unlink(cp); fprintf(stderr, "bandcamp: couldn't write %s\n", cp); }
                }
            }
            sd_io_end();
        }
    }
    int done = 0, best = 0;
    if(!err && j->q != BC_Q_STREAM){                             /* the best quality when Bandcamp gives it away */
        const char *why = job_best(j, j->q, &r, t, n, dir, &done);
        if(!why){ best = 1; pthread_mutex_lock(&g_mu); for(int i = 0; i < g_ndl; i++) if(g_dl[i].id == j->id) g_dl[i].total = done; pthread_mutex_unlock(&g_mu); }
        else { fprintf(stderr, "bandcamp: %s: %s - saving the stream\n", j->url, why); done = 0; }
    }
    char *took = calloc((size_t)(n > 0 ? n : 1), 1);
    if(!took && !err && !best) err = "Out of memory";
    while(!err && !best){
        /* the track that plays next comes first (Jo, 2026-11-22), then the rest from the first */
        pthread_mutex_lock(&g_mu);
        int stop = g_dl_cancel, i = -1, nx = playing_release(&r, j->url) ? g_next_track : -1;
        pthread_mutex_unlock(&g_mu);
        if(stop){ err = "Stopped"; break; }
        if(nx >= first && nx < last && !took[nx] && t[nx].stream[0]) i = nx;
        for(int k = first; i < 0 && k < last; k++) if(!took[k] && t[k].stream[0]) i = k;
        if(i < 0) break;
        took[i] = 1;
        char name[160], path[PATH_MAX + 168], part[PATH_MAX + 180];
        if(bc_dl_file(&t[i], name, sizeof name)){ err = "The names are too long for the card"; break; }
        snprintf(path, sizeof path, "%s/%s", dir, name);
        snprintf(part, sizeof part, "%s/.bandcamp-download", dir);
        struct stat sb;
        if(!sd_io_begin()){ err = "The SD card is not available right now"; break; }
        if(stat(path, &sb) == 0){                                        /* saved before: only into the library */
            sd_io_end(); lib_push(j, path); dl_set(j->id, -1, ++done, NULL); now_saved(&r, j->url, i, path);
            fprintf(stderr, "bandcamp: %s is already on the card\n", path);
            continue;
        }
        if(!room_for(dir, (long long)(t[i].ms / 1000 + 1) * 16000 * 2)){ sd_io_end(); err = "Not enough room on the SD card"; break; }
        /* the stream straight to the card (the lease covers wget, as an upload's), then tagged into its place */
        unlink(part);
        char *argv[] = { "wget", "-q", "-T", "30", "-t", "2", "-O", part, t[i].stream, NULL };
        int rc = command_wait(argv, BC_DL_TRACK_S * 1000);
        const char *werr = NULL;
        if(rc != 0) werr = "Bandcamp didn't send a track";
        else {
            size_t tlen = 0; unsigned char *tag = bc_id3_tag(&r, &t[i], r.type == 't' ? 0 : r.ntracks, jpg, jlen, &tlen);
            werr = tag ? write_tagged(part, path, tag, tlen) : "Out of memory";
            free(tag);
        }
        unlink(part);
        sd_io_end();
        if(werr){ err = werr; fprintf(stderr, "bandcamp: %s: %s (wget %d)\n", path, werr, rc); break; }
        fprintf(stderr, "bandcamp: saved %s\n", path);
        lib_push(j, path); dl_set(j->id, -1, ++done, NULL); now_saved(&r, j->url, i, path);
    }
    free(took); free(jpg); free(t);
    dl_set(j->id, err ? (!strcmp(err, "Stopped") ? D_CANCEL : D_FAIL) : D_DONE, done, err);
    fprintf(stderr, "bandcamp: download %s track %d: %d saved%s%s\n", j->url, j->idx, done, err ? ", " : "", err ? err : "");
}
static void *dl_worker(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_mu);
        int k;
        for(;;){
            for(k = 0; k < g_ndl && (g_dl[k].state != D_WAIT || !g_dl[k].who); k++){}   /* a guest's pick first: they wait for it */
            if(k == g_ndl) for(k = 0; k < g_ndl && g_dl[k].state != D_WAIT; k++){}
            if(k < g_ndl) break;
            pthread_cond_wait(&g_dl_cv, &g_mu);
        }
        g_dl[k].state = D_RUN; g_dl_cancel = 0;
        dl_job_t j = g_dl[k];
        pthread_mutex_unlock(&g_mu);
        job_download(&j);
        ui_loop_wake();
    }
    return NULL;
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
static int search_in(view_t *v, const char *q, const char **why){   /* caller holds g_mu */
    char t[sizeof v->sq]; snprintf(t, sizeof t, "%s", q ? q : "");
    size_t n = strlen(t); while(n && t[n - 1] == ' ') t[--n] = 0;
    if(!t[0]){ *why = "Type what to search for"; return -1; }
    snprintf(v->sq, sizeof v->sq, "%s", t); snprintf(v->job_sq, sizeof v->job_sq, "%s", t);
    v->sgen++; v->sstate = S_BUSY; v->nhits = 0;
    post_job(v, J_SEARCH);
    return 0;
}
static int open_in(view_t *v, const char *url, const char **why){  /* caller holds g_mu */
    char u[BC_URL_LEN];
    if(bc_url_join(NULL, url, u, sizeof u)){ *why = "That is not a Bandcamp address"; return -1; }
    snprintf(v->purl, sizeof v->purl, "%s", u); snprintf(v->job_purl, sizeof v->job_purl, "%s", u);
    v->pgen++; v->pstate = S_BUSY;
    post_job(v, J_OPEN);
    return 0;
}
int bandcamp_search(const char *q, const char **why){
    pthread_mutex_lock(&g_mu); int r = search_in(&g_host_view, q, why); pthread_mutex_unlock(&g_mu);
    return r;
}
int bandcamp_open(const char *url, const char **why){
    pthread_mutex_lock(&g_mu); int r = open_in(&g_host_view, url, why); pthread_mutex_unlock(&g_mu);
    return r;
}
static int download_as(const char *url, int idx, int who, const char **why);
static void play_job(const char *url, int idx, int retry){   /* caller holds g_mu */
    snprintf(g_job_play_url, sizeof g_job_play_url, "%s", url);
    g_job_play_idx = idx; g_job_play_retry = retry;
    g_play_gen++; g_play_ready = 0; g_pstate_play = P_STARTING; g_err[0] = 0;
    post_job(NULL, J_PLAY);
}
int bandcamp_play(const char *url, int idx, const char **why){
    char u[BC_URL_LEN];
    if(bc_url_join(NULL, url, u, sizeof u) || !is_release_url(u)){ *why = "That is not a Bandcamp album or track"; return -1; }
    if(idx < -1 || idx >= BC_MAX_TRACKS){ *why = "No such track"; return -1; }
    pthread_mutex_lock(&g_mu);
    play_job(u, idx, 0);
    g_retry_line = -1;
    pthread_mutex_unlock(&g_mu);
    /* what plays is saved too, by the download rules (Jo, 2026-11-22; diskos.conf bc_save_played=0: not) */
    const char *dwhy = NULL;
    if(cfg_get_int("bc_save_played", 1) && BC_CARD_MOUNTED() && download_as(u, -1, 0, &dwhy) && dwhy && strcmp(dwhy, "That is already being saved"))
        fprintf(stderr, "bandcamp: not saving what plays: %s\n", dwhy);
    return 0;
}
static int g_quality = -1;                       /* BC_Q_*, -1 not read yet */
int bandcamp_quality(void){ if(g_quality < 0){ g_quality = cfg_get_int("bc_quality", BC_Q_BEST); if(g_quality < 0 || g_quality > BC_Q_STREAM) g_quality = BC_Q_BEST; } return g_quality; }
void bandcamp_set_quality(int q){ if(q < 0 || q > BC_Q_STREAM) return; g_quality = q; cfg_set_int("bc_quality", q); }
const char *bandcamp_quality_name(int q){ static const char *n[] = { "Best (FLAC)", "MP3 V0", "MP3 320", "Stream (128 kbit/s)" }; return q >= 0 && q <= BC_Q_STREAM ? n[q] : ""; }
static int download_as(const char *url, int idx, int who, const char **why){
    char u[BC_URL_LEN];
    if(bc_url_join(NULL, url, u, sizeof u) || !is_release_url(u)){ *why = "That is not a Bandcamp album or track"; return -1; }
    if(idx < -1 || idx >= BC_MAX_TRACKS){ *why = "No such track"; return -1; }
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_ndl; i++)
        if((g_dl[i].state == D_WAIT || g_dl[i].state == D_RUN) && !strcmp(g_dl[i].url, u) && (g_dl[i].idx == idx || g_dl[i].idx == -1)){
            pthread_mutex_unlock(&g_mu); *why = "That is already being saved"; return -1; }
    if(g_ndl == BC_DL_JOBS){                                      /* forget the oldest finished one */
        int k; for(k = 0; k < g_ndl && (g_dl[k].state == D_WAIT || g_dl[k].state == D_RUN || (g_dl[k].who && g_dl[k].state == D_DONE && !g_dl[k].qstate)); k++){}
        if(k == g_ndl){ pthread_mutex_unlock(&g_mu); *why = "Too many downloads waiting - try again later"; return -1; }
        memmove(&g_dl[k], &g_dl[k + 1], sizeof g_dl[0] * (size_t)(g_ndl - k - 1)); g_ndl--;
    }
    dl_job_t *j = &g_dl[g_ndl++];
    memset(j, 0, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", u); j->idx = idx; j->state = D_WAIT; j->id = ++g_dl_id; j->q = bandcamp_quality(); j->who = who;
    snprintf(j->title, sizeof j->title, "%.127s", u);
    if(!g_dl_worker){
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if(pthread_create(&t, &a, dl_worker, NULL) == 0) g_dl_worker = 1;
        pthread_attr_destroy(&a);
        if(!g_dl_worker){ g_ndl--; pthread_mutex_unlock(&g_mu); *why = "Couldn't start the download"; return -1; }
    }
    pthread_cond_signal(&g_dl_cv);
    g_wake_bump++;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "bandcamp: download %s track %d queued%s\n", u, idx, who ? " (a guest's pick)" : "");
    return 0;
}
int bandcamp_download(const char *url, int idx, const char **why){ return download_as(url, idx, 0, why); }
void bandcamp_download_cancel(void){
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_ndl; i++) if(g_dl[i].state == D_WAIT){ g_dl[i].state = D_CANCEL; snprintf(g_dl[i].err, sizeof g_dl[i].err, "Stopped"); }
    g_dl_cancel = 1;                                              /* the running one stops after its track */
    g_wake_bump++;
    pthread_mutex_unlock(&g_mu);
}
int bandcamp_downloading(void){
    pthread_mutex_lock(&g_mu);
    int n = 0; for(int i = 0; i < g_ndl; i++) if(g_dl[i].state == D_WAIT || g_dl[i].state == D_RUN) n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}
/* ---- Jukebox guests (party.c, main thread): their own searches and pages; a pick is saved, then queued for them */
static void view_clear(view_t *v){                         /* caller holds g_mu; answers on their way are dropped */
    free(v->rel); free(v->page_t); v->rel = NULL; v->page_t = NULL; v->nrel = v->page_n = 0;
    v->jobs = 0; v->sgen++; v->pgen++; v->sstate = v->pstate = S_IDLE; v->nhits = 0; v->pkind = 0;
    v->sq[0] = v->purl[0] = v->artist_name[0] = 0;
}
static view_t *guest_view(int who){                        /* caller holds g_mu; NULL: out of memory */
    int at = -1;
    for(int i = 1; i < BC_VIEWS; i++) if(g_view[i] && g_view[i]->who == who){ g_view[i]->used = time(NULL); return g_view[i]; }
    for(int i = 1; i < BC_VIEWS && at < 0; i++) if(!g_view[i] || !g_view[i]->who) at = i;
    if(at < 0){ at = 1; for(int i = 2; i < BC_VIEWS; i++) if(g_view[i]->used < g_view[at]->used) at = i; }   /* the longest unused */
    if(!g_view[at] && !(g_view[at] = calloc(1, sizeof *g_view[at]))) return NULL;
    view_clear(g_view[at]);
    g_view[at]->who = who; g_view[at]->used = time(NULL);
    return g_view[at];
}
int bandcamp_guest_search(int who, const char *q, const char **why){
    if(who <= 0){ *why = "Too many guests - try again later"; return -1; }
    pthread_mutex_lock(&g_mu);
    view_t *v = guest_view(who);
    int r = v ? search_in(v, q, why) : -1;
    if(!v) *why = "Out of memory";
    pthread_mutex_unlock(&g_mu);
    return r;
}
int bandcamp_guest_open(int who, const char *url, const char **why){
    if(who <= 0){ *why = "Too many guests - try again later"; return -1; }
    pthread_mutex_lock(&g_mu);
    view_t *v = guest_view(who);
    int r = v ? open_in(v, url, why) : -1;
    if(!v) *why = "Out of memory";
    pthread_mutex_unlock(&g_mu);
    return r;
}
int bandcamp_guest_pick(int who, const char *url, int idx, const char **why){
    if(who <= 0){ *why = "Too many guests - try again later"; return -1; }
    if(idx < 0){ *why = "Pick one track"; return -1; }
    return download_as(url, idx, who, why);
}
static int pick_open(const dl_job_t *j){ return j->state == D_WAIT || j->state == D_RUN || (j->state == D_DONE && !j->qstate); }
int bandcamp_guest_waiting(int who){
    pthread_mutex_lock(&g_mu);
    int n = 0; for(int i = 0; i < g_ndl; i++) n += g_dl[i].who == who && pick_open(&g_dl[i]);
    pthread_mutex_unlock(&g_mu);
    return n;
}
int bandcamp_guest_waiting_track(const char *url, int idx){
    char u[BC_URL_LEN]; if(bc_url_join(NULL, url, u, sizeof u)) return 0;
    pthread_mutex_lock(&g_mu);
    int n = 0; for(int i = 0; i < g_ndl; i++) n += g_dl[i].who && pick_open(&g_dl[i]) && g_dl[i].idx == idx && !strcmp(g_dl[i].url, u);
    pthread_mutex_unlock(&g_mu);
    return n;
}
void bandcamp_guest_gone(int who){
    pthread_mutex_lock(&g_mu);
    for(int i = 1; i < BC_VIEWS; i++) if(g_view[i] && g_view[i]->who == who){ view_clear(g_view[i]); g_view[i]->who = 0; }
    for(int i = 0; i < g_ndl; i++) if(g_dl[i].who == who){ g_dl[i].who = 0; if(!g_dl[i].qstate) g_dl[i].qstate = -1; }
    pthread_mutex_unlock(&g_mu);
}
#ifndef BC_GUEST_SAVED
#include "party.h"
#define BC_GUEST_SAVED party_bandcamp_saved
#else
int BC_GUEST_SAVED(int who, int song_id, const char **why);
#endif
static void pick_done(const lib_t *l, int id){             /* a guest's pick is in the library: into the queue for them */
    pthread_mutex_lock(&g_mu);
    dl_job_t *j = NULL;
    for(int i = 0; i < g_ndl && !j; i++) if(g_dl[i].id == l->job && g_dl[i].who == l->who && !g_dl[i].qstate) j = &g_dl[i];
    pthread_mutex_unlock(&g_mu);
    if(!j) return;                                         /* queued already (one song a pick), or the guest left */
    const char *why = NULL;
    int r = id > 0 ? BC_GUEST_SAVED(l->who, id, &why) : -1;
    if(id <= 0) why = "The Disc couldn't read the song";
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_ndl; i++) if(g_dl[i].id == l->job){
        g_dl[i].qstate = r == 0 ? 1 : -1;
        snprintf(g_dl[i].qerr, sizeof g_dl[i].qerr, "%s", r == 0 ? "" : why ? why : "Couldn't queue it");
    }
    g_wake_bump++;
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "bandcamp: guest %d's pick %s%s%s\n", l->who, r == 0 ? "queued" : "not queued", r == 0 ? "" : ": ", r == 0 ? "" : why ? why : "");
}
/* saved files into the library (main thread: scanner_add_file); while a scan runs they wait */
static void lib_drain(void){
    if(scanner_active()) return;
    for(;;){
        pthread_mutex_lock(&g_mu);
        lib_t l = { 0 };
        if(g_nlib){ l = g_lib[0]; memmove(g_lib, g_lib + 1, sizeof g_lib[0] * (size_t)(g_nlib - 1)); g_nlib--; }
        pthread_mutex_unlock(&g_mu);
        if(!l.path) return;
        int id = scanner_add_file(l.path);
        if(id < 0 && scanner_active()){ pthread_mutex_lock(&g_mu);   /* a scan began: it waits for that */
            if(g_nlib < BC_LIB_MAX){ memmove(g_lib + 1, g_lib, sizeof g_lib[0] * (size_t)g_nlib); g_lib[0] = l; g_nlib++; l.path = NULL; }
            pthread_mutex_unlock(&g_mu); free(l.path); return; }
        if(id <= 0) fprintf(stderr, "bandcamp: the library didn't take %s (%d)\n", l.path, id);
        if(l.who) pick_done(&l, id);
        free(l.path);
    }
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
    {                                              /* the track after the one playing: saved first (job_download) */
        int l = bandcamp_active() ? now_line() : -1;
        pthread_mutex_lock(&g_mu);
        if(l < 0 && g_pstate_play != P_OFF) l = g_play_line;
        g_next_track = l >= 0 && l + 1 < g_lines ? g_map[l + 1] : -1;
        pthread_mutex_unlock(&g_mu);
    }
    pthread_mutex_lock(&g_mu); int lib = g_nlib; pthread_mutex_unlock(&g_mu);
    if(lib) lib_drain();
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

static void item_json(rmt_buf_t *b, const bc_item_t *it);
/* ================================================================ favourites (main thread; docs/BANDCAMP.md "Favourites")
 * Only on the SD card (Jo: user data on the card; a place on the player can follow as for the radio): read again
 * when the card comes or goes or the file changed (at most once a second); never written without the card or when
 * the file couldn't be read. Kept apart from the radio's and the library's favourites. */
#define BC_FAV_PATH BC_CARD_ROOT "/" BC_FAV_NAME
static bc_item_t *g_fav; static int g_nfav, g_fav_loaded, g_fav_nocard, g_fav_bad;
static struct { ino_t ino; off_t size; time_t mt; long mns; int have; } g_fav_sig;
static time_t g_fav_checked;
static unsigned g_fav_gen;
static void fav_sig(void){
    struct stat st; memset(&g_fav_sig, 0, sizeof g_fav_sig);
    if(stat(BC_FAV_PATH, &st)) return;
    g_fav_sig.ino = st.st_ino; g_fav_sig.size = st.st_size; g_fav_sig.mt = st.st_mtim.tv_sec; g_fav_sig.mns = st.st_mtim.tv_nsec; g_fav_sig.have = 1;
}
static void fav_load(void){
    g_fav_loaded = 1; g_fav_checked = time(NULL); g_fav_gen++;
    free(g_fav); g_fav = NULL; g_nfav = 0; g_fav_bad = 0; g_fav_nocard = 0;
    if(!BC_CARD_MOUNTED() || !sd_io_begin()){ g_fav_nocard = 1; memset(&g_fav_sig, 0, sizeof g_fav_sig); return; }
    size_t len = 0; errno = 0;
    char *t = slurp(BC_FAV_PATH, (size_t)BC_MAX_FAV * 1500, &len);
    if(!t && errno != ENOENT) g_fav_bad = 1;
    fav_sig();
    sd_io_end();
    if(!t) return;
    g_fav = malloc(sizeof *g_fav * BC_MAX_FAV);
    if(g_fav) g_nfav = bc_fav_parse(t, g_fav, BC_MAX_FAV); else g_fav_bad = 1;
    free(t);
}
static void fav_fresh(int force){
    if(!g_fav_loaded){ fav_load(); return; }
    if(!force && time(NULL) == g_fav_checked) return;
    g_fav_checked = time(NULL);
    int card = BC_CARD_MOUNTED();
    if(!card){ if(!g_fav_nocard) fav_load(); return; }
    if(g_fav_nocard || !sd_io_begin()){ if(g_fav_nocard) fav_load(); return; }
    struct stat st; int have = stat(BC_FAV_PATH, &st) == 0;
    sd_io_end();
    if(have != g_fav_sig.have || (have && (st.st_ino != g_fav_sig.ino || st.st_size != g_fav_sig.size ||
       st.st_mtim.tv_sec != g_fav_sig.mt || st.st_mtim.tv_nsec != g_fav_sig.mns))) fav_load();
}
static int fav_save(void){
    if(g_fav_bad || g_fav_nocard || !BC_CARD_MOUNTED() || !sd_io_begin()) return -1;
    size_t cap = (size_t)g_nfav * 1500 + 256;
    char *buf = malloc(cap), tmp[sizeof BC_FAV_PATH + 8];
    int len = buf ? bc_fav_format(g_fav, g_nfav, buf, cap) : -1, r = -1;
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", BC_FAV_PATH);
    int fd = len >= 0 ? mkstemp(tmp) : -1;
    if(fd >= 0){
        int ok = write(fd, buf, (size_t)len) == len && fsync(fd) == 0;
        if(close(fd)) ok = 0;
        if(ok && !rename(tmp, BC_FAV_PATH)) r = 0; else unlink(tmp);
    }
    if(!r){ fav_sig(); g_fav_gen++; }
    sd_io_end();
    free(buf);
    return r;
}
int bandcamp_fav_list(bc_item_t **out){
    fav_fresh(0);
    *out = g_nfav ? malloc(sizeof **out * (size_t)g_nfav) : NULL;
    if(!*out) return 0;
    memcpy(*out, g_fav, sizeof **out * (size_t)g_nfav);
    return g_nfav;
}
int bandcamp_fav_has(const char *url){ fav_fresh(0); return url && bc_fav_find(g_fav, g_nfav, url) >= 0; }
int bandcamp_fav_nocard(void){ fav_fresh(0); return g_fav_nocard; }
unsigned bandcamp_fav_gen(void){ fav_fresh(0); return g_fav_gen; }
static const char *const FAV_NOCARD = "Insert the SD card to keep Bandcamp favourites";
int bandcamp_fav_add(const bc_item_t *it, const char **why){
    char u[BC_URL_LEN];
    if(!it || !it->name[0] || !strchr("abt", it->type) || bc_url_join(NULL, it->url, u, sizeof u)){ *why = "That can't be a favourite"; return -1; }
    fav_fresh(1);
    if(bc_fav_find(g_fav, g_nfav, u) >= 0) return 0;
    if(g_fav_nocard){ *why = FAV_NOCARD; return -1; }
    if(g_fav_bad){ *why = "The favourites file couldn't be read - it is left as it is"; return -1; }
    if(g_nfav >= BC_MAX_FAV){ *why = "Favourites are full"; return -1; }
    if(!g_fav && !(g_fav = malloc(sizeof *g_fav * BC_MAX_FAV))){ *why = "Out of memory"; return -1; }
    g_fav[g_nfav] = *it; snprintf(g_fav[g_nfav].url, sizeof g_fav[g_nfav].url, "%s", u);
    if(g_fav[g_nfav].art[0] && !bc_url_ok(g_fav[g_nfav].art)) g_fav[g_nfav].art[0] = 0;
    g_nfav++;
    if(fav_save()){ fav_load(); *why = "Couldn't save favourites"; return -1; }
    return 0;
}
int bandcamp_fav_remove(const char *url, const char **why){
    char u[BC_URL_LEN];
    if(!url || bc_url_join(NULL, url, u, sizeof u)) snprintf(u, sizeof u, "%s", url ? url : "");
    fav_fresh(1);
    int i = bc_fav_find(g_fav, g_nfav, u);
    if(i < 0) return 0;
    if(g_fav_nocard){ *why = FAV_NOCARD; return -1; }
    memmove(&g_fav[i], &g_fav[i + 1], sizeof g_fav[0] * (size_t)(g_nfav - i - 1)); g_nfav--;
    if(fav_save()){ fav_load(); *why = "Couldn't save favourites"; return -1; }
    return 0;
}
static void fav_json(rmt_buf_t *b){
    fav_fresh(0);
    rmt_buf_printf(b, "{\"ok\":true,\"nocard\":%s,\"items\":[", g_fav_nocard ? "true" : "false");
    for(int i = 0; i < g_nfav; i++){ if(i) rmt_buf_put(b, ","); item_json(b, &g_fav[i]); }
    rmt_buf_put(b, "]}");
}
char *bandcamp_fav_json(void){ rmt_buf_t b = { 0 }; fav_json(&b); return rmt_buf_take(&b); }

/* ================================================================ for the Disc's screen */
unsigned bandcamp_gen(void){ pthread_mutex_lock(&g_mu); unsigned g = g_wake_bump; pthread_mutex_unlock(&g_mu); return g; }
int bandcamp_hits(bc_item_t *out, int max, int *state){
    pthread_mutex_lock(&g_mu);
    const view_t *v = &g_host_view;
    int n = v->nhits < max ? v->nhits : max;
    if(n > 0) memcpy(out, v->hits, sizeof out[0] * (size_t)n);
    *state = v->sstate;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int bandcamp_page(char *url, size_t ulen, int *state, int *kind, char *name, size_t nlen, bc_item_t **items,
                  bc_release_t *rel, bc_track_t **tracks){
    int n = 0;
    *items = NULL; *tracks = NULL; *kind = 0;
    pthread_mutex_lock(&g_mu);
    const view_t *v = &g_host_view;
    snprintf(url, ulen, "%s", v->purl); *state = v->pstate;
    if(v->pstate == S_DONE && v->pkind == 'b' && v->nrel > 0 && (*items = malloc(sizeof **items * (size_t)v->nrel))){
        memcpy(*items, v->rel, sizeof **items * (size_t)v->nrel); n = v->nrel; *kind = 'b';
        snprintf(name, nlen, "%s", v->artist_name);
    } else if(v->pstate == S_DONE && v->pkind == 'r'){
        *rel = v->page_rel; *kind = 'r'; snprintf(name, nlen, "%s", v->page_rel.title);
        if(v->page_n > 0 && (*tracks = malloc(sizeof **tracks * (size_t)v->page_n))){ memcpy(*tracks, v->page_t, sizeof **tracks * (size_t)v->page_n); n = v->page_n; }
    } else if(v->pstate == S_DONE) *state = S_FAIL;                     /* out of memory */
    pthread_mutex_unlock(&g_mu);
    return n;
}
int bandcamp_downloads(bc_dl_info_t *out, int max){
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for(int i = g_ndl - 1; i >= 0 && n < max; i--, n++){
        bc_dl_info_t *o = &out[n];
        snprintf(o->title, sizeof o->title, "%s", g_dl[i].title); snprintf(o->artist, sizeof o->artist, "%s", g_dl[i].artist);
        snprintf(o->err, sizeof o->err, "%s", g_dl[i].err);
        o->state = g_dl[i].state; o->done = g_dl[i].done; o->total = g_dl[i].total;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
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
        rmt_buf_put(b, ",\"page\":"); rmt_buf_str(b, t[i].page);
        rmt_buf_printf(b, ",\"ms\":%ld,\"num\":%d,\"plays\":%s}", t[i].ms, t[i].num, t[i].stream[0] ? "true" : "false");
    }
    rmt_buf_put(b, "]}");
}
static const char *sname(int s){ return s == S_BUSY ? "busy" : s == S_DONE ? "done" : s == S_FAIL ? "failed" : "idle"; }
static void view_json(rmt_buf_t *b, const view_t *v){     /* "search":{...},"page":{...  (the page left open) */
    rmt_buf_printf(b, "\"search\":{\"state\":\"%s\",\"q\":", sname(v->sstate));
    rmt_buf_str(b, v->sq);
    rmt_buf_put(b, ",\"items\":[");
    for(int i = 0; i < v->nhits; i++){ if(i) rmt_buf_put(b, ","); item_json(b, &v->hits[i]); }
    rmt_buf_printf(b, "]},\"page\":{\"state\":\"%s\",\"url\":", sname(v->pstate));
    rmt_buf_str(b, v->purl);
    if(v->pstate == S_DONE && v->pkind == 'b'){
        rmt_buf_put(b, ",\"kind\":\"artist\",\"name\":"); rmt_buf_str(b, v->artist_name);
        rmt_buf_put(b, ",\"items\":[");
        for(int i = 0; i < v->nrel; i++){ if(i) rmt_buf_put(b, ","); item_json(b, &v->rel[i]); }
        rmt_buf_put(b, "]");
    } else if(v->pstate == S_DONE && v->pkind == 'r'){
        rmt_buf_put(b, ",\"kind\":\"release\","); release_json(b, &v->page_rel, v->page_t, v->page_n);
    }
}
static const char *const DL_NAMES[] = { "waiting", "saving", "done", "failed", "stopped" };
char *bandcamp_guest_json(int who){
    rmt_buf_t b = { 0 };
    pthread_mutex_lock(&g_mu);
    const view_t *v = NULL;
    for(int i = 1; i < BC_VIEWS && !v; i++) if(g_view[i] && g_view[i]->who == who && who > 0) v = g_view[i];
    static const view_t none;
    rmt_buf_put(&b, "{\"ok\":true,"); view_json(&b, v ? v : &none);
    rmt_buf_put(&b, "},\"picks\":[");
    for(int i = g_ndl - 1, first = 1; i >= 0; i--){             /* newest first */
        const dl_job_t *j = &g_dl[i];
        if(who <= 0 || j->who != who) continue;
        rmt_buf_printf(&b, "%s{\"state\":\"%s\",\"queued\":%s,\"title\":", first ? "" : ",", DL_NAMES[j->state],
                       j->qstate > 0 ? "true" : "false");
        rmt_buf_str(&b, j->title);
        rmt_buf_put(&b, ",\"artist\":"); rmt_buf_str(&b, j->artist);
        rmt_buf_put(&b, ",\"error\":"); rmt_buf_str(&b, j->qstate < 0 ? j->qerr : j->err);
        rmt_buf_put(&b, "}");
        first = 0;
    }
    rmt_buf_put(&b, "]}");
    pthread_mutex_unlock(&g_mu);
    return rmt_buf_take(&b);
}
char *bandcamp_json(void){
    rmt_buf_t b = { 0 };
    int line = bandcamp_active() ? now_line() : -1, quality = bandcamp_quality();
    pthread_mutex_lock(&g_mu);
    rmt_buf_printf(&b, "{\"ok\":true,\"own_player\":%s,\"quality\":%d,", BC_OPL_RUNNING() ? "true" : "false", quality);
    view_json(&b, &g_host_view);
    int st = g_pstate_play;
    rmt_buf_printf(&b, "},\"now\":{\"state\":\"%s\",\"active\":%s,\"error\":",
                   st == P_STARTING ? "starting" : st == P_PLAYING ? "playing" : st == P_ERROR ? "error" : "off",
                   bandcamp_active() ? "true" : "false");
    rmt_buf_str(&b, g_err);
    if(g_now_t && st != P_OFF){
        rmt_buf_printf(&b, ",\"track\":%d,", line >= 0 && line < g_lines ? g_map[line] : -1);
        release_json(&b, &g_now, g_now_t, g_now_n);
    }
    rmt_buf_put(&b, "},\"downloads\":[");
    for(int i = g_ndl - 1; i >= 0; i--){                          /* newest first */
        const dl_job_t *j = &g_dl[i];
        rmt_buf_printf(&b, "%s{\"state\":\"%s\",\"track\":%d,\"done\":%d,\"total\":%d,\"guest\":%s,\"url\":", i < g_ndl - 1 ? "," : "",
                       DL_NAMES[j->state], j->idx, j->done, j->total, j->who ? "true" : "false");
        rmt_buf_str(&b, j->url);
        rmt_buf_put(&b, ",\"title\":"); rmt_buf_str(&b, j->title);
        rmt_buf_put(&b, ",\"artist\":"); rmt_buf_str(&b, j->artist);
        rmt_buf_put(&b, ",\"error\":"); rmt_buf_str(&b, j->err);
        rmt_buf_put(&b, "}");
    }
    rmt_buf_put(&b, "]}");
    pthread_mutex_unlock(&g_mu);
    return rmt_buf_take(&b);
}
