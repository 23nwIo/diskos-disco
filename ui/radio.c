/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Web radio: the Radio app (SCR_RADIO) and its link to the player.
 *
 * HOW IT PLAYS
 *   diskOS switches the source to Web radio (ui_set_source_mode(6): 0657000C000C, docs/COMMAND_MAP.md). The own
 *   player (docs/MPD_CORE.md) then gets the station as one command (0d10, send_direct). The stock mq_player plays
 *   internet streams only through its DLNA renderer: a worker thread finds it (SSDP, then known ports on the Wi-Fi
 *   address and loopback) and sends it SetAVTransportURI + Play. Device-tested by hand with
 *   the dlnatest tool on 2026-11-06: MP3, AAC, MP3 over HTTPS, Ogg Vorbis, Ogg Opus and FLAC streams all played.
 *   The existing Now Playing screens show the station (DIDL title) with the player's own time and controls.
 *   Any local play (Library, Folders, books, Up Next) returns the player to Playback first (main.c radio_yield_local).
 *
 * STATIONS
 *   Favourites live in /usr/data/radio_favorites.txt (radio_core.h has the format). Search and Popular ask the
 *   community directory radio-browser.info over HTTPS (wget, no shell, on the worker). Hold a station to save or
 *   remove it.
 *
 * Threads: one worker (started on first use) runs one job at a time; results are handed over under g_mu and picked up
 * by a main-thread timer. Nothing in the worker touches LVGL. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "screens.h"
#include "radio.h"
#include "theme.h"
#include "theme_kit.h"
#include "curvelist.h"
#include "vlist.h"
#include "zune_dial.h"
#include "command_spawn.h"
#include "version.h"
#include "ipc.h"
#include "config.h"
#include "sdio.h"
#include "system.h"
#include "own_player.h"     /* opl_running, OPL_STREAM: song titles from the own player */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int ui_get_source_mode(void);
int ui_set_source_mode(int mode);
void screen_show(int which);

#ifndef RADIO_FAV_PATH
#define RADIO_FAV_PATH "/usr/data/radio_favorites.txt"
#endif
#ifndef RADIO_FAV_SD_ROOT
#define RADIO_FAV_SD_ROOT "/tmp/sdcard"
#endif
#ifndef RADIO_MOUNTS
#define RADIO_MOUNTS "/proc/mounts"
#endif
#define RADIO_FAV_SD_NAME "radio_favorites.txt"
#define RADIO_FAV_CFG "radio_fav_sd"
#define RADIO_FAV_MAX_BYTES (RC_MAX_FAV * 800)         /* a full list is at most ~700 bytes a station */
#ifndef RADIO_TMP_JSON
#define RADIO_TMP_JSON "/tmp/radio_search.json"
#endif
#define RADIO_SOURCE 6
#define DIR_LIMIT "40"
static const char *const DIR_HOSTS[] = { "de1.api.radio-browser.info", "de2.api.radio-browser.info",
                                         "fi1.api.radio-browser.info", "at1.api.radio-browser.info", NULL };

/* ================================================================ shared state (g_mu) */
enum { J_NONE, J_SEARCH, J_PLAY, J_STOP };
enum { RS_OFF, RS_STARTING, RS_PLAYING, RS_ERROR };
enum { SR_IDLE, SR_BUSY, SR_DONE, SR_FAIL };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_worker;                                    /* started */
static int g_job;                                       /* the pending job */
static char g_job_query[128];                           /* J_SEARCH: "" = popular */
static rc_station_t g_job_station;                      /* J_PLAY */
static unsigned g_play_gen;                             /* bumped by every play/stop: a stale result is dropped */
static int g_rstate = RS_OFF;
static char g_err[96];
static rc_station_t g_now;                              /* the station playing / starting */
static rc_station_t g_hits[RC_MAX_HITS];
static int g_nhits, g_sstate = SR_IDLE;
static unsigned g_search_gen, g_search_seen;
static char g_ctl[RC_URL_LEN];                          /* the renderer's AVTransport control URL, "" unknown */
static char g_icy[160], g_icy_artist[160];              /* the song the stream names ("" none / not yet) */
static unsigned g_icy_gen;                              /* the play generation g_icy belongs to */
static unsigned g_icy_bump;                             /* bumped on every change (main thread republishes) */

/* ================================================================ HTTP + UPnP (worker only) */
static long long ms_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int wlan_ip(char *out, size_t n){                 /* the Wi-Fi address, 0 if there is none */
    int s = socket(AF_INET, SOCK_DGRAM, 0); if(s < 0) return 0;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr); snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
    int ok = ioctl(s, SIOCGIFADDR, &ifr) == 0 &&
             inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, out, (socklen_t)n) != NULL;
    close(s);
    return ok && strcmp(out, "0.0.0.0");
}
static int tcp_connect(const char *host, int port, int timeout_ms){
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if(fd < 0) return -1;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if(inet_pton(AF_INET, host, &a.sin_addr) != 1 || connect(fd, (struct sockaddr *)&a, sizeof a)){ close(fd); return -1; }
    return fd;
}
/* One plain-HTTP request (UPnP control is http, on the Disc itself). Returns the status, -1 on failure; the reply
 * (headers + body) lands in out. */
