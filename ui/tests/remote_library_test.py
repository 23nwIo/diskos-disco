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
sd = Path('/tmp/sdcard'); subprocess.run(['umount', str(sd)], capture_output=True); shutil.rmtree(sd, ignore_errors=True)
sd.mkdir(parents=True); subprocess.run(['mount', '--bind', str(sd), str(sd)], check=True)   # a mounted card: the radio favourites live on it (default)
(sd/'Music/Blue Album').mkdir(parents=True, exist_ok=True); (sd/'Music/Other').mkdir(parents=True)
os.symlink('/etc', sd/'Music/escape')
Path('/usr/data/diskos_queue.tsv').unlink(missing_ok=True); (ui/'tests/host_build/queue.tsv').unlink(missing_ok=True)   # host builds keep the Queue here
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
up = sd/'PartyUploads/Party_1'; up.mkdir(parents=True); (up/'Anna_Mine.mp3').write_bytes(b'x' * 1000); (up/'Anna_x.txt').write_text('x')   # saved by the server's upload thread
Path('/tmp/party-stations.txt').unlink(missing_ok=True)
favs = (sd/'radio_favorites.txt'); favs.write_text('Jazz <FM>\thttp://s.example/jazz\tMP3\t128\tGermany\n'
                                                            'Talk\thttps://s.example/talk\tAAC\t64\tFrance\n')

