/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* USB drive (see usbdrive.h). Device-verified 2026-11-14 with the Zune "usb-c out" route (0666 3 + 0642 5): a second
 * Disc in USB storage mode and a card reader behind a hub both enumerate as usb-storage /dev/sd*. Here only the
 * gadget selector 0642 5 is sent, so the audio route stays on the headphones; that this alone takes the host port is
 * NOT yet device-verified (the log says what happened). The stock firmware then tries to mount the new disk itself
 * with iocharset=utf8, which fails for ext4 (the same reason it can't mount an ext4 SD card), so diskOS mounts it. */
#include "usbdrive.h"
#include "usbdrive_core.h"
#include "screens.h"
#include "modes.h"
#include "ipc.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define UD_LOG     "/usr/data/usbdrive.log"
#define UD_LOG_MAX (64 * 1024)
#define UD_HOST_ON  "0642000C0005"
#define UD_HOST_OFF "0642000C0000"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ud_vol_t g_vol[UD_MAX];
static int g_nvol;
static char g_failed[UD_MAX][16];          /* devices that would not mount: not retried until they go away */
static int g_nfailed;
static _Atomic int g_state = UD_OFF, g_stop_req, g_worker_live, g_gen, g_pins;
static int g_seen_gen;
static uint32_t g_t_on;
static lv_timer_t *g_tm;
static void (*g_cb)(void);

static void ud_log(const char *fmt, ...){
    struct stat st;
    FILE *f = fopen(UD_LOG, (stat(UD_LOG, &st) == 0 && st.st_size > UD_LOG_MAX) ? "w" : "a");
    if(!f) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void log_sd_mounts(void){                     /* who else mounted a /dev/sd* (the stock firmware's own attempt) */
    FILE *m = fopen("/proc/mounts", "r"); if(!m) return;
    char l[512];
    while(fgets(l, sizeof l, m)) if(!strncmp(l, "/dev/sd", 7)){ l[strcspn(l, "\n")] = 0; ud_log("  mounts: %s", l); }
    fclose(m);
}

/* ---- the worker: mounts what appears, drops what goes, unmounts everything on stop ---------------------------- */
static int failed_has(const char *dev){ for(int i = 0; i < g_nfailed; i++) if(!strcmp(g_failed[i], dev)) return 1; return 0; }
static int try_mount(const char *dev, ud_vol_t *v){
    char path[32]; snprintf(path, sizeof path, "/dev/%.15s", dev);
    snprintf(v->mnt, sizeof v->mnt, UD_ROOT "/%.15s", dev);
    mkdir(UD_ROOT, 0755); mkdir(v->mnt, 0755);
    const unsigned long fl = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC;
    char label[24];
    if(udc_ext_label(path, label, sizeof label)){
        if(mount(path, v->mnt, "ext4", fl, NULL) == 0){ snprintf(v->fstype, sizeof v->fstype, "ext4"); snprintf(v->label, sizeof v->label, "%s", label); return 1; }
        ud_log("%s: ext superblock, mount failed: %s", dev, strerror(errno));
        rmdir(v->mnt); return 0;
    }
    static const char *const T[] = { "exfat", "vfat", "ntfs", "xfs" };
    for(unsigned k = 0; k < sizeof T / sizeof T[0]; k++){
        int xfs = !strcmp(T[k], "xfs");
        if((!xfs && mount(path, v->mnt, T[k], fl, "iocharset=utf8") == 0) || mount(path, v->mnt, T[k], fl, NULL) == 0){
            snprintf(v->fstype, sizeof v->fstype, "%s", T[k]); v->label[0] = 0; return 1;
        }
    }
    ud_log("%s: no file system diskOS can mount (last error: %s)", dev, strerror(errno));
    rmdir(v->mnt); return 0;
}
/* Is something mounted at `mnt` right now? (/proc/mounts, field 2) */
static int is_mounted(const char *mnt){
    FILE *m = fopen("/proc/mounts", "r"); if(!m) return 1;          /* can't tell: assume yes */
    char l[512], dev[256], dir[256]; int yes = 0;
    while(!yes && fgets(l, sizeof l, m)) if(sscanf(l, "%255s %255s", dev, dir) == 2 && !strcmp(dir, mnt)) yes = 1;
    fclose(m);
    return yes;
}
/* Device test 2026-11-14 (card reader, ext4 on sdb): the kernel logged our mount, yet a moment later /proc/mounts had
 * no /dev/sdb and /tmp/usbdrive/sdb was gone ("Can't open folder"). Most likely the stock firmware's own hotplug
 * handling for a new /dev/sd* (unmount, remove the mount point, `mount -o iocharset=utf8`, which ext4 rejects - its
 * SD card routine does exactly that) landed after ours. Unverified; the log records it. Mount it again, a few times
 * at most so we never fight the firmware in a loop. */
#define UD_REMOUNT_MAX 3
static int remount_lost(ud_vol_t *v){
    char path[32]; snprintf(path, sizeof path, "/dev/%.15s", v->dev);
    mkdir(UD_ROOT, 0755); mkdir(v->mnt, 0755);
    const unsigned long fl = MS_NOSUID | MS_NODEV | MS_NOEXEC | (v->rw ? 0 : MS_RDONLY);
    int win = strcmp(v->fstype, "ext4") && strcmp(v->fstype, "xfs");
    if((win && mount(path, v->mnt, v->fstype, fl, "iocharset=utf8") == 0) || mount(path, v->mnt, v->fstype, fl, NULL) == 0) return 1;
    ud_log("%s: mounting again failed: %s", v->dev, strerror(errno));
    return 0;
}
static void drop(ud_vol_t *v){
    if(umount(v->mnt) != 0){
        int e = errno;
        if(umount2(v->mnt, MNT_DETACH) != 0) ud_log("%s: unmount failed: %s", v->dev, strerror(errno));
        else ud_log("%s: busy (%s), detached", v->dev, strerror(e));
    }
    rmdir(v->mnt);
}
static void *worker(void *arg){
    (void)arg;
    ud_log("worker: looking for drives");
    while(!atomic_load(&g_stop_req)){
        udc_dev_t d[UD_MAX];
        int n = udc_scan("/sys/block", d, UD_MAX);
        int changed = 0;
        pthread_mutex_lock(&g_mu);
        for(int i = 0; i < g_nvol; ){                                     /* gone (unplugged, or the port was dropped) */
            int here = 0; for(int k = 0; k < n; k++) if(!strcmp(d[k].name, g_vol[i].dev)) here = 1;
            if(here){ i++; continue; }
            ud_log("%s: gone, unmounting", g_vol[i].dev);
            drop(&g_vol[i]);
            g_vol[i] = g_vol[--g_nvol]; changed = 1;
        }
        for(int i = 0; i < g_nfailed; ){
            int here = 0; for(int k = 0; k < n; k++) if(!strcmp(d[k].name, g_failed[i])) here = 1;
            if(here) i++; else memmove(g_failed[i], g_failed[--g_nfailed], sizeof g_failed[0]);
        }
        for(int i = 0; i < g_nvol; ){                                     /* still there, but unmounted by someone else */
            if(is_mounted(g_vol[i].mnt)){ i++; continue; }
            ud_log("%s: no longer mounted at %s (unmounted by the firmware?)", g_vol[i].dev, g_vol[i].mnt);
            log_sd_mounts();
            if(g_vol[i].remounts < UD_REMOUNT_MAX && remount_lost(&g_vol[i])){
                g_vol[i].remounts++; ud_log("%s: mounted again (%d)", g_vol[i].dev, g_vol[i].remounts);
                changed = 1; i++; continue;
            }
            ud_log("%s: giving up on it", g_vol[i].dev);
            rmdir(g_vol[i].mnt);
            if(g_nfailed < UD_MAX) snprintf(g_failed[g_nfailed++], sizeof g_failed[0], "%.15s", g_vol[i].dev);   /* until replugged */
            g_vol[i] = g_vol[--g_nvol]; changed = 1;
        }
        pthread_mutex_unlock(&g_mu);
        for(int k = 0; k < n && !atomic_load(&g_stop_req); k++){             /* new */
            pthread_mutex_lock(&g_mu);
            int known = failed_has(d[k].name) || g_nvol >= UD_MAX;
            for(int i = 0; i < g_nvol; i++) if(!strcmp(g_vol[i].dev, d[k].name)) known = 1;
            pthread_mutex_unlock(&g_mu);
            if(known) continue;
            ud_vol_t v; memset(&v, 0, sizeof v);
            snprintf(v.dev, sizeof v.dev, "%.15s", d[k].name); v.bytes = d[k].bytes;
            int ok = try_mount(d[k].name, &v);                               /* outside the lock: a mount can take a while */
            pthread_mutex_lock(&g_mu);
            if(ok){ g_vol[g_nvol++] = v; changed = 1;
                    ud_log("%s: mounted %s read-only at %s (%llu bytes, label \"%s\")", v.dev, v.fstype, v.mnt, v.bytes, v.label); }
            else if(g_nfailed < UD_MAX) snprintf(g_failed[g_nfailed++], sizeof g_failed[0], "%.15s", d[k].name);
            pthread_mutex_unlock(&g_mu);
        }
        if(changed){ log_sd_mounts(); atomic_fetch_add(&g_gen, 1); }
        for(int t = 0; t < 10 && !atomic_load(&g_stop_req); t++) usleep(100000);
    }
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_nvol; i++){ ud_log("%s: ejecting", g_vol[i].dev); drop(&g_vol[i]); }
    g_nvol = 0; g_nfailed = 0;
    pthread_mutex_unlock(&g_mu);
    sync();
    atomic_fetch_add(&g_gen, 1);
    atomic_store(&g_worker_live, 0);
    return NULL;
}

/* ---- UI thread ------------------------------------------------------------------------------------------------- */
static void tick(lv_timer_t *t){
    (void)t;
    int gen = atomic_load(&g_gen);
    if(atomic_load(&g_state) == UD_STOPPING && !atomic_load(&g_worker_live)){
        if(modes_output_route() != OUT_USB){                                 /* USB audio output still wants the port */
            if(ipc_send_cmd(UD_HOST_OFF) < 0) ud_log("host port: release (0642 0) failed to send");
            else ud_log("host port released (0642 0)");
        }
        atomic_store(&g_state, UD_OFF);
        lv_timer_delete(g_tm); g_tm = NULL;
        ui_toast("USB drive ejected");
        gen = -1;
    }
    if(gen != g_seen_gen){ g_seen_gen = gen; if(g_cb) g_cb(); }
}
const char *usbdrive_blocked(void){
    int m = ui_get_source_mode();
    if(m != 0 && m != 6) return "Switch to Playback first";
    if(modes_output_busy()) return "Switching output - try again";
    if(atomic_load(&g_state) == UD_STOPPING) return "Still ejecting - try again";
    return NULL;
}
int usbdrive_start(void){
    if(atomic_load(&g_state) == UD_ON) return 0;
    const char *why = usbdrive_blocked();
    if(why){ ui_toast(why); return -1; }
    if(ipc_send_cmd(UD_HOST_ON) < 0){ ui_toast("Couldn't reach the player"); return -1; }
    ud_log("host port requested (0642 5), source mode %d, output route %d", ui_get_source_mode(), modes_output_route());
    atomic_store(&g_stop_req, 0); atomic_store(&g_worker_live, 1);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 128 * 1024);
    if(pthread_create(&th, &at, worker, NULL) != 0){
        pthread_attr_destroy(&at); atomic_store(&g_worker_live, 0);
        if(modes_output_route() != OUT_USB) ipc_send_cmd(UD_HOST_OFF);
        ui_toast("Couldn't start"); return -1;
    }
    pthread_detach(th); pthread_attr_destroy(&at);
    atomic_store(&g_state, UD_ON); g_t_on = lv_tick_get();
    if(!g_tm) g_tm = lv_timer_create(tick, 300, NULL);
    return 0;
}
int usbdrive_stop(void){
    if(atomic_load(&g_state) != UD_ON) return 0;
    if(atomic_load(&g_pins) > 0){ ui_toast("A transfer is running"); return -1; }
    ud_log("eject requested");
    atomic_store(&g_state, UD_STOPPING); atomic_store(&g_stop_req, 1);
    atomic_fetch_add(&g_gen, 1);
    return 0;
}
int usbdrive_state(void){ return atomic_load(&g_state); }
unsigned usbdrive_on_ms(void){ return atomic_load(&g_state) == UD_ON ? lv_tick_elaps(g_t_on) : 0; }
int usbdrive_count(void){ pthread_mutex_lock(&g_mu); int n = g_nvol; pthread_mutex_unlock(&g_mu); return n; }
int usbdrive_get(int i, ud_vol_t *out){
    pthread_mutex_lock(&g_mu);
    int ok = i >= 0 && i < g_nvol;
    if(ok) *out = g_vol[i];
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int usbdrive_find(const char *path, ud_vol_t *out){
    int r = -1;
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_nvol && r < 0; i++){
        size_t n = strlen(g_vol[i].mnt);
        if(!strncmp(path, g_vol[i].mnt, n) && (path[n] == 0 || path[n] == '/')){ r = i; if(out) *out = g_vol[i]; }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
const char *usbdrive_name(const ud_vol_t *v, char *buf, int cap){
    if(v->label[0]) snprintf(buf, (size_t)cap, "%s", v->label);
    else snprintf(buf, (size_t)cap, "USB drive (%s)", v->dev);
    return buf;
}
void usbdrive_on_change(void (*cb)(void)){ g_cb = cb; }

int usbdrive_pin(const char *dev){
    atomic_fetch_add(&g_pins, 1);
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    if(atomic_load(&g_state) == UD_ON) for(int i = 0; i < g_nvol; i++) if(!strcmp(g_vol[i].dev, dev)) ok = 1;
    pthread_mutex_unlock(&g_mu);
    if(!ok) atomic_fetch_sub(&g_pins, 1);
    return ok;
}
void usbdrive_unpin(void){ atomic_fetch_sub(&g_pins, 1); }
int usbdrive_set_rw(const char *dev, int rw){
    int rc = -1;
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < g_nvol; i++){
        if(strcmp(g_vol[i].dev, dev)) continue;
        char path[32]; snprintf(path, sizeof path, "/dev/%s", dev);
        if(!rw) sync();
        unsigned long fl = MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC | (rw ? 0 : MS_RDONLY);
        int win = strcmp(g_vol[i].fstype, "ext4") && strcmp(g_vol[i].fstype, "xfs");
        rc = mount(path, g_vol[i].mnt, g_vol[i].fstype, fl, win ? "iocharset=utf8" : NULL);
        if(rc != 0 && win) rc = mount(path, g_vol[i].mnt, g_vol[i].fstype, fl, NULL);
        if(rc == 0) g_vol[i].rw = rw;
        ud_log("%s: remount %s %s%s%s", dev, rw ? "read-write" : "read-only", rc == 0 ? "ok" : "failed: ", rc == 0 ? "" : strerror(errno), "");
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

/* host renders only (tests/host_render.c USB_FAKE): two volumes as if mounted, nothing touched */
void usbdrive_test_fake(void){
    pthread_mutex_lock(&g_mu);
    memset(g_vol, 0, sizeof g_vol);
    snprintf(g_vol[0].dev, sizeof g_vol[0].dev, "sdb2"); snprintf(g_vol[0].mnt, sizeof g_vol[0].mnt, UD_ROOT "/sdb2");
    snprintf(g_vol[0].label, sizeof g_vol[0].label, "DISC"); snprintf(g_vol[0].fstype, sizeof g_vol[0].fstype, "ext4");
    g_vol[0].bytes = 126973952000ULL;
    snprintf(g_vol[1].dev, sizeof g_vol[1].dev, "sdc1"); snprintf(g_vol[1].mnt, sizeof g_vol[1].mnt, UD_ROOT "/sdc1");
    snprintf(g_vol[1].fstype, sizeof g_vol[1].fstype, "exfat"); g_vol[1].bytes = 31914983424ULL;
    g_nvol = getenv("USB_FAKE") ? atoi(getenv("USB_FAKE")) : 2;
    if(g_nvol < 0 || g_nvol > 2) g_nvol = 2;
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_state, UD_ON); g_t_on = lv_tick_get();
}
