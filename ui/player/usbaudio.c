/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "usbaudio.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>  /* struct timespec, needed by sound/asound.h */
#include <sound/asound.h>
#ifndef SNDRV_CTL_TLVT_DB_SCALE
#define SNDRV_CTL_TLVT_DB_SCALE 1      /* <sound/tlv.h>, not in every kernel header set */
#endif
#include <unistd.h>

char ua_root[256];

static void path_(char *p, unsigned cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void path_(char *p, unsigned cap, const char *fmt, ...){
    int n = snprintf(p, cap, "%s", ua_root);
    if(n < 0 || (unsigned)n >= cap){ p[0] = 0; return; }
    va_list ap; va_start(ap, fmt); vsnprintf(p + n, cap - (unsigned)n, fmt, ap); va_end(ap);
}
/* the first line of a small file, without the newline and trailing blanks; -1 when it can't be read */
static int read_line(const char *p, char *out, unsigned cap){
    FILE *f = fopen(p, "re");
    if(!f) return -1;
    char *r = fgets(out, (int)cap, f);
    fclose(f);
    if(!r){ out[0] = 0; return 0; }
    size_t n = strlen(out);
    while(n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
    return 0;
}

/* ---- the host role ---- */
int ua_data_role_parse(const char *s){
    if(!s) return -1;
    while(*s == ' ') s++;
    return !strncmp(s, "DFP", 3) ? 1 : 0;
}
static int g_we_host;              /* we wrote "host" and have not taken it back */
static int g_role_warned;
static int put_role(const char *p, const char *val, ua_log_fn log){
    FILE *f = fopen(p, "we");
    if(!f){ log("usb host: open %s: %s", p, strerror(errno)); return -1; }
    int ok = fputs(val, f) >= 0, e = errno;
    if(fclose(f) != 0){ ok = 0; e = errno; }
    if(!ok){ log("usb host: write \"%s\" to %s: %s", val, p, strerror(e)); return -1; }
    return 0;
}
int ua_role_tick(int storage, ua_log_fn log){
    char rp[320], dp[320], role[32], data[32];
    path_(rp, sizeof rp, "%s", UA_ROLE);
    path_(dp, sizeof dp, "%s", UA_DATA_ROLE);
    if(read_line(rp, role, sizeof role) != 0 || read_line(dp, data, sizeof data) != 0){
        if(!g_role_warned){ log("usb host: %s or %s: %s - no USB host mode", rp, dp, strerror(errno)); g_role_warned = 1; }
        return -1;
    }
    g_role_warned = 0;
    if(storage){ g_we_host = 0; return 0; }               /* USB storage mode owns the port ("device") */
    int dfp = ua_data_role_parse(data) == 1;
    if(dfp && strcmp(role, "host") != 0){
        if(put_role(rp, "host", log) == 0){ log("usb host: data role %s - role %s -> host", data, role); g_we_host = 1; return 1; }
        return 0;
    }
    if(dfp){ g_we_host = 1; return 1; }                    /* host already (ours, or set by hand) */
    if(g_we_host && !strcmp(role, "host")){
        g_we_host = 0;
        if(put_role(rp, "none", log) == 0){ log("usb host: data role %s - role host -> none", data[0] ? data : "(empty)"); return 0; }
    }
    return !strcmp(role, "host");
}

/* ---- the cards ---- */
void ua_make_key(char *key, unsigned cap, const char *usbid, const char *serial){
    char s[64]; unsigned k = 0;
    for(const char *p = serial ? serial : ""; *p && k + 1 < sizeof s; p++)   /* the key is one word in the store */
        s[k++] = isgraph((unsigned char)*p) && *p != '=' ? *p : '_';
    s[k] = 0;
    if(s[0]) snprintf(key, cap, "usb:%s:%s", usbid, s);
    else snprintf(key, cap, "usb:%s", usbid);
}
static int valid_usbid(const char *s){
    if(strlen(s) != 9 || s[4] != ':') return 0;
    for(int i = 0; i < 9; i++) if(i != 4 && !isxdigit((unsigned char)s[i])) return 0;
    return 1;
}
static int cmp_card(const void *a, const void *b){ return ((const ua_card_t *)a)->card - ((const ua_card_t *)b)->card; }
int ua_scan(ua_card_t *out, int max){
    char p[400];
    path_(p, sizeof p, "/proc/asound");
    DIR *d = opendir(p);
    if(!d) return 0;
    int n = 0;
    struct dirent *e;
    while((e = readdir(d)) && n < max){
        if(strncmp(e->d_name, "card", 4) || !isdigit((unsigned char)e->d_name[4])) continue;
        char *end; long num = strtol(e->d_name + 4, &end, 10);
        if(*end || num < 0 || num > 31) continue;
        ua_card_t c; memset(&c, 0, sizeof c);
        c.card = (int)num; c.pcm = -1;
        path_(p, sizeof p, "/proc/asound/card%d/usbid", c.card);
        if(read_line(p, c.usbid, sizeof c.usbid) != 0 || !valid_usbid(c.usbid)) continue;   /* not a USB card */
        for(int dev = 0; dev < 8 && c.pcm < 0; dev++){                /* a playback PCM: pcm<N>p */
            path_(p, sizeof p, "/proc/asound/card%d/pcm%dp", c.card, dev);
            if(access(p, F_OK) == 0) c.pcm = dev;
        }
        if(c.pcm < 0) continue;                                         /* a microphone only */
        path_(p, sizeof p, "/proc/asound/card%d/id", c.card);
        read_line(p, c.id, sizeof c.id);
        /* the USB device: the card's device link is its audio interface; the device is one up */
        path_(p, sizeof p, "/sys/class/sound/card%d/device/../serial", c.card);
        if(read_line(p, c.serial, sizeof c.serial) != 0) c.serial[0] = 0;
        path_(p, sizeof p, "/sys/class/sound/card%d/device/../product", c.card);
        if(read_line(p, c.name, sizeof c.name) != 0 || !c.name[0]) snprintf(c.name, sizeof c.name, "%s", c.id[0] ? c.id : "USB audio");
        ua_make_key(c.key, sizeof c.key, c.usbid, c.serial);
        out[n++] = c;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_card);
    return n;
}

/* ---- the hardware volume ---- */
int ua_mixer_rank(const char *name){
    static const char SUF[] = " Playback Volume";
    size_t n = strlen(name), s = sizeof SUF - 1;
    if(n <= s || strcmp(name + n - s, SUF)) return 0;
    static const char *const SKIP[] = { "Mic", "Line", "Aux", "CD", "Capture", "Input", "Phone", "Video", "Sidetone", NULL };
    for(int i = 0; SKIP[i]; i++) if(!strncmp(name, SKIP[i], strlen(SKIP[i]))) return 0;   /* an input's monitor */
    static const char *const PREF[] = { "Master", "PCM", "Speaker", "Headphone", NULL };
    for(int i = 0; PREF[i]; i++) if(!strncmp(name, PREF[i], strlen(PREF[i])) && name[strlen(PREF[i])] == ' ') return 10 - i;
    return 1;
}
static int ctl_open(int card, ua_log_fn log){
    char p[320];
    path_(p, sizeof p, "/dev/snd/controlC%d", card);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if(fd < 0 && log) log("usb audio: open %s: %s", p, strerror(errno));
    return fd;
}
int ua_mixer_find(int card, ua_mixer_t *m, ua_log_fn log){
    memset(m, 0, sizeof *m);
    int fd = ctl_open(card, log);
    if(fd < 0) return -1;
    struct snd_ctl_elem_list l; memset(&l, 0, sizeof l);
    int rc = -1;
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) != 0){ log("usb audio: card %d: list controls: %s", card, strerror(errno)); goto out; }
    unsigned cnt = l.count > 256 ? 256 : l.count;
    struct snd_ctl_elem_id *ids = calloc(cnt ? cnt : 1, sizeof *ids);
    if(!ids) goto out;
    l.space = cnt; l.pids = ids; l.offset = 0;
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) != 0){ log("usb audio: card %d: list controls: %s", card, strerror(errno)); free(ids); goto out; }
    int best = 0;
    for(unsigned i = 0; i < l.used; i++){
        if(ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER) continue;
        int r = ua_mixer_rank((const char *)ids[i].name);
        if(r <= best) continue;
        struct snd_ctl_elem_info in; memset(&in, 0, sizeof in);
        in.id.numid = ids[i].numid;
        if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) != 0 || in.type != SNDRV_CTL_ELEM_TYPE_INTEGER) continue;
        if(in.value.integer.max <= in.value.integer.min || !(in.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) continue;
        best = r;
        m->ok = 1; m->vol_numid = ids[i].numid; m->channels = (int)in.count;
        m->min = in.value.integer.min; m->max = in.value.integer.max;
        snprintf(m->name, sizeof m->name, "%s", (const char *)ids[i].name);
    }
    if(m->ok){                                      /* its switch: the same name with "Switch" for "Volume" */
        char sw[44]; size_t k = strlen(m->name) - strlen("Volume");
        snprintf(sw, sizeof sw, "%.*sSwitch", (int)k, m->name);
        for(unsigned i = 0; i < l.used; i++)
            if(ids[i].iface == SNDRV_CTL_ELEM_IFACE_MIXER && !strcmp((const char *)ids[i].name, sw)) m->sw_numid = ids[i].numid;
        log("usb audio: card %d: volume \"%s\" %ld..%ld, %d channel(s)%s", card, m->name, m->min, m->max, m->channels,
            m->sw_numid ? ", with its switch" : "");
        rc = 0;
    } else log("usb audio: card %d: no playback volume control", card);
    free(ids);
