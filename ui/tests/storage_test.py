#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Run the actual handoff controller with simulated IPC, mounts and gadget state.

The filesystem marker and SD admission implementation are real. Only hardware,
LVGL and running-player inspection are replaced at their boundaries.
"""
import os
from pathlib import Path
import subprocess
import tempfile

app = Path(__file__).resolve().parents[1]
source = (app / "main.c").read_text()
source = source[source.index("/* SD operations share"):source.index("static int book_session_active(void);")]
a = source.index("static int storage_player_guarded(void){")
b = source.index("static int source_send(", a)
source = source[:a] + "static int storage_player_guarded(void){ return test_guarded; }\n" + source[b:]
source = source.replace("static int rmguard_dir_ok(const char *dir);", "")

def run_harness(exe, timeout):
    """Run a compiled harness; its LAST stdout line must be HARNESS COMPLETE, so an early return 0 fails."""
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    if r.returncode != 0:
        raise SystemExit(f'FAIL harness exited {r.returncode}')
    lines = r.stdout.strip().splitlines()
    if not lines or lines[-1] != 'HARNESS COMPLETE':
        raise SystemExit('FAIL harness did not reach its HARNESS COMPLETE line')


with tempfile.TemporaryDirectory(prefix="diskos-storage-tests-") as tmp:
    root = Path(tmp)
    source = source.replace("/tmp/.diskos_sd_export_intent", str(root / "handoff"))
    headers = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include "sdio.h"
typedef void lv_timer_t;
static uint32_t test_clock;
static int test_mounted=1, test_exported, test_host, test_guarded=1, test_scan;
static int test_sends, test_fail, test_export_sends, test_seed, test_ext, test_restarts;
void settings_restart_now(void){ test_restarts++; }
static int g_playing;
static _Atomic int g_source_mode;
static uint32_t lv_tick_get(void){ return test_clock; }
static void lv_timer_set_period(lv_timer_t *t, uint32_t p){ (void)t; (void)p; }
static void ui_toast(const char *s){ (void)s; }
static void art_cancel(void){}
static void discshelf_stop(void){}
static void ui_route_forget(const char*why){(void)why;}
void bt_autoroute_reset(void){}
static void ma_mode_leaving(void){}
static void radio_mode_leaving(void){}
static void modes_output_reset(void){}   /* output routing is not under test here */
enum { OUT_INTERNAL=0, OUT_USB=2 };
static int test_output_route, test_restores;
static int modes_output_route(void){return test_output_route;}
static int modes_output_mode_switch(int target){assert(target==OUT_INTERNAL);test_restores++;return 0;}
static int modes_output_busy(void){ return 0; }
static int scanner_active(void){ return test_scan; }
static int scanner_start(void){ return 0; }
static int mdb_total_song_count(void){ return 1; }
static int mdb_load_failed(void){ return 0; }
static void albumwall_prewarm_seed(void){ test_seed++; }
static int ipc_send_cmd(const char *s){
    test_sends++;
    if(test_fail) return -1;
    if(!strcmp(s,"0642000C0001")){
        assert(!sd_io_allowed() && sd_io_active()==0);
        test_export_sends++;
    }
    return 0;
}
'''
    tests = r'''
static int sd_exported_to_host(void){ return test_exported; }
static int coldplug_mounted(void){ return test_mounted; }
/* Mirrors the real predicate in main.c, INCLUDING the launch verdict. Leaving the verdict out of this
 * stub is exactly what let a pending-verdict admission gap pass review. */
static int storage_media_local(void){
    if(g_launch_verdict != 1) return 0;
    return !g_sd_hold && test_mounted && !test_exported;
}
static int storage_host_confirmed(void){ return test_host; }
/* Mirrors the real predicate: released by the host, not mounted, ext2/3/4 superblock. */
static int sd_return_needs_restart(void){ return !test_exported && !test_mounted && test_ext; }
static void reset(void){
    g_launch_verdict = 1;   /* a confirmed launch; the gate itself is covered by launcher_test */
    assert(sd_io_active()==0);
    unlink(SD_EXPORT_MARKER);
    test_clock=0; test_mounted=1; test_exported=0; test_host=0;
    test_guarded=1; test_scan=0; test_sends=0; test_fail=0; test_export_sends=0; test_seed=0; test_ext=0; test_restarts=0;
    g_sd_hold=0; g_sd_phase=SD_LOCAL; g_source_mode=0; g_sd_reissue=0;
    atomic_store(&g_sd_writable,1); atomic_store(&g_sd_writers,0);
    storage_init(); assert(sd_io_allowed() && ui_local_playback_allowed());
}
static void enter_host(void){
    assert(ui_set_source_mode(3)==0);
    storage_tick(NULL);
    assert(g_sd_phase==SD_WAIT_HOST && test_export_sends==1);
    test_mounted=0; test_exported=1; test_host=1;
    storage_tick(NULL); assert(g_sd_phase==SD_HOST);
}
int main(void){
    /* Until the launcher publishes a positive verdict for THIS player, nothing may touch the card:
     * no lease, no resume, and no playback - whatever the mount and gadget state look like. */
    reset();
    g_launch_verdict = -1;
    assert(!sd_io_begin());
    assert(!sd_io_resume());
    assert(!ui_local_playback_allowed());
    g_launch_verdict = 0;
    assert(!sd_io_begin());
    assert(!sd_io_resume());
    assert(!ui_local_playback_allowed());
    puts("PASS a pending or negative launch verdict closes leases, resume and playback");

    reset();
    assert(sd_io_begin());
    assert(ui_set_source_mode(3)==0);
    assert(!sd_io_begin());
    storage_tick(NULL); assert(test_sends==0 && g_sd_phase==SD_DRAIN);
    sd_io_end(); storage_tick(NULL);
    assert(g_sd_phase==SD_WAIT_HOST && test_export_sends==1);
    storage_tick(NULL); assert(g_sd_phase==SD_WAIT_HOST && !sd_io_allowed());
    test_mounted=0; test_exported=1; test_host=1; storage_tick(NULL);
    assert(g_sd_phase==SD_HOST && !sd_io_begin());
    assert(ui_set_source_mode(0)==0);
    test_host=0; test_mounted=1; storage_tick(NULL);
    assert(g_sd_phase==SD_WAIT_LOCAL && !sd_io_allowed()); /* LUN still attached */
    test_exported=0; storage_tick(NULL);
    assert(g_sd_phase==SD_LOCAL && sd_io_begin()); sd_io_end();
    assert(sd_write_begin()); sd_write_end();
    assert(test_seed==1 && access(SD_EXPORT_MARKER,F_OK)!=0);
    puts("PASS drain, export acknowledgement, detach, mount and cache resume");

    reset(); test_guarded=0;
    assert(ui_set_source_mode(3)<0 && test_sends==0 && sd_io_allowed());
    test_guarded=1; test_scan=1;
    assert(ui_set_source_mode(3)<0 && test_sends==0 && sd_io_allowed());
    puts("PASS unprotected player and active scan reject before pausing media");

    reset(); assert(ui_set_source_mode(3)==0); test_fail=1; storage_tick(NULL);
    assert(g_sd_phase==SD_UNKNOWN && !sd_io_allowed());
    test_fail=0; test_clock=50000; storage_tick(NULL);
    assert(!sd_io_allowed() && access(SD_EXPORT_MARKER,F_OK)==0);
    puts("PASS failed IPC cannot reopen the card");

    reset(); assert(ui_set_source_mode(3)==0); storage_tick(NULL);
    test_clock=25000; storage_tick(NULL);
    assert(g_sd_phase==SD_UNKNOWN && !sd_io_allowed());
    puts("PASS elapsed time is not export completion");

    reset(); assert(ui_set_source_mode(3)==0); storage_tick(NULL);
    g_sd_phase=SD_LOCAL; storage_init();
    assert(g_sd_phase==SD_WAIT_HOST && !sd_io_allowed());
    test_mounted=0; test_exported=1; test_host=1; storage_tick(NULL);
    assert(g_sd_phase==SD_HOST);
    g_sd_phase=SD_LOCAL; storage_init();
    assert(g_sd_phase==SD_HOST && !sd_io_allowed());
    assert(ui_set_source_mode(0)==0);
    g_sd_phase=SD_LOCAL; storage_init();
    assert(g_sd_phase==SD_WAIT_LOCAL && !sd_io_allowed());
    test_mounted=1; test_exported=0; test_host=0; storage_tick(NULL);
    assert(g_sd_phase==SD_LOCAL && sd_io_allowed());
    puts("PASS restart restores pending, exported and returning ownership");

    reset(); assert(storage_mark('E',3));
    storage_init(); assert(g_sd_phase==SD_WAIT_HOST && !sd_io_allowed());
    storage_tick(NULL); assert(test_export_sends==1 && !g_sd_reissue);
    test_mounted=0; test_exported=1; test_host=1; storage_tick(NULL);
    assert(g_sd_phase==SD_HOST && storage_mark('R',0));
    storage_init(); int sent=test_sends; storage_tick(NULL);
    assert(test_sends>sent && !g_sd_reissue && g_sd_phase==SD_WAIT_LOCAL);
    puts("PASS crash between marker publication and IPC reissues the saved request");

    reset(); assert(storage_mark('D',3)); test_mounted=0;
    storage_init(); assert(g_sd_phase==SD_LOCAL && !sd_io_allowed());
    test_mounted=1; storage_tick(NULL); assert(sd_io_allowed());
    puts("PASS interrupted drain waits for coldplug without a permanent hold");

    /* An ext4 card: the stock player cannot remount it after USB storage, so once the host has let go the
     * UI restarts (the boot-time direct mount handles ext4) - never while the LUN still backs the card. */
    reset(); test_ext=1; enter_host();
    assert(ui_set_source_mode(0)==0 && g_sd_phase==SD_WAIT_LOCAL);
    test_host=0; test_clock+=6000; storage_tick(NULL);
    assert(test_restarts==0 && g_sd_phase==SD_WAIT_LOCAL);            /* still exported: no restart */
    test_exported=0; test_clock+=1000; storage_tick(NULL);
    assert(test_restarts==1 && g_sd_phase==SD_UNKNOWN && !sd_io_allowed());
    test_clock+=30000; storage_tick(NULL); assert(test_restarts==1);  /* one restart request only */
    puts("PASS returned ext4 card restarts once the host releases it");

    reset(); test_ext=1; enter_host();
    assert(ui_set_source_mode(0)==0);
    test_host=0; test_exported=0; storage_tick(NULL);
    assert(test_restarts==0 && g_sd_phase==SD_WAIT_LOCAL);            /* the first 5 s belong to the player */
    test_mounted=1; test_clock+=2000; storage_tick(NULL);
    assert(test_restarts==0 && g_sd_phase==SD_LOCAL);
    puts("PASS a card the player remounts in time is resumed without a restart");

    reset(); enter_host();
    assert(ui_set_source_mode(0)==0);
    test_host=0; test_exported=0; test_clock+=25000; storage_tick(NULL);
    assert(test_restarts==0 && g_sd_phase==SD_UNKNOWN);
    puts("PASS a FAT card that does not come back stays held without a restart");

    reset(); enter_host();
    assert(ui_set_source_mode(3)==0 && test_export_sends==1);
    assert(ui_set_source_mode(0)==0);
    assert(ui_set_source_mode(3)<0 && test_export_sends==1);
    puts("PASS repeated requests cannot queue a late export behind return");

    reset();test_output_route=OUT_USB;assert(ui_set_source_mode(3)<0&&test_export_sends==0);assert(ui_set_source_mode(0)==0&&test_restores==1);test_output_route=OUT_INTERNAL;
    puts("PASS USB output must restore headphones before another source or SD export");
    reset(); FILE *f=fopen(SD_EXPORT_MARKER,"w"); assert(f); fclose(f);
    storage_init(); assert(g_sd_phase==SD_UNKNOWN && !sd_io_allowed());
    puts("PASS old or malformed handoff marker remains held");
    reset(); assert(ui_set_source_mode(3)==0); sd_io_fault(); storage_tick(NULL);
    assert(g_sd_phase==SD_UNKNOWN && test_export_sends==0 && !sd_io_resume());
    storage_init(); assert(!sd_io_resume() && !ui_local_playback_allowed());
    puts("PASS unreaped decoder prevents export and survives UI restart");
    puts("HARNESS COMPLETE");
    return 0;
}
'''
    c = root / "test.c"
    c.write_text(headers + source + tests)
    exe = root / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-pthread", '-DSD_IO_FAULT_MARKER="' + str(root / "fault") + '"', "-I", str(app),
                    str(c), str(app / "sdio.c"), "-o", str(exe)], check=True)
    run_harness(exe, 10)

print('COMPLETE storage_test.py')   # last line: the runner requires it, so an early clean exit cannot pass
