#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hit&Guess (Jo, 2026-11-18) end to end: the host's setup and controls, the players' phones through the guests' link
(/Party_1/, "~"), against a real song.db through the production main-thread code (remote.c, party.c, hitguess.c,
musicdb.c) via host-render's REMOTE_REQS hook. Build host-render first (make -C ui CROSS= host-render). Like
remote_library_test.py it replaces the host's /usr/data/fiio/db/song.db and /tmp/sdcard. The player is not running
on a host: a song that starts shows as its ui_play_list call (all songs, by title); stopping parts is the Disc's."""
import json, os, shutil, sqlite3, subprocess, tempfile
from pathlib import Path
ui = Path(__file__).resolve().parents[1]
sd = Path('/tmp/sdcard'); subprocess.run(['umount', str(sd)], capture_output=True); shutil.rmtree(sd, ignore_errors=True)
sd.mkdir(parents=True); subprocess.run(['mount', '--bind', str(sd), str(sd)], check=True)
shutil.rmtree('/tmp/hitguess', ignore_errors=True)
Path('/usr/data/diskos_queue.tsv').unlink(missing_ok=True); (ui/'tests/host_build/queue.tsv').unlink(missing_ok=True)
db = Path('/usr/data/fiio/db/song.db'); db.parent.mkdir(parents=True, exist_ok=True); db.unlink(missing_ok=True)
con = sqlite3.connect(db)
cols = ('ID INTEGER PRIMARY KEY,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,'
        'ALBUM_ARTIST,IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,'
        'ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST_CODE')
for t in ['SONG', 'MY_LOVE']: con.execute(f'CREATE TABLE {t}({cols})')
con.execute('CREATE TABLE CUSTOM_PLAYLIST_INDEX(LIST_ID INTEGER PRIMARY KEY,LIST_NAME,M3U_PATH)')
con.execute(f'CREATE TABLE CUSTOM_PLAYLIST({cols},PLAYLIST_ID)')
songs = [('80s', 'Take On Me', 'a-ha', 'Hunting High and Low', '1985-06-01'), ('80s', 'Africa', 'Toto', 'Toto IV', '1982'),
         ('80s', 'Jump', 'Van Halen', '1984', ''), ('90s', 'Wonderwall', 'Oasis', 'Morning Glory', '1995-10-02'),
         ('90s', 'Zombie', 'The Cranberries', 'No Need to Argue', '1994')]
for i, (folder, title, artist, album, date) in enumerate(songs, 1):
    p = sd/'Music'/folder/f'{i:02d} {title}.flac'; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'x')
    con.execute('INSERT INTO SONG(ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,'
                'IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,SONG_PRODUCTION_YEAR) VALUES(' + ','.join('?'*23) + ')',
                (i, str(p), p.name, title, album, artist, 'Pop', 1, i, 0, 0, 0, 0, 200000, artist, 0, '', title, p.name, album, artist, 'Pop', date))
con.execute("INSERT INTO CUSTOM_PLAYLIST_INDEX VALUES(1,'Party hits','')")
con.execute('INSERT INTO CUSTOM_PLAYLIST SELECT *,1 FROM SONG WHERE ID IN (4,5)')
con.commit(); con.close()

