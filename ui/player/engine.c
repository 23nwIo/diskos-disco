/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "engine.h"
#include "mpdcli.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ENG_MAX_RESTARTS 5
#define ENG_TIMEOUT_MS   3000

static pid_t g_pid = -1;
static int g_cmd = -1, g_idle = -1, g_dry, g_restarts;
static eng_log_fn g_log;
/* MPD song ids of the two slots (see eng_load) and a paused start that has not been played yet */
static int g_cur = -1, g_next = -1, g_armed;
static long g_armed_ms;

/* MPD writes S32_LE like the stock player and nothing else touches the hardware; buffer and period come from MPD's
 * defaults with our period-first patch (mpd/patches). No database, no state file (resume is the player's job). */
static const char CONF[] =
    "bind_to_address \"" ENG_SOCK "\"\n"
    "log_file \"" ENG_RUN_DIR "/log\"\n"
    "log_level \"default\"\n"
    "pid_file \"" ENG_RUN_DIR "/pid\"\n"
    "audio_output {\n"
    "  type \"alsa\"\n"
    "  name \"dac\"\n"
    "  device \"hw:0,3\"\n"
    "  mixer_type \"none\"\n"
    "  auto_resample \"no\"\n"
    "  auto_format \"no\"\n"
    "  format \"*:32:*\"\n"
    "}\n";

static void disconnect(void){
    if(g_cmd >= 0) close(g_cmd);
    if(g_idle >= 0) close(g_idle);
    g_cmd = g_idle = -1;
}

/* MPD closes a client connection that has been silent for connection_timeout (60 s by default; a connection in "idle"
 * is exempt). The command connection is silent most of the time, so it is opened again whenever it is found closed. */
static int reconnect_cmd(void){
    if(g_pid <= 0) return -1;
    if(g_cmd >= 0) close(g_cmd);
    g_cmd = mc_connect(ENG_SOCK, ENG_TIMEOUT_MS);
    if(g_cmd < 0) g_log("engine: reconnect %s: %s", ENG_SOCK, strerror(errno));
    return g_cmd >= 0 ? 0 : -1;
}
/* every command goes through here: one retry on a fresh connection when the old one was closed */
static int xcmd(const char *c, char *out, size_t cap){
    if(g_cmd < 0 && reconnect_cmd() != 0) return -1;
    int x = mc_cmd(g_cmd, c, out, cap);
    if(x >= 0) return x;
    if(reconnect_cmd() != 0) return -1;
    x = mc_cmd(g_cmd, c, out, cap);
    if(x < 0){ g_log("engine: \"%s\": no answer from MPD", c); close(g_cmd); g_cmd = -1; }
    return x;
}

static int spawn(void){
    if(mkdir(ENG_RUN_DIR, 0755) != 0 && errno != EEXIST){ g_log("engine: mkdir %s: %s", ENG_RUN_DIR, strerror(errno)); return -1; }
    FILE *f = fopen(ENG_CONF, "we");
    if(!f || fputs(CONF, f) < 0){ g_log("engine: write %s: %s", ENG_CONF, strerror(errno)); if(f) fclose(f); return -1; }
    fclose(f);
    unlink(ENG_SOCK);
    pid_t p = fork();
    if(p < 0){ g_log("engine: fork: %s", strerror(errno)); return -1; }
    if(p == 0){
        execl(ENG_MPD_BIN, "mpd", "--no-daemon", ENG_CONF, (char *)NULL);
        _exit(127);
    }
    g_pid = p;
    g_log("engine: started %s (pid %d)", ENG_MPD_BIN, (int)p);
    /* MPD opens its socket within a fraction of a second; give it up to 5 s */
    for(int i = 0; i < 50; i++){
        g_cmd = mc_connect(ENG_SOCK, ENG_TIMEOUT_MS);
        if(g_cmd >= 0) break;
        usleep(100000);
    }
    if(g_cmd < 0){ g_log("engine: connect %s: %s", ENG_SOCK, strerror(errno)); return -1; }
    g_idle = mc_connect(ENG_SOCK, ENG_TIMEOUT_MS);
    if(g_idle < 0 || mc_idle_begin(g_idle) != 0){ g_log("engine: idle connection: %s", strerror(errno)); disconnect(); return -1; }
    char r[256];
    xcmd("consume 0", r, sizeof r);
    xcmd("single 0", r, sizeof r);
    g_log("engine: connected");
    return 0;
}

