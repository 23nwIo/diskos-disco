/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "btavrcp.h"
#include "core.h"
#include "engine.h"
#include "../dbusmini.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define RETRY_S       5.0          /* connect / register again after this long */
#define MAX_LINKS     4
#define MPRIS_PLAYER  "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE   "org.freedesktop.DBus.Properties"
#define BLUEZ         "org.bluez"
#define TRANSPORT_IF  "org.bluez.MediaTransport1"

static btav_cb g_cb;
static const char *g_bus;
static dm_conn g_c = { .fd = -1 };
static double g_now, g_next_conn, g_next_reg;
static int g_registered, g_on;
static uint32_t g_reg_serial, g_mo_serial;
static char g_last_err[128];
/* what the headphones were told */
static int g_state = -1;
static char g_title[256], g_artist[160];
static long g_dur = -1;
/* the A2DP links BlueZ gave a volume for */
static struct { char path[96]; char mac[18]; int vol; } g_links[MAX_LINKS];

#define L(...) do{ if(g_cb.log) g_cb.log(__VA_ARGS__); }while(0)

/* ---- pure helpers ---- */
int btav_mac_of_path(const char *path, char *mac, int cap){
    const char *p = path ? strstr(path, "/dev_") : NULL;
    if(!p || cap < 18) return -1;
    p += 5;
    for(int i = 0; i < 17; i++){
        char ch = p[i];
        if(i % 3 == 2){ if(ch != '_') return -1; mac[i] = ':'; continue; }
        if(!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))) return -1;
        mac[i] = (ch >= 'a' && ch <= 'f') ? (char)(ch - 32) : ch;
    }
    if(p[17] && p[17] != '/') return -1;
    mac[17] = 0;
    return 0;
}
int btav_to127(int vol, int max){
    if(max <= 0) return 0;
    if(vol < 0) vol = 0;
    if(vol > max) vol = max;
    return (vol * 127 + max / 2) / max;      /* as btout.c bto_volume */
}
int btav_from127(int v, int max){
    if(v < 0) v = 0;
    if(v > 127) v = 127;
    return (v * max + 63) / 127;
}

/* ---- the links ---- */
static int link_find(const char *path){
    for(int i = 0; i < MAX_LINKS; i++) if(g_links[i].path[0] && !strcmp(g_links[i].path, path)) return i;
    return -1;
}
static void link_volume(const char *path, int v){
    char mac[18];
    if(btav_mac_of_path(path, mac, sizeof mac) != 0) return;
    int i = link_find(path), first = 0;
    if(i < 0){
        for(i = 0; i < MAX_LINKS && g_links[i].path[0]; i++){}
        if(i == MAX_LINKS) i = 0;                       /* full: the oldest slot goes */
        snprintf(g_links[i].path, sizeof g_links[i].path, "%s", path);
        snprintf(g_links[i].mac, sizeof g_links[i].mac, "%s", mac);
        first = 1;
    } else if(g_links[i].vol == v) return;
    g_links[i].vol = v;
    L("avrcp: %s volume %d/127%s", mac, v, first ? " (absolute volume: the headphones have it)" : "");
    if(g_cb.volume) g_cb.volume(mac, v, first);
}
static void link_forget(const char *path){
    int i = link_find(path);
    if(i < 0) return;
    L("avrcp: %s link gone", g_links[i].mac);
    g_links[i].path[0] = 0;
}
int btav_volume_of(const char *mac){
    for(int i = 0; i < MAX_LINKS; i++) if(g_links[i].path[0] && !strcasecmp(g_links[i].mac, mac)) return g_links[i].vol;
    return -1;
}

/* ---- the player's properties ---- */
static const char *status_str(int st){ return st == ENG_PLAY ? "Playing" : st == ENG_PAUSE ? "Paused" : "Stopped"; }
static void w_metadata(dm_buf *w){
    dm_w_struct(w); dm_w_str(w, "Metadata"); dm_w_sig(w, "a{sv}");
    size_t a = dm_w_array_begin(w, 8);
    dm_w_dict_sv_str(w, "xesam:title", g_title);
    if(g_artist[0]){
        /* xesam:artist is a list of strings */
        dm_w_struct(w); dm_w_str(w, "xesam:artist"); dm_w_sig(w, "as");
        size_t b = dm_w_array_begin(w, 4); dm_w_str(w, g_artist); dm_w_array_end(w, b);
    }
    if(g_dur > 0) dm_w_dict_sv_i64(w, "mpris:length", (int64_t)g_dur * 1000);   /* microseconds */
    dm_w_array_end(w, a);
}
static void w_props(dm_buf *w, int with_caps){
    size_t a = dm_w_array_begin(w, 8);
    dm_w_dict_sv_str(w, "PlaybackStatus", status_str(g_state < 0 ? ENG_STOP : g_state));
    if(with_caps){
        dm_w_dict_sv_bool(w, "CanControl", 1);
        dm_w_dict_sv_bool(w, "CanPlay", 1);
        dm_w_dict_sv_bool(w, "CanPause", 1);
        dm_w_dict_sv_bool(w, "CanGoNext", 1);
        dm_w_dict_sv_bool(w, "CanGoPrevious", 1);
        dm_w_dict_sv_str(w, "Identity", "Snowsky Disc");
    }
    w_metadata(w);
    dm_w_array_end(w, a);
}