reqs = ['GET /api/library/albums', 'GET /api/library/albums?q=caf', 'GET /api/library/album?name=Blue+Album',
        'GET /api/library/artists', 'GET /api/library/artist?name=C%C3%A9line', 'GET /api/library/playlists',
        'GET /api/library/playlist?id=1', 'GET /api/library/favorites', 'GET /api/library/songs?q=dos',
        'GET /api/library/songs?limit=2&offset=1', 'GET /api/library/folder', 'GET /api/library/folder?path=Music',
        'GET /api/library/folder?path=Music%2Fescape', 'GET /api/library/folder?path=Music%2Fnope',
        'POST /api/play_item kind=album&name=Blue+Album&song_id=2', 'POST /api/play_item kind=album&name=Blue+Album&song_id=4',
        'POST /api/play_item kind=favorites&pos=2', 'POST /api/play_item kind=song&song_id=6',
        'POST /api/play_item kind=file&path=Music%2Funindexed.mp3', 'POST /api/play_item kind=file&path=Music%2Fescape%2Fpasswd',
        'POST /api/play_item kind=all', 'POST /api/play_item kind=folder', 'POST /api/play_item kind=folder&path=Music%2Fnope', 'POST /api/play_item kind=folder&path=Music%2FOther', 'GET /api/library/radio', 'GET /api/library/radio?q=fran',
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
        'GET /api/bluetooth', 'POST /api/bluetooth action=search', 'POST /api/bt_device action=connect&mac=00:11:22:33:44:55', 'POST /api/work_mode mode=3', 'GET /api/status', 'POST /api/radio_fav_add kind=radio_now',
        # 61.. the queue (queueview.c): the list's version starts at 1 on its first read and moves on every change
        'POST /api/queue action=add&kind=song&song_id=1', 'POST /api/queue action=add&kind=album&name=Caf%C3%A9+%3CNoir%3E',
        'GET /api/queue', 'POST /api/queue action=move&ver=1&i=2&to=0', 'POST /api/queue action=remove&ver=1&i=0', 'GET /api/queue',
        'POST /api/queue action=remove&ver=2&i=0', 'GET /api/queue', 'POST /api/queue action=clear&ver=3', 'GET /api/queue',
        'POST /api/queue action=jump&ver=4&kind=queue&i=0', 'POST /api/queue action=clear&ver=4', 'POST /api/queue action=add&kind=artist&name=Nobody', 'POST /api/queue action=add&kind=next&b=1', 'POST /api/list_end mode=1',
        # 76.. party mode: the host's settings (on the card), then guests through /Party_1/ ("~")
        'GET /api/party', 'POST /api/party on=1', 'POST /api/party on=1&name=Party_1&total=2',
        'GET ~/api/state', 'POST ~/api/queue action=add&kind=song&song_id=1',
        'POST ~/api/queue action=add&kind=song&song_id=1&g=AnnaAnnaAnna1234&n=Anna', 'POST ~/api/queue action=add&kind=song&song_id=2&g=AnnaAnnaAnna1234',
        'POST ~/api/queue action=add&kind=song&song_id=3&g=AnnaAnnaAnna1234', 'POST ~/api/queue action=add&kind=song&song_id=1&g=BenBenBenBen1234',
        'POST ~/api/queue action=add&kind=song&song_id=4&g=BenBenBenBen1234&n=Ben', 'GET ~/api/queue?g=BenBenBenBen1234', 'GET /api/queue',
        'POST ~/api/next g=BenBenBenBen1234', 'POST ~/api/volume value=50&g=BenBenBenBen1234', 'POST ~/api/eq preset=1', 'GET ~/system',
        'POST ~/api/queue action=add&kind=file&path=Music%2FOther%2F01+Solo.flac&g=CarlCarlCarl1234', 'POST ~/api/play_item kind=radio_hit&pos=1&url=http%3A%2F%2Fx&g=CarlCarlCarl1234',
        'GET ~/api/state?g=AnnaAnnaAnna1234', 'GET /api/party', 'POST /api/party name=x',
        'POST /api/party np=0', 'GET ~/api/queue?g=BenBenBenBen1234', 'GET ~/api/library/album?name=Other&g=BenBenBenBen1234', 'GET /api/library/album?name=Other',
        'POST ~/api/radio name=Jazz&url=http%3A%2F%2Fs.example%2Fj&g=BenBenBenBen1234', 'POST /api/party radio=1',
        'POST ~/api/radio name=Jazz&url=http%3A%2F%2Fs.example%2Fj&now=1&g=BenBenBenBen1234', 'POST ~/api/radio name=Jazz&url=http%3A%2F%2Fs.example%2Fj&g=BenBenBenBen1234',
        'POST /api/party skip=1', 'POST ~/api/radio name=Jazz&url=http%3A%2F%2Fs.example%2Fj&now=1&g=BenBenBenBen1234', 'GET ~/api/state?g=BenBenBenBen1234',
        # 108.. a stream address of the host's own, saved straight into the radio favourites
        'POST /api/radio_fav_add kind=stream&name=My+%3CStream%3E&url=https%3A%2F%2Fmine.example%2Flive', 'POST /api/radio_fav_add kind=stream&name=X&url=ftp%3A%2F%2Fx',
        'POST ~/api/radio_fav_add kind=stream&name=X&url=http%3A%2F%2Fx&g=BenBenBenBen1234', 'GET /api/library/radio',
        # 112.. a guest's upload is on the card: into the library (not the queue); a guest's own stations for the host
        'UPLOADED PartyUploads/Party_1/Anna_Mine.mp3 AnnaAnnaAnna1234', 'UPLOADED PartyUploads/Party_1/Anna_Mine.mp3 AnnaAnnaAnna1234',
        'UPLOADED PartyUploads/Party_1/Anna_x.txt AnnaAnnaAnna1234', 'GET /api/queue',
        'POST ~/api/stations g=AnnaAnnaAnna1234&s=http%3A%2F%2Fa.example%2Fs+Anna+%3CFM%3E&s=https%3A%2F%2Fb.example+B', 'GET /api/party',
        # 118.. the favourite symbols and colours: defaults, then the host's own on the card
        'GET ~/api/look', 'POST /api/look l=symbol%3D%E2%98%AD&l=color%3DBlack+%23000000', 'GET ~/api/look',
        # -5.. Bandcamp for guests (Jo, 2026-11-22): off unless the host allows it; their own search state
        'GET ~/api/bandcamp?g=DoraDoraDora1234', 'POST /api/party bandcamp=1', 'GET ~/api/bandcamp?g=DoraDoraDora1234',
        'POST ~/api/bandcamp action=play&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&g=DoraDoraDora1234',
        'POST /api/play_item kind=folder&path=Music%2FOther']   # last: the folder play's slot is checked after the run
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
party_conf = (sd/'.diskos/state/party.conf').read_text()
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
assert items(10) == [{'name': 'Music', 'dir': True}, {'name': 'PartyUploads', 'dir': True}]
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
# a folder plays as the exact queue (type 5, the reserved slot) of its songs and its subfolders' in path order. The host's
# player is not in local playback for that send, so the play itself is refused here; the queue it wrote is checked.
assert res[22]['rc'] == -1 and res[22]['why'] == 'No such folder'
slot = [t for (t,) in sqlite3.connect(db).execute('SELECT TITLE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000 ORDER BY rowid')]
assert slot == ['Nuit', 'Solo', 'Jour'], slot
assert items(24) == [{'pos': 1, 'name': 'Jazz <FM>', 'url': 'http://s.example/jazz', 'codec': 'MP3', 'bitrate': 128, 'country': 'Germany', 'fav': True},
                     {'pos': 2, 'name': 'Talk', 'url': 'https://s.example/talk', 'codec': 'AAC', 'bitrate': 64, 'country': 'France', 'fav': True}], items(24)