int eng_start(int dry, eng_log_fn log){
    g_log = log; g_dry = dry;
    if(dry){ log("engine: dry run, no MPD"); return -1; }
    if(access(ENG_MPD_BIN, X_OK) != 0){ log("engine: %s: %s - no sound", ENG_MPD_BIN, strerror(errno)); return -1; }
    return spawn();
}

void eng_stop(void){
    disconnect();
    if(g_pid > 0){ kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = -1; }
}

int eng_ready(void){ return g_pid > 0 && g_idle >= 0; }
int eng_idle_fd(void){ return g_idle; }

int eng_idle_event(void){
    if(g_idle < 0) return -1;
    int ev = mc_idle_end(g_idle);
    if(ev < 0 || mc_idle_begin(g_idle) != 0){
        /* open it again; MPD itself going away is noticed by eng_check */
        g_log("engine: idle connection lost, reconnecting");
        close(g_idle);
        g_idle = mc_connect(ENG_SOCK, ENG_TIMEOUT_MS);
        if(g_idle >= 0 && mc_idle_begin(g_idle) != 0){ close(g_idle); g_idle = -1; }
        if(g_idle < 0){ g_log("engine: idle connection: %s - no engine from now on", strerror(errno)); return -1; }
        return MC_EV_PLAYER;                     /* something may have changed meanwhile */
    }
    return ev;
}