static int http_req(const char *method, const char *url, const char *hdr, const char *body, char *out, size_t on){
    char host[64], path[256]; int port;
    if(on) out[0] = 0;
    if(rc_split_http(url, host, sizeof host, &port, path, sizeof path)) return -1;
    int fd = tcp_connect(host, port, 3000); if(fd < 0) return -1;
    size_t bl = body ? strlen(body) : 0;
    char head[768];
    int hl = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\nUser-Agent: " DISCO_NAME "/" DISCO_VERSION
                      "\r\n%sContent-Length: %zu\r\n\r\n", method, path, host, port, hdr ? hdr : "", bl);
    int ok = hl > 0 && hl < (int)sizeof head && write(fd, head, (size_t)hl) == hl && (!bl || write(fd, body, bl) == (ssize_t)bl);
    size_t got = 0; ssize_t r;
    while(ok && got + 1 < on && (r = read(fd, out + got, on - 1 - got)) > 0) got += (size_t)r;
    if(on) out[got] = 0;
    close(fd);
    int st = -1;
    if(ok && sscanf(out, "HTTP/%*s %d", &st) != 1) st = -1;
    return st;
}
static int try_desc(const char *loc){                   /* fills g_ctl from a description URL; 0 found */
    static char d[32768];
    if(http_req("GET", loc, NULL, NULL, d, sizeof d) != 200) return -1;
    char *body = strstr(d, "\r\n\r\n"); if(!body) return -1;
    char ctl[RC_URL_LEN];
    if(rc_desc_control_url(body, loc, ctl, sizeof ctl)) return -1;
    pthread_mutex_lock(&g_mu); snprintf(g_ctl, sizeof g_ctl, "%s", ctl); pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "radio: renderer %s -> %s\n", loc, ctl);
    return 0;
}
static int ssdp_find(const char *ip){                   /* M-SEARCH for an AVTransport; only answers from this Disc */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if(fd < 0) return -1;
    static const char msg[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
                              "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n";
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &a.sin_addr);
    unsigned char ttl = 1; setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    for(int i = 0; i < 2; i++) sendto(fd, msg, sizeof msg - 1, 0, (struct sockaddr *)&a, sizeof a);
    int found = -1; long long end = ms_now() + 2000;
    char buf[1536];
    while(found && ms_now() < end){
        struct pollfd p = { fd, POLLIN, 0 };
        if(poll(&p, 1, (int)(end - ms_now())) <= 0) break;
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0); if(n <= 0) break;
        buf[n] = 0;
        char *l = strcasestr(buf, "\nLOCATION:"); if(!l) continue;
        l += 10; while(*l == ' ') l++;
        char *e = strpbrk(l, "\r\n"); if(e) *e = 0;
        char host[64], path[256]; int port;
        if(rc_split_http(l, host, sizeof host, &port, path, sizeof path)) continue;
        if(strcmp(host, ip) && strcmp(host, "127.0.0.1")) continue;   /* another renderer on the network: not ours */
        found = try_desc(l);
    }
    close(fd);
    return found;
}
static int find_renderer(void){
    char ip[32] = "";
    if(!wlan_ip(ip, sizeof ip)) return -1;
    if(ssdp_find(ip) == 0) return 0;
    static const int ports[] = { 49494, 33291, 0 };      /* DLNA mode 0C, and the AirPlay + DLNA mode 0B (COMMAND_MAP.md) */
    static const char *const paths[] = { "/description.xml", "/DeviceDescription.xml", "/rootDesc.xml", "/dmr.xml",
                                         "/dmr/description.xml", "/upnp/description.xml", NULL };
    const char *hosts[2] = { ip, "127.0.0.1" };
    char url[128];
    for(int h = 0; h < 2; h++) for(int i = 0; ports[i]; i++) for(int j = 0; paths[j]; j++){
        snprintf(url, sizeof url, "http://%s:%d%s", hosts[h], ports[i], paths[j]);
        if(try_desc(url) == 0) return 0;
    }
    return -1;
}
static char g_soap_reply[8192];                         /* the last reply (worker thread only) */
static int soap(const char *action, const char *args){  /* 0 = HTTP 200 */
    char ctl[RC_URL_LEN];
    pthread_mutex_lock(&g_mu); snprintf(ctl, sizeof ctl, "%s", g_ctl); pthread_mutex_unlock(&g_mu);
    if(!ctl[0]) return -1;
    static char body[20480];
    char *reply = g_soap_reply;
    char hdr[192];
    if(rc_soap_body(action, args, body, sizeof body)) return -1;
    snprintf(hdr, sizeof hdr, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#%s\"\r\n", action);
    int st = http_req("POST", ctl, hdr, body, reply, sizeof g_soap_reply);
    if(st != 200) fprintf(stderr, "radio: %s -> HTTP %d\n", action, st);
    return st == 200 ? 0 : -1;
}

/* ================================================================ jobs (worker) */
static void icy_kick(void);
static void set_play(unsigned gen, int st, const char *err){
    pthread_mutex_lock(&g_mu);
    if(gen == g_play_gen){ g_rstate = st; snprintf(g_err, sizeof g_err, "%s", err ? err : ""); if(st == RS_PLAYING) icy_kick(); }
    pthread_mutex_unlock(&g_mu);
    ui_loop_wake();
}
/* The renderer can start the stream yet answer an action late or with an error (seen on the device: the station plays,
 * the start was reported as failed). Ask it what it is doing before calling a start failed. */
static int renderer_playing(const char *url, int set_ok){   /* 1: it plays this station */
    if(soap("GetTransportInfo", "") != 0) return 0;
    char st[32];
    if(rc_xml_value(g_soap_reply, "CurrentTransportState", st, sizeof st)) return 0;
    fprintf(stderr, "radio: transport state %s\n", st);
    if(strcmp(st, "PLAYING") && strcmp(st, "TRANSITIONING")) return 0;
    static char uri[RC_URL_LEN];                         /* not the previous station still playing */
    if(soap("GetMediaInfo", "") == 0 && rc_xml_value(g_soap_reply, "CurrentURI", uri, sizeof uri) == 0 && uri[0]) return !strcmp(uri, url);
    return set_ok;                                       /* no URI to compare: only after our SetAVTransportURI was taken */
}
static void job_play(const rc_station_t *s, unsigned gen){
    static char args[16384];
    if(rc_set_uri_args(s, args, sizeof args)){ set_play(gen, RS_ERROR, "Station address is too long"); return; }
    long long end = ms_now() + 20000;                     /* the renderer starts a few seconds after the mode switch */
    int tries = 0;
    while(ms_now() < end){
        pthread_mutex_lock(&g_mu); int stale = gen != g_play_gen, have = g_ctl[0] != 0; pthread_mutex_unlock(&g_mu);
        if(stale) return;
        if(!have && find_renderer() != 0){ usleep(1000 * 1000); continue; }
        int set = soap("SetAVTransportURI", args) == 0;
        if((set && soap("Play", "<Speed>1</Speed>") == 0) || renderer_playing(s->url, set)){
            fprintf(stderr, "radio: playing \"%s\" %s\n", s->name, s->url);
            set_play(gen, RS_PLAYING, NULL); return;
        }
        pthread_mutex_lock(&g_mu); g_ctl[0] = 0; pthread_mutex_unlock(&g_mu);   /* rediscover: the renderer may have moved */
        if(++tries >= 4) break;
        usleep(1000 * 1000);
    }
    char ip[32];
    set_play(gen, RS_ERROR, wlan_ip(ip, sizeof ip) ? "The player's radio receiver didn't answer" : "Wi-Fi is not connected");
}
static void job_stop(void){ soap("Stop", ""); }
static char *slurp(const char *path, size_t max, size_t *len){
    FILE *f = fopen(path, "rb"); if(!f) return NULL;
    char *b = malloc(max + 1); size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if(!b) return NULL;
    b[n] = 0; *len = n; return b;
}
static void job_search(const char *query){
    char q[400] = "", url[640];
    if(query[0]){ char enc[384]; rc_urlencode(query, enc, sizeof enc); snprintf(q, sizeof q, "name=%s&", enc); }
    static rc_station_t hits[RC_MAX_HITS];
    int n = -1;
    for(int h = 0; DIR_HOSTS[h] && n < 0; h++){
        snprintf(url, sizeof url, "https://%s/json/stations/search?%slimit=" DIR_LIMIT "&hidebroken=true&order=clickcount&reverse=true",
                 DIR_HOSTS[h], q);
        unlink(RADIO_TMP_JSON);
        char *argv[] = { "wget", "-q", "-T", "8", "-t", "1", "-U", DISCO_NAME "/" DISCO_VERSION, "-O", RADIO_TMP_JSON, url, NULL };
        int rc = command_wait(argv, 15000);
        size_t len = 0; char *js = rc == 0 ? slurp(RADIO_TMP_JSON, 1 << 20, &len) : NULL;
        if(js){ n = rc_parse_stations(js, len, hits, RC_MAX_HITS); free(js); }
        if(n < 0) fprintf(stderr, "radio: directory %s failed (wget %d)\n", DIR_HOSTS[h], rc);
    }
    unlink(RADIO_TMP_JSON);
    pthread_mutex_lock(&g_mu);
    if(g_job == J_SEARCH){ pthread_mutex_unlock(&g_mu); return; }   /* a newer search waits: this answer is out of date */
    if(n >= 0){ memcpy(g_hits, hits, sizeof hits[0] * (size_t)n); g_nhits = n; g_sstate = SR_DONE; }
    else { g_nhits = 0; g_sstate = SR_FAIL; }
    g_search_gen++;
    pthread_mutex_unlock(&g_mu);
    ui_loop_wake();
}
static void *worker(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&g_mu);
        while(g_job == J_NONE) pthread_cond_wait(&g_cv, &g_mu);
        int job = g_job; g_job = J_NONE;
        char query[sizeof g_job_query]; snprintf(query, sizeof query, "%s", g_job_query);
        rc_station_t st = g_job_station; unsigned gen = g_play_gen;
        pthread_mutex_unlock(&g_mu);
        if(job == J_SEARCH) job_search(query);
        else if(job == J_PLAY) job_play(&st, gen);
        else if(job == J_STOP) job_stop();
    }
    return NULL;
}
static void post_job(int job){                          /* caller holds g_mu; a newer job replaces a waiting one */
    if(!g_worker){
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if(pthread_create(&t, &a, worker, NULL) == 0) g_worker = 1;
        pthread_attr_destroy(&a);
    }
    g_job = job; pthread_cond_signal(&g_cv);
}