assert [x['pos'] for x in items(25)] == [2]
assert res[26]['rc'] == -1 and res[26]['why'] == 'Connect to Wi-Fi to play radio', res[26]   # the radio's own refusal (no Wi-Fi on a host)
assert res[27]['why'] == res[28]['why'] == 'The radio favourites changed - reload'
assert res[29]['rc'] == -1 and res[29]['why'] == 'The radio is not playing'
assert res[30]['rc'] == 0                                                   # not radio: next goes to the player as before
assert res[31]['rc'] == -1 and res[31]['why'] == 'Connect the Disc to Wi-Fi to find stations'
assert res[32]['rc'] == 0 and res[32]['body']['search'] == 'idle' and res[32]['body']['items'] == []
assert res[33]['why'] == 'The search results changed - search again'
assert res[34]['rc'] == 0                                                   # already a favourite: no duplicate
assert res[35]['rc'] == 0 and [x['name'] for x in items(36)] == ['Jazz <FM>']
assert favs.read_text().startswith('Jazz <FM>\thttp://s.example/jazz\tMP3\t128\tGermany\n'), favs.read_text()   # radio.c's own file format
assert 'My <Stream>\thttps://mine.example/live\t' in favs.read_text(), favs.read_text()   # a stream of your own (requests 108..)
favs.unlink()
assert res[37]['rc'] == 0 and res[37]['body']['id'] == 2, res[37]
assert res[38]['rc'] == 0 and res[38]['body']['added'] == 1
assert res[39]['rc'] == -1 and res[39]['why'] == 'Already in this playlist'
assert res[40]['rc'] == 0 and res[40]['body']['added'] == 2
assert [(x['title'], x['pos']) for x in items(41)] == [('Tres', 1), ('Nuit', 2), ('Jour', 3)], items(41)
assert res[42]['rc'] == -1 and res[42]['why'] == 'The playlist changed - reload'   # pos 1 holds Tres (3), not Jour (5)
assert res[43]['rc'] == 0, res[43]
assert res[44]['rc'] == 0 and {x['name'] for x in items(45)} == {'Road trip', 'Late drive'}, items(45)
assert res[46]['rc'] == 0 and items(47)[0]['fav'] is True
assert res[48]['rc'] == 0 and [x['title'] for x in items(49)] == ['Jour', 'Solo'] and all(x['fav'] for x in items(49)), items(49)
assert res[50]['rc'] == 0 and [x['name'] for x in items(51)] == ['Road trip']
assert res[52]['rc'] == -1 and res[52]['why'] == 'No scan is running'
assert res[53]['rc'] == 0
stat = {g['name']: dict(g['rows']) for g in res[54]['body']['groups']}
assert stat['Playback']['EQ'] == 'R&B' and stat['Playback']['Library'] == '6 songs, 3 albums, 3 artists' and res[54]['body']['scan']['active'] is False, stat
assert {'Bluetooth', 'Wi-Fi', 'Storage', 'System'} <= set(stat) and 'Uptime' in stat['System'], stat
# no Bluetooth radio on a host: the real bt.c reports it off and refuses device actions
assert res[55]['rc'] == 0 and res[55]['body']['state'] == 'off' and res[55]['body']['devices'] == [], res[55]
k = res[55]['body']['codec']; assert [c['name'] for c in k['choices']] == ['SBC', 'AAC', 'aptX/LDAC Mobile', 'aptX/LDAC Standard', 'aptX/LDAC High'] and k['live'] == '' and k['current'] == 0, k
assert res[56]['rc'] == -1 and res[56]['why'] == 'Bluetooth is off' and res[57]['rc'] == -1, res[53:]
# Working mode goes through the Working mode screen's own switch: on a host it refuses, and the page gets the Disc's own reason
assert res[58]['rc'] == -1 and res[58]['why'] and res[58]['why'] != "Couldn't switch mode", res[58]
assert res[59]['body']['work_mode'] == 0, res[59]['body'].get('work_mode')
assert res[60]['rc'] == -1 and res[60]['why'] == 'No station is playing', res[60]
print('PASS Remote Control library: lists, search, paging, folders (symlink escape refused), album/song/favourites/all plays, radio favourites, search, save/remove, playlist and favourite edits, rescan, EQ, status, Bluetooth, working mode, folder play')
subprocess.run(['umount', str(sd)], capture_output=True)
# the queue
assert res[61]['rc'] == 0 and res[61]['body'] == {'ok': True, 'added': 1}, res[61]
assert res[62]['rc'] == 0 and res[62]['body']['added'] == 2, res[62]
q = res[63]['body']; assert q['ver'] == 1 and q['more'] == 0, q
assert q['items'][0]['k'] == 'cur' and [(x['k'], x['i'], x['t'], x['a'], x['f']) for x in q['items'][1:]] == [('queue', 0, 'Uno', 'Ana & "Bo"', '01 Uno.flac'), ('queue', 1, 'Nuit', 'Céline', '01 Nuit.flac'), ('queue', 2, 'Jour', 'Céline', '02 Jour.flac')], q
assert res[64]['rc'] == 0, res[64]
assert res[65]['rc'] == -1 and res[65]['why'] == 'The queue changed - here it is again', res[65]
q = res[66]['body']; assert q['ver'] == 2 and [x['t'] for x in q['items'][1:]] == ['Jour', 'Uno', 'Nuit'], q
assert res[67]['rc'] == 0 and [x['t'] for x in res[68]['body']['items'][1:]] == ['Uno', 'Nuit'] and res[68]['body']['ver'] == 3, res[67:69]
assert res[69]['rc'] == 0 and [x['k'] for x in res[70]['body']['items']] == ['cur'] and res[70]['body']['ver'] == 4, res[69:71]
assert res[71]['rc'] == -1 and res[71]['why'] == 'The queue changed - here it is again', res[71]
assert res[72]['rc'] == -1 and res[72]['why'] == 'Nothing is queued', res[72]
assert res[73]['rc'] == -1 and res[73]['why'] == 'Nothing new to add', res[73]
assert res[74]['rc'] == -1 and res[74]['why'] == 'The queue changed - here it is again', res[74]   # no Up next on a host
assert res[75]['rc'] == -1 and res[75]['why'].startswith('Only with the own player'), res[75]
print('PASS Remote Control queue: add (song, album), list, move, remove, clear, stale version refused')
# party mode
p = res[76]['body']; assert p['settings'] == {'on': 0, 'vol': 1, 'maxvol': 100, 'np': 1, 'queue': 1, 'skip': 0, 'total': 3, 'consec': 1, 'repeat': 0, 'radio': 0, 'names': 1, 'upload': 0, 'upmax': 100, 'bandcamp': 0, 'name': '', 'own_player': False} and p['guests'] == [], p
assert res[77]['rc'] == -1 and res[77]['why'] == 'Choose a name for the link first', res[77]
assert res[78]['rc'] == 0 and res[78]['body']['settings']['name'] == 'Party_1' and res[78]['body']['settings']['total'] == 2, res[78]
bc = res[-5:-1]
assert bc[0]['rc'] == -1 and bc[0]['why'] == "The host doesn't let guests pick from Bandcamp", bc[0]
assert bc[1]['rc'] == 0 and 'bandcamp=1\n' in party_conf, party_conf
assert bc[2]['rc'] == 0 and bc[2]['body'] == {'ok': True, 'search': {'state': 'idle', 'q': '', 'items': []}, 'page': {'state': 'idle', 'url': ''}, 'picks': []}, bc[2]
assert bc[3]['req'].startswith('POST ~/api/bandcamp action=play') and bc[3]['rc'] != 0, bc[3]   # the host's own actions: not for guests
assert 'name=Party_1\n' in party_conf and 'on=1\n' in party_conf and 'total=2\n' in party_conf, party_conf   # on the SD card
g = res[79]['body']; assert g['party']['left'] == 2 and g['party']['mine'] == 0 and g['party']['radio'] == 0 and 'volume' in g and 'title' in g and 'balance' not in g and 'outputs' not in g, g
assert res[80]['rc'] == -100 - 8, res[80]                                         # adding needs the guest's id (400)
assert res[81]['rc'] == 0 and res[81]['body'] == {'ok': True, 'added': 1, 'place': 1, 'left': 1}, res[81]
assert res[82]['rc'] == 0 and res[82]['body']['place'] == 2, res[82]             # only Anna's songs wait: she may add in a row
assert res[83]['rc'] == -1 and 'once one plays' in res[83]['why'], res[83]       # but never more than her total
assert res[84]['rc'] == -1 and res[84]['why'] == 'That song is already in the queue', res[84]
assert res[85]['rc'] == 0 and res[85]['body']['place'] == 2, res[85]             # Ben's first song goes before Anna's second
q = res[86]['body']; assert [(x['k'], x['t'], x['by'], x['mine']) for x in q['items'][1:]] == [('queue', 'Uno', 'Anna', False), ('queue', 'Nuit', 'Ben', True), ('queue', 'Dos', 'Anna', False)] and 'f' not in q['items'][1], q
assert [x.get('by') for x in res[87]['body']['items'][1:4]] == ['Anna', 'Ben', 'Anna'], res[87]
assert res[88]['rc'] == -1 and res[88]['why'] == 'Skipping is off at this party', res[88]
assert res[89]['rc'] == 0, res[89]
assert res[90]['rc'] == -100 and res[91]['rc'] == -100, res[90:92]               # 404: not for guests
assert res[92]['rc'] == 0 and res[92]['body']['place'] == 3, res[92]             # a file from Folders: Carl's first song joins round 1
assert res[93]['rc'] == -100, res[93]                                            # search hits are picked through api/radio now
assert res[94]['body']['party']['mine'] == 2 and res[94]['body']['party']['left'] == 0 and res[94]['body']['party']['you'] == 'Anna', res[94]
assert sorted((x['name'], x['queued']) for x in res[95]['body']['guests']) == [('', 1), ('Anna', 2), ('Ben', 1)], res[95]
assert res[96]['rc'] == -100 - 8, res[96]                                        # a name of one letter
print('PASS Remote Control party mode: settings on the card, guest link, total and turns, repeats, names, scaled volume, guests kept out of the rest')
q = res[98]['body']['items']   # "See what plays now" off: no Playing now, others' songs without title (a music quiz)
assert res[97]['rc'] == 0 and [(x['t'], x['a'], x['d'], x['hidden'], x['by']) for x in q] == [('', '', 0, True, 'Anna'), ('Nuit', 'Céline', 200004, False, 'Ben'), ('', '', 0, True, 'a guest'), ('', '', 0, True, 'Anna')], q
print('PASS Remote Control party mode: a secret queue when guests may not see what plays')
assert [x['fav'] for x in res[99]['body']['items']] == [0] and [x['fav'] for x in res[100]['body']['items']] == [True], res[99:101]   # the host's favourites stay theirs
print('PASS Remote Control party mode: the host\'s favourites hidden from guests')
assert res[101]['why'] == "The host doesn't let guests choose the radio" and res[102]['rc'] == 0, res[101:103]
assert res[103]['why'].startswith('Skipping is off') and res[104]['why'].startswith('Only at once here'), res[103:105]   # no own player on a host
assert res[105]['rc'] == 0 and res[106]['rc'] == -1 and res[106]['why'] == 'Connect to Wi-Fi to play radio', res[105:107]   # the radio's own refusal
p = res[107]['body']['party']; assert p['radio'] == 1 and p['radio_now'] == 1 and p['radio_end'] == 0, p
print('PASS Remote Control party mode: guests choose a station (now with skipping, or after the queue)')
assert res[108]['rc'] == 0 and res[109]['rc'] == -100 - 8 and res[110]['rc'] == -100, res[108:111]   # ftp is no stream; guests keep theirs in the browser
assert [(x['name'], x['url']) for x in items(111)] == [('Jazz <FM>', 'http://s.example/jazz'), ('My <Stream>', 'https://mine.example/live')], items(111)
print('PASS Remote Control radio: a stream address of your own saved to the favourites')
u = res[112]['body']; assert res[112]['rc'] == 0 and u['file'] == 'PartyUploads/Party_1/Anna_Mine.mp3' and u['title'] == 'Anna_Mine' and u['song_id'] > 6, res[112]
assert res[113]['body']['song_id'] == u['song_id'], res[113]                     # once in the library
assert res[114]['rc'] == -1 and res[114]['why'] == "Saved, but the Disc can't read that file", res[114]
assert all(x.get('t') != 'Anna_Mine' for x in res[115]['body']['items']), res[115]   # not queued (Jo)
con = sqlite3.connect(db); row = con.execute('SELECT PATH,TITLE,ARTIST FROM SONG WHERE ID=?', (u['song_id'],)).fetchone(); con.close()
assert row == (str(sd/'PartyUploads/Party_1/Anna_Mine.mp3'), 'Anna_Mine', 'Unknown artist'), row
assert res[116]['rc'] == 0, res[116]
st = [g['stations'] for g in res[117]['body']['guests'] if g['name'] == 'Anna']; assert st == [[{'name': 'Anna <FM>', 'url': 'http://a.example/s'}, {'name': 'B', 'url': 'https://b.example'}]], res[117]
assert Path('/tmp/party-stations.txt').read_text().splitlines()[1:] == ['Anna\tAnna <FM>\thttp://a.example/s', 'Anna\tB\thttps://b.example'], Path('/tmp/party-stations.txt').read_text()
print('PASS Remote Control party mode: uploads into the library, guests\' stations for the host')
lk = res[118]['body']; assert lk['symbols'] == ['❤️', '⭐', '☭', '𖤐', '𐕣'] and [c['name'] for c in lk['colors']][-1] == 'Black' and not lk['on_card'] and lk['bad_line'] == 0, lk
assert res[119]['rc'] == 0 and res[120]['body']['symbols'] == ['☭'] and res[120]['body']['colors'] == [{'name': 'Black', 'dark': '#000000', 'light': '#000000'}] and res[120]['body']['on_card'], res[119:121]
assert (sd/'.diskos/state/look.conf').read_text() == 'symbol=☭\ncolor=Black #000000\n'
print('PASS Remote Control look: favourite symbols and colours from the card')