A, B, C = 'g=AnnaAnnaAnna1234', 'g=BenBenBenBen1234', 'g=CleoCleoCleo1234'
reqs = [
    'GET ~/api/game?' + A,                                                    # 0 the link is off
    'POST /api/party on=2&name=Party_1',                                       # 1 the guests' link: the game
    'GET /api/game_admin',                                                     # 2 the default setup
    'POST /api/game_admin form=1&win=0&win_n=2&dir=Music%2F80s&pool=1',        # 3
    'GET ~/api/game?' + A,                                                     # 4 not joined yet
    'POST ~/api/game action=join&' + A,                                        # 5 no name
    'POST ~/api/game action=join&n=Anna&' + A,                                 # 6
    'POST ~/api/game action=join&n=anna&' + B,                                 # 7 the same name
    'POST ~/api/game action=join&n=Ben&' + B,                                  # 8
    'POST /api/game_admin action=start',                                       # 9
    'POST /api/game_admin year=0',                                             # 10 the setup is locked while it runs
    'GET ~/api/game?' + A,                                                     # 11 Anna chooses: 3 suggestions from the 80s folder
    'GET ~/api/game_search?q=to&' + A,                                         # 12
    'GET ~/api/game_search?' + B,                                              # 13 not Ben's turn
    'POST ~/api/game action=pick&song_id=4&' + A,                              # 14 not in the game's folder
    'POST ~/api/game action=pick&song_id=1&' + A,                              # 15 Take On Me plays
    'GET ~/api/game?' + B,                                                     # 16 Ben doesn't see it; the year counts
    'GET ~/api/game?' + A,                                                     # 17 Anna does
    'GET ~/api/cover?' + B,                                                    # 18 not before the reveal
    'POST ~/api/game action=tip&a=A-Ha&t=Take+on+me&y=1984&' + B,              # 19
    'POST ~/api/game action=tip&a=x&t=y&' + A,                                 # 20 Anna knows it
    'POST ~/api/game action=join&n=Cleo&' + C,                                 # 21 joins late
    'POST ~/api/game action=reveal&' + B,                                      # 22 not Ben's turn
    'POST ~/api/game action=reveal&' + A,                                      # 23
    'GET ~/api/game?' + B,                                                     # 24 revealed: the song, Ben's ticks from his guess
    'POST ~/api/game action=pick&song_id=2&' + B,                              # 25 Ben chooses early
    'POST ~/api/game action=mark&p=2&m=1&' + A,                                # 26 Cleo said the artist aloud
    'POST ~/api/game action=stop&' + B,                                        # 27 only Anna may stop it (default)
    'POST ~/api/game action=confirm&' + A,                                     # 28 Ben's song starts at once
    'GET ~/api/game?' + C,                                                     # 29
    'POST ~/api/game action=reveal&' + B,                                      # 30
    'POST ~/api/game action=mark&p=0&m=15&' + B,                               # 31 year + near: the exact year wins
    'POST ~/api/game action=confirm&' + B,                                     # 32 two songs: the end
    'GET ~/api/game?' + C,                                                     # 33 ranking and songs
    'GET /api/game_admin',                                                     # 34
    'POST /api/game_admin action=again',                                       # 35 the same players, back to the lobby
    'POST /api/game_admin pool=2&pl=1&pick=2',                                 # 36 a playlist, only chance
    'POST /api/game_admin action=start',                                       # 37
    'POST ~/api/game action=pick&song_id=4&' + A,                              # 38 only chance
    'POST ~/api/game action=random&' + A,                                      # 39
    'GET ~/api/game?' + A,                                                     # 40 chance: Anna doesn't know it, guesses along
    'POST /api/game_admin action=remove&p=1',                                  # 41 the host removes Ben
    'POST /api/game_admin action=skip',                                        # 42 skips Anna's song
    'POST /api/game_admin action=end',                                         # 43
    'POST /api/party on=1',                                                    # 44 back to the party
    'GET ~/api/game?' + A,                                                     # 45 no game
    'GET ~/api/state?' + A,                                                    # 46 the party works again
    'GET /api/library/folder?limit=500&path=Music',                            # 47 the host's folder picker lists the card's folders
    'POST /api/game_admin dir=',                                               # 48 the whole card
    'POST /api/party on=2',                                                    # 49 the game again
    'POST ~/api/volume value=40&' + A,                                         # 50 every player may change the volume
    'GET ~/api/game?' + A,                                                     # 51
    'POST /api/party vol=0',                                                   # 52 the host keeps the volume
    'POST ~/api/volume value=40&' + A,                                         # 53
    'GET ~/api/game?' + A,                                                     # 54
]
with tempfile.TemporaryDirectory() as tmp:
    rf = Path(tmp)/'reqs.txt'; rf.write_text('\n'.join(reqs) + '\n')
    (ui/'tests/host_build/diskos.conf').unlink(missing_ok=True)
    r = subprocess.run([str(ui/'tests/host_build/host-render'), '10', '0', 'preview-home', '0', str(Path(tmp)/'o.ppm')],
                       env={**os.environ, 'REMOTE_REQS': str(rf), 'ZUNE_LIBRARY_FIXTURE': '1'}, capture_output=True, text=True, timeout=60)