/* ================================================================ the song title (its own thread)
 * The own player names the song from MPD (OPL_STREAM, read every OWN_EVERY_MS). The stock player does not pass
 * the stream's metadata on, so with it every ICY_EVERY_MS a second, short connection to the
 * station asks for it: ICY blocks (MP3/AAC from Icecast/SHOUTcast) or, for Ogg Vorbis/Opus, the comment header
 * the server sends every new listener. wget does DNS, HTTPS and redirects; it is killed once the title is read. */
#define ICY_EVERY_MS 20000
#define ICY_BUF (96 * 1024)
static pthread_cond_t g_icy_cv = PTHREAD_COND_INITIALIZER;
static int g_icy_thread;
/* reads until want bytes are there, the stream ends or the deadline passes; returns bytes held */
static size_t read_to(int fd, unsigned char *b, size_t have, size_t want, long long end){
    while(have < want){
        long long left = end - ms_now(); if(left <= 0) break;
        struct pollfd p = { fd, POLLIN, 0 };
        if(poll(&p, 1, (int)(left > 1000 ? 1000 : left)) <= 0) continue;
        ssize_t r = read(fd, b + have, want - have);
        if(r <= 0) break;
        have += (size_t)r;
    }
    return have;
}
/* 1 a title was read (title/artist set, may be ""), 0 the stream names none, -1 the connection failed */
static int icy_fetch(const char *url, char *title, size_t tn, char *artist, size_t an){
    static unsigned char buf[ICY_BUF], pay[ICY_BUF];
    title[0] = artist[0] = 0;
    int pfd[2]; if(pipe2(pfd, O_CLOEXEC)) return -1;
    char u[RC_URL_LEN]; snprintf(u, sizeof u, "%s", url);
    char *argv[] = { "wget", "-q", "-T", "8", "-t", "1", "--save-headers", "--header=Icy-MetaData: 1",
                     "-U", DISCO_NAME "/" DISCO_VERSION, "-O", "-", u, NULL };
    pid_t pid = -1; int rc = command_spawn_io(&pid, argv, pfd[1], -1);
    close(pfd[1]);
    if(rc){ close(pfd[0]); return -1; }
    long long end = ms_now() + 12000;
    int res = -1;
    size_t have = 0; int he = -1;
    while(he < 0 && have < 16384){                          /* the reply header (wget passes ICY 200 on as body: same shape) */
        size_t was = have; have = read_to(pfd[0], buf, have, have + 1, end);
        if(have == was) break;
        he = rc_icy_head_end((const char *)buf, have);
    }
    if(he > 0){
        long mi = rc_icy_metaint((const char *)buf, (size_t)he);
        if(mi > 0 && (size_t)he + (size_t)mi + 1 + 255 * 16 <= sizeof buf){
            have = read_to(pfd[0], buf, have, (size_t)he + (size_t)mi + 1, end);
            if(have >= (size_t)he + (size_t)mi + 1){
                size_t ml = buf[he + mi] * 16u, at = (size_t)he + (size_t)mi + 1;
                have = read_to(pfd[0], buf, have, at + ml, end);
                if(have >= at + ml){
                    char full[320];
                    if(ml && rc_icy_title((const char *)buf + at, ml, full, sizeof full) == 0){
                        rc_icy_split(full, artist, an, title, tn); res = 1;
                    } else res = 0;                     /* an empty block: the title has not changed / none */
                }
            }
        } else {                                        /* no ICY: an Ogg stream names its song in its headers */
            have = read_to(pfd[0], buf, have, sizeof buf < (size_t)he + 65536 ? sizeof buf : (size_t)he + 65536, end);
            if(have > (size_t)he + 4 && !memcmp(buf + he, "OggS", 4)){
                size_t pl = rc_ogg_payload(buf + he, have - (size_t)he, pay, sizeof pay);
                res = rc_ogg_tags(pay, pl, artist, an, title, tn) == 0 ? 1 : 0;
            } else if(have > (size_t)he) res = 0;
        }
    }
    close(pfd[0]);
    kill(-pid, SIGKILL); kill(pid, SIGKILL);
    while(waitpid(pid, NULL, 0) < 0 && errno == EINTR){}
    return res;
}
/* The own player names the song itself (OPL_STREAM, from MPD, which reads the stream anyway): 1 with title/artist
 * when it is about this station, 0 not (yet). */
