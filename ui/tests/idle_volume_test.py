from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[2];s=(repo/'ui/main.c').read_text();a=s.index('        if(st.volume_seq != last_vol_seq){');b=s.index('        /* the metadata',a);block=s[a:b]
head='''#include "power.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
static uint32_t now,last_activity,key_ms;static unsigned last_vol_seq;static int last_volume=-1, bars;static int bl_state=2,requests;
static struct {unsigned volume_seq;int volume;} st;
#define SCR_SAVER 1
static uint32_t lv_tick_get(void){return now;}
static void power_note_input(void){key_ms=now;}
static int screen_current(void){return 0;}
static void screen_back(void){}
static void ui_show_volume(int v){(void)v;bars++;}
static int request(void){requests++;return 0;}
int settings_outputs_recent(void){return 0;}   /* settings.c: no Settings > Audio > Outputs change just now */
static void volume_step(void){
'''
body='''}
int main(void){
 for(int panel=0;panel<=2;panel+=2){
  for(int changed=0;changed<2;changed++){
   power_idle_reset();now=last_activity=key_ms=0;requests=bars=0;
   last_vol_seq=0;last_volume=-1;st.volume=50;st.volume_seq=0;bl_state=panel;
   for(now=1000;now<=660000;now+=1000){
    if(now==1000||now%30000==0){st.volume_seq++;if(changed)st.volume=st.volume==50?51:50;}
    volume_step();
    power_idle_tick(now,300,0,key_ms>last_activity?key_ms:last_activity,request);
   }
   assert(requests==(changed?0:1));
   assert(changed ? key_ms>0 : key_ms==0);
   assert(panel==0&&changed ? bars>0 : bars==0);
  }
 }
 puts("PASS: identical reports expire idle on lit/dark panels; changed levels reset idle without waking dark panels");
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(head+block+body)
 subprocess.run(['cc','-DPOWER_CORE_ONLY','-ffunction-sections','-fdata-sections','-I'+str(repo/'ui'),str(p/'test.c'),str(repo/'ui/power.c'),'-Wl,--gc-sections','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
