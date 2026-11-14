/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* USB import / export (see usbsync.h): the dialogs on the UI thread, rsync on a worker thread. The command line and
 * the output parsing are in usbsync_core.c. */
#include "usbsync.h"
#include "usbsync_core.h"
#include "usbdrive.h"
#include "usbdrive_core.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "sdio.h"
#include "command_spawn.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* system.c: the system dialogs (one open at a time; Zune takes the whole glass) */
lv_obj_t *sys_modal_card(const char *title, int h);
lv_obj_t *sys_modal_text(lv_obj_t *card, const char *txt, int y, theme_color_role_t col);
void sys_modal_pill(lv_obj_t *card, int x, const char *txt, theme_color_role_t col, lv_event_cb_t cb, const char *action);
void sys_modal_simple(const char *title, const char *body);
void sys_modal_close(void);
int  sys_modal_open(void);
int  sd_write_begin(void);      /* main.c: the SD write gate (refused while the card is handed to a computer) */
void sd_write_end(void);
const lv_font_t *zune_font(const lv_font_t *base);
LV_FONT_DECLARE(font_zune_regular_16) LV_FONT_DECLARE(font_zune_regular_17)

#define SD_ROOT "/tmp/sdcard"
#define P 1024

enum { J_IDLE, J_COUNTING, J_COUNTED, J_RUNNING, J_DONE };
static struct {
    int exp;                                /* 0 import (drive -> player), 1 export (player -> drive) */
    char dev[16], vname[64], vfs[8];        /* the drive volume */
    char src[P], dst[P], troot[P], name[256];
    int src_is_dir;
    char backup[P + 32], log[P + 64];
    int phase, rc, cancel;
    pid_t pid;
    usc_count_t cnt;
    usc_progress_t prog;
    unsigned long long free_b;
    char err[160];
} J;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_update, g_checksum, g_backups = 1;   /* the dialog's choices, kept for the next one */
static void (*g_done)(void);
static lv_timer_t *g_tm;
static int g_hidden;                                 /* the progress dialog was hidden: report with a toast */
static int g_preview;                                /* host renders: no worker */

int usbsync_busy(void){ pthread_mutex_lock(&g_mu); int b = J.phase == J_COUNTING || J.phase == J_RUNNING; pthread_mutex_unlock(&g_mu); return b; }

/* ---------------------------------------------------------------- worker */
/* Run argv; every output line (split on \n and \r) goes to on_line. Returns rsync's exit status, 128+signal, or 127
 * when it couldn't be started. */