#define OWN_EVERY_MS 2000
static int own_stream_title(const char *url, char *title, size_t tn, char *artist, size_t an){
    static char b[2048], u[RC_URL_LEN];
    title[0] = artist[0] = 0;
    int fd = open(OPL_STREAM, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = 0;
    char t[320];
    if(opl_stream_parse(b, u, sizeof u, artist, an, t, sizeof t) != 0 || strcmp(u, url)) return 0;
    if(!artist[0] && t[0]) rc_icy_split(t, artist, an, title, tn);   /* ICY: "Artist - Title" in one */
    else { size_t l = strnlen(t, tn - 1); memcpy(title, t, l); title[l] = 0; }
    return 1;
}
static void *icy_thread(void *arg){
    (void)arg;
    unsigned done_gen = 0, own_gen = 0; int misses = 0, own = 0; long long next = 0;
    for(;;){
        pthread_mutex_lock(&g_mu);
        for(;;){
            int playing = g_rstate == RS_PLAYING;
            if(playing && g_play_gen != done_gen){ next = ms_now() + 3000; done_gen = g_play_gen; misses = 0;
                                                   g_icy[0] = g_icy_artist[0] = 0; g_icy_gen = g_play_gen; g_icy_bump++; }
            if(playing && ms_now() >= next) break;
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += playing ? 1 : 30;
            pthread_cond_timedwait(&g_icy_cv, &g_mu, &ts);
        }
        rc_station_t s = g_now; unsigned gen = g_play_gen;
        pthread_mutex_unlock(&g_mu);
        char t[160], a[160];
        if(own_gen != gen){ own = opl_running(); own_gen = gen;
                            fprintf(stderr, "radio: song titles from %s\n", own ? "the own player (MPD)" : "a second connection"); }
        int r = own ? own_stream_title(s.url, t, sizeof t, a, sizeof a) : icy_fetch(s.url, t, sizeof t, a, sizeof a);
        pthread_mutex_lock(&g_mu);
        if(gen == g_play_gen && r > 0 && (strcmp(t, g_icy) || strcmp(a, g_icy_artist))){
            snprintf(g_icy, sizeof g_icy, "%s", t); snprintf(g_icy_artist, sizeof g_icy_artist, "%s", a); g_icy_bump++;
            fprintf(stderr, "radio: now \"%s%s%s\"\n", a, a[0] ? " - " : "", t);
        }
        misses = r > 0 ? 0 : misses + 1;
        /* a station that names nothing is asked again only every few minutes; the own player's file is only read */
        next = ms_now() + (own ? OWN_EVERY_MS : misses >= 3 ? 5 * 60000 : ICY_EVERY_MS);
        pthread_mutex_unlock(&g_mu);
        if(r > 0) ui_loop_wake();
    }
    return NULL;
}
static void icy_kick(void){                             /* caller holds g_mu */
    if(!g_icy_thread){
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 128 * 1024);
        if(pthread_create(&t, &a, icy_thread, NULL) == 0) g_icy_thread = 1;
        pthread_attr_destroy(&a);
    }
    pthread_cond_signal(&g_icy_cv);
}

/* ================================================================ favourites (main thread) */
/* The list lives either on the player (RADIO_FAV_PATH) or on the SD card (RADIO_FAV_SD_ROOT/radio_favorites.txt), as
 * Settings > System > Library > Radio Favourites says (cfg radio_fav_sd). The file is read again when that place
 * changes, when the card comes or goes, or when the file itself changes (an edit on a computer or over SSH): checked
 * at most once a second. Without the card, the card list is empty and is never written. Nothing is copied between
 * the two places. */
static rc_station_t *g_fav;
static int g_nfav, g_fav_cap, g_fav_loaded, g_fav_test;
static int g_fav_where, g_fav_nocard;                   /* the place g_fav was read from; 1 = that card was missing */
static int g_fav_bad;                                   /* the file exists but could not be read: never overwrite it */
static struct { dev_t dev; ino_t ino; off_t size; time_t mtime; long mns; int have; } g_fav_sig;
static long long g_fav_checked;
static int fav_cfg_sd(void){ return cfg_get_int(RADIO_FAV_CFG, 1) ? 1 : 0; }   /* default: the card */
static const char *fav_path(int sd){ return sd ? RADIO_FAV_SD_ROOT "/" RADIO_FAV_SD_NAME : RADIO_FAV_PATH; }
static int fav_card(void){ return sys_is_mounted(RADIO_MOUNTS, RADIO_FAV_SD_ROOT); }
static int fav_grow(int n){
    if(n <= g_fav_cap) return 0;
    int cap = g_fav_cap ? g_fav_cap : 16;
    while(cap < n) cap *= 2;
    if(cap > RC_MAX_FAV) cap = RC_MAX_FAV;
    if(cap < n) return -1;
    rc_station_t *f = realloc(g_fav, sizeof *f * (size_t)cap);
    if(!f) return -1;
    g_fav = f; g_fav_cap = cap; return 0;
}
static void fav_stat(int sd){
    struct stat st;
    memset(&g_fav_sig, 0, sizeof g_fav_sig);
    if(stat(fav_path(sd), &st) != 0) return;
    g_fav_sig.dev = st.st_dev; g_fav_sig.ino = st.st_ino; g_fav_sig.size = st.st_size;
    g_fav_sig.mtime = st.st_mtim.tv_sec; g_fav_sig.mns = st.st_mtim.tv_nsec; g_fav_sig.have = 1;
}
/* read the list of place sd into g_fav; the caller holds the SD lease for the card */
static int fav_read(int sd, rc_station_t **out, int *n){
    *out = NULL; *n = 0;
    size_t len = 0; char *t = slurp(fav_path(sd), RADIO_FAV_MAX_BYTES, &len);
    if(!t) return errno == ENOENT ? 0 : -1;
    int lines = 1;
    for(size_t i = 0; i < len; i++) if(t[i] == '\n') lines++;
    if(lines > RC_MAX_FAV) lines = RC_MAX_FAV;
    rc_station_t *f = malloc(sizeof *f * (size_t)lines);
    if(!f){ free(t); return -1; }
    *n = rc_fav_parse(t, f, lines); *out = f;
    free(t); return 0;
}
static void fav_take(rc_station_t *f, int n){
    g_nfav = 0;
    if(n > 0 && fav_grow(n) == 0){ memcpy(g_fav, f, sizeof *f * (size_t)n); g_nfav = n; }
    free(f);
}
static unsigned g_fav_gen;                              /* bumped by every read of the file */
static void fav_load(void){
    g_fav_loaded = 1; g_nfav = 0; g_fav_gen++;
    g_fav_where = fav_cfg_sd(); g_fav_nocard = 0;
    g_fav_checked = ms_now();
    rc_station_t *f = NULL; int n = 0;
    g_fav_bad = 0;
    if(g_fav_where){
        if(!fav_card() || !sd_io_begin()){ g_fav_nocard = 1; memset(&g_fav_sig, 0, sizeof g_fav_sig); return; }
        g_fav_bad = fav_read(1, &f, &n) != 0; fav_stat(1);
        sd_io_end();
    } else { g_fav_bad = fav_read(0, &f, &n) != 0; fav_stat(0); }
    fav_take(f, n);
}
/* reload when the place, the card or the file changed; force skips the once-a-second limit */
static void fav_fresh(int force){
    if(g_fav_test) return;
    if(!g_fav_loaded){ fav_load(); return; }
    if(!force && ms_now() - g_fav_checked < 1000) return;
    g_fav_checked = ms_now();
    int sd = fav_cfg_sd();
    if(sd != g_fav_where){ fav_load(); return; }
    if(sd){
        int card = fav_card();
        if(!card){ if(!g_fav_nocard) fav_load(); return; }
        if(g_fav_nocard){ fav_load(); return; }
        if(!sd_io_begin()) return;
    }
    struct stat st; int have = stat(fav_path(sd), &st) == 0;
    if(sd) sd_io_end();
    if(have != g_fav_sig.have || (have && (st.st_dev != g_fav_sig.dev || st.st_ino != g_fav_sig.ino ||
       st.st_size != g_fav_sig.size || st.st_mtim.tv_sec != g_fav_sig.mtime || st.st_mtim.tv_nsec != g_fav_sig.mns))) fav_load();
}
/* write f[0..n) to place sd (temp file, fsync, rename); the caller holds the SD lease for the card */
static int fav_write(int sd, const rc_station_t *f, int n){
    size_t cap = (size_t)(n > 0 ? n : 1) * (RC_NAME_LEN + RC_URL_LEN + 96) + 1;
    char *buf = malloc(cap);
    if(!buf) return -1;
    int len = rc_fav_format(f, n, buf, cap);
    if(len < 0){ free(buf); return -1; }
    char tmp[sizeof RADIO_FAV_SD_ROOT "/" RADIO_FAV_SD_NAME + sizeof RADIO_FAV_PATH + 16];
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", fav_path(sd));
    int fd = mkstemp(tmp);
    if(fd < 0){ free(buf); return -1; }
    int ok = write(fd, buf, (size_t)len) == len && fsync(fd) == 0;
    if(close(fd) != 0) ok = 0;
    free(buf);
    if(!ok || rename(tmp, fav_path(sd)) != 0){ unlink(tmp); return -1; }
    return 0;
}
static const char *const FAV_NOCARD = "Insert the SD card to change radio favourites";
static int fav_save(void){
    if(g_fav_bad) return -1;
    if(g_fav_where){
        if(g_fav_nocard || !fav_card() || !sd_io_begin()) return -1;
        int r = fav_write(1, g_fav, g_nfav);
        if(!r) fav_stat(1);
        sd_io_end();
        return r;
    }
    int r = fav_write(0, g_fav, g_nfav);
    if(!r) fav_stat(0);
    return r;
}
int radio_favorites(rc_station_t *out, int cap){
    fav_fresh(0);
    int n = g_nfav < cap ? g_nfav : cap;
    if(n > 0) memcpy(out, g_fav, sizeof g_fav[0] * (size_t)n);
    return n < 0 ? 0 : n;
}
int radio_fav_count(void){ fav_fresh(0); return g_nfav; }
int radio_fav_get(int i, rc_station_t *out){
    fav_fresh(0);
    if(i < 0 || i >= g_nfav) return -1;
    *out = g_fav[i]; return 0;
}
int radio_fav_has(const char *url){ fav_fresh(0); return url && rc_fav_find(g_fav, g_nfav, url) >= 0; }
int radio_fav_nocard(void){ fav_fresh(0); return g_fav_where && g_fav_nocard; }
/* Settings changed the place to sd (the setting is already saved). Nothing is copied: each place keeps its own file
 * (docs/RADIO.md says where), the next look reads the new one. Returns 0, or 1 with a note in *why (no card). */
