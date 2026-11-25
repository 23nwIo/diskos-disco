/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* usbaudio.c against a fake /proc + /sys tree (ua_root): the host role, the card scan, keys, the volume mapping and
 * the ALSA file for MPD. */
#include "../player/usbaudio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(x) do{ if(!(x)){ printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } }while(0)
static void lg(const char *fmt, ...){ va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }

static void put(const char *rel, const char *text){
    char p[512], cmd[600];
    snprintf(p, sizeof p, "%s%s", ua_root, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname '%s')\"", p);
    if(system(cmd) != 0) printf("mkdir failed for %s\n", p);
    FILE *f = fopen(p, "w");
    if(!f){ printf("can't write %s\n", p); fails++; return; }
    fputs(text, f); fclose(f);
}
static void get(const char *rel, char *out, unsigned cap){
    char p[512]; snprintf(p, sizeof p, "%s%s", ua_root, rel);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if(!f) return;
    size_t n = fread(out, 1, cap - 1, f); out[n] = 0; fclose(f);
}

int main(void){
    char tmpl[] = "/tmp/usbaudio-test-XXXXXX";
    if(!mkdtemp(tmpl)){ perror("mkdtemp"); return 1; }
    snprintf(ua_root, sizeof ua_root, "%s", tmpl);
    char b[512];

    /* the data role */
    CHECK(ua_data_role_parse(NULL) == -1 && ua_data_role_parse("DFP\n") == 1 && ua_data_role_parse("UFP") == 0 && ua_data_role_parse("") == 0);

    /* the host role: missing nodes, DFP -> host, UFP after ours -> none, storage leaves it alone, host by hand kept */
    CHECK(ua_role_tick(0, lg) == -1);
    put(UA_ROLE, "none\n"); put(UA_DATA_ROLE, "UFP\n");
    CHECK(ua_role_tick(0, lg) == 0); get(UA_ROLE, b, sizeof b); CHECK(!strcmp(b, "none\n"));
    put(UA_DATA_ROLE, "DFP\n");
    CHECK(ua_role_tick(0, lg) == 1); get(UA_ROLE, b, sizeof b); CHECK(!strcmp(b, "host"));
    CHECK(ua_role_tick(0, lg) == 1);
    put(UA_DATA_ROLE, "UFP\n");
    CHECK(ua_role_tick(0, lg) == 0); get(UA_ROLE, b, sizeof b); CHECK(!strcmp(b, "none"));
    put(UA_ROLE, "device\n"); put(UA_DATA_ROLE, "DFP\n");
    CHECK(ua_role_tick(1, lg) == 0); get(UA_ROLE, b, sizeof b); CHECK(!strcmp(b, "device\n"));   /* USB storage mode */
    put(UA_ROLE, "host\n"); put(UA_DATA_ROLE, "UFP\n");
    CHECK(ua_role_tick(0, lg) == 1); get(UA_ROLE, b, sizeof b); CHECK(!strcmp(b, "host\n"));     /* not ours: kept */

    /* keys */
    ua_make_key(b, sizeof b, "0d8c:0014", ""); CHECK(!strcmp(b, "usb:0d8c:0014"));
    ua_make_key(b, sizeof b, "0d8c:0014", "AB 12=x"); CHECK(!strcmp(b, "usb:0d8c:0014:AB_12_x"));

    /* the scan: card 0 is the Disc's own (no usbid), card 1 a C-Media without a serial, card 2 a DAC with one,
     * card 3 a USB microphone (no playback PCM), card 4 a broken usbid */
    ua_card_t c[UA_MAX_CARDS];
    CHECK(ua_scan(c, UA_MAX_CARDS) == 0);
    put("/proc/asound/card0/id", "Disc\n"); put("/proc/asound/card0/pcm3p/info", "x");
    put("/proc/asound/card1/id", "Device\n"); put("/proc/asound/card1/usbid", "0d8c:0014\n"); put("/proc/asound/card1/pcm0p/info", "x");
    put("/sys/class/sound/card1/device/x", "");   /* a directory "device" (the tests can't rely on symlinks) */
    put("/sys/class/sound/card1/product", "USB Audio Device\n");
    put("/proc/asound/card2/id", "DAC\n"); put("/proc/asound/card2/usbid", "20b1:3008\n"); put("/proc/asound/card2/pcm1p/info", "x");
    put("/sys/class/sound/card2/device/x", "");
    put("/sys/class/sound/card2/serial", "SN42\n"); put("/sys/class/sound/card2/product", "Hi-Fi DAC\n");
    put("/proc/asound/card3/id", "Mic\n"); put("/proc/asound/card3/usbid", "1111:2222\n"); put("/proc/asound/card3/pcm0c/info", "x");
    put("/proc/asound/card4/usbid", "zz\n"); put("/proc/asound/card4/pcm0p/info", "x");
    int n = ua_scan(c, UA_MAX_CARDS);
    CHECK(n == 2);
    CHECK(c[0].card == 1 && c[0].pcm == 0 && !strcmp(c[0].usbid, "0d8c:0014") && !c[0].serial[0] && !strcmp(c[0].key, "usb:0d8c:0014"));
    CHECK(!strcmp(c[0].name, "USB Audio Device") && !strcmp(c[0].id, "Device"));
    CHECK(c[1].card == 2 && c[1].pcm == 1 && !strcmp(c[1].serial, "SN42") && !strcmp(c[1].key, "usb:20b1:3008:SN42") && !strcmp(c[1].name, "Hi-Fi DAC"));
    CHECK(ua_scan(c, 1) == 1);

    /* the volume control: names and the mapping */
    CHECK(ua_mixer_rank("Speaker Playback Volume") > ua_mixer_rank("Headphone Playback Volume"));
    CHECK(ua_mixer_rank("Master Playback Volume") > ua_mixer_rank("PCM Playback Volume") && ua_mixer_rank("PCM Playback Volume") > ua_mixer_rank("Speaker Playback Volume"));
    CHECK(ua_mixer_rank("Mic Playback Volume") == 0 && ua_mixer_rank("Line Playback Volume") == 0 && ua_mixer_rank("Speaker Playback Switch") == 0);
    CHECK(ua_mixer_rank("Mic Capture Volume") == 0 && ua_mixer_rank("Something Playback Volume") == 1);
    ua_mixer_t m; memset(&m, 0, sizeof m); m.ok = 1; m.min = 0; m.max = 37; m.channels = 2;
    CHECK(ua_mixer_raw(&m, 0, 120) == 0 && ua_mixer_raw(&m, 120, 120) == 37 && ua_mixer_raw(&m, 60, 120) == 19);
    CHECK(ua_mixer_raw(&m, -5, 120) == 0 && ua_mixer_raw(&m, 500, 120) == 37);
    m.min = -10240; m.max = 0;
    CHECK(ua_mixer_raw(&m, 0, 120) == -10240 && ua_mixer_raw(&m, 120, 120) == 0);
    memset(&m, 0, sizeof m);
    CHECK(ua_mixer_set(1, &m, 50, 120, lg) < 0);                  /* no control: refused */
    CHECK(ua_mixer_find(9, &m, lg) == -1 && !m.ok);                /* no control device */

    /* MPD's ALSA file */
    char conf[400]; snprintf(conf, sizeof conf, "%s/asound.conf", ua_root);
    CHECK(ua_alsa_conf_write(conf, 2, 1, lg) == 0);
    get("/asound.conf", b, sizeof b); CHECK(strstr(b, "pcm.diskos_usb {\n  type hw\n  card 2\n  device 1\n}\n") != NULL);
    CHECK(ua_alsa_conf_write(conf, -1, 0, lg) == 0);
    get("/asound.conf", b, sizeof b); CHECK(strstr(b, "type null") != NULL && !strstr(b, "type hw"));
    CHECK(ua_alsa_conf_write("/nonexistent-dir/x.conf", 1, 0, lg) == -1);

    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    if(system(cmd) != 0) printf("cleanup failed\n");
    if(fails){ printf("%d check(s) failed\n", fails); return 1; }
    printf("usbaudio_test: all passed\n");
    return 0;
}