out:
    close(fd);
    return rc;
}
int ua_softvol_prepare(int card, ua_log_fn log){
    int fd = ctl_open(card, log);
    if(fd < 0) return -1;
    int rc = -1;
    struct snd_ctl_elem_info in; memset(&in, 0, sizeof in);
    in.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)in.id.name, sizeof in.id.name, "%s", UA_SOFTVOL_NAME);
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) == 0){
        if(in.type == SNDRV_CTL_ELEM_TYPE_INTEGER && in.count == 2 && in.value.integer.min == 0 && in.value.integer.max == UA_SOFTVOL_MAX
           && (in.access & SNDRV_CTL_ELEM_ACCESS_TLV_READ)){ rc = 0; goto out; }   /* ours from before: kept */
        struct snd_ctl_elem_id id = in.id;
        if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_REMOVE, &id) != 0){ log("usb audio: card %d: remove \"%s\": %s", card, UA_SOFTVOL_NAME, strerror(errno)); goto out; }
    }
    memset(&in, 0, sizeof in);
    in.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    snprintf((char *)in.id.name, sizeof in.id.name, "%s", UA_SOFTVOL_NAME);
    in.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    in.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE;
    in.count = 2;
    in.value.integer.min = 0; in.value.integer.max = UA_SOFTVOL_MAX; in.value.integer.step = 0;
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_ADD, &in) != 0){ log("usb audio: card %d: add \"%s\": %s", card, UA_SOFTVOL_NAME, strerror(errno)); goto out; }
    /* the dB scale as softvol writes it (add_tlv_info): min, step | mute */
    struct { struct snd_ctl_tlv h; unsigned int v[4]; } t; memset(&t, 0, sizeof t);
    t.h.numid = in.id.numid; t.h.length = sizeof t.v;
    t.v[0] = SNDRV_CTL_TLVT_DB_SCALE; t.v[1] = 2 * sizeof(int);
    t.v[2] = (unsigned int)(int)(UA_SOFTVOL_MIN_DB * 100);
    t.v[3] = (unsigned int)(int)((0.0 - UA_SOFTVOL_MIN_DB) * 100 / UA_SOFTVOL_MAX);
    if(ioctl(fd, SNDRV_CTL_IOCTL_TLV_WRITE, &t) != 0) log("usb audio: card %d: dB scale of \"%s\": %s", card, UA_SOFTVOL_NAME, strerror(errno));
    struct snd_ctl_elem_value v; memset(&v, 0, sizeof v);
    v.id.numid = in.id.numid;                              /* silence until the player sets the level */
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) != 0){ log("usb audio: card %d: \"%s\" = 0: %s", card, UA_SOFTVOL_NAME, strerror(errno)); goto out; }
    log("usb audio: card %d: no volume control of its own - \"%s\" (softvol) made, at silence", card, UA_SOFTVOL_NAME);
    rc = 0;
