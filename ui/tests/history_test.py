#!/usr/bin/env python3
"""Exercise the production history hook without LVGL or a player process."""
from pathlib import Path
import subprocess
import tempfile

ui = Path(__file__).resolve().parents[1]
src = (ui / 'ui.c').read_text()
hook = src[src.index('void ui_history_tick('):src.index('void ui_update(')]
main = (ui / 'main.c').read_text()
assert main.index('ui_history_tick(&st);') < main.index('if(bl_state != 2 && (st.seq')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int have_track, state; char path[256];} track_state_t;
static char g_stat_path[256];
static int calls;
static uint32_t now;   /* the hook counts a song only after it has been playing for 1 s */
static uint32_t lv_tick_get(void){return now;}
static uint32_t lv_tick_elaps(uint32_t t){return now-t;}
static void copy_cstr(char *d,size_t n,const char *s){snprintf(d,n,"%s",s);}
static int mdb_is_book_path(const char *s){return strstr(s,".m4b")!=NULL;}
static int mdb_is_stream_path(const char *s){return !strncmp(s,"http",4);}
static void mdb_record_play(const char *s){assert(s[0]);calls++;}
''' + hook + r'''
static void tick(const track_state_t *s){ui_history_tick(s);now+=1500;ui_history_tick(s);}   /* seen, then 1.5 s later */
int main(void){
 track_state_t s={.have_track=1,.state=1,.path="A.flac"};
 tick(&s); assert(calls==0);
 s.state=2;tick(&s);tick(&s);assert(calls==1);
 s.state=1;strcpy(s.path,"B.flac");tick(&s);assert(calls==1);
 s.state=2;tick(&s);assert(calls==2);
 strcpy(s.path,"book.m4b");tick(&s);assert(calls==2);
 strcpy(s.path,"B.flac");tick(&s);assert(calls==3);
 s.have_track=0;tick(&s);s.have_track=1;tick(&s);assert(calls==4);
 tick(NULL);tick(&s);assert(calls==5);
 s.path[0]=0;tick(&s);assert(calls==5);
 /* a burst of skips: songs shown for less than 1 s are not counted, the one that stays is */
 s.state=2;strcpy(s.path,"C.flac");ui_history_tick(&s);now+=400;
 strcpy(s.path,"D.flac");ui_history_tick(&s);now+=400;ui_history_tick(&s);assert(calls==5);
 now+=700;ui_history_tick(&s);assert(calls==6);
 /* web radio stations are not songs */
 strcpy(s.path,"http://radio.example/stream");tick(&s);tick(&s);assert(calls==6);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'history.c';binary=Path(d)/'history'
    source.write_text(program)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: production History hook, paused skips, 1 s skip bursts, deduplication, books, stations, reset and screen-off placement')
