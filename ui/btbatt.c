/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The Bluetooth headphones' battery (btbatt.h): BlueALSA's org.bluealsa.RFCOMM1.Battery over the system D-Bus
 * (dbusmini.c), with either player. Main thread only; never blocks for long: the bus connection is made at most every
 * BB_RETRY_MS (200 ms timeout), everything else is non-blocking. */
#include "btbatt.h"
#include "dbusmini.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define BB_RETRY_MS  30000         /* bus connection retry */
#define BB_ASK_MS    60000         /* ask again (a missed signal, a second pair of headphones) */
#define BB_MAX       4
/* The hands-free link (HFP), which carries the battery, may be missing: headphones that open only A2DP themselves keep it
 * that way, and the Disc's bluetoothd (5.54, --plugin=a2dp,avrcp: no policy plugin, and 5.54's policy would not add HFP
 * after A2DP either) never adds it. Device check 2026-11-23 (Jo, two Hi-X25BT): the sessions without battery had
 * "Adding new Stream End-Point" and no RFCOMM thread in /tmp/bluealsa.log; those with battery opened HFP first. So when
 * the level is not known BB_HFP_WAIT_MS after the headphones appeared, ask BlueZ for the hands-free link
 * (Device1.ConnectProfile, the remote's Handsfree UUID), at most BB_HFP_TRIES times per connection, and ask for the
 * level again BB_SOON_MS later. */
#ifndef BB_HFP_WAIT_MS            /* the tests make these short */
#define BB_HFP_WAIT_MS 6000
#define BB_HFP_RETRY_MS 20000
#define BB_SOON_MS     5000
#endif
#define BB_HFP_TRIES   2
#define BB_HF_UUID     "0000111e-0000-1000-8000-00805f9b34fb"

static dm_conn g_c = { .fd = -1 };
static long g_next_conn, g_next_ask;
static char g_asked_mac[18];
static uint32_t g_ask_serial;
static struct { char mac[18]; int level; } g_lv[BB_MAX];
static char g_cur_mac[18];          /* the headphones of the last poll, and since when (for BB_HFP_WAIT_MS) */
static long g_cur_since;
static int g_hfp_tries;
static uint32_t g_hfp_serial;
static void (*g_log)(const char *fmt, ...);

static long ms_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

int btbatt_mac_of_path(const char *path, char *mac, int cap){
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

static void set_level(const char *mac, int level){
    int i, free_i = -1;
    for(i = 0; i < BB_MAX; i++){
        if(g_lv[i].mac[0] && !strcasecmp(g_lv[i].mac, mac)) break;
        if(!g_lv[i].mac[0] && free_i < 0) free_i = i;
    }
    if(i == BB_MAX){ i = free_i >= 0 ? free_i : 0; snprintf(g_lv[i].mac, sizeof g_lv[i].mac, "%s", mac); g_lv[i].level = -1; }
    if(g_lv[i].level == level) return;
    if(g_log) g_log("bt battery: %s %d %%\n", mac, level);
    g_lv[i].level = level;
}

int btbatt_level(const char *mac){
    if(!mac || !mac[0]) return -1;
    for(int i = 0; i < BB_MAX; i++) if(g_lv[i].mac[0] && !strcasecmp(g_lv[i].mac, mac)) return g_lv[i].level;
    return -1;
}

static void on_msg(void *ctx, const dm_msg *m){
    (void)ctx;
    if((m->type == DM_RETURN || m->type == DM_ERROR) && m->reply_serial && m->reply_serial == g_hfp_serial){
        g_hfp_serial = 0;
        if(g_log) g_log("bt battery: hands-free link for %s: %s\n", g_cur_mac,
                        m->type == DM_RETURN ? "connected" : m->errname ? m->errname : "error");
        return;
    }
    if((m->type == DM_RETURN || m->type == DM_ERROR) && m->reply_serial && m->reply_serial == g_ask_serial){
        g_ask_serial = 0;
        dm_rd b = m->body; dm_val v, x;
        if(m->type == DM_RETURN && dm_r_expect(&b, 'v', &v)){
            dm_rd vr = v.sub;
            if(dm_r_expect(&vr, 'y', &x)){ set_level(g_asked_mac, x.u <= 100 ? (int)x.u : -1); g_next_ask = ms_now() + BB_ASK_MS; }
        } else if(m->type == DM_ERROR) set_level(g_asked_mac, -1);   /* no RFCOMM (no hands-free link): not known */
        return;
    }
    if(m->type != DM_SIGNAL || !m->member || strcmp(m->member, "PropertiesChanged") || !m->path) return;
    dm_rd b = m->body; dm_val ifn, props, v;
    if(!dm_r_expect(&b, 's', &ifn) || strcmp(ifn.s, "org.bluealsa.RFCOMM1") || !dm_r_expect(&b, 'a', &props)) return;
    char mac[18];
    if(btbatt_mac_of_path(m->path, mac, sizeof mac) == 0 && dm_r_dict_find(props.sub, "Battery", &v) && v.type == 'y')
        set_level(mac, v.u <= 100 ? (int)v.u : -1);
}

static void mac_dev(const char *mac, char d[18]){
    for(int i = 0; i < 17; i++) d[i] = mac[i] == ':' ? '_' : (mac[i] >= 'a' && mac[i] <= 'f') ? (char)(mac[i] - 32) : mac[i];
    d[17] = 0;
}
static void connect_hfp(const char *mac){
    char path[64], d[18]; mac_dev(mac, d);
    snprintf(path, sizeof path, "/org/bluez/hci0/dev_%s", d);
    dm_buf b; dm_buf_init(&b); dm_w_str(&b, BB_HF_UUID);
    g_hfp_serial = dm_call(&g_c, "org.bluez", path, "org.bluez.Device1", "ConnectProfile", "s", &b);
    dm_buf_free(&b);
    if(g_log) g_log("bt battery: %s: no hands-free link, asking BlueZ for it (%d/%d)\n", mac, g_hfp_tries, BB_HFP_TRIES);
}
static void ask(const char *mac){
    char path[96], d[18]; mac_dev(mac, d);
    snprintf(path, sizeof path, "/org/bluealsa/hci0/dev_%s/rfcomm", d);
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, "org.bluealsa.RFCOMM1"); dm_w_str(&b, "Battery");
    g_ask_serial = dm_call(&g_c, "org.bluealsa", path, "org.freedesktop.DBus.Properties", "Get", "ss", &b);
    dm_buf_free(&b);
    snprintf(g_asked_mac, sizeof g_asked_mac, "%s", mac);
}