/* ---- talking ---- */
static void drop(const char *why){
    if(g_c.fd >= 0) L("avrcp: D-Bus connection closed (%s)", why);
    dm_close(&g_c);
    g_registered = 0; g_reg_serial = g_mo_serial = 0;
    g_next_conn = g_now + RETRY_S;
    memset(g_links, 0, sizeof g_links);
}
static void say_err(const char *what, const char *err){
    char b[sizeof g_last_err];
    snprintf(b, sizeof b, "%s: %s", what, err ? err : "?");
    if(strcmp(b, g_last_err)){ L("avrcp: %s (trying again every %.0f s)", b, RETRY_S); snprintf(g_last_err, sizeof g_last_err, "%s", b); }
}
static void do_register(void){
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, BTAV_PLAYER_PATH);
    w_props(&b, 1);
    g_reg_serial = dm_call(&g_c, BLUEZ, "/org/bluez/hci0", "org.bluez.Media1", "RegisterPlayer", "oa{sv}", &b);
    dm_buf_free(&b);
    if(!g_reg_serial) drop("send RegisterPlayer failed");
    g_next_reg = g_now + RETRY_S;
}
static void ask_objects(void){
    g_mo_serial = dm_call(&g_c, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, NULL);
}
static void reply_empty(const dm_msg *m){
    if(m->flags & DM_NO_REPLY) return;
    dm_send(&g_c, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, NULL, NULL);
}
static void reply_error(const dm_msg *m, const char *name, const char *text){
    if(m->flags & DM_NO_REPLY) return;
    dm_buf b; dm_buf_init(&b); dm_w_str(&b, text);
    dm_send(&g_c, DM_ERROR, DM_NO_REPLY, m->sender, NULL, NULL, NULL, name, m->serial, "s", &b);
    dm_buf_free(&b);
}

/* a{sv} of a MediaTransport1: its Volume, if any */
static void transport_props(const char *path, dm_rd props){
    dm_val v;
    if(dm_r_dict_find(props, "Volume", &v) && v.type == 'q') link_volume(path, (int)v.u);
}
/* GetManagedObjects: a{oa{sa{sv}}} */
static void objects_reply(const dm_msg *m){
    dm_rd b = m->body; dm_val objs, o;
    if(!dm_r_expect(&b, 'a', &objs)) return;
    while(dm_r_next(&objs.sub, &o)){
        dm_rd e = o.sub; dm_val path, ifs, ie;
        if(!dm_r_expect(&e, 'o', &path) || !dm_r_expect(&e, 'a', &ifs)) continue;
        while(dm_r_next(&ifs.sub, &ie)){
            dm_rd ier = ie.sub; dm_val name, props;
            if(dm_r_expect(&ier, 's', &name) && dm_r_expect(&ier, 'a', &props) && !strcmp(name.s, TRANSPORT_IF))
                transport_props(path.s, props.sub);
        }
    }
}