static int run_rsync(char **argv, void (*on_line)(const char *)){
    int fds[2];
    if(pipe2(fds, O_CLOEXEC) != 0) return 127;
    pid_t pid;
    int rc = command_spawn_out(&pid, argv, fds[1]);
    close(fds[1]);
    if(rc){ close(fds[0]); return 127; }
    pthread_mutex_lock(&g_mu); J.pid = pid; int cancel = J.cancel; pthread_mutex_unlock(&g_mu);
    if(cancel) kill(-pid, SIGTERM);
    char buf[4096], line[1024]; size_t ln = 0; ssize_t r;
    while((r = read(fds[0], buf, sizeof buf)) > 0 || (r < 0 && errno == EINTR)){
        for(ssize_t i = 0; i < r; i++){
            char c = buf[i];
            if(c == '\n' || c == '\r'){ line[ln] = 0; if(ln) on_line(line); ln = 0; }
            else if(ln + 1 < sizeof line) line[ln++] = c;
        }
    }
    if(ln){ line[ln] = 0; on_line(line); }
    close(fds[0]);
    int st = 0;
    while(waitpid(pid, &st, 0) < 0 && errno == EINTR){}
    pthread_mutex_lock(&g_mu); J.pid = 0; pthread_mutex_unlock(&g_mu);
    if(WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}
static void keep_err(const char *l){
    if(strncmp(l, "rsync", 5) && strncmp(l, "ERROR", 5)) return;
    pthread_mutex_lock(&g_mu); snprintf(J.err, sizeof J.err, "%s", l); pthread_mutex_unlock(&g_mu);
}
static void on_count(const char *l){ pthread_mutex_lock(&g_mu); usc_count_line(l, &J.cnt); pthread_mutex_unlock(&g_mu); keep_err(l); }
static void on_progress(const char *l){
    usc_progress_t p = { 0 };
    if(usc_parse_progress(l, &p)){ pthread_mutex_lock(&g_mu); J.prog = p; pthread_mutex_unlock(&g_mu); return; }
    keep_err(l);
}
static int build(usc_opts_t *o, int dry, char **argv, char *buf, size_t cap){
    memset(o, 0, sizeof *o);
    o->rsync = USC_RSYNC; o->src = J.src; o->src_is_dir = J.src_is_dir; o->dst = J.dst;
    o->update = g_update; o->checksum = g_checksum; o->dry_run = dry;
    o->backup_dir = (g_update && g_backups && !dry) ? J.backup : NULL;
    o->log_file = dry ? NULL : J.log;
    return usc_argv(o, argv, buf, cap);
}
/* the SD side of the run: an import writes the card, an export only reads it */
static int sd_take(int exp){ return exp ? sd_io_begin() : sd_write_begin(); }
static void sd_give(int exp){ if(exp) sd_io_end(); else sd_write_end(); }
static void finish(int rc){ pthread_mutex_lock(&g_mu); J.rc = rc; J.phase = (J.phase == J_COUNTING) ? J_COUNTED : J_DONE; pthread_mutex_unlock(&g_mu); }

static void *count_worker(void *arg){
    (void)arg;
    if(access(USC_RSYNC, X_OK) != 0){ finish(127); return NULL; }
    if(!usbdrive_pin(J.dev)){ snprintf(J.err, sizeof J.err, "The drive is gone"); finish(-1); return NULL; }
    int rc = -1;
    if(sd_io_begin()){
        struct statvfs vs;
        if(statvfs(J.troot, &vs) == 0) J.free_b = (unsigned long long)vs.f_bavail * vs.f_frsize;
        usc_opts_t o; char *argv[USC_ARGV_MAX]; char buf[8192];
        rc = build(&o, 1, argv, buf, sizeof buf) < 0 ? -1 : run_rsync(argv, on_count);
        sd_io_end();
    } else snprintf(J.err, sizeof J.err, "The SD card is not ready");
    usbdrive_unpin();
    finish(rc);
    return NULL;
}
static void write_header(void){
    char dir[P + 16]; snprintf(dir, sizeof dir, "%s/synclog", J.troot);
    mkdir(dir, 0755);
    FILE *f = fopen(J.log, "a"); if(!f) return;
    time_t t = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "# diskOS %s %s\n# from: %s\n# to:   %s\n# mode: %s%s%s\n", J.exp ? "export" : "import", ts, J.src, J.dst,
            g_update ? "update changed files" : "only missing", g_update && g_checksum ? ", compare thoroughly" : "",
            g_update && g_backups ? ", keep backups" : "");
    if(g_update && g_backups) fprintf(f, "# replaced files go to: %s\n", J.backup);
    fclose(f);
}
static void write_footer(int rc){
    FILE *f = fopen(J.log, "a"); if(!f) return;
    fprintf(f, "# finished: exit %d (%s)%s%s\n", rc, usc_exit_text(rc), J.err[0] ? " - " : "", J.err);
    fclose(f);
}
static void *run_worker(void *arg){
    (void)arg;
    int rc = -1;
    if(!usbdrive_pin(J.dev)){ snprintf(J.err, sizeof J.err, "The drive is gone"); finish(-1); return NULL; }
    if(!sd_take(J.exp)){ snprintf(J.err, sizeof J.err, J.exp ? "The SD card is not ready" : "The SD card can't be written now"); usbdrive_unpin(); finish(-1); return NULL; }
    if(J.exp && usbdrive_set_rw(J.dev, 1) != 0){ snprintf(J.err, sizeof J.err, "Couldn't make the drive writable"); sd_give(J.exp); usbdrive_unpin(); finish(-1); return NULL; }
    char rel[P];
    if(usc_rel(J.troot, J.dst, rel, sizeof rel) != 0) rel[0] = 0;
    int bi = usc_backup_index(J.troot);
    snprintf(J.backup, sizeof J.backup, "%s/_changed%d%s", J.troot, bi < 0 ? 0 : bi, rel);
    usc_log_name(J.troot, (long)time(NULL), J.exp ? "export" : "import", J.log, sizeof J.log);
    write_header();
    usc_opts_t o; char *argv[USC_ARGV_MAX]; char buf[8192];
    if(build(&o, 0, argv, buf, sizeof buf) >= 0) rc = run_rsync(argv, on_progress);
    write_footer(rc);
    sync();
    if(J.exp && usbdrive_set_rw(J.dev, 0) != 0) snprintf(J.err, sizeof J.err, "The drive stayed writable - eject it");
    sd_give(J.exp);
    usbdrive_unpin();
    finish(rc);
    return NULL;
}
static int start_thread(void *(*fn)(void *)){
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
    int ok = pthread_create(&th, &at, fn, NULL) == 0;
    if(ok) pthread_detach(th);
    pthread_attr_destroy(&at);
    return ok;
}

