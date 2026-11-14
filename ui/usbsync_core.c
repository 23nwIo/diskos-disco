/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "usbsync_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Never copied in either direction: the backups and logs of earlier runs, each player's own cache (.diskos: covers,
 * Album Roulette), diskOS temp files, and the ext4 root's lost+found. */
static const char *const EXCLUDES[] = {
    "--exclude=_changed[0-9]*/", "--exclude=synclog/", "--exclude=.diskos/", "--exclude=.diskos-*", "--exclude=lost+found/",
};

int usc_argv(const usc_opts_t *o, char **argv, char *buf, size_t cap){
    int n = 0; size_t used = 0;
#define ARG(...) do{                                                              \
        if(n >= USC_ARGV_MAX - 1) return -1;                                      \
        int w_ = snprintf(buf + used, cap - used, __VA_ARGS__);                   \
        if(w_ < 0 || (size_t)w_ >= cap - used) return -1;                         \
        argv[n++] = buf + used; used += (size_t)w_ + 1;                           \
    }while(0)
    if(!o || !o->rsync || !o->src || !o->dst || o->src[0] != '/' || o->dst[0] != '/') return -1;
    ARG("%s", o->rsync);
    ARG("-rt");                       /* recursive, keep modification times (what the quick check compares); no owners/permissions */
    ARG("--modify-window=2");         /* FAT stores times in 2 s steps: the same file must not look changed */
    ARG("--mkpath");                  /* create the target folder and its parents */
    for(unsigned k = 0; k < sizeof EXCLUDES / sizeof EXCLUDES[0]; k++) ARG("%s", EXCLUDES[k]);
    if(!o->update) ARG("--ignore-existing");
    else {
        if(o->checksum) ARG("--checksum");
        if(o->backup_dir){ ARG("--backup"); ARG("--backup-dir=%s", o->backup_dir); }
    }
    if(o->dry_run){ ARG("--dry-run"); ARG("--itemize-changes"); ARG("--stats"); }
    else { ARG("--no-inc-recursive"); ARG("--info=progress2"); }   /* whole-run percentage: the file list comes first */
    if(o->log_file) ARG("--log-file=%s", o->log_file);
    ARG("--");
    size_t sl = strlen(o->src), dl = strlen(o->dst);
    if(o->src_is_dir) ARG("%s%s", o->src, sl && o->src[sl - 1] == '/' ? "" : "/");   /* the folder's contents ... */
    else ARG("%s", o->src);
    ARG("%s%s", o->dst, dl && o->dst[dl - 1] == '/' ? "" : "/");                     /* ... into dst */
    argv[n] = NULL;
#undef ARG
    return n;
}

static unsigned long long num_commas(const char *s, const char **end){
    unsigned long long v = 0; int any = 0;
    while(*s == ' ') s++;
    for(; *s; s++){
        if(*s >= '0' && *s <= '9'){ v = v * 10 + (unsigned)(*s - '0'); any = 1; }
        else if(*s == ',' || *s == '.') { if(!any) break; }      /* thousands separators (locale: ',' or '.') */
        else break;
    }
    if(end) *end = s;
    return any ? v : 0;
}
void usc_count_line(const char *l, usc_count_t *c){
    if(!l || !c) return;
    if((l[0] == '>' || l[0] == 'c') && l[1] == 'f' && strlen(l) > 12 && l[11] == ' '){
        int fresh = 1; for(int k = 2; k < 11; k++) if(l[k] != '+') fresh = 0;
        if(fresh) c->files_new++; else c->files_changed++;
        return;
    }
    static const char K[] = "Total transferred file size:";
    if(!strncmp(l, K, sizeof K - 1)){ c->bytes = num_commas(l + sizeof K - 1, NULL); c->have_bytes = 1; }
}

int usc_parse_progress(const char *l, usc_progress_t *p){
    const char *e;
    while(*l == ' ') l++;
    if(!(*l >= '0' && *l <= '9')) return 0;
    unsigned long long b = num_commas(l, &e);
    while(*e == ' ') e++;
    char *pe; long pct = strtol(e, &pe, 10);
    if(pe == e || *pe != '%' || pct < 0 || pct > 100) return 0;
    p->bytes = b; p->pct = (int)pct;
    const char *x = strstr(pe, "xfr#"); if(x) p->xfr = strtol(x + 4, NULL, 10);
    const char *k = strstr(pe, "to-chk="); if(!k) k = strstr(pe, "ir-chk=");
    if(k){ char *s; p->chk_left = strtol(k + 7, &s, 10); if(*s == '/') p->chk_total = strtol(s + 1, NULL, 10); }
    return 1;
}

const char *usc_exit_text(int code){
    switch(code){
        case 0:  return "Done";
        case 11: return "Write error - is the target full?";
        case 20: return "Cancelled";
        case 23: return "Some files could not be copied (names or sizes the target can't store?) - see synclog";
        case 24: return "Some source files vanished during the copy";
        case 3: case 12: return "Couldn't read or create the folders - see synclog";
        case 127: return "rsync not found";
        default: return "Transfer failed - see synclog";
    }
}

int usc_backup_index(const char *root){
    for(int i = 0; i < 10000; i++){
        char p[1024]; struct stat st;
        snprintf(p, sizeof p, "%s/_changed%d", root, i);
        if(stat(p, &st) != 0) return i;
    }
    return -1;
}

int usc_rel(const char *root, const char *path, char *out, size_t cap){
    size_t n = strlen(root);
    while(n > 1 && root[n - 1] == '/') n--;
    if(strncmp(root, path, n) != 0 || (path[n] != 0 && path[n] != '/')) return -1;
    const char *r = path + n;
    size_t rl = strlen(r);
    while(rl > 1 && r[rl - 1] == '/') rl--;
    if(rl == 1 && r[0] == '/') rl = 0;
    if(rl + 1 > cap) return -1;
    memcpy(out, r, rl); out[rl] = 0;
    return 0;
}

void usc_log_name(const char *root, long t, const char *what, char *out, size_t cap){
    time_t tt = (time_t)t; struct tm tm; localtime_r(&tt, &tm);
    snprintf(out, cap, "%s/synclog/%04d-%02d-%02d_%02d%02d%02d-%s.log", root, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, what);
}
