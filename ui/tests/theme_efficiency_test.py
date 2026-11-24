#!/usr/bin/env python3
"""Actual LVGL list/menu animation pixels and Null timer lifecycle regression checks.
Build the host renderer first. Any pixel change fails the test. After an INTENDED visual change, look at the changed
screens, then re-record with --record-baseline (writes theme-efficiency-baseline.json and the settings digests in
settings-roots-baseline.json; see docs/TESTING.md). Never re-record to make an unexplained difference go away.
"""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, tempfile
from artdec_test import png
app=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--record-baseline',action='store_true');p.add_argument('--renderer',type=Path,default=app/'tests/host_build/host-render');a=p.parse_args()
fixture=app/'tests/fixtures/theme-efficiency-baseline.json'
cases=[(6,0,0),(6,1,0),(8,0,0),(8,1,0),(9,0,0),(9,1,0),(10,0,0),(10,1,0),(11,0,0),(11,0,1),(11,0,2),(12,0,0),(12,1,0),(13,0,0)]
expected=None if a.record_baseline else json.loads(fixture.read_text())
if expected:
 for case,digest in zip(expected['cases'],json.loads((app/'tests/fixtures/settings-roots-baseline.json').read_text())['sha256']):
  case['static']['settings']=digest
with tempfile.TemporaryDirectory(prefix='theme-efficiency-') as tmp:
 art=Path(tmp)/'track.png';art.write_bytes(png(64,64,(209,57,12)));out=Path(tmp)/'frame.ppm'
 results=[]
 for index,(preset,variant,palette) in enumerate(cases):
  env={**os.environ,'ZNP_PATH':str(art),'CFG_INT':'disco_hints=2','NULL_PALETTE':str(palette)}
  def run(extra,page='home'):
   (app/'tests/host_build/diskos.conf').unlink(missing_ok=True)
   r=subprocess.run([str(a.renderer.resolve()),str(preset),str(variant),'preview-'+page,'0',str(out)],env={**env,**extra},capture_output=True,text=True,timeout=45)
   assert r.returncode==0,r.stdout+r.stderr
   return r.stdout
  text=run({'REVIEW_THEME_SCROLL':'1'})
  frames=[s for s in text.splitlines() if s.startswith(('THEME_FRAME','NULL_FRAME'))]
  assert len(frames)==(123 if preset==11 else 63)
  static={page:None for page in ['home','np','quick','settings']}
  for page in static:
   run({},page);static[page]=hashlib.sha256(out.read_bytes()).hexdigest()
  styles=[int(s.split()[1]) for s in text.splitlines() if s.startswith('NULL_MENU_STYLES')]
  colors=[int(s.split()[1]) for s in text.splitlines() if s.startswith('NULL_MENU_COLORS')]
  result={'preset':preset,'variant':variant,'palette':palette,'frames':frames,'static':static,'menu_style_events':styles,'menu_color_writes':colors};results.append(result)
  if expected:
   old=expected['cases'][index];assert frames==old['frames'],(preset,variant,palette,'animation pixels differ');assert static==old['static'],(preset,variant,palette,'static pixels differ on',[k for k in static if static[k]!=old['static'].get(k)])
   if styles:assert styles[0]<=old['menu_style_events'][0],(styles,old['menu_style_events'])
   if colors:assert colors[0]<=old['menu_color_writes'][0],(colors,old['menu_color_writes'])   # never more style work than recorded
  print(f'PASS {preset}/{variant}/palette{palette}: {len(frames)} animation frames, four static screens'+(f', Null color writes {colors[0]}' if styles else ''),flush=True)
 if a.record_baseline:
  rev=subprocess.run(['git','-C',str(app),'rev-parse','HEAD'],capture_output=True,text=True).stdout.strip()
  fixture.write_text(json.dumps({'baseline':rev,'cases':results},indent=2)+'\n')
  roots=app/'tests/fixtures/settings-roots-baseline.json';r=json.loads(roots.read_text())
  r['sha256']=[c['static']['settings'] for c in results];roots.write_text(json.dumps(r,indent=2)+'\n')
 else:
  print(run({'REVIEW_THEME_STABILITY':'1'}).strip())
  print('PASS animation, Music, Home, Quick Settings and Settings match the recorded baseline; Null timer cleanup verified')
