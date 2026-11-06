#!/usr/bin/env python3
"""Real LVGL navigation plus independent ordering checks of production settings rows."""
from pathlib import Path
import json,os,subprocess,tempfile
from artdec_test import png
ui=Path(__file__).resolve().parents[1]
roots=['Playback','Audio','Display','Connectivity','System','Battery & power','Help']
playback=['Play Mode','Equalizer','Artists','Library seek','Resume Playback','Auto-tag','Play Through Folders','Up Next','SongSphere']
audio=['Balance','DAC Filter','DSD Output','Gain','Max Volume','ReplayGain','Working Mode','DRE','Gapless']
all_cases=[]
with tempfile.TemporaryDirectory(prefix='settings-alignment-') as tmp:
 p=Path(tmp);art=p/'art.png';art.write_bytes(png(64,64,(110,50,160)))
 for theme in [6,8,9,10,11,12,13]:
  for variant in ([0] if theme in [11,13] else [0,1]):
   (ui/'tests/host_build/diskos.conf').unlink(missing_ok=True)
   r=subprocess.run([str(ui/'tests/host_build/host-render'),str(theme),str(variant),'preview-home','0',str(p/'out.ppm')],env={**os.environ,'REVIEW_SETTINGS_ALIGNMENT':'1','ZNP_PATH':str(art),'CFG_INT':'disco_hints=2'},capture_output=True,text=True,timeout=60)
   assert r.returncode==0,(theme,variant,r.stdout[-2000:],r.stderr[-2500:])
   actual_roots=[x.split('|')[2] for x in r.stdout.splitlines() if x.startswith('SETTINGS_ROOT|')]
   assert actual_roots==roots,(theme,actual_roots)
   pages={}
   for line in r.stdout.splitlines():
    if not line.startswith('SETTINGS_ROW|'):continue
    _,page,label,toggle,key,kind=line.split('|');pages.setdefault(page,[]).append({'label':label,'toggle':int(toggle),'key':key,'kind':int(kind)})
   assert [x['label'] for x in pages['Playback']]==playback,(theme,pages['Playback'])
   assert [x['label'] for x in pages['Audio']]==audio,(theme,pages['Audio'])
   assert not any(x['label']=='Custom EQ' for items in pages.values() for x in items)
   # Validate with an independent rule, not the production comparator.
   for page,items in pages.items():
    labels=[x['label'] for x in items];assert len(labels)==len(set(labels)),(theme,page,'duplicate',labels)
    anchors={
      'Display':['Brightness','Theme', {6:'Stone',8:'Ring',9:'Braun',10:'Disco',11:'Null',12:'Squash',13:'Zune'}[theme]+' settings'],
      'Connectivity':['Wi-Fi','Bluetooth'], 'System':['Language'],
      'Battery & power':['Details','Idle Power-off'], 'Help':['How to'],
      'Text & layout':['Text size'], 'Library appearance':['Album view'],
      'Immersive':['Style'], 'Sleep timer':['Duration'],
      'Controls':['Volume Keys'], 'Date & Time':['Set Date & Time'],
      'Library':['Rescan Library'], 'Maintenance':['Update from SD Card'],
      'MA Sendspin':['Server'], 'Remote Control':['Control Page'], 'Bluetooth':['Devices'],
      'Volume Keys':['Press','Double Press','Long Press']}.get(page,[])
    if page=='Playback':continue
    assert labels[:len(anchors)]==anchors,(theme,page,labels,anchors)
    rest=items[len(anchors):]
    want=sorted(rest,key=lambda x:(x['toggle'],x['label'].casefold()))
    assert rest==want,(theme,page,labels)
   assert 'Power' not in pages and 'Network' not in pages
   assert all(x['label']!='Power' for x in pages['System'])
   assert pages['Battery & power'][-1]['label']=='Charging Limit'
   for name in ['Screen-off delay','Screensaver delay','Restart','Shut down player','Sleep timer']:
    assert name in [x['label'] for x in pages['Battery & power']],(theme,name)
   assert not any(x['key']=='disc_colour' for rows in pages.values() for x in rows)
   assert not any(x['key']=='eq_preset' for rows in pages.values() for x in rows)
   assert [x['key'] for x in pages['Sleep timer']]==['sleep_idx','sleep_action']
   assert all(x['key'] not in ['saver_idx','screenoff_idx'] for x in pages['Standby']) if 'Standby' in pages else True
   all_cases.append({'theme':theme,'variant':variant,'roots':actual_roots,'pages':pages})
   print(f'PASS theme {theme}/{variant}: common roots, {sum(map(len,pages.values()))} settings rows, ordering and real navigation',flush=True)
# Optional export supports the review document without changing production settings.
if os.environ.get('SETTINGS_MANIFEST_OUT'):Path(os.environ['SETTINGS_MANIFEST_OUT']).write_text(json.dumps(all_cases,indent=2)+'\n')
print('PASS settings alignment across all seven themes / 12 appearances')