static void on_call(const dm_msg *m){
    const char *mem = m->member ? m->member : "";
    if(!m->path || strcmp(m->path, BTAV_PLAYER_PATH)){ reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return; }
    if(m->iface && !strcmp(m->iface, PROPS_IFACE)){
        if(!strcmp(mem, "GetAll")){
            if(m->flags & DM_NO_REPLY) return;
            dm_buf b; dm_buf_init(&b); w_props(&b, 1);
            dm_send(&g_c, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, "a{sv}", &b);
            dm_buf_free(&b);
            return;
        }
        if(!strcmp(mem, "Get")){
            dm_rd r = m->body; dm_val ifn, prop;
            if(!dm_r_expect(&r, 's', &ifn) || !dm_r_expect(&r, 's', &prop)){ reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "ss expected"); return; }
            if(m->flags & DM_NO_REPLY) return;
            dm_buf b; dm_buf_init(&b);
            if(!strcmp(prop.s, "PlaybackStatus")){ dm_w_sig(&b, "s"); dm_w_str(&b, status_str(g_state < 0 ? ENG_STOP : g_state)); }
            else if(!strncmp(prop.s, "Can", 3)){ dm_w_sig(&b, "b"); dm_w_bool(&b, 1); }
            else { dm_buf_free(&b); reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "no such property"); return; }
            dm_send(&g_c, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, "v", &b);
            dm_buf_free(&b);
            return;
        }
        reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "read only");
        return;
    }
    if(m->iface && strcmp(m->iface, MPRIS_PLAYER)){ reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown interface"); return; }
    int op = -1;
    /* Play toggles: the Hi-X25BT sends PLAY for both play and pause (device log 2026-11-16) */
    if(!strcmp(mem, "Play") || !strcmp(mem, "PlayPause")) op = PL_T_TOGGLE;
    else if(!strcmp(mem, "Pause") || !strcmp(mem, "Stop")) op = PL_T_PAUSE;
    else if(!strcmp(mem, "Next")) op = PL_T_NEXT;
    else if(!strcmp(mem, "Previous")) op = PL_T_PREV;
    L("avrcp: button %s from %s%s", mem, m->sender ? m->sender : "?", op < 0 ? " (not used)" : "");
    reply_empty(m);
    if(op >= 0 && g_cb.transport) g_cb.transport(op);
}

static void on_msg(void *ctx, const dm_msg *m){
    (void)ctx;
    if(m->type == DM_CALL){ on_call(m); return; }
    if((m->type == DM_RETURN || m->type == DM_ERROR) && m->reply_serial){
        if(m->reply_serial == g_reg_serial){
            g_reg_serial = 0;
            if(m->type == DM_RETURN){
                g_registered = 1; g_last_err[0] = 0;
                L("avrcp: player registered with BlueZ (%s): headphone buttons and volume reach the player", BTAV_PLAYER_PATH);
                ask_objects();
            } else if(m->errname && !strcmp(m->errname, "org.bluez.Error.AlreadyExists")){
                g_registered = 1;                      /* ours from before (the same connection) */
            } else {
                dm_rd b = m->body; dm_val t;
                int has = dm_r_expect(&b, 's', &t);
                char e[160]; snprintf(e, sizeof e, "%s%s%s", m->errname ? m->errname : "error", has ? " " : "", has ? t.s : "");
                say_err("RegisterPlayer", e);
            }
        } else if(m->reply_serial == g_mo_serial){
            g_mo_serial = 0;
            if(m->type == DM_RETURN) objects_reply(m);
        }
        return;
    }
    if(m->type != DM_SIGNAL || !m->member || !m->iface) return;
    if(!strcmp(m->member, "NameOwnerChanged") && !strcmp(m->iface, "org.freedesktop.DBus")){
        dm_rd b = m->body; dm_val name, old, nw;
        if(!dm_r_expect(&b, 's', &name) || strcmp(name.s, BLUEZ) || !dm_r_expect(&b, 's', &old) || !dm_r_expect(&b, 's', &nw)) return;
        L("avrcp: bluetoothd %s", nw.s[0] ? "started" : "stopped");
        g_registered = 0; g_reg_serial = 0;
        memset(g_links, 0, sizeof g_links);
        g_next_reg = g_now + (nw.s[0] ? 1.0 : RETRY_S);   /* its adapter object needs a moment */
        return;
    }
    if(!strcmp(m->member, "PropertiesChanged") && !strcmp(m->iface, PROPS_IFACE) && m->path){
        dm_rd b = m->body; dm_val ifn, props;
        if(!dm_r_expect(&b, 's', &ifn) || strcmp(ifn.s, TRANSPORT_IF) || !dm_r_expect(&b, 'a', &props)) return;
        transport_props(m->path, props.sub);
        return;
    }
    if(!strcmp(m->member, "InterfacesRemoved")){
        dm_rd b = m->body; dm_val path, ifs, n;
        if(!dm_r_expect(&b, 'o', &path) || !dm_r_expect(&b, 'a', &ifs)) return;
        while(dm_r_next(&ifs.sub, &n)) if(n.type == 's' && !strcmp(n.s, TRANSPORT_IF)) link_forget(path.s);
    }
}

/* ---- API ---- */
void btav_init(const btav_cb *cb, const char *bus){
    g_cb = *cb; g_bus = bus; g_on = 1;
    g_c.fd = -1;
    g_next_conn = 0; g_next_reg = 0;
}
int btav_fd(void){ return g_c.fd; }
int btav_registered(void){ return g_registered; }

