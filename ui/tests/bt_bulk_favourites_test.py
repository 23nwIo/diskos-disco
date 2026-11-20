#!/usr/bin/env python3
"""Compile the production Bluetooth resume guard, bulk Favourites action and favourite DB functions; fake only the
device/UI boundaries. Contributed with the 2.5.6 review and adapted to the shipped behaviour: after a codec switch the
guard only shows a hint; after a re-route it plays once, then hints; it never records a codec failure.
"""
from pathlib import Path
import subprocess
import tempfile
import sys
ui = Path(__file__).resolve().parents[1]
baseline = False

def fn(src, signature):
    start = src.index(signature)
    opening = src.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]

main = (ui/'main.c').read_text()
guard = main[main.index('static uint32_t g_rg_until'):main.index('static char g_play_initpath')]
g = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ipc.h"
static uint32_t now=100;static unsigned gen=1;
static int g_playing=1,g_play_pending,sends,failed,notices;
static char route[20]="AA:BB:CC:DD:EE:FF";
static track_state_t current={.have_track=1,.path="/music/a.flac",.pos_id=1,.path_seq=1,.position_ms=10000,.duration_ms=100000};
static uint32_t lv_tick_get(void){return now;}
static uint32_t lv_tick_elaps(uint32_t t){return now-t;}
unsigned ipc_generation(void){return gen;}
static const char *ui_route_mac(void){return route;}
void ipc_get_state(track_state_t *s){*s=current;}
int ipc_send_cmd(const char *s){(void)s;sends++;return 0;}
void bt_codec_note_failed(const char *s){(void)s;failed++;}
static int ui_bt_codec_frame(const char *m,char *o,int cap){(void)m;snprintf(o,cap,"SBC");return 0;}
static void ui_toast(const char *s){(void)s;notices++;}
''' + guard + r'''
static void stall(void){now+=100;resume_guard_tick(0);now+=3600;resume_guard_tick(0);}
int main(void){
 ui_resume_guard_arm(2,route);stall();stall();
 assert(sends==0 && failed==0 && notices==1);          /* codec switch: a hint only, once */
 /* P01 (delayed-pause report): after a route too, a stalled position is only a hint - a toggle could PAUSE music that
  * is still playing with late position reports */
 notices=0;now=10000;ui_resume_guard_arm(1,route);stall();assert(sends==0 && notices==1 && failed==0);
 sends=0;
 notices=0;now=20000;ui_resume_guard_arm(1,route);route[0]=0;stall();assert(!notices && !sends);
 strcpy(route,"AA:BB:CC:DD:EE:FF");now=40000;ui_resume_guard_arm(2,route);gen++;stall();assert(!notices);
 now=60000;ui_resume_guard_arm(2,route);current.pos_id++;stall();assert(!notices);
 now=80000;ui_resume_guard_arm(2,route);current.path_seq++;stall();assert(!notices);
 now=100000;ui_resume_guard_arm(2,route);strcpy(current.path,"/music/b.flac");stall();assert(!notices);
 now=120000;ui_resume_guard_arm(2,route);current.position_ms=current.duration_ms;stall();assert(!notices);
 current.position_ms=10000;now=140000;ui_resume_guard_arm(2,route);ui_resume_guard_cancel();stall();assert(!notices);
 now=160000;ui_resume_guard_arm(2,route);now+=11000;resume_guard_tick(0);assert(!notices);
 now=180000;ui_resume_guard_arm(2,route);resume_guard_tick(0);now+=2000;resume_guard_tick(1);now+=2000;resume_guard_tick(0);assert(!notices);
 puts("PASS BT: after a codec switch or a route a stalled position only hints, never a toggle; never a codec failure; stale route/generation/track, EOF, cancel, expiry and transient stall suppressed");
}
'''
db = (ui/'musicdb.c').read_text()
f = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
static sqlite3 *handle;static int admitted=1,leases,exits;static char toast[100];
static sqlite3 *db(void){return handle;}
static sqlite3 *db_read(void){return handle;}
static int sd_io_begin(void){if(!admitted)return 0;leases++;return 1;}
static void sd_io_end(void){assert(leases>0);leases--;}
static const char *colt(sqlite3_stmt *s,int n){const char *p=(const char*)sqlite3_column_text(s,n);return p?p:"";}
static void ui_toast(const char *s){snprintf(toast,sizeof toast,"%s",s);}
typedef struct mp_host {uint8_t *sel;int cap;int (*collect)(const uint8_t*,int,int**);} mp_host_t;
static void mp_exit(mp_host_t *h){(void)h;exits++;}
static int chosen=1;
static int collect(const uint8_t *s,int cap,int **out){(void)s;(void)cap;*out=malloc(chosen*sizeof **out);for(int i=0;i<chosen;i++)(*out)[i]=i+1;return chosen;}
'''
for sig in ('int mdb_song_path(', 'static int mdb_is_favorite_path_leased(', 'int mdb_is_favorite_path(', 'static int mdb_set_favorite_path_leased(', 'int mdb_set_favorite_path('):
    f += '\n'+fn(db, sig)
