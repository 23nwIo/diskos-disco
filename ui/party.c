/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Party mode: guests use a link of their own, http://<Disc>:8088/<party name>/, with a reduced page (party.html):
 * what plays now, the queue, the library to add songs from, and the volume - each only as far as the host allows on
 * the admin page (System > Party, GET/POST api/party). Nothing changes on the Disc's own screens. With "See what plays
 * now" off, the queue shows other guests' songs without title, artist or length (only the guest's own in full).
 *
 * Guests are told apart by a random id their browser makes and keeps (g=); a private window is a new guest. The ids,
 * names and the songs each queued are kept in memory only: they start afresh after a restart. The settings are kept on
 * the SD card (Jo, 2026-11-16), in .diskos/state/party.conf (sdstate.h), written only when the host changes them;
 * without a card the defaults apply and party mode is off. A guest's radio station becomes the station after the queue
 * (After the List Ends, radio.c), or plays at once when skipping is allowed too: guests never cut a song short otherwise.
 *
 * Main thread only (remote.c's dispatch calls party_dispatch for every guest request). */
#include "party.h"
#include "remote.h"
#include "remote_srv.h"
#include "queueview.h"
#include "screens.h"
#include "musicdb.h"
#include "sdstate.h"
#include "radio.h"
#include "scanner.h"
#include "folderbrowser.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "own_player.h"
int settings_outputs_state(opl_outputs_t *o, int *auto_out);   /* settings.c */
#define GUESTS 128
static struct { char gid[RMT_GID_MAX + 1], name[RMT_GNAME_MAX + 1]; uint32_t seen; char *stations; } g_guest[GUESTS];

#define PARTY_FILE "party.conf"
/* the settings as read from the card: read again every few seconds (another card, a copy edited on a computer) */
static int g_val[RMT_P_N]; static char g_name[RMT_PARTY_NAME_MAX + 1]; static uint32_t g_read_at;
static void parse(const char *text){
    for(int k = 0; k < RMT_P_N; k++) g_val[k] = RMT_PARTY_KEYS[k].def;
    g_name[0] = 0;
    for(const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL){
        char key[24], val[RMT_PARTY_NAME_MAX + 2];
        if(sscanf(l, "%23[a-z_]=%33[^\n]", key, val) != 2) continue;
        if(!strcmp(key, "name")){ if(rmt_party_name_ok(val)) snprintf(g_name, sizeof g_name, "%.32s", val); continue; }
        for(int k = 0; k < RMT_P_N; k++){
            char *e; long v = strtol(val, &e, 10);
            if(!strcmp(key, RMT_PARTY_KEYS[k].key) && !*e && v >= RMT_PARTY_KEYS[k].min && v <= RMT_PARTY_KEYS[k].max) g_val[k] = (int)v;
        }
    }
}
static void load(void){
    if(g_read_at && lv_tick_elaps(g_read_at) < 5000) return;
    g_read_at = lv_tick_get() | 1;
    char *t = NULL; size_t n = 0; int r = 0;
    if(sds_begin()){ r = sds_read(PARTY_FILE, &t, &n, 4096); sds_end(); }
    parse(r == 1 ? t : "");
    if(r < 0) fprintf(stderr, "party: %s on the SD card could not be read - party mode stays off\n", PARTY_FILE);
    if(r < 0) g_val[RMT_P_ON] = 0;
    free(t);
}
static int cfg_val(int k){ load(); return g_val[k]; }
static const char *cfg_name(void){ load(); return g_name; }
static int save(const int *val, const char *name){
    char text[512]; int o = 0;
    o += snprintf(text + o, sizeof text - (size_t)o, "# diskOS party mode (Remote Control: System > Party)\nname=%s\n", name);
    for(int k = 0; k < RMT_P_N && o < (int)sizeof text; k++) o += snprintf(text + o, sizeof text - (size_t)o, "%s=%d\n", RMT_PARTY_KEYS[k].key, val[k]);
    if(o >= (int)sizeof text) return -1;
    int r = -1;
    if(sds_begin()){ r = sds_mkdir() == 0 ? sds_write(PARTY_FILE, text, (size_t)o) : -1; sds_end(); }
    if(r == 0){ memcpy(g_val, val, sizeof g_val); snprintf(g_name, sizeof g_name, "%s", name); g_read_at = lv_tick_get() | 1; }
    return r;
}
const char *party_link_name(void){ return cfg_val(RMT_P_ON) ? cfg_name() : ""; }
const char *party_upload_dir(long long *max_bytes){
    static char dir[PATH_MAX];
    *max_bytes = (long long)cfg_val(RMT_P_UPMAX) * 1024 * 1024;
    if(!party_link_name()[0] || !cfg_val(RMT_P_UPLOAD)) return "";
    snprintf(dir, sizeof dir, "%s/%s/%s", FOLDERBROWSER_ROOT, RMT_UPLOAD_DIR, cfg_name());
    return dir;
}
/* Guests' own stations, for the host (Jo, 2026-11-16): in RAM, and as /tmp/party-stations.txt (guest, station, URL). */
#define STATIONS_FILE "/tmp/party-stations.txt"
static void stations_write(void){
    FILE *f = fopen(STATIONS_FILE ".new", "w");
    if(!f){ perror("party: " STATIONS_FILE); return; }
    fprintf(f, "# guest\tstation\tURL (diskOS party mode: the stations each guest saved in their browser)\n");
    for(int i = 0; i < GUESTS; i++){
        if(!g_guest[i].gid[0] || !g_guest[i].stations) continue;
        for(const char *l = g_guest[i].stations; *l; ){
            const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
            fprintf(f, "%s\t%.*s\n", g_guest[i].name[0] ? g_guest[i].name : g_guest[i].gid, (int)n, l);
            l += n + (e ? 1 : 0);
        }
    }
    if(fclose(f) || rename(STATIONS_FILE ".new", STATIONS_FILE)) perror("party: " STATIONS_FILE);
}

/* ---------------------------------------------------------------- guests */
static int guest_slot(const char *gid){                  /* 1-based "who", 0 none / no room */
    if(!gid || !gid[0]) return 0;
    for(int i = 0; i < GUESTS; i++) if(!strcmp(g_guest[i].gid, gid)){ g_guest[i].seen = lv_tick_get() | 1; return i + 1; }
    int free_at = -1; uint32_t oldest = 0;
    for(int i = 0; i < GUESTS; i++){
        if(!g_guest[i].gid[0]){ free_at = i; break; }
        int queued = 0;                                   /* a guest with songs in the queue keeps the slot (the queue names them) */
        for(int q = 0, n = queue_count(); q < n && !queued; q++) queued = queue_item_who(q) == i + 1;
        if(queued || queue_now_who() == i + 1) continue;
        uint32_t age = lv_tick_elaps(g_guest[i].seen);
        if(free_at < 0 || age > oldest){ free_at = i; oldest = age; }
    }
    if(free_at < 0) return 0;
    free(g_guest[free_at].stations);
    memset(&g_guest[free_at], 0, sizeof g_guest[free_at]);
    snprintf(g_guest[free_at].gid, sizeof g_guest[free_at].gid, "%s", gid);
    g_guest[free_at].seen = lv_tick_get() | 1;
    return free_at + 1;
}
const char *party_guest_name(int who){
    if(who < 1 || who > GUESTS || !g_guest[who - 1].gid[0]) return "";
    return g_guest[who - 1].name[0] ? g_guest[who - 1].name : "a guest";
}
static int queued_by(int who){
    int k = 0;
    for(int i = 0, n = queue_count(); i < n; i++) k += queue_item_who(i) == who;
    return k;
}

/* ---------------------------------------------------------------- the host's settings (System > Party) */
static int admin(const rmt_cmd_t *c, const char **why, char **body){
    if(c->arg){
        const char *tok = remote_token();
        if(c->name[0] && tok && !strcasecmp(c->name, tok)){ *why = "Choose a party name that is not your own link"; return -1; }
        int turn_on = c->party[RMT_P_ON] == 1;
        if(turn_on && !c->name[0] && !cfg_name()[0]){ *why = "Choose a party name first"; return -1; }
        int val[RMT_P_N]; char name[RMT_PARTY_NAME_MAX + 1];
        g_read_at = 0; load();                            /* the card's current file, not an old copy */
        for(int k = 0; k < RMT_P_N; k++) val[k] = c->party[k] >= 0 ? c->party[k] : g_val[k];
        snprintf(name, sizeof name, "%.32s", c->name[0] ? c->name : g_name);
        if(save(val, name)){ *why = "Couldn't save on the SD card - is a card in the Disc?"; return -1; }
    }
    rmt_buf_t b; memset(&b, 0, sizeof b);
    rmt_buf_put(&b, "{\"ok\":true,\"settings\":{");
    for(int k = 0; k < RMT_P_N; k++) rmt_buf_printf(&b, "%s\"%s\":%d", k ? "," : "", RMT_PARTY_KEYS[k].key, cfg_val(k));
    { opl_outputs_t o; int au; rmt_buf_printf(&b, ",\"own_player\":%s", settings_outputs_state(&o, &au) == 0 ? "true" : "false"); }
    rmt_buf_put(&b, ",\"name\":"); rmt_buf_str(&b, cfg_name());
    rmt_buf_put(&b, "},\"guests\":[");
    for(int i = 0, first = 1; i < GUESTS; i++){
        if(!g_guest[i].gid[0]) continue;
        rmt_buf_printf(&b, "%s{\"name\":", first ? "" : ","); rmt_buf_str(&b, g_guest[i].name);
        rmt_buf_printf(&b, ",\"queued\":%d,\"seen_s\":%u,\"stations\":[", queued_by(i + 1), (unsigned)(lv_tick_elaps(g_guest[i].seen) / 1000));
        for(const char *l = g_guest[i].stations, *sep = ""; l && *l; sep = ","){
            const char *t = strchr(l, '\t'), *e = strchr(l, '\n'); if(!t || !e || t > e) break;
            char nm[160], url[600];
            snprintf(nm, sizeof nm, "%.*s", (int)(t - l), l); snprintf(url, sizeof url, "%.*s", (int)(e - t - 1), t + 1);
            rmt_buf_printf(&b, "%s{\"name\":", sep); rmt_buf_str(&b, nm); rmt_buf_put(&b, ",\"url\":"); rmt_buf_str(&b, url); rmt_buf_put(&b, "}");
            l = e + 1;
        }
        rmt_buf_put(&b, "]}");
        first = 0;
    }
    rmt_buf_put(&b, "]}");
    *body = rmt_buf_take(&b);
    if(!*body){ *why = "Out of memory"; return -1; }
    return 0;
}

/* Guests' radio: with "radio" on they pick a station - it plays when the queue is empty (own player: After the List
 * Ends), or at once when they may also skip songs (Jo, 2026-11-16). Searching and their own station list always work. */
static int own_player(void){ opl_outputs_t o; int au; return settings_outputs_state(&o, &au) == 0; }
static int radio_ok(void){ return cfg_val(RMT_P_RADIO); }

/* ---------------------------------------------------------------- what guests see */
static int scale_down(int v, int vmax, int maxpct){       /* the Disc's volume -> the guests' 0..100 */
    long top = (long)vmax * maxpct / 100; if(top < 1) top = 1;
    long g = ((long)v * 100 + top / 2) / top;
    return g > 100 ? 100 : (int)g;
}
static char *state_json(int who){
    rmt_state_t s; remote_state(&s);
    int np = cfg_val(RMT_P_NP), names = cfg_val(RMT_P_NAMES), total = cfg_val(RMT_P_TOTAL), mine = who ? queued_by(who) : 0;
    rmt_buf_t b; memset(&b, 0, sizeof b);
    rmt_buf_printf(&b, "{\"ok\":true,\"party\":{\"np\":%d,\"queue\":%d,\"vol\":%d,\"skip\":%d,\"radio\":%d,\"names\":%d,"
                   "\"total\":%d,\"consec\":%d,\"repeat\":%d,\"mine\":%d,\"left\":%d,\"radio_end\":%d,\"radio_now\":%d,\"upload\":%d,\"upmax\":%d,\"you\":",
                   np, cfg_val(RMT_P_QUEUE), cfg_val(RMT_P_VOL), cfg_val(RMT_P_SKIP), radio_ok(), names,
                   total, cfg_val(RMT_P_CONSEC), cfg_val(RMT_P_REPEAT), mine, total > mine ? total - mine : 0,
                   radio_ok() && own_player(), radio_ok() && cfg_val(RMT_P_SKIP), cfg_val(RMT_P_UPLOAD), cfg_val(RMT_P_UPMAX));
    rmt_buf_str(&b, who && names ? g_guest[who - 1].name : "");
    rmt_buf_printf(&b, "},\"playing\":%s,\"has_track\":%s,\"controllable\":%s,\"source\":",
                   s.playing ? "true" : "false", np && s.has_track ? "true" : "false", s.controllable ? "true" : "false");
    rmt_buf_str(&b, s.source ? s.source : "other");
    if(cfg_val(RMT_P_VOL)) rmt_buf_printf(&b, ",\"volume\":%d", scale_down(s.volume, s.volume_max, cfg_val(RMT_P_MAXVOL)));
    if(np){
        rmt_buf_put(&b, ",\"title\":"); rmt_buf_str(&b, s.title);
        rmt_buf_put(&b, ",\"artist\":"); rmt_buf_str(&b, s.artist);
        rmt_buf_put(&b, ",\"album\":"); rmt_buf_str(&b, s.album);
        rmt_buf_put(&b, ",\"station\":"); rmt_buf_str(&b, s.radio_state && strcmp(s.radio_state, "off") ? s.station : "");
        rmt_buf_put(&b, ",\"stream_title\":"); rmt_buf_str(&b, s.stream_title);
        rmt_buf_put(&b, ",\"art\":"); rmt_buf_str(&b, s.art);
        rmt_buf_printf(&b, ",\"position_ms\":%ld,\"duration_ms\":%ld", s.position_ms, s.duration_ms);
        rmt_buf_put(&b, ",\"by\":"); rmt_buf_str(&b, names ? party_guest_name(queue_now_who()) : "");
        rmt_buf_printf(&b, ",\"by_you\":%s", who && queue_now_who() == who ? "true" : "false");
    }
    if(cfg_val(RMT_P_QUEUE)){ const qv_view_t *q = qv_view(&s, 0); rmt_buf_printf(&b, ",\"queue_ver\":%u", q ? q->ver : 0); }
    rmt_buf_put(&b, "}");
    return rmt_buf_take(&b);
}
static char *queue_json(int who){
    rmt_state_t s; remote_state(&s);
    const qv_view_t *v = qv_view(&s, 0);
    if(!v) return NULL;
    int np = cfg_val(RMT_P_NP), names = cfg_val(RMT_P_NAMES);
    rmt_buf_t b; memset(&b, 0, sizeof b);
    rmt_buf_printf(&b, "{\"ok\":true,\"ver\":%u,\"items\":[", v->ver);
    for(int i = 0, first = 1; i < v->n; i++){
        const qv_entry_t *e = &v->e[i];
        if(e->kind == QV_NEXT || ((e->kind == QV_CUR || e->kind == QV_OTHER) && !np)) continue;   /* the host's own list stays theirs */
        int w = e->kind == QV_QUEUE ? queue_item_who(e->idx) : e->kind == QV_CUR ? queue_now_who() : 0;
        int mine = who && w == who, hide = !np && !mine;   /* a music quiz: others' queued songs without title (Jo, 2026-11-16) */
        rmt_buf_printf(&b, "%s{\"k\":\"%s\",\"d\":%d,\"mine\":%s,\"hidden\":%s,\"t\":", first ? "" : ",", e->kind == QV_QUEUE ? "queue" : "cur",
                       hide ? 0 : e->dur_ms, mine ? "true" : "false", hide ? "true" : "false");
        rmt_buf_str(&b, hide ? "" : e->title); rmt_buf_put(&b, ",\"a\":"); rmt_buf_str(&b, hide ? "" : e->artist);
        rmt_buf_put(&b, ",\"by\":"); rmt_buf_str(&b, names ? party_guest_name(w) : "");
        rmt_buf_put(&b, "}");
        first = 0;
    }
    rmt_buf_put(&b, "]}");
    return rmt_buf_take(&b);
}

/* ---------------------------------------------------------------- a guest adds a song */
static int add_song(int who, int id, const char **why, char **body){
    if(!who){ *why = "Too many guests - try again later"; return -1; }
    int total = cfg_val(RMT_P_TOTAL), mine = queued_by(who), n = queue_count();
    if(mine >= total){
        *why = total == 1 ? "Your song is still in the queue - you can add another once it plays"
                          : "Your songs are all in the queue - you can add another once one plays";
        return -1;
    }
    char path[512];
    if(mdb_song_path(id, path, sizeof path) != 1){ *why = "No such song"; return -1; }
    if(!cfg_val(RMT_P_REPEAT))
        for(int i = 0; i < n; i++){
            char qp[512];
            if(queue_item_song(i) == id || (queue_item(i, qp, sizeof qp, NULL, 0, NULL, 0, NULL) && !strcmp(qp, path) && !queue_item_song(i))){
                *why = "That song is already in the queue"; return -1; }
        }
    static int owners[1000];
    if(n > (int)(sizeof owners / sizeof owners[0])) n = (int)(sizeof owners / sizeof owners[0]);
    for(int i = 0; i < n; i++) owners[i] = queue_item_who(i);
    int at = rmt_party_place(owners, n, who, cfg_val(RMT_P_CONSEC), queue_now_who());   /* also when only g's songs wait */
    if(at < 0){
        *why = cfg_val(RMT_P_CONSEC) == 1 ? "Your song would play right after your own - wait until someone else adds one"
                                          : "Too many of your songs in a row - wait until someone else adds one";
        return -1;
    }
    int r = queue_add_ids_at(&id, 1, at, who);
    if(r == -2){ *why = "The library is busy - try again"; return -1; }
    if(r == -1){ *why = "The queue is full"; return -1; }
    if(r <= 0){ *why = "Couldn't add that song"; return -1; }
    qv_invalidate();
    *body = malloc(64);
    if(*body) snprintf(*body, 64, "{\"ok\":true,\"added\":1,\"place\":%d,\"left\":%d}", at + 1, total - mine - 1);
    return 0;
}

/* ---------------------------------------------------------------- the guests' requests */
int party_dispatch(const rmt_cmd_t *c, const char **why, char **body){
    *body = NULL;
    if(c->kind == RMT_CMD_PARTY) return admin(c, why, body);
    if(!party_link_name()[0]){ *why = "The party is over"; return -1; }
    int who = guest_slot(c->gid);
    if(who && c->gname[0] && cfg_val(RMT_P_NAMES)) snprintf(g_guest[who - 1].name, sizeof g_guest[who - 1].name, "%s", c->gname);
    rmt_cmd_t x = *c; x.guest = 0;
    switch(c->kind){
    case RMT_CMD_PARTY_STATE:
        *body = state_json(who); if(!*body) *why = "Out of memory";
        return *body ? 0 : -1;
    case RMT_CMD_COVER:
        if(!cfg_val(RMT_P_NP)){ *why = "The host keeps the song a secret"; return -1; }
        return remote_exec(&x, why, body);
    case RMT_CMD_NEXT:
        if(!cfg_val(RMT_P_SKIP)){ *why = "Skipping is off at this party"; return -1; }
        return remote_exec(&x, why, body);
    case RMT_CMD_VOLUME: {
        if(!cfg_val(RMT_P_VOL)){ *why = "The volume is the host's at this party"; return -1; }
        rmt_state_t s; remote_state(&s);
        long top = (long)s.volume_max * cfg_val(RMT_P_MAXVOL) / 100;
        x.arg = (c->arg * top + 50) / 100;                  /* the guests' 100 = the host's maximum */
        return remote_exec(&x, why, body); }
    case RMT_CMD_QUEUE:
        if(c->sub == RMT_Q_ADD){
            int id = (int)c->song_id;
            if(c->item == RMT_I_FILE && !remote_card_song(c->name, &id, why)) return -1;
            return add_song(who, id, why, body);
        }
        if(!cfg_val(RMT_P_QUEUE)){ *why = "The host keeps the queue a secret"; return -1; }
        *body = queue_json(who); if(!*body) *why = "Out of memory";
        return *body ? 0 : -1;
    case RMT_CMD_BROWSE:
        if(remote_exec(&x, why, body)) return -1;
        for(char *f = *body; f && (f = strstr(f, "\"fav\":true")); f += 10) memcpy(f, "\"fav\":0   ", 10);   /* the host's favourites stay theirs */
        return 0;
    case RMT_CMD_RADIO_SEARCH:
        return remote_exec(&x, why, body);
    case RMT_CMD_PARTY_STATIONS:
        if(!who){ *why = "Too many guests - try again later"; return -1; }
        free(g_guest[who - 1].stations);
        g_guest[who - 1].stations = c->text[0] ? strdup(c->text) : NULL;
        stations_write();
        return 0;
    case RMT_CMD_PARTY_UPLOAD: {                           /* the server thread saved it: into the library, not the queue (Jo) */
        char full[PATH_MAX]; int id;
        if(snprintf(full, sizeof full, "%s/%s", FOLDERBROWSER_ROOT, c->name) >= (int)sizeof full){ *why = "No such file"; return -1; }
        id = scanner_add_file(full);
        if(id < 0){ *why = scanner_active() ? "Saved - it shows up once the library scan is done" : "Saved, but the library couldn't take it now"; return -1; }
        if(id == 0){ *why = "Saved, but the Disc can't read that file"; return -1; }
        mdb_song_t s; char p[512];
        rmt_buf_t b; memset(&b, 0, sizeof b);
        rmt_buf_printf(&b, "{\"ok\":true,\"song_id\":%d,\"file\":", id); rmt_buf_str(&b, c->name);
        rmt_buf_put(&b, ",\"title\":"); rmt_buf_str(&b, mdb_queue_song(id, p, sizeof p, &s) ? s.title : "");
        rmt_buf_put(&b, "}");
        *body = rmt_buf_take(&b); if(!*body){ *why = "Out of memory"; return -1; }
        return 0; }
    case RMT_CMD_PARTY_RADIO: {
        if(!radio_ok()){ *why = "The host doesn't let guests choose the radio"; return -1; }
        if(c->arg){                                        /* at once: cuts the song short, so only with skipping allowed */
            if(!cfg_val(RMT_P_SKIP)){ *why = "Skipping is off at this party - the station can only follow the queue"; return -1; }
            if(radio_play_url(c->q, c->name) == 0) return 0;
            const char *e = radio_last_error(); *why = e && e[0] ? e : "The radio couldn't start";
            return -1;
        }
        if(!own_player()){ *why = "Only at once here - the Disc plays nothing after the queue with its stock player"; return -1; }
        rc_station_t st; memset(&st, 0, sizeof st);
        snprintf(st.name, sizeof st.name, "%s", c->q); snprintf(st.url, sizeof st.url, "%s", c->name);
        if(radio_end_set(RADIO_END_STATION, &st)){ *why = "Couldn't save that"; return -1; }
        return 0; }
    default:
        *why = "Not at a party"; return -1;
    }
}