/* ---------------------------------------------------------------- dialogs */
static const char *verb(void){ return J.exp ? "Export" : "Import"; }
static void tick(lv_timer_t *t);
static void show_options(void);
static void show_counted(void);
static void show_progress(void);
static void show_result(void);
static void ensure_tick(void){ if(!g_tm) g_tm = lv_timer_create(tick, 250, NULL); }
static void cb_close(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) sys_modal_close(); }
static void cb_cancel_job(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pthread_mutex_lock(&g_mu); J.cancel = 1; pid_t p = J.pid; int ph = J.phase; pthread_mutex_unlock(&g_mu);
    if(p > 0) kill(-p, SIGTERM);
    if(ph == J_COUNTED || ph == J_IDLE){ pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu); sys_modal_close(); }
}
static void cb_hide(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED){ g_hidden = 1; sys_modal_close(); } }
static void cb_start(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pthread_mutex_lock(&g_mu); J.phase = J_RUNNING; J.cancel = 0; memset(&J.prog, 0, sizeof J.prog); J.err[0] = 0; pthread_mutex_unlock(&g_mu);
    g_hidden = 0;
    if(!g_preview && !start_thread(run_worker)){ pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu); sys_modal_close(); ui_toast("Couldn't start"); return; }
    show_progress(); ensure_tick();
}
static void cb_next(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pthread_mutex_lock(&g_mu); memset(&J.cnt, 0, sizeof J.cnt); J.phase = J_COUNTING; J.cancel = 0; J.err[0] = 0; J.free_b = 0; pthread_mutex_unlock(&g_mu);
    if(!g_preview && !start_thread(count_worker)){ pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu); sys_modal_close(); ui_toast("Couldn't start"); return; }
    lv_obj_t *c = sys_modal_card(J.exp ? "Export to USB drive" : "Import to player", 190);
    sys_modal_text(c, "Counting what would be copied...", 60, THEME_CLR_TEXT_MUTED);
    sys_modal_pill(c, 0, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_cancel_job, "usbsync.cancel");
    ensure_tick();
}

