#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Remote Control library API against a real song.db through the production main-thread code (remote.c's browse and
play_item, musicdb.c), via host-render's REMOTE_REQS hook. Build host-render first (make -C ui CROSS= host-render).
Like the site renderer it uses the host's /usr/data/fiio/db/song.db and /tmp/sdcard (both replaced). The player is
not running on a host, so only plays that end in ui_play_list (album, song, favourites, all) are checked end to end;
artist, playlist and file plays go through main.c's own local-playback admission and are covered on the Disc."""
import json, os, shutil, sqlite3, subprocess, tempfile
from pathlib import Path
ui = Path(__file__).resolve().parents[1]
sd = Path('/tmp/sdcard'); shutil.rmtree(sd, ignore_errors=True)
(sd/'Music/Blue Album').mkdir(parents=True); (sd/'Music/Other').mkdir(parents=True)
os.symlink('/etc', sd/'Music/escape')
db = Path('/usr/data/fiio/db/song.db'); db.parent.mkdir(parents=True, exist_ok=True); db.unlink(missing_ok=True)
con = sqlite3.connect(db)
cols = ('ID INTEGER PRIMARY KEY,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,'
        'ALBUM_ARTIST,IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,'
        'ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST_CODE')
for t in ['SONG', 'MY_LOVE']: con.execute(f'CREATE TABLE {t}({cols})')
con.execute('CREATE TABLE CUSTOM_PLAYLIST_INDEX(LIST_ID INTEGER PRIMARY KEY,LIST_NAME,M3U_PATH)')
con.execute(f'CREATE TABLE CUSTOM_PLAYLIST({cols},PLAYLIST_ID)')
i = 0
for album, artist, folder, titles in [('Blue Album', 'Ana & "Bo"', 'Blue Album', ['Uno', 'Dos', 'Tres']),
                                      ('Café <Noir>', 'Céline', 'Other', ['Nuit', 'Jour']), ('Other', 'Zed', 'Other', ['Solo'])]:
    for t, title in enumerate(titles):
        i += 1; name = f'{t+1:02d} {title}.flac'; p = sd/'Music'/folder/name; p.write_bytes(b'x')
        con.execute('INSERT INTO SONG(ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE) VALUES(' + ','.join('?'*22) + ')', (i, str(p), name, title, album, artist, 'Pop', 1, t+1, 0, 0, 0, 0,
                    200000+i, artist, 0, '', title, name, album, artist, 'Pop'))
con.execute('INSERT INTO MY_LOVE SELECT * FROM SONG WHERE ID IN (2,5)')
con.execute("INSERT INTO CUSTOM_PLAYLIST_INDEX VALUES(1,'Road trip','')")
con.execute('INSERT INTO CUSTOM_PLAYLIST SELECT *,1 FROM SONG WHERE ID IN (6,1)')
con.commit(); con.close()
(sd/'Music/notes.txt').write_text('x'); (sd/'Music/unindexed.mp3').write_bytes(b'x')
favs = Path('/usr/data/radio_favorites.txt'); favs.write_text('Jazz <FM>\thttp://s.example/jazz\tMP3\t128\tGermany\n'
                                                            'Talk\thttps://s.example/talk\tAAC\t64\tFrance\n')