void btbatt_poll(const char *mac, void (*log)(const char *fmt, ...)){
    g_log = log;
    long now = ms_now();
    int want = mac && strlen(mac) == 17;
    if(g_c.fd < 0){
        if(!want || now < g_next_conn) return;
        int r = dm_connect(&g_c, NULL, 200);
        if(r){ g_next_conn = now + BB_RETRY_MS; if(log) log("bt battery: system bus: %s\n", strerror(-r)); return; }
        dm_add_match(&g_c, "type='signal',sender='org.bluealsa',interface='org.freedesktop.DBus.Properties',"
                           "member='PropertiesChanged',arg0='org.bluealsa.RFCOMM1'");
        g_next_ask = 0; g_asked_mac[0] = 0;
    }
    if(dm_read_msgs(&g_c, on_msg, NULL) < 0){
        dm_close(&g_c); g_next_conn = now + BB_RETRY_MS; g_ask_serial = 0;
        return;
    }
    if(!want){ g_cur_mac[0] = 0; return; }
    if(strcasecmp(g_cur_mac, mac)){                          /* new headphones (or the same, connected again) */
        snprintf(g_cur_mac, sizeof g_cur_mac, "%s", mac); g_cur_since = now; g_hfp_tries = 0; g_hfp_serial = 0;
        for(int i = 0; i < BB_MAX; i++)                      /* a level from an earlier connection is stale */
            if(g_lv[i].mac[0] && !strcasecmp(g_lv[i].mac, mac)) g_lv[i].level = -1;
        g_asked_mac[0] = 0;                                  /* ask now */
    }
    if(btbatt_level(mac) < 0 && !g_ask_serial && !g_hfp_serial && g_hfp_tries < BB_HFP_TRIES
       && !strcasecmp(g_asked_mac, mac) && now - g_cur_since >= BB_HFP_WAIT_MS + g_hfp_tries * (long)BB_HFP_RETRY_MS){
        g_hfp_tries++;
        connect_hfp(mac);
        g_next_ask = now + BB_SOON_MS;
        return;
    }
    if(strcasecmp(g_asked_mac, mac) || now >= g_next_ask){   /* other headphones, or time to ask again */
        if(g_ask_serial && !strcasecmp(g_asked_mac, mac) && now < g_next_ask + 5000) return;   /* an answer is due */
        ask(mac);
        g_next_ask = now + (btbatt_level(mac) < 0 && g_hfp_tries < BB_HFP_TRIES ? BB_SOON_MS : BB_ASK_MS);
    }
}