out:
    close(fd);
    return rc;
}
long ua_mixer_raw(const ua_mixer_t *m, int volume, int scale){
    if(scale <= 0) return m->min;
    if(volume < 0) volume = 0;
    if(volume > scale) volume = scale;
    return m->min + ((m->max - m->min) * (long)volume + scale / 2) / scale;
}
int ua_mixer_set(int card, const ua_mixer_t *m, int volume, int scale, ua_log_fn log){
    if(!m->ok) return -ENOENT;
    int fd = ctl_open(card, log);
    if(fd < 0) return -errno;
    int rc = 0;
    struct snd_ctl_elem_value v; memset(&v, 0, sizeof v);
    v.id.numid = m->vol_numid;
    long raw = ua_mixer_raw(m, volume, scale);
    for(int i = 0; i < m->channels && i < 128; i++) v.value.integer.value[i] = raw;
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0){ rc = -errno; log("usb audio: card %d: %s = %ld: %s", card, m->name, raw, strerror(-rc)); }
    if(m->sw_numid){
        memset(&v, 0, sizeof v);
        v.id.numid = m->sw_numid;
        struct snd_ctl_elem_info in; memset(&in, 0, sizeof in);
        in.id.numid = m->sw_numid;
        int n = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) == 0 ? (int)in.count : 1;
        for(int i = 0; i < n && i < 128; i++) v.value.integer.value[i] = volume > 0;
        if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0 && !rc){ rc = -errno; log("usb audio: card %d: switch: %s", card, strerror(-rc)); }
    }
    close(fd);
    return rc;
}