/* one line of text in the dialog: centred, ends in "..." when too long */
static lv_obj_t *line(lv_obj_t *c, const char *txt, int y, theme_color_role_t col){
    lv_obj_t *l = lv_label_create(c); kit_keep(l);
    lv_label_set_text(l, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    const lv_font_t *f = th_zune() ? zune_font(&font_zune_regular_16) : TF(UI_14);
    lv_obj_set_size(l, 250, lv_font_get_line_height(f));       /* one line, "..." at the end: never wraps */
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, theme_color(col), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}
static lv_obj_t *g_seg[2], *g_chk[2], *g_desc;
static void paint_options(void);
static void cb_seg(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED){ g_update = (int)(intptr_t)lv_event_get_user_data(e); paint_options(); } }
static void cb_chk(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if((intptr_t)lv_event_get_user_data(e)) g_backups = !g_backups; else g_checksum = !g_checksum;
    paint_options();
}
static lv_obj_t *button(lv_obj_t *c, const char *txt, int w, int x, int y, lv_event_cb_t cb, void *ud, const char *action){
    lv_obj_t *b = lv_obj_create(c); lv_obj_remove_style_all(b); kit_keep(b);
    lv_obj_set_size(b, w, 34); lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(b, 10, 0); lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, TC(SURFACE_RAISED), 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(b, 3);
    ui_on(b, cb, LV_EVENT_CLICKED, ud, action, UI_CORE);
    lv_obj_t *l = lv_label_create(b); kit_keep(l); lv_label_set_text(l, txt);
    const lv_font_t *f = th_zune() ? zune_font(&font_zune_regular_16) : TF(UI_14);
    lv_obj_set_style_text_font(l, f, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_obj_set_size(l, w - 10, lv_font_get_line_height(f));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0);
    lv_obj_center(l);
    return b;
}
static void paint_options(void){
    for(int i = 0; i < 2; i++){
        if(!g_seg[i]) continue;
        int on = i == g_update;
        lv_obj_set_style_bg_color(g_seg[i], on ? TC(ACCENT_PRIMARY) : TC(SURFACE_RAISED), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(g_seg[i], 0), on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    }
    static const char *const CAP[2] = { "Compare thoroughly", "Keep backups" };
    for(int i = 0; i < 2; i++){
        if(!g_chk[i]) continue;
        int v = i ? g_backups : g_checksum;
        char b[48]; snprintf(b, sizeof b, "%s  %s", v ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, CAP[i]);
        lv_label_set_text(lv_obj_get_child(g_chk[i], 0), b);
        lv_obj_set_style_text_color(lv_obj_get_child(g_chk[i], 0), v ? TC(TEXT_PRIMARY) : TC(TEXT_MUTED), 0);
        if(g_update) lv_obj_clear_flag(g_chk[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_chk[i], LV_OBJ_FLAG_HIDDEN);
    }
    if(g_desc){
        lv_label_set_text(g_desc, "Copies what the target doesn't have. Files already there stay as they are.");
        if(g_update) lv_obj_add_flag(g_desc, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(g_desc, LV_OBJ_FLAG_HIDDEN);
    }
}
static void cb_opt_cancel(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED){ g_seg[0] = g_seg[1] = g_chk[0] = g_chk[1] = g_desc = NULL; sys_modal_close(); } }
static void cb_opt_next(lv_event_t *e){ g_seg[0] = g_seg[1] = g_chk[0] = g_chk[1] = g_desc = NULL; cb_next(e); }
static void show_options(void){
    int z = th_zune(), y = z ? 100 : 42;
    lv_obj_t *c = sys_modal_card(J.exp ? "Export to USB drive" : "Import to player", 276);
    char t[300], rel[P] = "";
    snprintf(t, sizeof t, "%s", J.name[0] ? J.name : J.exp ? "Everything on the player" : "Everything on the drive");
    line(c, t, y, THEME_CLR_TEXT_PRIMARY);
    if(usc_rel(J.troot, J.dst, rel, sizeof rel) != 0) rel[0] = 0;
    snprintf(t, sizeof t, "to %s%s", J.exp ? J.vname : "Files", rel[0] ? rel : "");
    line(c, t, y + 20, THEME_CLR_TEXT_MUTED);
    g_seg[0] = button(c, "Only missing", 130, -67, y + 46, cb_seg, (void *)0, "usbsync.only_missing");
    g_seg[1] = button(c, "Update", 130, 67, y + 46, cb_seg, (void *)1, "usbsync.update");
    g_chk[0] = button(c, "", 230, 0, y + (z ? 84 : 88), cb_chk, (void *)0, "usbsync.checksum");
    g_chk[1] = button(c, "", 230, 0, y + (z ? 116 : 124), cb_chk, (void *)1, "usbsync.backups");
    for(int i = 0; i < 2; i++) lv_obj_set_style_bg_opa(g_chk[i], LV_OPA_TRANSP, 0);
    g_desc = lv_label_create(c); kit_keep(g_desc);
    lv_label_set_long_mode(g_desc, LV_LABEL_LONG_WRAP); lv_obj_set_width(g_desc, 240);
    lv_obj_set_style_text_align(g_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_desc, z ? zune_font(&font_zune_regular_16) : TF(UI_14), 0);
    lv_obj_set_style_text_color(g_desc, TC(TEXT_MUTED), 0);
    lv_obj_align(g_desc, LV_ALIGN_TOP_MID, 0, y + (z ? 88 : 94));
    paint_options();
    sys_modal_pill(c, -58, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_opt_cancel, "usbsync.cancel");
    sys_modal_pill(c, 58, "Next", THEME_CLR_ACCENT_EMPHASIS, cb_opt_next, "usbsync.next");
}
static void show_counted(void){
    lv_obj_t *c;
    if(J.rc == 127){ sys_modal_simple(verb(), "rsync is not installed on this player (/usr/data/sshd/bin/rsync, see SSH_ACCESS.md)."); return; }
    if(J.rc != 0){
        char b[260]; snprintf(b, sizeof b, "Couldn't look at the files: %s", J.err[0] ? J.err : usc_exit_text(J.rc));
        sys_modal_simple(verb(), b); return;
    }
    long n = J.cnt.files_new + J.cnt.files_changed;
    if(!n){ sys_modal_simple(verb(), "Nothing to copy: the target already has all of it."); pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu); return; }
    char sz[24], fr[24], b[400]; int k = 0;
    udc_fmt_size(J.cnt.bytes, sz, sizeof sz); udc_fmt_size(J.free_b, fr, sizeof fr);
    k += snprintf(b + k, sizeof b - (size_t)k, "%ld new file%s", J.cnt.files_new, J.cnt.files_new == 1 ? "" : "s");
    if(J.cnt.files_changed) k += snprintf(b + k, sizeof b - (size_t)k, ", %ld changed", J.cnt.files_changed);
    k += snprintf(b + k, sizeof b - (size_t)k, " \xC2\xB7 %s\nFree on the %s: %s", sz, J.exp ? "drive" : "player", fr);
    int room = !J.free_b || J.cnt.bytes < J.free_b;
    if(!room) k += snprintf(b + k, sizeof b - (size_t)k, "\nNot enough space.");
    else {
        if(J.cnt.files_changed && g_backups) k += snprintf(b + k, sizeof b - (size_t)k, "\nReplaced files are kept as backups.");
        if(J.exp) k += snprintf(b + k, sizeof b - (size_t)k, "\nThe drive is made writable for this export.");
        if(udc_is_windows_fs(J.vfs)) k += snprintf(b + k, sizeof b - (size_t)k, "\nWindows file systems not fully supported, use at your own risk.");
    }
    c = sys_modal_card(J.exp ? "Export to USB drive" : "Import to player", 250);
    sys_modal_text(c, b, 46, room ? THEME_CLR_TEXT_MUTED : THEME_CLR_STATUS_WARNING);
    if(room){
        sys_modal_pill(c, -58, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_cancel_job, "usbsync.cancel");
        sys_modal_pill(c, 58, "Start", THEME_CLR_ACCENT_EMPHASIS, cb_start, "usbsync.start");
    } else sys_modal_pill(c, 0, "OK", THEME_CLR_ACCENT_EMPHASIS, cb_cancel_job, "usbsync.ok");
}
static lv_obj_t *g_prog_lbl;
static void progress_text(char *b, size_t cap){
    char done[24], all[24];
    udc_fmt_size(J.prog.bytes, done, sizeof done); udc_fmt_size(J.cnt.bytes, all, sizeof all);
    long n = J.cnt.files_new + J.cnt.files_changed;
    snprintf(b, cap, "%d%% \xC2\xB7 %s of %s\nFile %ld of %ld", J.prog.pct, done, all, J.prog.xfr < n ? J.prog.xfr + 1 : n, n);
}
static void show_progress(void){
    char t[40], b[96]; snprintf(t, sizeof t, "%s...", J.exp ? "Exporting" : "Importing");
    lv_obj_t *c = sys_modal_card(t, 200);
    progress_text(b, sizeof b);
    g_prog_lbl = sys_modal_text(c, b, 54, THEME_CLR_TEXT_MUTED);
    sys_modal_pill(c, -58, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_cancel_job, "usbsync.cancel");
    sys_modal_pill(c, 58, "Hide", THEME_CLR_ACCENT_EMPHASIS, cb_hide, "usbsync.hide");
}
static void result_text(char *b, size_t cap, int *ok){
    char rel[P]; const char *lg = J.log;
    if(usc_rel(J.troot, J.log, rel, sizeof rel) == 0) lg = rel + 1;
    long n = J.prog.xfr;
    *ok = J.rc == 0;
    if(J.rc == 0) snprintf(b, cap, "%ld file%s copied.\nLog: %s", n, n == 1 ? "" : "s", lg);
    else if(J.cancel) snprintf(b, cap, "Cancelled after %ld file%s. Nothing half-copied is left.\nLog: %s", n, n == 1 ? "" : "s", lg);
    else snprintf(b, cap, "%s%s%s\nLog: %s", usc_exit_text(J.rc), J.err[0] ? "\n" : "", J.err, lg);
}
static void show_result(void){
    char b[600]; int ok;
    result_text(b, sizeof b, &ok);
    char t[40]; snprintf(t, sizeof t, "%s %s", verb(), ok ? "done" : J.cancel ? "cancelled" : "incomplete");
    sys_modal_simple(t, b);
}
static void tick(lv_timer_t *t){
    (void)t;
    pthread_mutex_lock(&g_mu); int ph = J.phase; pthread_mutex_unlock(&g_mu);
    if(ph == J_COUNTING || (ph == J_RUNNING && g_hidden)) return;
    if(ph == J_RUNNING){
        if(!sys_modal_open()) g_prog_lbl = NULL;
        if(g_prog_lbl && lv_obj_is_valid(g_prog_lbl)){ char b[96]; pthread_mutex_lock(&g_mu); progress_text(b, sizeof b); pthread_mutex_unlock(&g_mu); lv_label_set_text(g_prog_lbl, b); }
        return;
    }
    lv_timer_delete(g_tm); g_tm = NULL; g_prog_lbl = NULL;
    if(ph == J_COUNTED){
        if(J.cancel){ pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu); return; }
        show_counted(); return;
    }
    if(ph != J_DONE) return;
    pthread_mutex_lock(&g_mu); J.phase = J_IDLE; pthread_mutex_unlock(&g_mu);
    if(g_hidden){ char b[600]; int ok; result_text(b, sizeof b, &ok); char *nl = strchr(b, '\n'); if(nl) *nl = 0; ui_toast(b); }
    else show_result();
    g_hidden = 0;
    if(!J.exp && J.prog.xfr > 0) ui_rescan_library();          /* the library follows the card */
    if(g_done) g_done();
}