mp = (ui/'multipick.c').read_text()
f += '\n'+fn(mp,'static int songs_of(')+'\n'+fn(mp,'static void act_favourite(')
f += r'''
static void sql(const char *s){assert(sqlite3_exec(handle,s,0,0,0)==SQLITE_OK);}
static int rows(void){sqlite3_stmt *s;assert(sqlite3_prepare_v2(handle,"SELECT COUNT(*) FROM MY_LOVE",-1,&s,0)==SQLITE_OK);assert(sqlite3_step(s)==SQLITE_ROW);int n=sqlite3_column_int(s,0);sqlite3_finalize(s);return n;}
int main(void){
 assert(sqlite3_open(":memory:",&handle)==SQLITE_OK);
 sql("CREATE TABLE SONG(ID INTEGER PRIMARY KEY,PATH TEXT,TITLE TEXT);CREATE TABLE MY_LOVE(ID INTEGER PRIMARY KEY,PATH TEXT,TITLE TEXT);INSERT INTO SONG VALUES(1,'/a.flac','A'),(2,'/b.flac','B');");
 mp_host_t h={.collect=collect};
 sql("PRAGMA query_only=ON;");assert(mdb_set_favorite_path("/a.flac",1)==0);act_favourite(&h);assert(rows()==0);
 assert(!exits && !strcmp(toast,"Couldn't add favourites"));
 sql("PRAGMA query_only=OFF;");act_favourite(&h);assert(exits==1 && rows()==1);
 act_favourite(&h);assert(exits==2 && rows()==1); /* retry is idempotent */
 sql("CREATE TRIGGER fail_b BEFORE INSERT ON MY_LOVE WHEN NEW.PATH='/b.flac' BEGIN SELECT RAISE(FAIL,'test denied'); END;");
 chosen=2;act_favourite(&h);assert(exits==2 && rows()==1 && !strcmp(toast,"1 saved, 1 failed"));
 admitted=0;act_favourite(&h);assert(exits==2 && !strcmp(toast,"Couldn't add favourites"));assert(!leases);
 sqlite3_close(handle);puts("PASS favourites: real SQLite success, read-only denial, partial trigger failure, retry and storage refusal");
}
'''
with tempfile.TemporaryDirectory(prefix='disco-bt-fav-') as tmp:
    root=Path(tmp)
    for name,source in [('guard',g),('favourites',f)]:
        path=root/(name+'.c');path.write_text(source)
        command=['cc','-O0','-std=gnu11','-Wall','-Wextra','-Wno-unused-function','-DBASELINE='+str(int(baseline)),'-I'+str(ui),str(path)]
        if name=='favourites':command += [str(ui/'sqlite3.c'),'-pthread','-ldl','-lm']
        binary=root/name
        subprocess.run(command+['-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

# ---- 2.5.8: route hold (music waits for the AAC/LDAC switch, then plays once on the final codec) ----
hold = main[main.index('static uint32_t g_hold_since'):main.index('int ui_route_analog(void){')]   # incl. ui_route_bt
h = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ipc.h"
static uint32_t now=100;static unsigned gen=1;
static int g_playing=0,sends,notices,pending,cancels; static uint32_t g_play_pending;
static char route[20]="AA:BB:CC:DD:EE:FF", g_route_mac[20]="AA:BB:CC:DD:EE:FF";
static track_state_t current={.have_track=1,.path="/music/a.flac",.pos_id=1,.path_seq=1,.position_ms=0,.duration_ms=100000};
static uint32_t lv_tick_get(void){return now;}
static uint32_t lv_tick_elaps(uint32_t t){return now-t;}
unsigned ipc_generation(void){return gen;}
static const char *ui_route_mac(void){return route;}
void ipc_get_state(track_state_t *s){*s=current;}
int ipc_send_cmd(const char *s){(void)s;sends++;return 0;}
static int opl_running(void){return 0;}   /* own_player.h: the stock player runs here */
static void ui_toast(const char *s){(void)s;notices++;}
int bt_codec_upgrade_pending(void){return pending;}
static int audio_run=-1; int ui_audio_running(void){return audio_run;}
void ui_bt_codec_upgrade_cancel(void){pending=0;cancels++;}
static int arms;
enum { OUT_INTERNAL=0, OUT_USB=2 };
static int test_output_route;
static int modes_output_route(void){return test_output_route;}
static int modes_output_busy(void){return 0;}
int ui_local_playback_allowed(void){return 1;}
int ui_get_source_mode(void){return 0;}
static int ui_player_settling(void){return 0;}
static void modes_output_reset(void){}
int ui_bt_codec_frame(const char *m,char *o,int c){snprintf(o,c,"06b3%s",m);return 0;}
static int ui_seek_to(long ms){(void)ms;return 0;}
void ui_bt_codec_upgrade_arm(const char *m,int fresh){(void)m;if(fresh){arms++;pending=1;}}
static void route_uppercase(const char *in,char *out,int cap){int j=0;for(int i=0;in[i]&&j<cap-1;i++){char c=in[i];if(c>='a'&&c<='z')c-=32;out[j++]=c;}out[j]=0;}
static const char *norm_mac_for_defer(const char *mac){static char n[20];route_uppercase(mac,n,sizeof n);return n;}
""" + guard + hold + r"""
static void run(uint32_t until){while(now<until){now+=100;route_hold_tick();}}
int main(void){
 pending=1;route_hold_start(route);run(9900);assert(sends==0);           /* still switching: music waits */
 run(10300);assert(sends==1 && cancels==1);                             /* no confirmed switch in 10 s: play once */
 sends=0;cancels=0;now=20000;pending=1;route_hold_start(route);run(22000);pending=0;
 run(22400);assert(sends==0);run(22800);assert(sends==1 && !cancels);   /* switch confirmed: settle 0.5 s, then play */
 sends=0;now=30000;pending=1;route_hold_start(route);run(31000);ui_route_hold_cancel();pending=0;run(40000);assert(!sends);   /* user pressed Play/Pause */
 now=50000;pending=1;route_hold_start(route);strcpy(g_route_mac,"11:22:33:44:55:66");run(60000);assert(!sends);              /* routed elsewhere */
 /* B03 (2.5.9 Bluetooth audit): the hold belongs to its song and session, and never toggles a playing song */
 strcpy(g_route_mac,"AA:BB:CC:DD:EE:FF");
 sends=0;now=70000;pending=1;route_hold_start(route);run(71000);
 strcpy(current.path,"/music/b.flac");current.path_seq++;pending=0;run(80000);assert(!sends);   /* another song chosen */
 strcpy(current.path,"/music/a.flac");
 sends=0;now=90000;pending=1;route_hold_start(route);run(91000);gen++;pending=0;run(99000);assert(!sends);   /* player restarted */
 sends=0;now=100000;pending=1;route_hold_start(route);run(101000);g_playing=1;pending=0;run(109000);assert(!sends);   /* already playing */
 g_playing=0;
 /* P02 (delayed-pause report): no route while a song is still starting - its codec switch would land mid-song, unheld */
 g_route_mac[0]=0;pending=0;arms=0;sends=0;now=200000;g_playing=0;g_play_pending=now;
 test_output_route=OUT_USB;assert(ui_route_bt("11:22:33:44:55:66")==-1&&sends==0);test_output_route=OUT_INTERNAL;
 assert(ui_route_bt("11:22:33:44:55:66")==-2 && sends==0 && arms==0 && !g_hold_since);   /* deferred: nothing sent */
 g_play_pending=0;g_playing=1;now=210000;
 assert(ui_route_bt("11:22:33:44:55:66")==0 && arms==1 && g_hold_since);   /* the song plays: routed, held for the codec */
 int before=sends;ui_route_hold_cancel();g_playing=0;(void)before;
 /* 2.6.1 audit: Next/Previous during the hold - it follows to the new song and plays it once the switch is done */
 ui_route_hold_cancel(); strcpy(g_route_mac,"AA:BB:CC:DD:EE:FF"); g_playing=0; sends=0; now=150000;
 strcpy(current.path,"/music/a.flac"); pending=1; route_hold_start(route); run(151000);
 ui_route_hold_follow(); strcpy(current.path,"/music/b.flac"); current.path_seq++; current.pos_id++;   /* skipped */
 run(152000); assert(g_hold_since && !sends);                       /* still held, now for the new song */
 pending=0; run(153000); assert(sends==1 && !g_hold_since);          /* switch done: plays the new song, once */
 run(158000); strcpy(current.path,"/music/a.flac");                  /* follow window over: back to normal checks */
 sends=0; pending=1; route_hold_start(route); run(160000);
 strcpy(current.path,"/music/c.flac"); current.path_seq++; run(161000); assert(!g_hold_since);   /* no skip: cancelled */
 pending=0; run(165000); assert(!sends); strcpy(current.path,"/music/a.flac");
 /* 2.6.1: sound going out though not seen playing - the hold end never toggles it (that would pause it) */
 ui_route_hold_cancel(); g_playing=0; sends=0; now=170000; pending=1; route_hold_start(route); run(171000);
 audio_run=1; pending=0; run(173000); assert(!sends && !g_hold_since); audio_run=-1;
 /* gate (2.6.0): an AAC/LDAC switch never hits a playing song */
 ui_route_hold_cancel(); sends=0; g_playing=0; g_play_pending=0; now=200000;
 assert(ui_codec_switch_gate(route)==1 && sends==0);                       /* nothing playing: switch now */
 g_play_pending=now; assert(ui_codec_switch_gate(route)==0 && sends==0);    /* a song starting: wait */
 g_play_pending=0; g_playing=1;
 assert(ui_codec_switch_gate(route)==1 && sends==1 && g_hold_since);       /* playing: paused once, held */
 assert(ui_codec_switch_gate(route)==1 && sends==1);                       /* already held: no second toggle */
 ui_route_hold_cancel(); g_playing=0; sends=0;
 puts("PASS BT route hold: waits for the codec switch, settles 0.5 s after the confirmed switch, plays once; plays after 10 s; user and route changes win; another song, another session or music already playing: no toggle; no route while a song is starting; a switch never hits a playing song (pause first, once); a skip during the hold follows to the new song");
 return 0;
}
"""
import subprocess as sp2, tempfile as tf2, pathlib as pl2
with tf2.TemporaryDirectory() as d2:
    src2=pl2.Path(d2)/'hold.c'; src2.write_text(h)
    r=sp2.run(['cc','-std=gnu11','-Wall','-Werror=implicit-function-declaration','-I',str(ui),str(src2),'-o',str(pl2.Path(d2)/'hold')],capture_output=True,text=True)
    assert r.returncode==0, r.stderr[-3000:]
    r=sp2.run([str(pl2.Path(d2)/'hold')],capture_output=True,text=True)
    print(r.stdout.strip()); assert r.returncode==0, r.stderr[-2000:]