assert r.returncode == 0, r.stderr[-2000:]
res, plays = [], []
for line in r.stdout.splitlines():
    if line.startswith('PLAY_LIST '): plays.append(line[10:])
    elif line.startswith('REQ '): res.append({'req': line[4:].strip(), 'plays': plays}); plays = []
    elif line.startswith('RC '): res[-1]['rc'] = int(line.split()[1]); res[-1]['why'] = line.split(' WHY ')[1].split(' SENT ')[0]
    elif line.startswith('BODY '): res[-1]['body'] = None if line == 'BODY -' else json.loads(line[5:])
assert len(res) == len(reqs), r.stdout
def ok(k): assert res[k]['rc'] == 0, res[k]; return res[k]['body']
def no(k, why=None): assert res[k]['rc'] == -1 and (why is None or why in res[k]['why']), res[k]
def names(b): return [p['name'] for p in b['players']]
no(0, 'The party is over')                                        # the harness routes it; the server answers 404 while the link is off
ok(1); s = ok(2)['set']
assert s['pick'] == 1 and s['year'] == 1 and s['stagger'] == 1 and s['skip'] == 1 and s['late'] == 1 and s['form'] == 0 and s['countdown'] == 0
assert ok(3)['set']['form'] == 1 and res[3]['body']['dir'] == 'Music/80s'
conf = (sd/'.diskos/state/hitguess.conf').read_text()
assert 'form=1' in conf and 'pool=2' in conf and 'pl=1' in conf, conf   # as saved last (36; dir: 48)
b = ok(4); assert b['you'] == -1 and b['phase'] == 'lobby' and b['players'] == []
no(5, 'name'); assert ok(6)['you'] == 0; no(7, 'that name'); assert ok(8)['you'] == 1
b = ok(9); assert b['phase'] == 'pick' and b['cur'] == 0 and b['pool'] == 3
no(10, 'before the game')
b = ok(11); assert b['can']['choose'] and b['can']['pick'] and len(b['sugg']) == 3 and {x['id'] for x in b['sugg']} == {1, 2, 3}, b
assert [x['title'] for x in ok(12)['items']] == ['Africa'], res[12]   # "to": the artist Toto
no(13, 'not your turn'); no(14, "isn't part of this game")
ok(15); assert len(res[15]['plays']) == 1 and res[15]['plays'][0].startswith('type=1 '), res[15]   # in the all-songs list
b = ok(16); t = b['turn']; assert b['phase'] == 'play' and t['song'] is None and t['year_counts'] == 1 and b['players'][1]['guesses'] == 1
b = ok(17); assert b['turn']['song']['title'] == 'Take On Me' and b['turn']['song']['date'] == '1985-06-01' and b['can']['reveal'] and not b['can']['tip']
no(18, 'Not revealed'); ok(19); no(20, 'You know this song')
assert ok(21)['you'] == 2 and res[21]['body']['players'][2]['joined'] == 1
no(22, 'whose turn'); ok(23)
b = ok(24); assert b['phase'] == 'reveal' and b['turn']['song']['year'] == 1985 and b['players'][1]['mark'] == 1 | 2 | 8, b['players']
assert b['players'][1]['tip'] == {'a': 'A-Ha', 't': 'Take on me', 'y': 1984} and b['can']['choose'], b    # Ben is next: he may choose now
ok(25); assert not res[25]['plays'] and res[25]['body']['next_ready']
ok(26); no(27, 'may not stop')
b = ok(28); assert len(res[28]['plays']) == 1 and res[28]['plays'] != res[15]['plays'] and b['phase'] == 'play' and b['cur'] == 1, (res[28]['plays'], b)   # Africa
assert [p['pts'] for p in b['players']] == [0, 2.5, 1] and b['players'][1]['py'] == 0.5   # the year off by 1: half a point
b = ok(29); assert b['turn']['song'] is None and b['turn']['chance'] == 0
assert b['rank'] == [[1, 1], [2, 2], [3, 0]] and 'tracks' not in b, b['rank']   # the ranking for the phones while the game runs, not the songs
ok(30); ok(31); b = ok(32); assert b['phase'] == 'end' and b['ended'] == 0
b = ok(33)
assert b['rank'] == [[1, 0], [2, 1], [3, 2]] and [p['pts'] for p in b['players']] == [3, 2.5, 1], b   # Anna: artist, title, exact 1982
assert [x['s']['title'] for x in b['tracks']] == ['Take On Me', 'Africa'] and [x['by'] for x in b['tracks']] == [0, 1]
b = ok(34); assert b['songs'] == 2 and len(b['tracks']) == 2
b = ok(35); assert b['phase'] == 'lobby' and names(b) == ['Anna', 'Ben', 'Cleo'] and all(p['pts'] == 0 for p in b['players'])
assert ok(36)['set']['pool'] == 2 and res[36]['body']['pl'] == 1
b = ok(37); assert b['pool'] == 2
no(38, 'Only chance'); ok(39); assert len(res[39]['plays']) == 1, res[39]
b = ok(40); assert b['turn']['song'] is None and b['turn']['chance'] == 1 and b['players'][0]['guesses'] == 1
b = ok(41); assert not b['players'][1]['in']
b = ok(42); assert b['cur'] == 2 and b['phase'] == 'pick'                     # Ben's out: Cleo's turn
b = ok(43); assert b['phase'] == 'end' and b['ended'] == 2 and b['tracks'][0]['skipped'] == 1
ok(44); no(45, 'No game'); ok(46)
assert 'workmode 2 for now (not saved' in r.stderr and 'again (rc=' in r.stderr, r.stderr[-3000:]   # Repeat One while the game plays, the saved mode after
assert [x['name'] for x in ok(47)['items'] if x['dir']][:1] == ['80s'], res[47]
ok(49); ok(50); assert 'volume' in ok(51), res[51]
ok(52); no(53, "host's"); assert 'volume' not in ok(54)
assert ok(48)['dir'] == '' and 'dir=\n' in (sd/'.diskos/state/hitguess.conf').read_text()
logs = sorted(Path('/tmp/hitguess').glob('game-*.log')); assert logs, 'no log'
assert len(logs) == 2, logs
text = '\n'.join(l.read_text() for l in logs)
for want in ['game starts: 3 songs (folder Music/80s)', 'Cleo joined the running game', 'song 1 by Anna (chosen): "Take On Me"',
             'Ben guesses: artist "A-Ha"', 'Anna revealed the song', 'Ben chose the next song early', '  Ben: artist title near year = 2.5',
             'game over (goal reached) after 2 songs', '1. Anna: 3 points (artist 1, title 1, year 1)', '2. Ben: 2.5 points (artist 1, title 1, year 0.5)', 'the host removed Ben',
             'the host skipped the turn', 'game over (ended by the host)', 'Cleo: 1 point (artist 1']:
    assert want in text, (want, text)
subprocess.run(['umount', str(sd)], capture_output=True)
print('PASS Hit&Guess: setup on the card, joining, choosing, guessing, revealing, ticking, the end, the host\'s controls, the log')