/* ---------------------------------------------------------------- entry points */
static int busy_toast(void){ if(usbsync_busy() || (J.phase == J_COUNTED && sys_modal_open())){ ui_toast("A transfer is already running"); return 1; } return 0; }
static int path_join(char *out, size_t cap, const char *a, const char *b){
    int n = b && b[0] ? snprintf(out, cap, "%s/%s", a, b) : snprintf(out, cap, "%s", a);
    return n > 0 && (size_t)n < cap;
}
/* src = dir/name on one side; dst = the same path relative to the other side's root */
static int setup(int exp, const char *dir, const char *name, const ud_vol_t *v){
    memset(&J, 0, sizeof J);
    J.exp = exp;
    snprintf(J.dev, sizeof J.dev, "%s", v->dev); usbdrive_name(v, J.vname, sizeof J.vname); snprintf(J.vfs, sizeof J.vfs, "%s", v->fstype);
    const char *sroot = exp ? SD_ROOT : v->mnt, *troot = exp ? v->mnt : SD_ROOT;
    snprintf(J.troot, sizeof J.troot, "%s", troot);
    if(!path_join(J.src, sizeof J.src, dir, name)){ ui_toast("Path too long"); return -1; }
    snprintf(J.name, sizeof J.name, "%s", name ? name : "");
    struct stat st;
    J.src_is_dir = stat(J.src, &st) == 0 && S_ISDIR(st.st_mode);
    char rel[P];
    if(usc_rel(sroot, J.src_is_dir ? J.src : dir, rel, sizeof rel) != 0){ ui_toast("Can't copy from here"); return -1; }
    int n = snprintf(J.dst, sizeof J.dst, "%s%s", troot, rel);
    if(n <= 0 || (size_t)n >= sizeof J.dst){ ui_toast("Path too long"); return -1; }
    return 0;
}
void usbsync_import_open(const char *dir, const char *name, void (*done)(void)){
    if(busy_toast()) return;
    ud_vol_t v;
    if(usbdrive_find(dir, &v) < 0){ ui_toast("The drive is gone"); return; }
    if(setup(0, dir, name, &v) != 0) return;
    g_done = done; show_options();
}
static char g_pend_dir[P], g_pend_name[256];
static void (*g_pend_done)(void);
static void export_to(int vi){
    ud_vol_t v;
    if(!usbdrive_get(vi, &v)){ ui_toast("The drive is gone"); return; }
    if(setup(1, g_pend_dir, g_pend_name, &v) != 0) return;
    g_done = g_pend_done; show_options();
}
static void cb_pick_vol(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) export_to((int)(intptr_t)lv_event_get_user_data(e)); }
void usbsync_export_open(const char *dir, const char *name, void (*done)(void)){
    if(busy_toast()) return;
    int n = usbdrive_count();
    if(usbdrive_state() != UD_ON || n <= 0){ ui_toast("Connect a drive first: Files > USB drive"); return; }
    snprintf(g_pend_dir, sizeof g_pend_dir, "%s", dir); snprintf(g_pend_name, sizeof g_pend_name, "%s", name ? name : "");
    g_pend_done = done;
    if(n == 1){ export_to(0); return; }
    lv_obj_t *c = sys_modal_card("Export to which drive?", 250);                /* two or more volumes: choose */
    int y = th_zune() ? 110 : 48;
    for(int i = 0; i < n && i < 4; i++){
        ud_vol_t v; if(!usbdrive_get(i, &v)) break;
        char nm[64]; button(c, usbdrive_name(&v, nm, sizeof nm), 240, 0, y + i * 40, cb_pick_vol, (void *)(intptr_t)i, "usbsync.pick_drive");
    }
    sys_modal_pill(c, 0, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_close, "usbsync.cancel");
}

