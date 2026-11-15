# Player protocol used by diskOS

What the diskOS UI (`mq_ui`) sends to the player and what it reads back. An own player (step 2 of
[MQ_PLAYER_REPLACEMENT.md](MQ_PLAYER_REPLACEMENT.md)) that speaks exactly this is a drop-in replacement for the stock
`/usr/bin/mq_player`: the UI does not need to know which player runs, so no UI change is needed to switch between
them. Written 2026-11-15 from `ui/ipc.c` and every call site in `ui/*.c` (dev 16ca8f8), plus
[COMMAND_MAP.md](COMMAND_MAP.md), [RE_CATALOGUE.md](RE_CATALOGUE.md) and the device traces in
[MQ_PLAYER_TRACE.md](MQ_PLAYER_TRACE.md). The stock player understands about 200 tags; diskOS uses the ones below.

## Transport

- Two POSIX message queues. `/player`: UI to player; the UI opens it `O_WRONLY|O_NONBLOCK` and never creates it.
  `/ui`: player to UI; the UI opens it `O_RDONLY`, and creates it (maxmsg 10, msgsize 8192) if the player has not yet.
  The player owns both and may unlink and recreate them on its own start; the UI notices that by comparing the queue's
  `(dev, ino)` with a fresh open (`ipc_health_check`, `rx_do_reopen`) and reattaches. A replacement player must
  therefore create `/player` (msgsize at least 8192, the play frame carries a name of up to 380 bytes, `a2` frames
  can be several KiB) and open `/ui`, creating it with the same attributes if absent.
- Frame = ASCII text, no terminator: `TAG` (4 hex) + `LEN` (4 hex, length of the whole frame in characters) + data.
  Most commands are "class 1" with one 4-hex value: `TTTT000Cvvvv`.
- An idle stock player sends nothing to `/ui`. The UI solicits state with `0202` and treats any received frame as
  "the player is up" (`ipc_rx_frames`).

## Commands (UI to player)

"Step 2" says what a first own player (local playback, headphones) has to do with the command: **do** = implement,
**keep** = accept and remember (no audible effect yet, or the value is applied later), **ack** = accept and ignore
(the feature belongs to a later step; the stock player stays the fallback for it).

| Tag | Frame | Meaning | Sent from | Step 2 |
|---|---|---|---|---|
| 0100 | `0100` LEN `<pos 4h><type 4h><name>` | Play a list from index `pos` (0-based). The player builds its queue from `song.db` by `type`: 1 all songs, 2 artist, 3 album, 5 custom playlist (`name` = its sequence number in `CUSTOM_PLAYLIST_INDEX` by `LIST_ID`, diskOS sends 0), 6 favourites, 7 artist+album (`{"artist":"X", "album":"Y"}`), 8 genre+album (`{"style":"X", "album":"Y"}`), 10 genre; type 0 = jump to `pos` in the current queue without a rebuild (`LIST_SONG_0` ID order, also in shuffle). `name` selects the artist/album/genre | main.c `ui_play_list_ex`, `ui_play_restore` | do |
| 0102 | `0102000C000v` | Play mode: 0 in order, 1 shuffle, 2 repeat one, 3 repeat all, 4 single | main.c `send_play_mode` | do |
| 0103 | `01030010<ms 8h>` | Seek to ms | main.c `ui_seek_to` | do |
| 0104 | `0104000C000v` | Favourite on/off for the current song (`MY_LOVE` table) | main.c | do |
| 0201 | `0201000C000v` | Transport: 0 play/pause toggle, 1 next, 2 previous | ui.c, themes, remote.c | do |
| 0202 | `02020008` | Request current state; the answer is an `a2` frame | main.c, wifi.c | do |
| 0622 | `0622000C0001` | Rescan (stock: no-op on V2.09; diskOS scans itself, ui/scanner.c) | - | ack |
| 0642 | `0642000C000v` | USB gadget selector: 0 none, 1 and 2 with the USB modes, 5 USB audio out | main.c, modes.c, usbdrive.c | ack; 1 (USB storage) done, see MQ_PLAYER_TRACE.md run 5 |
| 0645 / 0649 | `06xx000C000v` | Gain 0 low / 1 high (tag depends on firmware, ui/fwcaps.c) | main.c `ui_set_gain` | do |
| 0646 | `0646000C000v` | Rotation/key layout option | controls.c | keep |
| 0647 | `0647000C000v` | Gapless on/off | main.c | keep |
| 0648 | `0648000C000v` | Artist grouping: 0 artist, 1 album artist (queue building for type 2) | main.c | do |
| 064d | `064d000C000v` | Track numbers (stored only) | main.c | keep |
| 0653 | `0653000C000v` | DAC filter 0..5 | main.c `ui_set_dac_filter` | do |
| 0657 | `0657000C000v` | Work mode: 8 local playback; 1 USB DAC, 6 BT sink, 7 BT streaming, 10 AirPlay, 12 DLNA | main.c, modes.c | do 8, ack others |
| 0664 | `0664000C<s 4h>` | Idle power off after s seconds (0 off) | power.c | do |
| 0666 | `0666000C000v` | Output route: 6 internal DAC, 4 S/PDIF, 3 USB audio, 2 Bluetooth | main.c, modes.c | do 6, ack others |
| 0684 | `0684000C000v` | Resume memory mode 0..2 | main.c | keep |
| 0687 | `0687000C000v` | Play through folders on/off | main.c | keep |
| 0689 | `0689000C<p 4h>` | EQ preset 0..10, custom PEQ slots 11..20 | main.c `ui_apply_eq` | do |
| 06b3 | `06b3` LEN `<codec 4h>0000<MAC>` | Bluetooth codec select | bt.c, main.c | ack |
| 06c1 | `06c1000C0000` | Start the player's Bluetooth init | main.c | ack |
| 0711 | `0711000C<v 4h>` | Maximum volume 0..120 | main.c | do |
| 0713 | `0713000C<v 4h>` | Balance: 0 centre, `00NN` one side, `01NN` the other, N 1..10 | main.c `ui_set_balance` | do |
| 0715 | `0715000C<v 4h>` | Volume 0..120 | main.c | do |
| 0718 | `0718000C000v` | ReplayGain 0 off, 1 track, 2 album | main.c | keep (later do) |
| 0806 | `0806000C<s 4h>` | Sleep timer s seconds, 0 cancels. diskOS's "Shut down player" sends 1 s after it has unmounted the card, and the player runs its power off routine | power.c | do |
| 0808 | `0808000C000v` | Charge option on/off | power.c | keep |
| 0812 | `0812000C000v` | DRE: 0 on, 1 off | main.c `ui_set_dre` | do |
| 0818 | `0818000C0000` | Sent before switching to Bluetooth receiver mode | main.c | ack |
| 0820-0822 | `08xx000C000v` | Hardware key single/double/long press action | controls.c | keep |