void eng_check(void){
    if(g_pid <= 0) return;
    int st;
    pid_t r = waitpid(g_pid, &st, WNOHANG);
    if(r != g_pid) return;
    g_log("engine: MPD exited (%s %d)", WIFEXITED(st) ? "status" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    g_pid = -1; disconnect();
    g_cur = g_next = -1; g_armed = 0;                    /* its queue is gone with it */
    if(++g_restarts > ENG_MAX_RESTARTS){ g_log("engine: too many restarts, no sound from now on"); return; }
    spawn();
}

static int cmd(const char *c){
    char r[512];
    int x = xcmd(c, r, sizeof r);
    if(x > 0) g_log("engine: \"%s\": %s", c, r);
    return x == 0 ? 0 : -1;
}

int eng_cmd(const char *c, char *out, unsigned cap){
    return xcmd(c, out, cap);
}

/* MPD holds at most two songs: the current one and, so that it can play gaplessly, the one after it. The queue itself
 * is ours (queue.c, LIST_SONG_0/3 in song.db); g_cur/g_next are the MPD song ids of the two slots. A paused start
 * (resume) does not touch MPD at all until play is pressed ("armed"), so nothing is heard and the DAC stays idle. */

static int addid(const char *path){
    char uri[1060], q[1100], line[1200], r[256], v[32];
    snprintf(uri, sizeof uri, "file://%s", path);
    if(mc_quote(q, sizeof q, uri) < 0){ g_log("engine: path too long: %s", path); return -1; }
    snprintf(line, sizeof line, "addid %s", q);
    int x = xcmd(line, r, sizeof r);
    if(x != 0){ g_log("engine: add %s: %s", path, x > 0 ? r : "no answer"); return -1; }
    return mc_get(r, "Id", v, sizeof v) ? atoi(v) : -1;
}

static int start_cur(long ms){
    char line[64];
    if(ms > 0) snprintf(line, sizeof line, "seekid %d %ld.%03ld", g_cur, ms / 1000, ms % 1000);
    else snprintf(line, sizeof line, "playid %d", g_cur);
    return cmd(line);
}

int eng_load(const char *path, long start_ms, int paused){
    if(!eng_ready()) return -1;
    g_armed = 0; g_cur = g_next = -1;
    if(cmd("clear")) return -1;
    g_cur = addid(path);
    if(g_cur < 0) return -1;
    if(paused){ g_armed = 1; g_armed_ms = start_ms > 0 ? start_ms : 0; return 0; }
    return start_cur(start_ms);
}

int eng_set_next(const char *path){
    if(!eng_ready()) return -1;
    if(g_next >= 0){
        char line[48]; snprintf(line, sizeof line, "deleteid %d", g_next);
        char r[256]; xcmd(line, r, sizeof r);                   /* already gone is fine */
        g_next = -1;
    }
    if(!path) return 0;
    g_next = addid(path);
    return g_next >= 0 ? 0 : -1;
}

int eng_advanced(const eng_status_t *s){
    if(g_next < 0 || s->songid != g_next) return 0;
    if(g_cur >= 0){
        char line[48], r[256]; snprintf(line, sizeof line, "deleteid %d", g_cur);
        xcmd(line, r, sizeof r);
    }
    g_cur = g_next; g_next = -1;
    return 1;
}

int eng_toggle(void){
    if(g_armed){ g_armed = 0; return start_cur(g_armed_ms); }
    eng_status_t s;
    if(eng_status(&s) != 0) return -1;
    if(s.state == ENG_STOP) return g_cur >= 0 ? start_cur(0) : -1;
    return cmd(s.state == ENG_PLAY ? "pause 1" : "pause 0");
}
int eng_pause(int on){
    if(g_armed) return on ? 0 : eng_toggle();
    return cmd(on ? "pause 1" : "pause 0");
}
int eng_seek_ms(long ms){
    if(g_armed){ g_armed_ms = ms; return 0; }
    char c[64];
    snprintf(c, sizeof c, "seekcur %ld.%03ld", ms / 1000, ms % 1000);
    return cmd(c);
}

int eng_status(eng_status_t *st){
    memset(st, 0, sizeof *st);
    st->song = st->songid = -1;
    if(!eng_ready()) return -1;
    char r[MC_RESP_MAX] = "", v[1100];
    if(xcmd("status", r, sizeof r) != 0){ g_log("engine: status: %s", r[0] ? r : "no answer"); return -1; }
    if(mc_get(r, "state", v, sizeof v)) st->state = !strcmp(v, "play") ? ENG_PLAY : !strcmp(v, "pause") ? ENG_PAUSE : ENG_STOP;
    if(mc_get(r, "elapsed", v, sizeof v)) st->elapsed_ms = (long)(atof(v) * 1000.0 + 0.5);
    if(mc_get(r, "duration", v, sizeof v)) st->duration_ms = (long)(atof(v) * 1000.0 + 0.5);
    if(mc_get(r, "song", v, sizeof v)) st->song = atoi(v);
    if(mc_get(r, "playlistlength", v, sizeof v)) st->songs = atoi(v);
    if(mc_get(r, "songid", v, sizeof v)) st->songid = atoi(v);
    {   /* MPD's own error (e.g. the output could not be opened): logged once per new text */
        static char last_err[256];
        char e[256] = "";
        mc_get(r, "error", e, sizeof e);
        if(strcmp(e, last_err)){ if(e[0]) g_log("engine: MPD error: %s", e); snprintf(last_err, sizeof last_err, "%s", e); }
    }
    if(mc_get(r, "bitrate", v, sizeof v)) st->kbps = atoi(v);
    if(mc_get(r, "audio", v, sizeof v)){
        char *p; st->rate = (unsigned)strtoul(v, &p, 10);
        if(*p == ':'){ st->bits = (int)strtol(p + 1, &p, 10); if(*p == ':') st->channels = (int)strtol(p + 1, NULL, 10); }
    }
    if(st->song >= 0 && xcmd("currentsong", r, sizeof r) == 0 && mc_get(r, "file", v, sizeof v)){
        const char *p = !strncmp(v, "file://", 7) ? v + 7 : v;
        snprintf(st->file, sizeof st->file, "%s", p);
        if(!st->rate && mc_get(r, "Format", v, sizeof v)) st->rate = (unsigned)strtoul(v, NULL, 10);
    }
    if(g_armed && st->state == ENG_STOP){ st->state = ENG_PAUSE; st->elapsed_ms = g_armed_ms; }   /* a paused start */
    return 0;
}
