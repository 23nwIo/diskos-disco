#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MPD server commands against a real song.db through the production main-thread handler (mpd.c, remote.c, queue.c,
musicdb.c), via host-render's MPD_REQS hook. Build host-render first (make -C ui CROSS= host-render).
Like the Remote Control test it uses the host's /usr/data/fiio/db/song.db and /tmp/sdcard (both replaced). The player
is not running on a host: commands that end in a player frame are checked by the frame they send (SENT)."""
import os, shutil, sqlite3, subprocess, tempfile
from pathlib import Path
ui = Path(__file__).resolve().parents[1]
sd = Path('/tmp/sdcard'); shutil.rmtree(sd, ignore_errors=True)
(sd/'Music/Blue Album').mkdir(parents=True); (sd/'Music/Other').mkdir(parents=True); (sd/'Music/Other/Deep').mkdir(parents=True)
db = Path('/usr/data/fiio/db/song.db'); db.parent.mkdir(parents=True, exist_ok=True); db.unlink(missing_ok=True)
Path('/usr/data/diskos_queue.tsv').unlink(missing_ok=True)
con = sqlite3.connect(db)
cols = ('ID INTEGER PRIMARY KEY,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,'
        'ALBUM_ARTIST,IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,'
        'ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST_CODE')
for t in ['SONG', 'MY_LOVE', 'LIST_SONG_0']: con.execute(f'CREATE TABLE {t}({cols})')
con.execute('CREATE TABLE LIST_SONG_3(ID INTEGER PRIMARY KEY, POS_ID)')
con.execute('CREATE TABLE CUSTOM_PLAYLIST_INDEX(LIST_ID INTEGER PRIMARY KEY,LIST_NAME,M3U_PATH)')
con.execute(f'CREATE TABLE CUSTOM_PLAYLIST({cols},PLAYLIST_ID)')
i = 0
for album, artist, folder, titles in [('Blue Album', 'Ana & "Bo"', 'Blue Album', ['Uno', 'Dos', 'Tres']),
                                      ('Café', 'Céline', 'Other', ['Nuit', 'Jour']), ('Deep', 'Zed', 'Other/Deep', ['Solo'])]:
    for t, title in enumerate(titles):
        i += 1; name = f'{t+1:02d} {title}.flac'; p = sd/'Music'/folder/name; p.write_bytes(b'x')
        con.execute('INSERT INTO SONG(ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,'
                    'IS_M3U,M3U_PATH,TITLE_CODE,NAME_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,SONG_PRODUCTION_YEAR) VALUES(' + ','.join('?'*23) + ')',
                    (i, str(p), name, title, album, artist, 'Pop' if i < 4 else 'Jazz', 1, t+1, 0, 0, 0, 0, 200000+i*1000, artist, 0, '',
                     title, name, album, artist, 'Pop', '2001'))
# a CUE image with two tracks
cue = sd/'Music/Other/live.flac'; cue.write_bytes(b'x')
for t in (1, 2):
    i += 1
    con.execute('INSERT INTO SONG(ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,TRACK,IS_CUE,IS_ISO,DURATION) VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                (i, str(cue), 'live.flac', f'Live {t}', 'Live', 'Zed', 'Jazz', t, 1, 0, 100000))
# the player's list: the Blue Album, playing song 2 (LIST_SONG_0.ID 11 .. 13)
for k, sid in enumerate([1, 2, 3]):
    con.execute(f'INSERT INTO LIST_SONG_0 SELECT * FROM SONG WHERE ID={sid}')
    con.execute(f'UPDATE LIST_SONG_0 SET ID={11+k} WHERE ID={sid}')
con.execute("INSERT INTO CUSTOM_PLAYLIST_INDEX VALUES(1,'Road trip','')")
con.execute('INSERT INTO CUSTOM_PLAYLIST SELECT *,1 FROM SONG WHERE ID IN (6,1)')
con.commit(); con.close()
sc = Path('/usr/data/fiio/db/sysconfig.db'); sc.unlink(missing_ok=True)
c2 = sqlite3.connect(sc); c2.execute('CREATE TABLE SYSCONFIG(ID INTEGER PRIMARY KEY, PLAY_MODE, VOLUME)'); c2.execute('INSERT INTO SYSCONFIG VALUES(1,0,60)'); c2.commit(); c2.close()

def run(reqs, track=None):
    with tempfile.TemporaryDirectory() as tmp:
        rf = Path(tmp)/'reqs.txt'; rf.write_text('\n'.join(reqs) + '\n')
        (ui/'tests/host_build/diskos.conf').unlink(missing_ok=True)
        env = {**os.environ, 'MPD_REQS': str(rf), 'ZUNE_LIBRARY_FIXTURE': '1'}
        env['MPD_TRACK'] = track or ''
        r = subprocess.run([str(ui/'tests/host_build/host-render'), '10', '0', 'preview-home', '0', str(Path(tmp)/'o.ppm')],
                           env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr[-3000:]
    out, cur = [], None
    for line in r.stdout.splitlines():
        if line.startswith('REQ '): cur = {'req': line[4:], 'lines': []}; continue
        if cur is None: continue
        if line.startswith('SENT ') and 'sent' not in cur: cur['sent'] = line[5:]; continue
        if line == 'END': out.append(cur); cur = None; continue
        cur['lines'].append(line)
    assert len(out) == len(reqs), r.stdout[-3000:]
    if os.environ.get('MPD_DUMP'):
        for x in out: print(x['req'], '|', x['sent'], '|', [l for l in x['lines'] if l.split(':')[0] in ('Title', 'Id', 'Pos', 'state', 'song', 'songid', 'playlistlength', 'random', 'repeat', 'single', 'volume', 'OK') or l.startswith('ACK')])
    return out
def kv(res, key): return [l.split(': ', 1)[1] for l in res['lines'] if l.startswith(key + ': ')]
def ok(res): assert res['lines'] and res['lines'][-1] == 'OK', res; return res
def ack(res, code): assert res['lines'] and res['lines'][-1].startswith(f'ACK [{code}@'), res; return res['lines'][-1]

# ---- the library, with nothing playing
r = run(['lsinfo', 'lsinfo Music', 'lsinfo "Music/Other"', 'listall Music/Other', 'lsinfo Music/nope', 'lsinfo ../etc',
         'find artist "Ana & \\"Bo\\""', 'search title u', 'find "(album == \'Café\')"', 'find "((genre == \'Jazz\') AND (track != \'1\'))"',
         'list album', 'list albumartist', 'list album group albumartist', 'count genre Pop', 'find artist Zed window 1:2',
         'lsinfo "Music/Other/live.flac/track0002"', 'listallinfo Music/Other/Deep', 'listplaylists', 'listplaylist "Road trip"',
         'listplaylistinfo "Road trip"', 'listplaylist nope', 'stats', 'status', 'currentsong', 'playlistinfo', 'find "(artist =~ \'x\')"',
         'commands', 'outputs', 'nonsense', 'ping\\nstatus', 'getvol', 'search any blue'])
assert kv(ok(r[0]), 'directory') == ['Music']
assert kv(ok(r[1]), 'directory') == ['Music/Blue Album', 'Music/Other']
assert kv(ok(r[2]), 'directory') == ['Music/Other/Deep'] and kv(r[2], 'file') == ['Music/Other/01 Nuit.flac', 'Music/Other/02 Jour.flac', 'Music/Other/live.flac/track0001', 'Music/Other/live.flac/track0002']
assert [l for l in ok(r[3])['lines'][:-1]] == ['file: Music/Other/01 Nuit.flac', 'file: Music/Other/02 Jour.flac', 'directory: Music/Other/Deep',
                                               'file: Music/Other/Deep/01 Solo.flac', 'file: Music/Other/live.flac/track0001', 'file: Music/Other/live.flac/track0002'], r[3]
ack(r[4], 50); ack(r[5], 50)
assert kv(ok(r[6]), 'Title') == ['Uno', 'Dos', 'Tres'] and kv(r[6], 'Artist')[0] == 'Ana & "Bo"' and kv(r[6], 'Date')[0] == '2001'
assert kv(ok(r[7]), 'Title') == ['Uno', 'Nuit', 'Jour'], r[7]
assert kv(ok(r[8]), 'Title') == ['Nuit', 'Jour']
assert kv(ok(r[9]), 'Title') == ['Jour', 'Live 2'], r[9]
assert kv(ok(r[10]), 'Album') == ['Blue Album', 'Café', 'Deep', 'Live'], r[10]
assert kv(ok(r[11]), 'AlbumArtist') == ['Ana & "Bo"', 'Céline', 'Zed']
assert ok(r[12])['lines'][:-1] == ['AlbumArtist: Ana & "Bo"', 'Album: Blue Album', 'AlbumArtist: Céline', 'Album: Café', 'AlbumArtist: Zed', 'Album: Deep', 'Album: Live'], r[12]
assert kv(ok(r[13]), 'songs') == ['3'] and kv(r[13], 'playtime') == ['606']
assert kv(ok(r[14]), 'Title') == ['Live 1'], r[14]
assert kv(ok(r[15]), 'Title') == ['Live 2'] and kv(r[15], 'Track') == ['2']
assert kv(ok(r[16]), 'file') == ['Music/Other/Deep/01 Solo.flac'] and kv(r[16], 'directory') == []
assert kv(ok(r[17]), 'playlist') == ['Road trip']
assert kv(ok(r[18]), 'file') == ['Music/Blue Album/01 Uno.flac', 'Music/Other/Deep/01 Solo.flac'], r[18]
assert kv(ok(r[19]), 'Title') == ['Uno', 'Solo']
ack(r[20], 50)
assert kv(ok(r[21]), 'songs') == ['8'] and kv(r[21], 'albums') == ['4']
assert kv(ok(r[22]), 'state') == ['stop'] and kv(r[22], 'playlistlength') == ['0'] and kv(r[22], 'volume') == ['50'], r[22]
assert ok(r[23])['lines'] == ['OK'] and ok(r[24])['lines'] == ['OK']
assert 'Regular expressions' in ack(r[25], 2)
assert 'command: lsinfo' in ok(r[26])['lines'] and 'command: password' in r[26]['lines']
assert kv(ok(r[27]), 'outputname') == ['Disc']
assert 'unknown command "nonsense"' in ack(r[28], 5)
assert r[29]['lines'][0] == 'list_OK' and 'state: stop' in r[29]['lines'] and r[29]['lines'][-1] == 'OK'
assert kv(ok(r[30]), 'volume') == ['50']
assert kv(ok(r[31]), 'Album') == ['Blue Album'] * 3

# ---- a new list with nothing playing: add collects, play hands it over (type-5 slot), status says stop meanwhile
r = run(['add "Music/Other"', 'add Music/Other/live.flac/track0001', 'playlistinfo', 'status', 'addid "Music/Blue Album/03 Tres.flac"',
         'delete 0', 'move 0 2', 'playlistinfo', 'play 1', 'add nope', 'add http://x/y'])
assert [l for l in kv(ok(r[2]), 'file')] == ['Music/Other/01 Nuit.flac', 'Music/Other/02 Jour.flac', 'Music/Other/Deep/01 Solo.flac',
                                            'Music/Other/live.flac/track0001', 'Music/Other/live.flac/track0002'], r[2]   # the second add is already there
assert kv(r[2], 'Id')[0] == '2000000'
assert kv(ok(r[3]), 'state') == ['stop'] and kv(r[3], 'playlistlength') == ['5']
assert kv(ok(r[4]), 'Id') == ['2000005']
assert kv(ok(r[7]), 'Title') == ['Solo', 'Live 1', 'Jour', 'Live 2', 'Tres'], r[7]
# play: the list is written to the reserved slot in this order and handed to the player (main.c's own local-playback
# admission refuses the final frame on a host, as for artist and playlist plays in remote_library_test)
assert r[8]['sent'].startswith('2 0657000C0008'), r[8]
con = sqlite3.connect(db)
slot = [t for (t,) in con.execute('SELECT TITLE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=-1000 ORDER BY ROWID')]
con.close()
assert slot == ['Solo', 'Live 1', 'Jour', 'Live 2', 'Tres'], slot
ack(r[9], 50); ack(r[10], 50)

# ---- playing the Blue Album at song 2 (LIST_SONG_0.ID 12): Up Next, the diskOS Queue, delete/move rules, options, volume
T = f"{sd}/Music/Blue Album/02 Dos.flac\tDos\t12"
r = run(['status', 'currentsong', 'playlistinfo', 'add "Music/Other/01 Nuit.flac"', 'add "Music/Other/02 Jour.flac"', 'playlistinfo',
         'delete 3', 'delete 0', 'move 1 2', 'playlistinfo', 'playid 1000000', 'play 3', 'random 1', 'repeat 1', 'single 1',
         'setvol 100', 'volume -10', 'seekcur +10', 'seek 1 5', 'seek 0 5', 'pause', 'stop', 'next', 'deleteid 1000001', 'playlistinfo',
         'clear', 'status', 'playlistinfo', 'consume 1', 'crossfade 0', 'plchanges 0', 'save "Night drive"', 'listplaylists', 'rm "Night drive"'],
        track=T)
st = ok(r[0])
assert kv(st, 'state')[0] in ('play', 'pause') and kv(st, 'song') == ['0'] and kv(st, 'songid') == ['12'] and kv(st, 'playlistlength') == ['2'], st
assert kv(st, 'nextsongid') == ['13'] and kv(st, 'time') == ['5:200'] and kv(st, 'elapsed') == ['5.000'], st
assert kv(ok(r[1]), 'Title') == ['Dos'] and kv(r[1], 'Pos') == ['0'] and kv(r[1], 'Album') == ['Blue Album']
assert kv(ok(r[2]), 'Title') == ['Dos', 'Tres'] and kv(r[2], 'Id') == ['12', '13']
ok(r[3]); ok(r[4])
assert kv(ok(r[5]), 'Title') == ['Dos', 'Nuit', 'Jour', 'Tres'] and kv(r[5], 'Id') == ['12', '1000000', '1000001', '13'], r[5]
assert 'Only songs added' in ack(r[6], 52) and 'playing now' in ack(r[7], 52)
ok(r[8]); assert kv(ok(r[9]), 'Title') == ['Dos', 'Jour', 'Nuit', 'Tres'], r[9]
ok(r[10])                                                        # playid of a queued song: queue_jump
# Up Next jumps, seeks and Next end in main.c's own admission (ui_queue_jump / ui_seek_to / queue_next), which a host
# refuses: the refusal must come back as an ACK with the reason, never as OK
assert 'refused' in ack(r[11], 52)
ok(r[12]); assert r[12]['sent'] == '1 0102000C0001', r[12]       # random 1 = the Shuffle play mode
assert 'random off first' in ack(r[13], 52) and 'random off first' in ack(r[14], 52)
ok(r[15]); assert r[15]['sent'] == '1 0715000C0078', r[15]       # setvol 100 = 120 on the Disc
ok(r[16]); assert r[16]['sent'] == '1 0715000C0030', r[16]       # volume -10 from 50 % (60) = 40 % (48)
ack(r[17], 52); assert 'Only the song playing now' in ack(r[18], 52); ack(r[19], 52)
ok(r[20]); assert r[20]['sent'] == '1 0201000C0000', r[20]       # pause while playing
ok(r[21]); ack(r[22], 52)
ok(r[23]); assert kv(ok(r[24]), 'Title') == ['Dos', 'Jour'], r[24]
ok(r[25]); assert kv(ok(r[26]), 'state') == ['stop'] and kv(r[26], 'playlistlength') == ['0'], r[26]
assert ok(r[27])['lines'] == ['OK']
ack(r[28], 52); ok(r[29])
ok(r[30]); ok(r[31]); assert kv(ok(r[32]), 'playlist')[-1] == 'Night drive'; ok(r[33])
print('mpd_library_test: all passed')