int radio_fav_move(int sd, const char **why){
    g_fav_loaded = 0;
    if(sd && !fav_card()){ *why = "Your favourites appear when the SD card is in"; return 1; }
    return 0;
}
static void fav_toggle(const rc_station_t *s){
    fav_fresh(1);
    if(g_fav_where && g_fav_nocard){ ui_toast(FAV_NOCARD); return; }
    int i = rc_fav_find(g_fav, g_nfav, s->url);
    if(i >= 0){ memmove(&g_fav[i], &g_fav[i + 1], sizeof g_fav[0] * (size_t)(g_nfav - i - 1)); g_nfav--; }
    else {
        if(g_nfav >= RC_MAX_FAV){ ui_toast("Favourites are full"); return; }
        if(fav_grow(g_nfav + 1)){ ui_toast("Couldn't save favourites"); return; }
        g_fav[g_nfav++] = *s;
    }
    if(fav_save()){ fav_load(); ui_toast("Couldn't save favourites"); return; }
    ui_toast(i >= 0 ? "Removed from favourites" : "Saved to favourites");
}
static void refuse(const char *why);
int radio_fav_add(const rc_station_t *s){
    if(!s || !s->name[0] || !rc_url_ok(s->url)){ refuse("That station address can't be saved"); return -1; }
    fav_fresh(1);
    if(rc_fav_find(g_fav, g_nfav, s->url) >= 0) return 0;
    if(g_fav_where && g_fav_nocard){ refuse(FAV_NOCARD); return -1; }
    if(g_nfav >= RC_MAX_FAV){ refuse("Favourites are full"); return -1; }
    if(fav_grow(g_nfav + 1)){ refuse("Couldn't save favourites"); return -1; }
    g_fav[g_nfav++] = *s;
    if(fav_save()){ fav_load(); refuse("Couldn't save favourites"); return -1; }
    return 0;
}
int radio_fav_remove(const char *url){
    fav_fresh(1);
    int i = url ? rc_fav_find(g_fav, g_nfav, url) : -1;
    if(i < 0) return 0;
    if(g_fav_where && g_fav_nocard){ refuse(FAV_NOCARD); return -1; }
    memmove(&g_fav[i], &g_fav[i + 1], sizeof g_fav[0] * (size_t)(g_nfav - i - 1)); g_nfav--;
    if(fav_save()){ fav_load(); refuse("Couldn't save favourites"); return -1; }
    return 0;
}