reqs = ['GET /api/library/albums', 'GET /api/library/albums?q=caf', 'GET /api/library/album?name=Blue+Album',
        'GET /api/library/artists', 'GET /api/library/artist?name=C%C3%A9line', 'GET /api/library/playlists',
        'GET /api/library/playlist?id=1', 'GET /api/library/favorites', 'GET /api/library/songs?q=dos',
        'GET /api/library/songs?limit=2&offset=1', 'GET /api/library/folder', 'GET /api/library/folder?path=Music',
        'GET /api/library/folder?path=Music%2Fescape', 'GET /api/library/folder?path=Music%2Fnope',
        'POST /api/play_item kind=album&name=Blue+Album&song_id=2', 'POST /api/play_item kind=album&name=Blue+Album&song_id=4',
        'POST /api/play_item kind=favorites&pos=2', 'POST /api/play_item kind=song&song_id=6',
        'POST /api/play_item kind=file&path=Music%2Funindexed.mp3', 'POST /api/play_item kind=file&path=Music%2Fescape%2Fpasswd',
        'POST /api/play_item kind=all', 'GET /api/library/radio', 'GET /api/library/radio?q=fran',
        'POST /api/play_item kind=radio&pos=1&url=http%3A%2F%2Fs.example%2Fjazz', 'POST /api/play_item kind=radio&pos=2&url=http%3A%2F%2Fs.example%2Fjazz',
        'POST /api/play_item kind=radio&pos=3', 'POST /api/radio_stop', 'POST /api/next',
        'POST /api/radio_search q=jazz', 'GET /api/library/radio_search', 'POST /api/play_item kind=radio_hit&pos=1&url=http%3A%2F%2Fx',
        'POST /api/radio_fav_add kind=radio&pos=1&url=http%3A%2F%2Fs.example%2Fjazz',
        'POST /api/radio_fav_remove kind=radio&pos=2&url=https%3A%2F%2Fs.example%2Ftalk', 'GET /api/library/radio',
        'POST /api/playlist_create name=Night+drive', 'POST /api/playlist_add id=2&kind=song&song_id=3',
        'POST /api/playlist_add id=2&kind=song&song_id=3', 'POST /api/playlist_add id=2&kind=album&name=Caf%C3%A9+%3CNoir%3E',
        'GET /api/library/playlist?id=2', 'POST /api/playlist_remove id=2&pos=1&song_id=5', 'POST /api/playlist_remove id=2&pos=1&song_id=3',
        'POST /api/playlist_rename id=2&name=Late+drive', 'GET /api/library/playlists',
        'POST /api/favorite on=1&song_id=6', 'GET /api/library/album?name=Other', 'POST /api/favorite on=0&song_id=2', 'GET /api/library/favorites',
        'POST /api/playlist_delete id=2', 'GET /api/library/playlists', 'POST /api/rescan action=stop', 'POST /api/eq preset=3', 'GET /api/status',
        'GET /api/bluetooth', 'POST /api/bluetooth action=search', 'POST /api/bt_device action=connect&mac=00:11:22:33:44:55', 'POST /api/work_mode mode=3', 'GET /api/status']
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
def items(k): assert res[k]['rc'] == 0, res[k]; return res[k]['body']['items']
assert [a['name'] for a in items(0)] == ['Blue Album', 'Café <Noir>', 'Other'] and items(0)[0] == {'name': 'Blue Album', 'artist': 'Ana & "Bo"', 'count': 3}
assert [a['name'] for a in items(1)] == ['Café <Noir>']
assert [s['title'] for s in items(2)] == ['Uno', 'Dos', 'Tres']
assert [a['name'] for a in items(3)] == ['Ana & "Bo"', 'Céline', 'Zed']
assert sorted(s['title'] for s in items(4)) == ['Jour', 'Nuit']
assert items(5) == [{'id': 1, 'name': 'Road trip'}]
assert [(s['title'], s['pos']) for s in items(6)] == [('Uno', 1), ('Solo', 2)]
assert [(s['title'], s['pos']) for s in items(7)] == [('Dos', 1), ('Jour', 2)]
assert [s['title'] for s in items(8)] == ['Dos']
assert res[9]['body']['total'] == 6 and len(items(9)) == 2 and res[9]['body']['offset'] == 1
assert items(10) == [{'name': 'Music', 'dir': True}]
assert items(11) == [{'name': 'Blue Album', 'dir': True}, {'name': 'escape', 'dir': True}, {'name': 'Other', 'dir': True},
                     {'name': 'unindexed.mp3', 'dir': False}] or items(11) == [{'name': 'Blue Album', 'dir': True}, {'name': 'Other', 'dir': True},
                     {'name': 'unindexed.mp3', 'dir': False}], items(11)
assert res[12]['rc'] == -1 and res[12]['why'] == 'No such folder'          # a symlink off the card
assert res[13]['rc'] == -1
assert res[14]['rc'] == 0 and res[14]['plays'] == ["type=3 name='Blue Album' pos=2"]
assert res[15]['rc'] == -1 and res[15]['why'] == 'That song is not in this album' and not res[15]['plays']
assert res[16]['rc'] == 0 and res[16]['plays'] == ["type=1 name='' pos=2"]   # Jour, in the all-songs title order
assert res[17]['rc'] == 0 and res[17]['plays'] == ["type=1 name='' pos=4"]   # Solo
assert res[18]['rc'] == -1 and 'rescan' in res[18]['why']
assert res[19]['rc'] == -1 and res[19]['why'] == 'No such file'
assert res[20]['rc'] == 0 and res[20]['plays'] == ["type=1 name='' pos=1"]
assert items(21) == [{'pos': 1, 'name': 'Jazz <FM>', 'url': 'http://s.example/jazz', 'codec': 'MP3', 'bitrate': 128, 'country': 'Germany', 'fav': True},
                     {'pos': 2, 'name': 'Talk', 'url': 'https://s.example/talk', 'codec': 'AAC', 'bitrate': 64, 'country': 'France', 'fav': True}], items(21)
