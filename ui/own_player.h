/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Own player switch (docs/MQ_PLAYER_REPLACEMENT.md, development phase). When /usr/data/diskos-own-player exists, the
 * launcher (mq_ui started as /usr/data/mq_player) execs /usr/data/diskos-own/mq_player instead of the stock
 * /usr/bin/mq_player, so the process is named "mq_player" like the stock one (see OPL_BIN). The own player
 * speaks the stock protocol (docs/PLAYER_PROTOCOL.md), so the UI does not change. Without the switch file nothing
 * changes at all.
 *
 * Fail safe, two counters; when either runs out the switch file is renamed to diskos-own-player.failed and the stock
 * player runs from then on:
 *   - every own-player start in this boot is counted in a tmpfs file (fiio_init respawns a dead player through the
 *     launcher, so a player that keeps dying is seen here): at most OPL_MAX_STARTS per boot;
 *   - every start is also counted in /usr/data/diskos-own-player.unstable, which the player deletes once it has run
 *     for a while (OPL_STABLE_S). A player that crashes early, or makes the Disc reboot (it arms the hardware
 *     watchdog like the stock player), leaves it behind: at most OPL_MAX_UNSTABLE such starts in a row, so a broken
 *     player cannot cause an endless reboot loop.
 * A missing or non-ELF binary also means stock (the switch file stays). The exec itself falls back to stock.
 * Header-only so the host test (ui/tests) can drive it with its own paths. Runs inside the launcher's deadline: plain
 * file calls only. */
#ifndef OWN_PLAYER_H
#define OWN_PLAYER_H
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OPL_SWITCH "/usr/data/diskos-own-player"
/* The file itself is named mq_player: the kernel takes a process's name (/proc/<pid>/comm) from the executed file, not
 * from argv[0], and both the UI's launch verdict (launch_status_get, PLAYER_COMM_NAME) and fiio_init's pgrep compare
 * that name. Named diskos-player, the player ran but the UI never accepted the verdict: no card access, "Return to
 * local playback first" (device test 2026-11-15). */
#define OPL_DIR    "/usr/data/diskos-own"
#define OPL_BIN    OPL_DIR "/mq_player"
#define OPL_OLD_BIN "/usr/data/diskos-player"            /* where 0b45afa..52b9909 put it; removed */
#define OPL_STARTS "/tmp/.diskos_own_player_starts"   /* tmpfs: counts this boot only */
#define OPL_UNSTABLE "/usr/data/diskos-own-player.unstable"   /* NAND: survives a reboot the player caused */
#define OPL_MAX_STARTS 3
#define OPL_MAX_UNSTABLE 2
#define OPL_STABLE_S 60     /* the player removes OPL_UNSTABLE after running this long */
/* Outputs (Settings > Audio > Outputs): the own player writes their state here whenever it changes, one "key=value"
 * per line (aux, bt, vol_aux, vol_bt, bt_ready, jack, bal, codec, codecs, switching); the UI reads it. tmpfs. */
#define OPL_OUTPUTS "/tmp/diskos-player-outputs"
#define OPL_VOLUME_LOG "/tmp/diskos-volume.log"
/* Web radio: there = MPD does not check the stations' HTTPS certificates (Settings > Library > Radio Certificates). The
 * UI puts it there or takes it away; the player reads it when it starts MPD and before each station. */
#define OPL_NO_CERT_CHECK OPL_DIR "/radio-no-cert-check"
/* Web radio: the song the station names, as MPD has it (ICY StreamTitle, Ogg comments), for ui/radio.c instead of a
 * second connection to the station. "uri=", "artist=", "title=" lines; only while the own player plays a station. */
#define OPL_STREAM "/tmp/diskos-player-stream"   /* one line per volume change (player/main.c volume_log) */
#define OPL_VOLUMES OPL_DIR "/volumes"          /* kept by the player: "main=N" and "dev KEY off=N cap=N fade=N" per device
                                                 * (aux, dig, bt:MAC, usb:VID:PID:SERIAL; player/core.c pl_core_vol_format) */
/* A stream list (Bandcamp, ui/bandcamp.c): the UI writes the tracks here and sends 0d14 with the track to start at;
 * the player plays them one after another in the radio work mode (playback.c pb_slist_*). tmpfs. One track per line:
 * "<duration ms>\t<stream url>\t<title>\t<artist>\t<album>" (no tabs or newlines in the texts); instead of the URL an
 * absolute path: the track saved on the SD card (BandcampDownloads), played from there. When a track can't be
 * played (the stream links expire), the player stops and sends ad14 with its index; the UI fetches fresh links,
 * writes the list again and sends 0d14 for that track. */
#define OPL_BC_LIST "/tmp/diskos-stream-list"
#define OPL_SLIST_MAX 500
/* The own player's playing list (tmpfs, RAM only; player/playback.c list_publish, read by ui/musicdb.c). The player
 * keeps the list as SONG.IDs in RAM and no longer writes LIST_SONG_0/LIST_SONG_3 to song.db (Jo, 2026-11-22). Text:
 *   "diskos-list 1 <n> <shuffled 0/1>\n", then n lines SONG.ID in list order (position k = line k, the a2 pos_id),
 *   then, when shuffled, n lines with the play order as 1-based positions.
 * Written to OPL_LIST ".tmp" and renamed, so a reader sees a whole list. Written again at every new list, shuffle
 * on/off and the player's start (empty: n 0); removed when the stock player runs. */