/* ---- MPD's view ---- */
int ua_alsa_conf_write(const char *path, int card, int pcm, int soft, ua_log_fn log){
    char tmp[320], b[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if(card >= 0 && soft)
        snprintf(b, sizeof b, "# written by diskos-player (usbaudio.c): the USB sound card MPD plays to, its volume by softvol\n"
                              "pcm.diskos_usb {\n  type softvol\n  slave.pcm {\n    type hw\n    card %d\n    device %d\n  }\n"
                              "  control {\n    name \"%s\"\n    card %d\n    count 2\n  }\n  min_dB %.1f\n  max_dB 0.0\n  resolution %d\n}\n",
                 card, pcm < 0 ? 0 : pcm, UA_SOFTVOL_NAME, card, UA_SOFTVOL_MIN_DB, UA_SOFTVOL_MAX + 1);
    else if(card >= 0) snprintf(b, sizeof b, "# written by diskos-player (usbaudio.c): the USB sound card MPD plays to\n"
                                        "pcm.diskos_usb {\n  type hw\n  card %d\n  device %d\n}\n", card, pcm < 0 ? 0 : pcm);
    else snprintf(b, sizeof b, "# written by diskos-player (usbaudio.c): no USB sound card\npcm.diskos_usb {\n  type null\n}\n");
    FILE *f = fopen(tmp, "we");
    if(!f){ log("usb audio: write %s: %s", tmp, strerror(errno)); return -1; }
    int ok = fputs(b, f) >= 0;
    if(fclose(f) != 0) ok = 0;
    if(!ok){ log("usb audio: write %s: %s", tmp, strerror(errno)); return -1; }
    if(rename(tmp, path) != 0){ log("usb audio: rename %s: %s", tmp, strerror(errno)); return -1; }   /* a new inode: alsa-lib reads it again */
    return 0;
}