The UI keeps its own copy of every setting (diskos.conf) and re-sends it once the player answers
(`ui_reapply_audio`), so a player that starts with defaults is corrected within seconds. The stock player also
persists settings to `sysconfig.db` (`SYSCONFIG` table) on shutdown, and the UI reads some of them from there (for
example `BT_STATUS`, `MAX_VOL`).

## Replies (player to UI, on `/ui`)

| Frame | Meaning | Parsed in |
|---|---|---|
| `a1030010<ms 8h>` | Playback position in ms (`LEN` = total length, payload up to 15 hex chars) | ipc.c `parse_frame` |
| `a2` LEN `<json>` | State JSON: `state` (number), `playing_num`, `work_mode`, `love` (bool), `song` = a JSON object **as a string** with `song_name`, `song_artist_name`, `song_album_name`, `song_file_path` (required for a track to count), `song_duration_time` (ms), `song_sample_rate`, `is_dsd`, `pos_id`. `song` as an empty string = no current track. Partial frames (only `state`) are allowed | ipc.c `parse_a2_frame` |
| `a714000C00<VV 2h>...` | Volume level 0..120 (hex chars 10-11) | ipc.c |
| `a607000C<mode 4h>` | Work mode, announced after a `0657` change (8 = local playback) | ipc.c |

### Measured on a live stream (protocol capture, stock player, 2026-11-15)

Recorded by the player trace (`mq_send`/`mq_receive` hooks) while playing, pausing, resuming, seeking and with
natural track changes. Hex in the stock player's frames is upper case (`a20202A2`, `a10300100002BB38`).

- **`a1` position**: from its own thread once a second (1.000 s apart) while playing, nothing while paused.
- **`state`**: `0` = playing, `1` = paused, `2` = a new track is being loaded (only inside the full `a2` frame at a
  track start). Pause and resume send the partial frame `a2020013{"state":1}` / `{"state":0}` twice.
- **Full `a2`** at every track start, tag `a202`: `{"song":"<song JSON as a string>","love":false,"playerflag":3,
  "state":2,"playing_num":"1/8","work_mode":"LOCAL"}`. The song JSON has, in this order: `id` (SONG.ID), `pos_id`
  (LIST_SONG_0.ID), `song_name`, `song_duration_time` (ms), `song_file_path` (`/` written as `\/`),
  `song_sample_rate`, `song_encoding_rate` (bits), `song_bit_rate` (kbit/s), `song_artist_name`, `song_album_name`,
  `song_style_name` (`"(null)"` when empty), `song_track`, `song_channel`, `is_sacd`, `is_cue`, `is_dsd`, `is_m3u`,
  `m3u_file_path` (`"(null)"`).