void btav_io(void){
    if(g_c.fd < 0) return;
    if(dm_read_msgs(&g_c, on_msg, NULL) < 0) drop("the bus hung up");
}

void btav_tick(double now){
    g_now = now;
    if(!g_on) return;
    if(g_c.fd < 0){
        if(now < g_next_conn) return;
        int r = dm_connect(&g_c, g_bus, 500);
        if(r){ g_next_conn = now + RETRY_S; say_err("system bus", strerror(-r)); return; }
        L("avrcp: on the system bus as %s", g_c.unique);
        g_last_err[0] = 0;
        dm_add_match(&g_c, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                           "member='NameOwnerChanged',arg0='org.bluez'");
        dm_add_match(&g_c, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
                           "member='PropertiesChanged',arg0='org.bluez.MediaTransport1'");
        dm_add_match(&g_c, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',"
                           "member='InterfacesRemoved'");
        g_next_reg = now;
    }
    if(!g_registered && !g_reg_serial && now >= g_next_reg) do_register();
    else if(g_reg_serial && now >= g_next_reg + RETRY_S){ g_reg_serial = 0; say_err("RegisterPlayer", "no answer"); }
}

void btav_status(int state, const char *title, const char *artist, long dur){
    if(!title) title = "";
    if(!artist) artist = "";
    int st = state != g_state, tr = strcmp(title, g_title) || strcmp(artist, g_artist) || dur != g_dur;
    if(!st && !tr) return;
    g_state = state;
    snprintf(g_title, sizeof g_title, "%s", title);
    snprintf(g_artist, sizeof g_artist, "%s", artist);
    g_dur = dur;
    if(!g_registered || g_c.fd < 0) return;              /* the registration carries the current values */
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, MPRIS_PLAYER);
    size_t a = dm_w_array_begin(&b, 8);
    if(st) dm_w_dict_sv_str(&b, "PlaybackStatus", status_str(state));
    if(tr) w_metadata(&b);
    dm_w_array_end(&b, a);
    size_t inv = dm_w_array_begin(&b, 4); dm_w_array_end(&b, inv);
    if(!dm_send(&g_c, DM_SIGNAL, DM_NO_REPLY, NULL, BTAV_PLAYER_PATH, PROPS_IFACE, "PropertiesChanged", NULL, 0, "sa{sv}as", &b))
        drop("send PropertiesChanged failed");
    dm_buf_free(&b);
}

static int pcm_path(const char *mac, char *path, size_t cap){
    if(!mac || strlen(mac) != 17) return -1;
    char d[18];
    for(int i = 0; i < 17; i++) d[i] = mac[i] == ':' ? '_' : (mac[i] >= 'a' && mac[i] <= 'f') ? (char)(mac[i] - 32) : mac[i];
    d[17] = 0;
    snprintf(path, cap, "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", d);
    return 0;
}
int btav_pcm_volume(const char *mac, int v){
    char path[96];
    if(g_c.fd < 0 || pcm_path(mac, path, sizeof path)) return -1;
    if(v < 0) v = 0;
    if(v > 127) v = 127;
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, "org.bluealsa.PCM1"); dm_w_str(&b, "Volume"); dm_w_sig(&b, "q"); dm_w_u16(&b, (uint16_t)(v << 8 | v));
    uint32_t s = dm_call(&g_c, "org.bluealsa", path, PROPS_IFACE, "Set", "ssv", &b);
    dm_buf_free(&b);
    return s ? 0 : -1;
}
int btav_soft_volume(const char *mac, int on){
    char path[96];
    if(g_c.fd < 0 || pcm_path(mac, path, sizeof path)) return -1;
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, "org.bluealsa.PCM1"); dm_w_str(&b, "SoftVolume"); dm_w_sig(&b, "b"); dm_w_bool(&b, on);
    uint32_t s = dm_call(&g_c, "org.bluealsa", path, PROPS_IFACE, "Set", "ssv", &b);
    dm_buf_free(&b);
    L("avrcp: %s SoftVolume %s (%s)", path, on ? "on" : "off: the volume goes to the headphones", s ? "sent" : "send failed");
    return s ? 0 : -1;
}

void btav_close(void){
    if(g_c.fd >= 0 && g_registered){
        dm_buf b; dm_buf_init(&b); dm_w_str(&b, BTAV_PLAYER_PATH);
        dm_send(&g_c, DM_CALL, DM_NO_REPLY, BLUEZ, "/org/bluez/hci0", "org.bluez.Media1", "UnregisterPlayer", NULL, 0, "o", &b);
        dm_buf_free(&b);
    }
    dm_close(&g_c);
    g_registered = 0;
}