assert [x['pos'] for x in items(22)] == [2]
assert res[23]['rc'] == -1 and res[23]['why'] == 'Connect to Wi-Fi to play radio', res[23]   # the radio's own refusal (no Wi-Fi on a host)
assert res[24]['why'] == res[25]['why'] == 'The radio favourites changed - reload'
assert res[26]['rc'] == -1 and res[26]['why'] == 'The radio is not playing'
assert res[27]['rc'] == 0                                                   # not radio: next goes to the player as before
assert res[28]['rc'] == -1 and res[28]['why'] == 'Connect the Disc to Wi-Fi to find stations'
assert res[29]['rc'] == 0 and res[29]['body']['search'] == 'idle' and res[29]['body']['items'] == []
assert res[30]['why'] == 'The search results changed - search again'
assert res[31]['rc'] == 0                                                   # already a favourite: no duplicate
assert res[32]['rc'] == 0 and [x['name'] for x in items(33)] == ['Jazz <FM>']
assert favs.read_text() == 'Jazz <FM>\thttp://s.example/jazz\tMP3\t128\tGermany\n', favs.read_text()   # radio.c's own file format
favs.unlink()
assert res[34]['rc'] == 0 and res[34]['body']['id'] == 2, res[34]
assert res[35]['rc'] == 0 and res[35]['body']['added'] == 1
assert res[36]['rc'] == -1 and res[36]['why'] == 'Already in this playlist'
assert res[37]['rc'] == 0 and res[37]['body']['added'] == 2
assert [(x['title'], x['pos']) for x in items(38)] == [('Tres', 1), ('Nuit', 2), ('Jour', 3)], items(38)
assert res[39]['rc'] == -1 and res[39]['why'] == 'The playlist changed - reload'   # pos 1 holds Tres (3), not Jour (5)
assert res[40]['rc'] == 0, res[40]
assert res[41]['rc'] == 0 and {x['name'] for x in items(42)} == {'Road trip', 'Late drive'}, items(42)
assert res[43]['rc'] == 0 and items(44)[0]['fav'] is True
assert res[45]['rc'] == 0 and [x['title'] for x in items(46)] == ['Jour', 'Solo'] and all(x['fav'] for x in items(46)), items(46)
assert res[47]['rc'] == 0 and [x['name'] for x in items(48)] == ['Road trip']
assert res[49]['rc'] == -1 and res[49]['why'] == 'No scan is running'
assert res[50]['rc'] == 0
stat = {g['name']: dict(g['rows']) for g in res[51]['body']['groups']}
assert stat['Playback']['EQ'] == 'R&B' and stat['Playback']['Library'] == '6 songs, 3 albums, 3 artists' and res[51]['body']['scan']['active'] is False, stat
assert {'Bluetooth', 'Wi-Fi', 'Storage', 'System'} <= set(stat) and 'Uptime' in stat['System'], stat
# no Bluetooth radio on a host: the real bt.c reports it off and refuses device actions
assert res[52]['rc'] == 0 and res[52]['body']['state'] == 'off' and res[52]['body']['devices'] == [], res[52]
k = res[52]['body']['codec']; assert [c['name'] for c in k['choices']] == ['SBC', 'AAC', 'aptX/LDAC Mobile', 'aptX/LDAC Standard', 'aptX/LDAC High'] and k['live'] == '' and k['current'] == 0, k
assert res[53]['rc'] == -1 and res[53]['why'] == 'Bluetooth is off' and res[54]['rc'] == -1, res[53:]
# Working mode goes through the Working mode screen's own switch: on a host it refuses, and the page gets the Disc's own reason
assert res[55]['rc'] == -1 and res[55]['why'] and res[55]['why'] != "Couldn't switch mode", res[55]
assert res[56]['body']['work_mode'] == 0, res[56]['body'].get('work_mode')
print('PASS Remote Control library: lists, search, paging, folders (symlink escape refused), album/song/favourites/all plays, radio favourites, search, save/remove, playlist and favourite edits, rescan, EQ, status, Bluetooth, working mode')