/* ================================================================ player link (main thread) */
static char g_err_main[96], g_name_main[RC_NAME_LEN];
static int g_shown_state = RS_OFF;
int radio_active(void){ return ui_get_source_mode() == RADIO_SOURCE; }
const char *radio_state(void){
    pthread_mutex_lock(&g_mu); int st = g_rstate; pthread_mutex_unlock(&g_mu);
    return st == RS_STARTING ? "starting" : st == RS_PLAYING ? "playing" : st == RS_ERROR ? "error" : "off";
}
int radio_station(rc_station_t *out){
    pthread_mutex_lock(&g_mu); int on = g_rstate != RS_OFF; if(on && out) *out = g_now; pthread_mutex_unlock(&g_mu);
    return on;
}
const char *radio_station_name(void){
    pthread_mutex_lock(&g_mu); snprintf(g_name_main, sizeof g_name_main, "%s", g_rstate == RS_OFF ? "" : g_now.name); pthread_mutex_unlock(&g_mu);
    return g_name_main;
}
const char *radio_stream_title(void){
    static char t[330];
    pthread_mutex_lock(&g_mu);
    int on = g_rstate == RS_PLAYING && g_icy_gen == g_play_gen && g_icy[0];
    if(!on) t[0] = 0;
    else snprintf(t, sizeof t, "%s%s%s", g_icy_artist, g_icy_artist[0] ? " - " : "", g_icy);
    pthread_mutex_unlock(&g_mu);
    return t;
}
/* the words on the Now Playing screens while the radio plays (1 s timer, main thread) */
static int g_tags_on;
static void tags_cb(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu);
    int on = (g_rstate == RS_PLAYING || g_rstate == RS_STARTING) && radio_active();
    rc_station_t s = g_now;
    char song[160] = "", artist[160] = "";
    if(on && g_rstate == RS_PLAYING && g_icy_gen == g_play_gen){ snprintf(song, sizeof song, "%s", g_icy); snprintf(artist, sizeof artist, "%s", g_icy_artist); }
    pthread_mutex_unlock(&g_mu);
    if(!on){ if(g_tags_on){ ipc_clear_tags(); g_tags_on = 0; } return; }
    char detail[128]; rc_detail(&s, detail, sizeof detail);
    if(song[0] && artist[0]) ipc_set_tags(song, artist, s.name);           /* the song, its artist, the station */
    else if(song[0]) ipc_set_tags(song, s.name, detail);                   /* a show name or "Title" only */
    else ipc_set_tags(s.name, detail[0] ? detail : "Web radio", "");      /* no song named (yet) */
    g_tags_on = 1;
}
const char *radio_last_error(void){
    pthread_mutex_lock(&g_mu); snprintf(g_err_main, sizeof g_err_main, "%s", g_err); pthread_mutex_unlock(&g_mu);
    return g_err_main;
}
static void refuse(const char *why){
    pthread_mutex_lock(&g_mu); snprintf(g_err, sizeof g_err, "%s", why); pthread_mutex_unlock(&g_mu);
    ui_toast(why);
}
static void poll_start(void);
/* The own player takes the station as one command (0d10, rc_station_frame) in the radio work mode; the stock player
 * only through its DLNA renderer (job_play on the worker). g_direct: the station went the first way. Main thread. */
static int g_direct;
static int send_direct(const rc_station_t *s){
    char f[16 + RC_NAME_LEN + RC_URL_LEN];
    int n = rc_station_frame(s, f, sizeof f);
    if(n < 0 || ipc_send_cmd(f) < 0){ fprintf(stderr, "radio: the player didn't take the station\n"); return -1; }
    fprintf(stderr, "radio: %s%s%s (own player, direct)\n", s ? "playing \"" : "stop", s ? s->name : "", s ? "\"" : "");
    return 0;
}
/* after a start was decided: direct (sent = send_direct's result; PLAYING at once, so the titles start) or through the
 * worker. Caller holds g_mu; the command itself is sent before, outside the lock (ipc may wait for the player). */
static void start_locked(int sent){
    if(!g_direct){ post_job(J_PLAY); return; }
    if(sent == 0){ g_rstate = RS_PLAYING; icy_kick(); }
    else { g_rstate = RS_ERROR; snprintf(g_err, sizeof g_err, "%s", "The player didn't take the station"); }
}
int radio_play(const rc_station_t *s){
    if(!s || !s->name[0] || !rc_url_ok(s->url)){ refuse("That station address can't be played"); return -1; }
    char ip[32];
    if(!wlan_ip(ip, sizeof ip)){ refuse("Connect to Wi-Fi to play radio"); return -1; }
    if(!radio_active() && ui_set_source_mode(RADIO_SOURCE) != 0){ refuse("Couldn't switch to the radio"); return -1; }   /* the reason was toasted */
    g_direct = opl_running();
    int sent = g_direct ? send_direct(s) : 0;
    pthread_mutex_lock(&g_mu);
    g_play_gen++; g_now = *s; g_job_station = *s; g_rstate = RS_STARTING; g_err[0] = 0;
    start_locked(sent);
    pthread_mutex_unlock(&g_mu);
    g_shown_state = RS_STARTING;
    ui_toast("Tuning in...");
    poll_start();
    radio_end_set(-1, s);                              /* the last station played is the one after the list */
    return 0;
}
/* ================================================================ after the list ends */
#define END_CFG_MODE "end_mode"
#define END_CFG_NAME "end_st_name"
#define END_CFG_URL  "end_st_url"
int radio_end_get(rc_station_t *st){
    if(st){
        memset(st, 0, sizeof *st);
        snprintf(st->name, sizeof st->name, "%s", cfg_get_str(END_CFG_NAME, ""));
        snprintf(st->url, sizeof st->url, "%s", cfg_get_str(END_CFG_URL, ""));
    }
    int m = cfg_get_int(END_CFG_MODE, RADIO_END_STOP);
    return m >= RADIO_END_STOP && m <= RADIO_END_STATION ? m : RADIO_END_STOP;
}
void radio_end_sync(void){
    if(!opl_running()) return;                           /* the stock player knows none of this */
    rc_station_t st; int m = radio_end_get(&st);
    char f[16 + RC_NAME_LEN + RC_URL_LEN];
    if(rc_station_frame_tag("0d12", st.url[0] ? &st : NULL, f, sizeof f) < 0 || ipc_send_cmd(f) < 0)
        fprintf(stderr, "radio: the player didn't take the station after the list\n");
    snprintf(f, sizeof f, "0d11000C%04X", (unsigned)m);
    if(ipc_send_cmd(f) < 0) fprintf(stderr, "radio: the player didn't take \"after the list ends\" (%d)\n", m);
}
int radio_end_set(int mode, const rc_station_t *s){
    if(mode != -1 && (mode < RADIO_END_STOP || mode > RADIO_END_STATION)) return -1;
    if(s && (!rc_url_ok(s->url))) return -1;
    int changed = 0;
    if(mode != -1 && cfg_get_int(END_CFG_MODE, RADIO_END_STOP) != mode){ cfg_set_int(END_CFG_MODE, mode); changed = 1; }
    if(s && (strcmp(cfg_get_str(END_CFG_URL, ""), s->url) || strcmp(cfg_get_str(END_CFG_NAME, ""), s->name))){
        cfg_set_str(END_CFG_NAME, s->name); cfg_set_str(END_CFG_URL, s->url); changed = 1;   /* only when it changes: flash */
    }
    if(changed) radio_end_sync();
    return 0;
}
int radio_search(const char *query){
    char ip[32];
    if(!wlan_ip(ip, sizeof ip)) return -1;
    pthread_mutex_lock(&g_mu);
    g_sstate = SR_BUSY; snprintf(g_job_query, sizeof g_job_query, "%s", query ? query : ""); post_job(J_SEARCH);
    pthread_mutex_unlock(&g_mu);
    return 0;
}
int radio_search_result(rc_station_t *out, int cap, int *state){
    pthread_mutex_lock(&g_mu);
    int n = g_sstate == SR_DONE ? g_nhits : 0;
    if(n > cap) n = cap;
    if(n > 0) memcpy(out, g_hits, sizeof g_hits[0] * (size_t)n);
    if(state) *state = g_sstate;
    pthread_mutex_unlock(&g_mu);
    return n;
}
int radio_play_url(const char *name, const char *url){
    rc_station_t s; memset(&s, 0, sizeof s);
    snprintf(s.name, sizeof s.name, "%s", name && name[0] ? name : "Web radio");
    if(!url || strlen(url) >= sizeof s.url){ refuse("That station address can't be played"); return -1; }
    snprintf(s.url, sizeof s.url, "%s", url);
    return radio_play(&s);
}
static void forget_play(void){                          /* any stream ends; a running start is dropped */
    pthread_mutex_lock(&g_mu);
    int had = g_rstate != RS_OFF;
    g_play_gen++; g_rstate = RS_OFF; g_err[0] = 0;
    if(had && !g_direct && g_ctl[0]) post_job(J_STOP);   /* best effort: the work-mode switch stops it too */
    pthread_mutex_unlock(&g_mu);
    if(had && g_direct && radio_active()) send_direct(NULL);
    g_shown_state = RS_OFF;
}
void radio_mode_leaving(void){ forget_play(); }
void radio_restart(void){
    pthread_mutex_lock(&g_mu);
    int on = g_rstate == RS_PLAYING || g_rstate == RS_STARTING;
    rc_station_t now = g_now;
    pthread_mutex_unlock(&g_mu);
    int sent = on && g_direct ? send_direct(&now) : 0;
    pthread_mutex_lock(&g_mu);
    if(on){ g_play_gen++; g_job_station = g_now; g_rstate = RS_STARTING; g_err[0] = 0; start_locked(sent); }
    pthread_mutex_unlock(&g_mu);
    if(on){ g_shown_state = RS_STARTING; poll_start(); }
}
void radio_stop(void){
    if(radio_active()) ui_set_source_mode(0);           /* -> radio_mode_leaving */
    else forget_play();
}