static char g_menu_mnt[40];
static void (*g_menu_done)(void);
static void cb_menu(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    sys_modal_close();
    if(a == 2){ usbdrive_stop(); if(g_menu_done) g_menu_done(); return; }
    ud_vol_t v; int vi = usbdrive_find(g_menu_mnt, &v);
    if(vi < 0){ ui_toast("The drive is gone"); return; }
    if(a == 0){ if(busy_toast()) return; if(setup(0, v.mnt, "", &v) == 0){ g_done = g_menu_done; show_options(); } }
    else { snprintf(g_pend_dir, sizeof g_pend_dir, SD_ROOT); g_pend_name[0] = 0; g_pend_done = g_menu_done; if(!busy_toast()) export_to(vi); }
}
void usbsync_volume_menu(const char *mnt, void (*done)(void)){
    ud_vol_t v;
    if(usbdrive_find(mnt, &v) < 0){ ui_toast("The drive is gone"); return; }
    snprintf(g_menu_mnt, sizeof g_menu_mnt, "%s", mnt); g_menu_done = done;
    char nm[64];
    lv_obj_t *c = sys_modal_card(usbdrive_name(&v, nm, sizeof nm), 250);
    int y = th_zune() ? 110 : 48;
    button(c, "Import everything", 240, 0, y, cb_menu, (void *)0, "usbsync.import_all");
    button(c, "Export everything", 240, 0, y + 42, cb_menu, (void *)1, "usbsync.export_all");
    button(c, "Eject", 240, 0, y + 84, cb_menu, (void *)2, "usbsync.eject");
    sys_modal_pill(c, 0, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_close, "usbsync.cancel");
}

