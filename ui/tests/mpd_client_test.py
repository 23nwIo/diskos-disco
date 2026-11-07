#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MPD server network thread against a real MPD client library (python-mpd2: pip install python-mpd2), with the fake
player of mpd_test.c behind it (make -C ui/tests mpd_test). Checks what clients rely on: the greeting, password,
command lists, idle/noidle across two connections, quoting, errors as exceptions, and that a connection survives them."""
import socket, subprocess, threading, time
from pathlib import Path
import mpd

tests = Path(__file__).resolve().parent
s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
srv = subprocess.Popen([str(tests/'mpd_test'), '--serve', str(port)], stdout=subprocess.PIPE, text=True)
try:
    assert srv.stdout.readline().startswith('serving'), 'server did not start'
    c = mpd.MPDClient(); c.timeout = 5
    c.connect('127.0.0.1', port)
    assert c.mpd_version == '0.23.5'
    try: c.status(); raise SystemExit('status without a password must fail')
    except mpd.CommandError as e: assert 'permission' in str(e)
    try: c.password('wrong'); raise SystemExit('a wrong password must fail')
    except mpd.CommandError as e: assert 'incorrect password' in str(e)
    c.password('testtesttesttesttest')
    st = c.status(); assert st['state'] == 'stop' and st['volume'] == '50', st
    c.add('Music/a b.flac'); c.add('Music/"quoted" \\ name.flac')
    assert [x['file'] for x in c.playlistinfo()] == ['Music/a b.flac', 'Music/"quoted" \\ name.flac']
    c.play(); assert c.status()['state'] == 'play'
    assert c.currentsong()['file'] == 'Music/a b.flac'
    ls = c.lsinfo(); assert ls[0] == {'directory': 'Music'} and ls[1]['title'] == 'A "quoted" title', ls
    c.command_list_ok_begin(); c.setvol(30); c.status(); c.pause(1); res = c.command_list_end()
    assert res[1]['volume'] == '30', res
    try: c.setvol(300); raise SystemExit('setvol 300 must fail')
    except mpd.CommandError as e: assert 'Invalid volume' in str(e)
    try: c.random(1); c.repeat(1); raise SystemExit('shuffle + repeat must fail')
    except mpd.CommandError as e: assert 'no such play mode' in str(e)
    c.random(0)
    assert c.outputs()[0]['outputname'] == 'Disc'
    # idle on a second client, changes from the first
    w = mpd.MPDClient(); w.timeout = 5; w.connect('127.0.0.1', port); w.password('testtesttesttesttest')
    got = []
    t = threading.Thread(target=lambda: got.append(w.idle('mixer', 'options'))); t.start()
    time.sleep(0.3); c.setvol(70); t.join(5)
    assert got == [['mixer']], got
    r = socket.create_connection(('127.0.0.1', port)); f = r.makefile('rw')   # noidle by hand (python-mpd2 hides it)
    assert f.readline().startswith('OK MPD'); f.write('password testtesttesttesttest\nidle\n'); f.flush()
    assert f.readline() == 'OK\n'; time.sleep(0.2); f.write('noidle\n'); f.flush(); assert f.readline() == 'OK\n'; r.close()
    c.clear(); assert w.idle('playlist', 'player') in (['playlist', 'player'], ['player', 'playlist'], ['playlist'])
    c.ping(); c.close(); c.disconnect(); w.disconnect()
    print('mpd_client_test: all passed')
finally:
    srv.kill(); srv.wait()