/* ================================================================ the screen */
enum { V_FAV, V_HITS };
static int g_view = V_FAV, g_hits_popular;
static rc_station_t g_res[RC_MAX_HITS];                 /* the results on screen: the main thread's copy of g_hits */
static int g_nres, g_res_state = SR_IDLE;
static lv_obj_t *g_root, *g_title, *g_list;
static curvelist_t g_cl;
static lv_timer_t *g_poll;
#define ROW_W 268
#define ROW_H TH_ROW_H
enum { ACT_SEARCH = -1, ACT_POPULAR = -2, ACT_STOP = -3 };

static void rebuild(void);
static void poll_cb(lv_timer_t *t);
static void poll_start(void){ if(!g_poll) g_poll = lv_timer_create(poll_cb, 300, NULL); }

static void note(const char *msg, const char *help){       /* an empty / busy / failure line in the list */
    if(th_zune()){ zune_empty(g_list, msg, help); return; }
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, help ? help : msg);
    if(help){ char b[256]; snprintf(b, sizeof b, "%s\n%s", msg, help); lv_label_set_text(l, b); }
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, ROW_W - 40);
    lv_obj_set_style_pad_top(l, 12, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}
static void row_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);
static lv_obj_t *add_row(intptr_t id, const char *glyph, const char *name, const char *detail, int accent){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_add_flag(r, LV_OBJ_FLAG_USER_1);                                     /* curves with the circle */
    lv_obj_set_style_radius(r, TH_R_ROW, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, 2);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_SHORT_CLICKED, (void *)id);
    if(id >= 0) lv_obj_add_event_cb(r, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)id);

    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, glyph);
    lv_obj_set_pos(ic, 12, 17);
    lv_obj_set_style_text_font(ic, TF(UI_16), 0);
    lv_obj_set_style_text_color(ic, accent ? ui_current_accent() : TC(TEXT_DISABLED), 0);

    int has_detail = detail && detail[0];
    lv_obj_t *nm = lv_label_create(r);
    lv_label_set_text(nm, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);               /* sideways only */
    lv_obj_set_pos(nm, 40, has_detail ? 6 : 15);
    lv_obj_set_size(nm, ROW_W - 50, 22);
    lv_obj_set_style_text_font(nm, ui_font_user(18), 0);                      /* station names in any script */
    lv_obj_set_style_text_color(nm, TC(TEXT_PRIMARY), 0);
    if(has_detail){
        lv_obj_t *dt = lv_label_create(r);
        lv_label_set_text(dt, detail);
        lv_label_set_long_mode(dt, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(dt, 40, 30);
        lv_obj_set_width(dt, ROW_W - 56);
        lv_obj_set_style_text_font(dt, TH_F_DETAIL, 0);
        lv_obj_set_style_text_color(dt, TC(TEXT_SECONDARY), 0);
    }
    theme_list_row(r);
    if(th_zune()) ui_library_row(r);
    return r;
}
static const rc_station_t *row_station(intptr_t id){
    if(g_view == V_FAV) return id >= 0 && id < g_nfav ? &g_fav[id] : NULL;
    return id >= 0 && id < g_nres ? &g_res[id] : NULL;
}
static void station_detail(const rc_station_t *s, char *out, size_t n){
    char d[128]; rc_detail(s, d, sizeof d);
    int playing = radio_active() && !strcmp(radio_state(), "playing") && !strcmp(g_now.url, s->url);
    int saved = g_view == V_HITS && rc_fav_find(g_fav, g_nfav, s->url) >= 0;
    snprintf(out, n, "%s%s%s%s", playing ? "Playing" : saved ? "Saved" : "", (playing || saved) && d[0] ? " \xC2\xB7 " : "", d,
             !d[0] && !playing && !saved ? "Web radio" : "");
}
static vlist_t g_fv;
static unsigned g_fav_shown;                            /* the g_fav_gen the favourite rows show */
static void fav_add_row(int i){ char d[160]; station_detail(&g_fav[i], d, sizeof d); add_row(i, LV_SYMBOL_AUDIO, g_fav[i].name, d, 0); }
static void fav_scroll_cb(lv_event_t *e){ (void)e; vlist_follow(&g_fv); }
static void rebuild(void){
    if(!g_list) return;
    vlist_end(&g_fv);
    lv_obj_clean(g_list);
    if(g_view == V_FAV){
        theme_title_text(g_title, th_zune() ? "radio" : "Radio");
        add_row(ACT_SEARCH, LV_SYMBOL_EDIT, "Search stations", NULL, 1);
        add_row(ACT_POPULAR, LV_SYMBOL_LIST, "Popular stations", NULL, 1);
        if(radio_active()) add_row(ACT_STOP, LV_SYMBOL_CLOSE, "Stop radio", radio_station_name(), 1);
        fav_fresh(0); g_fav_shown = g_fav_gen;
        if(g_fav_where && g_fav_nocard) note("No SD card", "Your radio favourites are kept on the SD card.");
        else if(!g_nfav) note("No favourites yet", "Hold a station to save it here.");
        else if(g_nfav >= VLIST_MIN){                    /* long lists: only the rows near the view exist */
            int lead = (lv_obj_get_child_count(g_list)) * (ROW_H + 4);
            vlist_begin(&g_fv, g_list, g_nfav, ROW_H, 4, lead, th_zune() ? 236 : 272, fav_add_row);
        } else for(int i = 0; i < g_nfav; i++) fav_add_row(i);
    } else {
        int st = g_res_state, n = g_nres;
        theme_title_text(g_title, th_zune() ? (g_hits_popular ? "popular" : "results") : (g_hits_popular ? "Popular" : "Results"));
        if(st == SR_BUSY) note("Looking for stations...", NULL);
        else if(st == SR_FAIL) note("Couldn't reach the station directory", "Check Wi-Fi and try again.");
        else if(n == 0) note("No stations found", "Try another name.");
        else for(int i = 0; i < n; i++){ char d[160]; station_detail(&g_res[i], d, sizeof d); add_row(i, LV_SYMBOL_AUDIO, g_res[i].name, d, 0); }
    }
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
    lv_obj_update_layout(g_list);
    curvelist_update(&g_cl);
}
static void start_search(const char *query){
    char ip[32];
    g_view = V_HITS; g_hits_popular = !query[0];
    int online = wlan_ip(ip, sizeof ip);
    g_nres = 0; g_res_state = online ? SR_BUSY : SR_FAIL;
    pthread_mutex_lock(&g_mu);
    if(online){ g_sstate = SR_BUSY; snprintf(g_job_query, sizeof g_job_query, "%s", query); post_job(J_SEARCH); }
    g_search_seen = g_search_gen;
    pthread_mutex_unlock(&g_mu);
    rebuild();
    if(!online) ui_toast("Connect to Wi-Fi to find stations");
    poll_start();
}
static char g_last_query[128];
static void search_done(const char *text){
    if(!text) return;                                   /* cancelled */
    while(*text == ' ') text++;
    if(!text[0]) return;
    snprintf(g_last_query, sizeof g_last_query, "%s", text);
    start_search(g_last_query);
}
static int fav_stale(intptr_t id){                     /* a favourite row of an older read of the file: show the new list */
    if(g_view != V_FAV || id < 0) return 0;
    fav_fresh(1);
    if(g_fav_gen == g_fav_shown) return 0;
    rebuild(); return 1;
}
static void row_cb(lv_event_t *e){
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    if(fav_stale(id)) return;
    if(id == ACT_SEARCH){ kbinput_open("Station name", g_last_query, search_done); return; }
    if(id == ACT_POPULAR){ start_search(""); return; }
    if(id == ACT_STOP){ radio_stop(); rebuild(); return; }
    const rc_station_t *s = row_station(id);
    if(s){ rc_station_t c = *s; radio_play(&c); }
}
static void row_long_cb(lv_event_t *e){
    if(fav_stale((intptr_t)lv_event_get_user_data(e))) return;
    const rc_station_t *s = row_station((intptr_t)lv_event_get_user_data(e));
    if(!s) return;
    rc_station_t c = *s;
    fav_toggle(&c);
    int y = lv_obj_get_scroll_y(g_list);
    rebuild();
    lv_obj_update_layout(g_list); lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF); vlist_follow(&g_fv); curvelist_update(&g_cl);
}
static void poll_cb(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu);
    int st = g_rstate, sgen_new = g_search_gen != g_search_seen, busy = g_job != J_NONE || g_sstate == SR_BUSY;
    char err[96]; snprintf(err, sizeof err, "%s", g_err);
    if(sgen_new && g_sstate != SR_BUSY){                 /* a finished search (an older one may finish first: a newer one keeps "busy") */
        g_search_seen = g_search_gen;
        g_nres = g_nhits; g_res_state = g_sstate;
        memcpy(g_res, g_hits, sizeof g_hits[0] * (size_t)g_nhits);
    } else sgen_new = 0;
    pthread_mutex_unlock(&g_mu);
    if(sgen_new && g_view == V_HITS && screen_current() == SCR_RADIO) rebuild();
    if(st != g_shown_state){
        int was = g_shown_state; g_shown_state = st;
        if(st == RS_PLAYING && was == RS_STARTING){
            if(screen_current() == SCR_RADIO) screen_show(SCR_NOWPLAYING);
        } else if(st == RS_ERROR) ui_toast(err[0] ? err : "Radio couldn't start");
        if(screen_current() == SCR_RADIO && g_view == V_FAV) rebuild();
    }
    if(st != RS_STARTING && !busy && g_poll){ lv_timer_delete(g_poll); g_poll = NULL; }
}
int radio_back_consumed(void){
    if(g_view != V_HITS) return 0;
    g_view = V_FAV; rebuild();
    return 1;
}
static void header_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && !radio_back_consumed()) screen_back();
}
void radio_create(lv_obj_t *root){
    g_root = root;
    lv_timer_create(tags_cb, 1000, NULL);
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    if(th_zune()){ g_title = zune_pivot(root, "radio", header_back_cb); zune_status(root, SCR_RADIO); }
    else g_title = ui_header_cb(root, "Radio", header_back_cb);
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, (360 - ROW_W) / 2, th_zune() ? 124 : 72);
    lv_obj_set_size(g_list, ROW_W, th_zune() ? 236 : 272);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);                               /* the last row clears the round bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 4, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_list, fav_scroll_cb, LV_EVENT_SCROLL, NULL);  /* the window first, then the curve */
    curvelist_attach(&g_cl, g_list, root, ROW_W);
    if(th_zune()) ui_library_list_watch(g_list);
}
void radio_open(void){
    g_view = V_FAV;
    fav_fresh(1);
    rebuild();
    screen_show(SCR_RADIO);
}
/* host renders: favourites and results without /usr/data or the network */
void radio_test_favorites(const rc_station_t *s, int n){
    g_fav_test = 1; g_fav_loaded = 1; g_fav_where = 0; g_fav_nocard = 0;
    n = n < 0 ? 0 : n > RC_MAX_FAV ? RC_MAX_FAV : n;
    g_nfav = n && fav_grow(n) == 0 ? n : 0;
    if(g_nfav) memcpy(g_fav, s, sizeof s[0] * (size_t)g_nfav);
}
void radio_test_scroll(int y){ if(g_list){ lv_obj_scroll_to_y(g_list, y, LV_ANIM_OFF); vlist_follow(&g_fv); lv_obj_update_layout(g_list); curvelist_update(&g_cl); } }
void radio_test_results(const rc_station_t *s, int n, int popular){
    pthread_mutex_lock(&g_mu);
    if(n > RC_MAX_HITS) n = RC_MAX_HITS;
    if(n > 0) memcpy(g_res, s, sizeof s[0] * (size_t)n);
    g_nres = n < 0 ? 0 : n; g_res_state = n < 0 ? SR_FAIL : SR_DONE;
    pthread_mutex_unlock(&g_mu);
    g_view = V_HITS; g_hits_popular = popular; rebuild();
}