- **Play `0100`**: `a102000C<mode>` (the play mode), `aa1c000C0000`, the full `a2`, `aa05000C0001`, `aa1c000C0000`,
  then `{"state":0}` twice. A natural track change: `{"state":1}`, the full `a2`, `aa1c`, `aa05`, `aa1c`,
  `{"state":0}` twice. `aa1c` and `aa05` are not used by the UI; their meaning is open.
- **Seek `0103`**: answered only with `{"state":0}`; the next `a1` carries the new position.
- **`0202` while idle** (no track loaded): no answer. At start, before any command, the player sends `aa1d000C0061`
  (twice), `a634000C0000`, `aa24` with all its settings as JSON (`volume`, `balance` 258 = `0x0102`, `gain`,
  `dac_filter`, `dre_status`, `eq_type`, `play_mode`, `input_mode` 8, ...), `a102000C0000`, `a712000C<balance>`, and
  after opening `/player` `a620` with `SN`, `NB` and `WIFI_MAC`. After the first `0657`: `aa1c`, `a824000C0000`,
  `a607000C0008`.
- Not seen in this capture: `a714` (no volume change was made), Next/Previous (see below).

The UI decides "playing" from the `a1` position advancing (ui/playstate.c, 1.6 s window), so an own player has to send
`a1` once a second while playing and stop while paused, as the stock player does.

## Shared data the player uses

- `/usr/data/fiio/db/song.db`: the library. diskOS writes the `SONG` table itself (ui/scanner.c, stock schema). On
  `0100` the player rebuilds the queue table `LIST_SONG_0` from `SONG` (or `MY_LOVE`, or a custom playlist) with
  fixed `ORDER BY` clauses, and in shuffle a permutation `LIST_SONG_3`; it copies `SONG.DURATION` into the rows. The
  UI reads these tables for Up Next and to land on the tapped song, and copies the exact orderings (ui/musicdb.c:
  `mdb_play_pos`, `MDB_FAV_ORDER`, the Up Next reader), so the own player must build them identically; `a2`
  `pos_id` is the `LIST_SONG_0.ID` playing. The player also keeps `MY_LOVE` (favourites), `CUSTOM_PLAYLIST` /
  `PLAYLIST_INFO` and `MEMORY_PLAY` (resume) there. All keyed by file path.
- Queue table schema (V2.57, `sqlite3 .schema` on a Disc, 2026-11-15): `LIST_SONG_0 (ID INTEGER PRIMARY KEY
  autoincrement, LIST_ID INT, POS_ID INT, PATH TEXT, NAME TEXT, TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC
  INT, TRACK INT, IS_CUE INT, IS_ISO INT, OFFSET BIGINT, DURATION BIGINT, ADD_TIME INT8, IS_SELECT INT, SONG_TYPE INT,
  ALBUM_ARTIST TEXT, IS_M3U INT, M3U_PATH TEXT)`; `LIST_SONG_3` (shuffle) only exists while shuffle is on and holds
  `POS_ID` (`INSERT INTO LIST_SONG_%d (POS_ID) VALUES (?1)`). `MEMORY_PLAY (ID, MUSIC_ID, IS_PLAYING, POSITION, IS_CUE,
  IS_ISO, TRACK, IS_NAS, IS_M3U)`. The stock fill statements (`strings mq_player`): `INSERT INTO LIST_SONG_%d (PATH,
  NAME, TITLE, ALBUM, ARTIST, ALBUM_ARTIST, GENRE, DISC, TRACK, DURATION, IS_CUE, IS_ISO, OFFSET, IS_M3U, M3U_PATH)
  SELECT ... FROM SONG WHERE <ARTIST=? | COALESCE(ALBUM_ARTIST, ARTIST)=? | ALBUM=? | GENRE=? | ...> <ORDER BY>`. A row
  as written (example): ID 1, LIST_ID and POS_ID empty, DURATION in ms, ADD_TIME, SONG_TYPE 3.
- `/usr/data/fiio/db/sysconfig.db`: settings (`SYSCONFIG`).
- Custom PEQ bands for slots 11..20: stored by the UI's EQ editor (ui/eqcustom.c); where the player reads them from
  has to be checked before the EQ is built.

## Hardware side

The DAC, ALSA, screen, power key and power off sequences the player performs are in
[MQ_PLAYER_TRACE.md](MQ_PLAYER_TRACE.md) (third run). Volume `0715` 0..120 still has to be mapped to the DAC
attenuation byte (the trace shows the byte, not the UI level at that moment).
