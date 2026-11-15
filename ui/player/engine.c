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
    mc_cmd(g_cmd, "consume 0", r, sizeof r);
    mc_cmd(g_cmd, "single 0", r, sizeof r);
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

int eng_ready(void){ return g_cmd >= 0; }
int eng_idle_fd(void){ return g_idle; }

int eng_idle_event(void){
    if(g_idle < 0) return -1;
    int ev = mc_idle_end(g_idle);
    if(ev < 0 || mc_idle_begin(g_idle) != 0){ g_log("engine: idle connection lost"); disconnect(); return -1; }
    return ev;
}

void eng_check(void){
    if(g_pid <= 0) return;
    int st;
    pid_t r = waitpid(g_pid, &st, WNOHANG);
    if(r != g_pid) return;
    g_log("engine: MPD exited (%s %d)", WIFEXITED(st) ? "status" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
    g_pid = -1; disconnect();
    if(++g_restarts > ENG_MAX_RESTARTS){ g_log("engine: too many restarts, no sound from now on"); return; }
    spawn();
}

static int cmd(const char *c){
    if(g_cmd < 0) return -1;
    char r[512];
    int x = mc_cmd(g_cmd, c, r, sizeof r);
    if(x != 0) g_log("engine: \"%s\": %s", c, x > 0 ? r : "no answer");
    if(x < 0) disconnect();
    return x == 0 ? 0 : -1;
}

int eng_cmd(const char *c, char *out, unsigned cap){
    if(g_cmd < 0) return -1;
    int x = mc_cmd(g_cmd, c, out, cap);
    if(x < 0) disconnect();
    return x;
}

int eng_play_files(const char *const *paths, int n, int start, long start_ms, int start_paused){
    if(g_cmd < 0) return -1;
    if(cmd("clear")) return -1;
    char line[1200], q[1100];
    int added = 0;
    for(int i = 0; i < n; i++){
        char uri[1060];
        snprintf(uri, sizeof uri, "file://%s", paths[i]);
        if(mc_quote(q, sizeof q, uri) < 0){ g_log("engine: path too long: %s", paths[i]); continue; }
        snprintf(line, sizeof line, "add %s", q);
        if(cmd(line) == 0) added++;
        else if(g_cmd < 0) return -1;
    }
    if(!added) return -1;
    if(start < 0 || start >= added) start = 0;
    if(start_ms > 0) snprintf(line, sizeof line, "seek %d %ld.%03ld", start, start_ms / 1000, start_ms % 1000);
    else snprintf(line, sizeof line, "play %d", start);
    if(cmd(line)) return -1;
    if(start_ms > 0 && !start_paused) cmd("pause 0");
    if(start_paused) cmd("pause 1");
    return 0;
}

int eng_toggle(void){
    eng_status_t s;
    if(eng_status(&s) != 0) return -1;
    if(s.state == ENG_STOP) return s.songs > 0 ? cmd("play") : -1;
    return cmd(s.state == ENG_PLAY ? "pause 1" : "pause 0");
}
int eng_pause(int on){ return cmd(on ? "pause 1" : "pause 0"); }
int eng_next(void){ return cmd("next"); }
int eng_prev(void){ return cmd("previous"); }
int eng_seek_ms(long ms){
    char c[64];
    snprintf(c, sizeof c, "seekcur %ld.%03ld", ms / 1000, ms % 1000);
    return cmd(c);
}

int eng_status(eng_status_t *st){
    memset(st, 0, sizeof *st);
    st->song = -1;
    if(g_cmd < 0) return -1;
    char r[MC_RESP_MAX], v[1100];
    if(mc_cmd(g_cmd, "status", r, sizeof r) != 0){ disconnect(); return -1; }
    if(mc_get(r, "state", v, sizeof v)) st->state = !strcmp(v, "play") ? ENG_PLAY : !strcmp(v, "pause") ? ENG_PAUSE : ENG_STOP;
    if(mc_get(r, "elapsed", v, sizeof v)) st->elapsed_ms = (long)(atof(v) * 1000.0 + 0.5);
    if(mc_get(r, "duration", v, sizeof v)) st->duration_ms = (long)(atof(v) * 1000.0 + 0.5);
    if(mc_get(r, "song", v, sizeof v)) st->song = atoi(v);
    if(mc_get(r, "playlistlength", v, sizeof v)) st->songs = atoi(v);
    if(mc_get(r, "audio", v, sizeof v)) st->rate = (unsigned)strtoul(v, NULL, 10);
    if(st->song >= 0 && mc_cmd(g_cmd, "currentsong", r, sizeof r) == 0 && mc_get(r, "file", v, sizeof v)){
        const char *p = !strncmp(v, "file://", 7) ? v + 7 : v;
        snprintf(st->file, sizeof st->file, "%s", p);
        if(!st->rate && mc_get(r, "Format", v, sizeof v)) st->rate = (unsigned)strtoul(v, NULL, 10);
    }
    return 0;
}