void usbsync_preview(int step){
    g_preview = 1;
    memset(&J, 0, sizeof J);
    snprintf(J.vname, sizeof J.vname, "DISC"); snprintf(J.vfs, sizeof J.vfs, "ext4"); snprintf(J.dev, sizeof J.dev, "sdb2");
    snprintf(J.src, sizeof J.src, UD_ROOT "/sdb2/Agent Orange - Living in the Darkness (1981)");
    snprintf(J.name, sizeof J.name, "Agent Orange - Living in the Darkness (1981)");
    snprintf(J.troot, sizeof J.troot, SD_ROOT);
    snprintf(J.dst, sizeof J.dst, SD_ROOT "/Agent Orange - Living in the Darkness (1981)");
    snprintf(J.log, sizeof J.log, SD_ROOT "/synclog/2026-11-14_172700-import.log");
    J.src_is_dir = 1;
    g_update = step == 1; g_checksum = 0; g_backups = 1;
    if(step <= 1){ show_options(); return; }
    J.cnt.files_new = 12; J.cnt.files_changed = 3; J.cnt.bytes = 412000000ULL; J.free_b = 40100000000ULL; g_update = 1;
    if(step == 2){ show_counted(); return; }
    J.prog.pct = 42; J.prog.bytes = 173000000ULL; J.prog.xfr = 6;
    if(step == 3){ show_progress(); return; }
    if(step == 4){ J.prog.xfr = 15; show_result(); return; }
    sys_modal_close();
    lv_obj_t *c = sys_modal_card("DISC", 250);
    int y = th_zune() ? 110 : 48;
    button(c, "Import everything", 240, 0, y, cb_menu, (void *)0, "usbsync.import_all");
    button(c, "Export everything", 240, 0, y + 42, cb_menu, (void *)1, "usbsync.export_all");
    button(c, "Eject", 240, 0, y + 84, cb_menu, (void *)2, "usbsync.eject");
    sys_modal_pill(c, 0, "Cancel", THEME_CLR_TEXT_SECONDARY, cb_close, "usbsync.cancel");
}