#ifndef OPL_LIST                                  /* the tests put it elsewhere */
#define OPL_LIST "/tmp/diskos-list"
#endif

enum { OPL_OFF = 0, OPL_ON, OPL_NO_BINARY, OPL_GAVE_UP };

typedef struct { const char *sw, *bin, *starts, *unstable; } opl_paths_t;

static inline const char *opl_reason(int r){
    switch(r){
    case OPL_OFF:       return "off";
    case OPL_ON:        return "on";
    case OPL_NO_BINARY: return "binary missing or not an executable ELF file - stock player started";
    case OPL_GAVE_UP:   return "started too often, or crashed early twice in a row - switched off (switch file renamed to .failed), stock player started";
    }
    return "?";
}

/* 1 if p is a regular executable file that starts with the ELF magic */
static inline int opl_binary_ok(const char *p){
    struct stat st;
    if(stat(p, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return 0;
    unsigned char m[4] = {0};
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if(fd < 0) return 0;
    ssize_t n = read(fd, m, 4);
    close(fd);
    return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

static inline int opl_read_int(const char *p){
    char b[16] = {0}; int n = 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd >= 0){ if(read(fd, b, sizeof b - 1) > 0) n = atoi(b); close(fd); }
    return (n < 0 || n > 1000) ? 0 : n;
}
static inline void opl_write_int(const char *p, int n, int sync){
    char b[16]; int len = snprintf(b, sizeof b, "%d\n", n);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(fd < 0) return;
    if(write(fd, b, (size_t)len) != len){ /* best effort: an uncounted start only delays the give-up */ }
    if(sync) fsync(fd);
    close(fd);
}

/* Decide. OPL_ON: exec p->bin. Anything else: the stock player. Counts the start only when it returns OPL_ON. */
static inline int opl_prepare(const opl_paths_t *p, int *start_no){
    if(start_no) *start_no = 0;
    if(access(p->sw, F_OK) != 0) return OPL_OFF;
    int n = opl_read_int(p->starts), u = opl_read_int(p->unstable);
    if(n >= OPL_MAX_STARTS || u >= OPL_MAX_UNSTABLE){
        char failed[256];
        snprintf(failed, sizeof failed, "%s.failed", p->sw);
        if(rename(p->sw, failed) != 0) unlink(p->sw);
        unlink(p->unstable);
        return OPL_GAVE_UP;
    }
    if(!opl_binary_ok(p->bin)) return OPL_NO_BINARY;
    opl_write_int(p->unstable, u + 1, 1);      /* first, and synced: a reboot right after the exec must still count */
    opl_write_int(p->starts, n + 1, 0);
    if(start_no) *start_no = n + 1;
    return OPL_ON;
}
/* 1 when the running player (the process named mq_player) is the own one: its executable is OPL_BIN (the stock one is
 * /usr/bin/mq_player; ps shows "mq_player" for both). /proc reads only, no spawn; for the UI (Settings > About,
 * Bluetooth at boot). */
static inline int opl_running(void){
    DIR *d = opendir("/proc"); if(!d) return 0;
    struct dirent *e; int own = 0;
    while(!own && (e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], c[32] = {0}, x[256];
        snprintf(p, sizeof p, "/proc/%.40s/comm", e->d_name);
        FILE *f = fopen(p, "r"); if(!f) continue;
        char *r = fgets(c, sizeof c, f); fclose(f);
        if(!r || strcmp(c, "mq_player\n") != 0) continue;
        snprintf(p, sizeof p, "/proc/%.40s/exe", e->d_name);
        ssize_t n = readlink(p, x, sizeof x - 1);
        if(n > 0){ x[n] = 0; own = !strcmp(x, OPL_BIN); }
    }
    closedir(d);
    return own;
}
/* The outputs state the own player writes to OPL_OUTPUTS (player/main.c write_outputs). opl_outputs_parse: 0, or -1
 * when the text has no aux= / bt= lines; opl_outputs_read: the same from the file (-1 also when it can't be read). */
typedef struct {
    int aux, bt, dig;              /* 1 = playing to it: AUX analog (the DAC), Bluetooth, AUX digital (S/PDIF) */
    int vol_aux, vol_bt, vol_dig;  /* 0..120 */
    int bt_ready;                  /* headphones connected and routed */
    int jack, bal;                 /* 3.5 mm / 4.4 mm plug: 1 in, 0 out, -1 unknown */
    int switching;                 /* a codec switch is running */
    char codec[16];                /* the link's codec, upper case ("LDAC"), "" when not on Bluetooth */
    char codecs[64];               /* the codecs the headphones offer ("SBC AAC LDAC") */
    int auto_out;                  /* 0d05 as the player has it: 0 off, 1 newest, 2 all (player/core.h PL_AUTO_*) */
    unsigned notice_seq;           /* bumps when a device connected that is not on (auto off) */
    int notice;                    /* what: 1 AUX, 2 Bluetooth, 3 play asked but no output is on, 4 a USB sound card
                                    * connected that is off (player/core.h) */
    int usb, vol_usb;              /* playing to the USB sound card; its volume 0..120 */
    int usb_ready;                 /* a USB sound card is there */
    int usb_soft;                  /* it has no volume control of its own: ours by alsa-lib softvol (usbaudio.c) */
    char usb_name[80];             /* its product name */
    int main_vol;                  /* the main volume (each output: main + its offset, within its cap); -1 unknown */
    int cap[4], fade[4];           /* per output (0 AUX, 1 Bluetooth, 2 digital, 3 USB: player/core.h PL_O_*) */
} opl_outputs_t;
static inline int opl_outputs_parse(const char *t, opl_outputs_t *o){
    memset(o, 0, sizeof *o);
    o->jack = o->bal = -1; o->main_vol = -1;
    for(int i = 0; i < 4; i++){ o->cap[i] = 120; o->fade[i] = 1; }
    int seen = 0;
    for(const char *l = t; l && *l; ){
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        char line[96];
        if(n >= sizeof line) n = sizeof line - 1;
        memcpy(line, l, n); line[n] = 0;
        char *v = strchr(line, '=');
        if(v){
            *v++ = 0;
            if(!strcmp(line, "aux")){ o->aux = atoi(v); seen |= 1; }
            else if(!strcmp(line, "bt")){ o->bt = atoi(v); seen |= 2; }
            else if(!strcmp(line, "vol_aux")) o->vol_aux = atoi(v);
            else if(!strcmp(line, "vol_bt")) o->vol_bt = atoi(v);
            else if(!strcmp(line, "dig")) o->dig = atoi(v);
            else if(!strcmp(line, "vol_dig")) o->vol_dig = atoi(v);
            else if(!strcmp(line, "bt_ready")) o->bt_ready = atoi(v);
            else if(!strcmp(line, "jack")) o->jack = atoi(v);
            else if(!strcmp(line, "bal")) o->bal = atoi(v);
            else if(!strcmp(line, "switching")) o->switching = atoi(v);
            else if(!strcmp(line, "codec")) snprintf(o->codec, sizeof o->codec, "%s", v);
            else if(!strcmp(line, "codecs")) snprintf(o->codecs, sizeof o->codecs, "%s", v);
            else if(!strcmp(line, "auto")) o->auto_out = atoi(v);
            else if(!strcmp(line, "notice")){ char *e; o->notice_seq = (unsigned)strtoul(v, &e, 10); o->notice = atoi(e); }
            else if(!strcmp(line, "usb")) o->usb = atoi(v);
            else if(!strcmp(line, "vol_usb")) o->vol_usb = atoi(v);
            else if(!strcmp(line, "usb_ready")) o->usb_ready = atoi(v);
            else if(!strcmp(line, "usb_soft")) o->usb_soft = atoi(v);
            else if(!strcmp(line, "usb_name")) snprintf(o->usb_name, sizeof o->usb_name, "%s", v);
            else if(!strcmp(line, "main")) o->main_vol = atoi(v);
            else if(!strcmp(line, "cap") || !strcmp(line, "fade")){
                int *dst = line[0] == 'c' ? o->cap : o->fade;
                if(sscanf(v, "%d %d %d %d", &dst[0], &dst[1], &dst[2], &dst[3]) != 4) for(int i = 0; i < 4; i++) dst[i] = line[0] == 'c' ? 120 : 1;
            }
        }
        l = e ? e + 1 : NULL;
    }
    return seen == 3 ? 0 : -1;
}
/* OPL_STREAM's text: 0 with the three values ("" when a line is missing), -1 without a uri= line */
static inline int opl_stream_parse(const char *t, char *uri, size_t un, char *artist, size_t an, char *title, size_t tn){
    uri[0] = artist[0] = title[0] = 0;
    int seen = 0;
    for(const char *l = t; l && *l; ){
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        char *dst = NULL; size_t cap = 0, k = 0;
        if(n >= 4 && !strncmp(l, "uri=", 4)){ dst = uri; cap = un; k = 4; seen = 1; }
        else if(n >= 7 && !strncmp(l, "artist=", 7)){ dst = artist; cap = an; k = 7; }
        else if(n >= 6 && !strncmp(l, "title=", 6)){ dst = title; cap = tn; k = 6; }
        if(dst && cap){ size_t m = n - k < cap - 1 ? n - k : cap - 1; memcpy(dst, l + k, m); dst[m] = 0; }
        l = e ? e + 1 : NULL;
    }
    return seen ? 0 : -1;
}
static inline int opl_outputs_read(opl_outputs_t *o){
    char b[1024];
    int fd = open(OPL_OUTPUTS, O_RDONLY | O_CLOEXEC);
    if(fd < 0){ memset(o, 0, sizeof *o); return -1; }
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = 0;
    return opl_outputs_parse(b, o);
}
#endif
