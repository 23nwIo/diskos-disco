/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef WIFI_SCRIPT_H
#define WIFI_SCRIPT_H
/* diskOS's own Wi-Fi start script and the Disc's DHCP hostname (Jo, 2026-11-23).
 *
 * Stock /usr/bin/wifi_up.sh (read-only squashfs) starts udhcpc together with wpa_supplicant, with BusyBox's defaults
 * (3 discovers 3 s apart, then a 20 s pause) and the hostname "FIIO_" (empty $product_model on the Disc). So mq_ui
 * carries its own script (WSC_SCRIPT below), writes it to WSC_SRC when the bytes differ (once per mq_ui version) and
 * binds it over the stock script at every start (wd_override.h wdo_apply: verify, self-test with --help, bind). Every
 * Disc then gets the same DHCP timing from an update, and only the name differs per Disc:
 *   - /usr/data/diskos-hostname, one line (letters, digits, '-', at most 63, not starting or ending with '-');
 *   - else "Disc-" + the last four hex digits of the Wi-Fi MAC, so two Discs never share a name.
 * mq_ui resolves the name into WSC_NAME_TMP (tmpfs) before each Wi-Fi start; the script reads it from there.
 *
 * Moving from the user copy of the stock script (/usr/data/wifi_up.sh, wifi_override.h, used since 7f0adba): once,
 * its "-x hostname:NAME" becomes /usr/data/diskos-hostname (unless that file exists) and the copy is renamed to
 * wifi_up.sh.old. Who wants a script of their own creates /usr/data/diskos-wifi-custom: then /usr/data/wifi_up.sh is
 * bound as before and nothing is migrated.
 * The name helpers are pure and header-only, for ui/tests/wifi_script_test.c. */
#include <stdio.h>
#include <string.h>

#define WSC_SRC       "/usr/data/diskos-wifi_up.sh"   /* the embedded script, written by mq_ui */
#define WSC_DST       "/usr/bin/wifi_up.sh"
#define WSC_CUSTOM    "/usr/data/diskos-wifi-custom"  /* exists: the user's /usr/data/wifi_up.sh instead */
#define WSC_USER      "/usr/data/wifi_up.sh"
#define WSC_USER_OLD  "/usr/data/wifi_up.sh.old"
#define WSC_NAME_FILE "/usr/data/diskos-hostname"
#define WSC_NAME_TMP  "/tmp/diskos-hostname"
#define WSC_MAC_FILE  "/sys/class/net/wlan0/address"
#define WSC_NAME_MAX  63

static const char WSC_SCRIPT[] =
    "#!/bin/sh\n"
    "# diskOS Wi-Fi start. mq_ui writes this file and binds it over /usr/bin/wifi_up.sh at every start, so edits here\n"
    "# are lost. DHCP hostname: /usr/data/diskos-hostname (one line), default Disc-<end of the Wi-Fi MAC>; mq_ui puts\n"
    "# the name in /tmp/diskos-hostname. A script of your own: create /usr/data/diskos-wifi-custom and use\n"
    "# /usr/data/wifi_up.sh.\n"
    "[ \"$1\" = \"--help\" ] && exit 0\n"
    "name=Disc\n"
    "[ -r /tmp/diskos-hostname ] && read -r name < /tmp/diskos-hostname\n"
    "rfkill unblock wifi\n"
    "ifconfig wlan0 up\n"
    "wpa_supplicant -B -i wlan0 -c /usr/data/wpa_supplicant.conf\n"
    "# DHCP once the supplicant runs. Joining the network can take longer than BusyBox's default 3 x 3 s, after which\n"
    "# it waits 20 s: ask 10 x every 2 s instead, and again after 3 s.\n"
    "udhcpc -i wlan0 -x hostname:\"$name\" -t 10 -T 2 -A 3 &\n";

/* A valid hostname from the first line of `in` (surrounding spaces, tabs and CR ignored) into out[cap]: 1, else 0. */
static inline int wsc_name_clean(const char *in, char *out, size_t cap){
    if(!in || cap < 2) return 0;
    while(*in == ' ' || *in == '\t') in++;
    size_t n = 0;
    while(in[n] && in[n] != '\n' && in[n] != '\r') n++;
    while(n && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if(n == 0 || n > WSC_NAME_MAX || n >= cap || in[0] == '-' || in[n - 1] == '-') return 0;
    for(size_t i = 0; i < n; i++){
        char c = in[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    memcpy(out, in, n); out[n] = 0;
    return 1;
}

/* "Disc-" + the last four hex digits of a MAC "aa:bb:cc:dd:ee:ff" (upper case); "Disc" when it is not one. */
static inline void wsc_name_from_mac(const char *mac, char *out, size_t cap){
    char h[13]; int k = 0, ok = mac != NULL;
    for(int i = 0; ok && i < 17; i++){
        char c = mac[i];
        if(i % 3 == 2){ if(c != ':') ok = 0; continue; }
        if(c >= 'a' && c <= 'f') c = (char)(c - 32);
        if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) ok = 0; else h[k++] = c;
    }
    if(ok && mac[17] != 0 && mac[17] != '\n') ok = 0;
    if(ok){ h[12] = 0; snprintf(out, cap, "Disc-%s", h + 8); }
    else snprintf(out, cap, "Disc");
}

/* The name a script passes as "-x hostname:NAME" on a udhcpc line that is not a comment: 1 and out, else 0 (none, or
 * one built from a variable like the stock FIIO_$product_model). */
static inline int wsc_name_from_script(const char *text, char *out, size_t cap){
    const char *l = text;
    while(l && *l){
        const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
        const char *p = l; while(p < l + n && (*p == ' ' || *p == '\t')) p++;
        if(p < l + n && *p != '#'){
            char line[512]; size_t m = n < sizeof line - 1 ? n : sizeof line - 1;
            memcpy(line, l, m); line[m] = 0;
            char *h = strstr(line, "hostname:");
            if(strstr(line, "udhcpc") && h){
                h += 9;
                if(*h == '"' || *h == '\'') h++;
                size_t k = 0; char v[WSC_NAME_MAX + 2];
                while(h[k] && h[k] != ' ' && h[k] != '\t' && h[k] != '&' && h[k] != '"' && h[k] != '\'' && h[k] != ';'
                      && k < sizeof v - 1){ v[k] = h[k]; k++; }
                v[k] = 0;
                if(wsc_name_clean(v, out, cap)) return 1;
            }
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}

/* Read up to cap-1 bytes of a small file into buf: 1 when something was read. */
static inline int wsc_read(const char *path, char *buf, size_t cap){
    FILE *f = fopen(path, "r"); if(!f){ buf[0] = 0; return 0; }
    size_t n = fread(buf, 1, cap - 1, f); fclose(f);
    buf[n] = 0;
    return n > 0;
}

/* The Disc's hostname: the name file, else from the MAC (files given, so tests can point elsewhere). */
static inline void wsc_resolve(const char *name_file, const char *mac_file, char *out, size_t cap){
    char b[256];
    if(wsc_read(name_file, b, sizeof b) && wsc_name_clean(b, out, cap)) return;
    wsc_read(mac_file, b, sizeof b);
    wsc_name_from_mac(b, out, cap);
}

#endif
