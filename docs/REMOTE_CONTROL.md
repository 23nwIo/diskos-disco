# Remote Control over Wi-Fi

Control playback on the Disc from a phone or computer browser on the same Wi-Fi network: play/pause, next/previous,
seek, volume and play mode, with the current song shown, and browse the library (albums, artists, playlists,
favourites, songs, card folders) or the web radio favourites to start something. Off by default.

> **Not yet tested on a Disc.** The control page, the JSON API and the server are tested on a host build
> (`ui/tests/remote_test.c`, a real HTTP round trip, and the page in Chromium), the library API runs against a test
> `song.db` through the production code (`ui/tests/remote_library_test.py`), and the settings screens are rendered
> in every theme. A device test (MIPS build, real Wi-Fi, the player's reaction to each command) is still open.

## Turn it on

1. Connect the Disc to Wi-Fi.
2. Settings > Connectivity > Remote Control > **Control Page**: On.
3. **Show Link**: scan the QR code with your phone, or type the address shown under it
   (`http://<Disc address>:8088/<link code>/`). Bookmark the page to come back later.

**New Link** replaces the link code: every old link and bookmark stops working at once. Use it if the link reached
someone it should not have. The link code is also the [MPD Server](MPD.md)'s password, so it changes there too.

The page and the link survive restarts. If the Disc gets a new Wi-Fi address (another network, a new DHCP lease), the
server follows it within a second or two; scan Show Link again for the new address.

## What it can do

| Control | Notes |
|---|---|
| Play/Pause, Next, Previous | Exactly what the Music screen's buttons do: audiobooks skip 30 s / back 15 s, fast repeated skips are paced, Music Assistant (MA Sendspin) is controlled when it plays. |
| Song details | Cover (the Disc's 148 px decode, so a bit soft on a large screen), format, bit depth, sample rate and bit rate (Song Info's values: the scanner's rate, else the file's average; for the radio, the station's codec and rate from the directory), and the Bluetooth codec and headphones when they play. A heart saves the song, or the station while the radio plays, to the favourites (song lists and stations have the same heart on every row). |
| Position slider | Seeks the current song. |
| Volume | 0-120, the player's own scale (the same as the hardware keys). After a start-up, until the volume first changes, the level the player restored at boot (its saved SYSCONFIG.VOLUME). |
| Balance | -10 (left) to 10 (right), 0 centred, with a **Centre** button: the same setting as Settings > Audio > Balance, saved on the Disc. |
| Outputs | Only while the own player runs (Settings > About > Player): AUX and Bluetooth each on or off, each with its own volume, Auto Outputs (Off, Newest, All connected) and, while Bluetooth plays, the codec of this connection (only those the headphones offer; the music pauses briefly), the same as Settings > Audio > Outputs. One output always stays on; Bluetooth needs connected headphones. The Volume slider moves every output that is on. |
| After the list ends | Only with the own player: **Stop**, **Shuffle all songs**, **Play a radio station** or **Play a playlist** when the last song of the list and of the queue has ended (Settings > Playback > After the List Ends). **Choose** picks one of the radio favourites (by default the station played last), or a playlist and how it plays: in order once, in order again and again, or shuffled (endless). Songs added to the queue interrupt the station and it comes back when they have played; during the playlist they play after the current song and the playlist goes on after them. |
| EQ | Any built-in preset or user profile, by the names the EQ screen shows; saved like a choice on the Disc. |
| Play mode | Sequential, Shuffle, Repeat one, Repeat all, Single. Saved like the Disc's own setting. Not available while Music Assistant plays. |

Commands that need local playback are refused while the Disc is in another working mode (USB DAC, Bluetooth DAC,
AirPlay, USB storage) and the page says why.

### Library

The page's other tabs list **Albums, Artists, Playlists, Favourites, Songs** and **Folders** (the card's own folders,
audio files only). Each list has a search field and loads 100 entries at a time. Tap an album, artist, playlist or
folder to open it; tap a song to play it in that context (the album, the artist's songs, the playlist, favourites or
the whole library), or press **Play all**. A file in Folders plays only if the library already knows it (rescan the
card on the Disc after copying new music). In Folders, **Play all** plays the open folder with its subfolders, in path order (A to Z), as one queue; files the library doesn't know are skipped. Folders never leave the card: `..` and links pointing elsewhere are refused.

### Queue

The **Queue** tab shows what plays next, the same list MPD clients see: the song playing now, the songs queued on the
Disc (its Add to queue, from an MPD client or from this page), then **Up next**, the rest of the list the player plays
(in shuffled order in Shuffle; the first 200, with a count of the rest). Tap a queued or upcoming song to play it now.
The **⋯** on a queued song moves it (to the top, up, down, to the end) or removes it; **Clear queue** removes all
queued songs (Up next stays). Up next is the player's own list and cannot be reordered (the player has no command
for it); its **⋯** has **Add to the queue**, which copies that exact song (also a CUE/ISO track) into the queue,
where it can be moved. The song **⋯**, and the **⋯** of an album or artist, have **Add to the queue**.

While the tab is open the page asks for the list's version every second and reads the list again when it changed (a
song added on the Disc, the next song started, ...). Every change sends the version it was made on; when the list
changed in between, the Disc refuses ("The queue changed") and the page shows the new list, so a tap never moves or
removes the wrong song.

### Playlists and favourites

The **⋯** button on a song adds it to or removes it from the favourites, adds it to a playlist (or a new one), and in a
playlist removes it. The **⋯** in an album's or artist's header adds all its songs to a playlist. Playlists has **New
playlist**; inside a playlist the header **⋯** renames or deletes it (the songs stay on the card). The heart on Now
playing marks the song that is playing, through the player like the Disc's own heart. These are the Disc's own
playlist and favourite functions; a Library list open on the Disc refreshes. Songs cannot be reordered inside a
playlist (the Disc has no reorder function either).

### System page

**System ›** at the top right opens a second page, `http://<address>:8088/<link code>/system`, with the **Status**,
**Bluetooth**, **Screen** and **Party** tabs; **‹ Remote** goes back. The address ends in `#status`, `#bt`, `#scr` or `#party`, so a bookmark
opens that tab. Keeping them apart keeps the control page small and quick to load on a phone.

### Party mode

For a party, guests get a page of their own: `http://<address>:8088/<party name>/`, without the link code. Nothing
changes on the Disc's screens. The host sets it up on the system page's **Party** tab:

| Setting | Default | What it does |
|---|---|---|
| Party mode | off | Off: the guests' link answers 404. |
| Party name | — | The guests' link: at least 2 letters, digits, `-` or `_` (upper and lower case differ). A new name locks out the old link. **Copy** copies the link. |
| Change the volume | on | Guests get a volume slider. **Up to N %**: their 100 % is N % of the Disc's volume (scaled, so their slider still goes from 0 to 100). |
| See what plays now | on | Off: title, artist, cover and position are hidden (for a music quiz), and the queue shows other guests' and the host's songs as "A secret song" (with who queued it); each guest still sees their own songs in full. |
| See the queue | on | Guests see the song playing now and the queued songs, with who queued them; their own are marked with a heart or a star. The host's own list after the queue (Up next) stays hidden. |
| Skip songs | off | Guests get a Next button. |
| Songs each guest may have in the queue | 3 | A place frees up once one of their songs starts playing. |
| Songs of one guest in a row | 1 | Guests take turns: a guest's new song joins their next round instead of the end of the queue (Anna, Ben, Anna, Ben, …), never before the host's songs. When it would still play right after the same guest's song, it is refused, unless only that guest's songs are waiting. |
| Add a song that is already queued | off | Off: a song already in the queue is refused. |
| Give their name | on | Guests can enter a name (on their **You** tab), shown with their songs on both pages. |
| Choose a radio station | off | Guests pick a station from their Radio tab: **Play when the queue is empty** makes it the station **When the queue is empty** (own player only), **Play now** switches to it at once, also from a station already playing (only when they may skip songs too). |
| When the queue is empty | — | The same setting as **After the list ends**: stop, random songs from the library, a radio station or a playlist. |

**Guests** lists the guests the Disc has seen, with their names, how many songs each has queued and when they were last
seen.

Guests always have a Radio tab, the Remote page's station browser: **Find stations** searches the station directory,
the heart saves a station to their own list (kept in their browser, like their favourite songs). Playing one needs the
setting above.

Guests can browse the whole library (songs, albums, artists, playlists, folders) and add single songs,
one at a time; a file in Folders that holds several tracks (a CUE or ISO image) is added from Albums or Songs instead.
They cannot pause, go back, seek, change the EQ, the outputs, Bluetooth, playlists or favourites, and the system page
and every other command answer 404 on their link; the host's favourites are not shown. Instead each guest keeps
favourites of their own in their browser (a song's menu: Add to your favourites; the Favourites tab). Each guest's page
may pick its own colour and heart or star.

A guest is told apart by a random id their browser keeps; a private window or another browser counts as a new guest.
The ids, names and who queued what are kept in RAM only and start afresh after a restart. The settings are kept on
the SD card in `.diskos/state/party.conf` (written only when the host changes one); without a card party mode is off.
The host's own page has no limits and shows "by <name>" on guests' songs. Not yet tested on a Disc.

### Page look

Below **Restart the Disc** in the system page's Status tab: **Page colour** (orange, the default, blue, green, purple or red: buttons,
tabs, sliders and scroll bars) and **Favourites** (red hearts, the default, or yellow stars). Both are kept in the
browser, so each phone or computer has its own; light and dark mode follow the device.

### Status

The **Status** tab starts with **Working mode** (Playback, USB DAC, Bluetooth DAC, Bluetooth streaming, AirPlay, USB storage: the same switch as a tap on the Disc's Working mode screen, with its checks; a refusal shows the Disc's reason; USB storage asks first, since the card then belongs to the computer) and **Restart the Disc** (as Settings > Restart: it waits for card writes and a sync, or says why not; the page reconnects once the Disc is back, the link stays the same). Below it shows, refreshed every 3 s while it is open: working mode, output, volume, EQ and library size;
Bluetooth (on/off, connected headphones, the codec they run); Wi-Fi (network name and access point MAC, security and cipher, channel, signal and link quality, IP address with prefix and network, gateway, DNS, data
received and sent since start-up; Bluetooth data since Bluetooth came on, from `hciconfig`); SD card (free space, file system and device) and internal storage; battery (charge, charging, voltage, temperature,
health, cycles, as the battery gauge reports them); and diskOS and firmware versions (**Update** is the label given to `diskos-sign.sh` for the SD or online update that runs now, or "none" for a flashed build), uptime, load, CPU use (since the
previous refresh), free memory and the UI's own memory. The Disc has no CPU temperature sensor
([HARDWARE](HARDWARE.md)). **Rescan library** starts the same scan as Settings (it needs local playback and a ready
card) and can stop it; the tab shows its progress.

### Bluetooth

The **Bluetooth** tab does what the Disc's Bluetooth screen does: **Turn on**/**Turn off** the radio (switching on takes
up to half a minute), **Search** for headphones and speakers in pairing mode (about 15 s), tap a device to connect
(it is paired first if needed) or the connected one to disconnect, and under **⋯** give it its own name on this Disc
(an empty name or the device's own name goes back to that) or **Forget** it. **Bluetooth codec** below the list is the Settings choice (SBC, AAC, aptX/LDAC Mobile/Standard/High; firmware V2.57 only) with the codec running now; a new choice applies from the next connection, as on the Disc. Local names are the same ones the Disc
shows; two devices with the same name get the end of their address. The list refreshes every 2 s while the tab is
open; the Disc shows its usual messages ("Connected: …") on its own screen.

### Radio

The **Radio** tab lists the favourites of the Disc's [Radio app](RADIO.md). Tap one to play it; the heart removes it.
**Find stations** searches the same station directory as the Radio app (radio-browser.info, through the Disc's own
search): an empty field shows the popular stations, typing searches by name. The Disc downloads the answer, which takes
a few seconds. Tap a hit to play it, or its star to save it as a favourite. The Disc and the page share one search, so
a search started on the Disc at the same moment replaces the page's (search again). Every station row shows its stream URL. While the radio plays, Now playing shows
the song the station names (its ICY title, or the Ogg Vorbis/Opus comments; refreshed about every 20 s), the station, its stream URL and a **Stop radio** button; play/pause and volume work, while the position, Next, Previous and Play mode
are hidden or refused because a live stream has none of them. Starting anything from the library ends the radio, as on
the Disc.

## Security

- **Off by default.** Nothing listens until Control Page is switched on.
- **Wi-Fi only.** The server binds to the Disc's Wi-Fi address only, never to every interface, on TCP port 8088.
- **Secret link.** Every page and command lives under a random link code (100 bits from `/dev/urandom`). Anything
  else gets a plain 404. The code is compared in constant time. It is kept in `/usr/data/diskos.conf` so bookmarks
  keep working; New Link replaces it.
- **Plain HTTP.** Like the Last.fm setup page, the traffic is not encrypted: anyone who can watch your Wi-Fi traffic
  can see the link code and use it. Use it on networks you trust, and press New Link if in doubt.
- **No side effects from links.** Commands are POST only, so a link preview or prefetch can never press a button.
  The page loads nothing from the internet and sends a strict Content-Security-Policy, `no-store`, `nosniff`,
  `X-Frame-Options: DENY` and `Referrer-Policy: no-referrer`.
- **Bounded.** One client is served at a time; each request has a 4 KB limit and a 6 s deadline, so a slow or broken
  client cannot exhaust the Disc. A command waits at most 5 s for the UI; one that was not picked up by then is
  withdrawn rather than run late.

## API

All paths are relative to `http://<Disc address>:8088/<link code>/`. Parameters go in the query string or in an
`application/x-www-form-urlencoded` body. Responses are JSON.

| Request | Effect |
|---|---|
| `GET api/state` | `{"ok":true,"has_track":…,"playing":…,"title":"…","artist":"…","album":"…","position_ms":…,"duration_ms":…,"volume":…,"volume_max":120,"balance":-10..10,"queue_ver":N (with `?q=1`, else 0),"play_mode":0..4 or -1,"source":"local"\|"music_assistant"\|"radio"\|"other","controllable":…,"radio_state":"off"\|"starting"\|"playing"\|"error","station":"…","station_url":"…","stream_title":"Artist - Title" or "","bt_codec":"LDAC" or "" (the codec the Bluetooth headphones run),"bt_device":"…" (their name),"station_fav":true|false|null,"radio_error":"…","format":"FLAC"…,"kbps":…,"bits":…,"sample_rate":…,"channels":…,"favorite":true\|false\|null,"art":"…"}` (0 or "" = unknown) |
| `GET api/status` | `{"ok":true,"groups":[{"name":"Wi-Fi","rows":[["Signal","-60 dBm (good)"],…]},…]}` (display text) |
| `GET api/eq`, `POST api/eq` with `preset=0..20` | The presets (`{"current":N,"presets":[{"id":0,"name":"Off"},…]}`) / select one. |
| `POST api/playlist_create` (`name`, 1-64 bytes), `playlist_rename` (`id`, `name`), `playlist_delete` (`id`) | Playlists. Create answers `{"ok":true,"id":N}`. |
| `POST api/playlist_add` with `id` + `kind=song&song_id=` \| `kind=album&name=` \| `kind=artist&name=` | Add (duplicates are skipped); answers `{"ok":true,"added":N}`. |
| `POST api/playlist_remove` with `id`, `pos`, `song_id` | Remove one row; refused if that row no longer holds that song. |
| `POST api/favorite` with `on=0\|1` and `song_id` (none: the song playing now) | Favourite on/off. Song lists carry `"fav":true\|false`. |
| `POST api/work_mode` with `mode=0..5` | 0 Playback, 1 USB DAC, 2 Bluetooth DAC, 3 USB storage, 4 Bluetooth streaming, 5 AirPlay. `api/state` and `api/status` carry `"work_mode"` (6 = web radio). |
| `POST api/reboot` with `confirm=1` | Restart the Disc. |
| `POST api/rescan` with `action=start\|stop` | Library rescan; `GET api/status` has `"scan":{"active":…,"done":N}`. |
| `GET api/bluetooth` | `{"ok":true,"state":"on"\|"off"\|"turning_on","busy":…,"devices":[{"mac":"AA:BB:…","name":"shown name","own":"device's name","alias":"local name or empty","connected":…,"battery":80,"codec":"LDAC"…}],"codec":{"settable":…,"current":N,"live":"LDAC","choices":[{"id":0,"name":"SBC","hint":"…"},…]}}` (connected first; `busy`: a search or listing is running; `battery`: the connected headphones' battery in %, -1 unknown). |
| `POST api/bluetooth` with `action=on\|off\|search\|refresh`, or `action=codec&codec=N` | Radio on/off (only acts if needed), a ~15 s search, list the known devices again, or the Bluetooth Codec setting (an id from `choices`). |
| `POST api/bt_device` with `action=connect\|disconnect\|forget\|rename` and `mac=AA:BB:CC:DD:EE:FF` (+ `name`, up to 47 bytes, for rename) | As on the Disc; refused (409) while Bluetooth is off. |
| `GET api/cover` | The playing song's cover as the Disc decoded it (148 px BMP); `art` in the state changes with it, so `api/cover?v=<art>` can be cached by the page. 409 when there is none. |
| `POST api/play_pause` | Toggle. |
| `POST api/play`, `POST api/pause` | Only acts if needed. |
| `POST api/next`, `POST api/previous` | Skip. |
| `POST api/volume` with `value=0..120` or `step=-120..120` | Absolute or relative volume. |
| `POST api/balance` with `value=-10..10` | Left/right balance (negative = left), applied and saved as Settings > Audio > Balance. |
| `GET api/queue` (`limit=1..500` Up next rows, default 100) | `{"ok":true,"ver":N,"shuffle":…,"items":[{"k":"cur"\|"queue"\|"next"\|"other","i":queue index,"b":Up next row id,"t":"title","a":"artist","f":"file name","d":ms,"st":true for a station}],"more":N}` |
| `POST api/queue` with `action=add` and `kind=song&song_id=N`, `kind=album&name=…` or `kind=artist&name=…` or `kind=next&b=ID` (an Up next row) | Adds to the end of the queue (`{"ok":true,"added":N}`). |
| `POST api/queue` with `ver` and `action=jump&kind=queue&i=N` \| `action=jump&kind=next&b=ID` \| `action=remove&i=N` \| `action=move&i=N&to=M` \| `action=clear` | Refused with "The queue changed" when `ver` is not the list's version any more. |
| `POST api/list_end` with `mode=0..3` (stop, shuffle all, station, playlist) and optionally `pos=N&url=U` (radio favourite N as the station) or, with mode 3, `pl=ID&flags=0..3` (the playlist; Repeat 1, Shuffle 2) | After the List Ends (own player only). `api/state` reports `"list_end":{"mode":N,"station":"name","playlist":"name","pl_id":ID,"pl_flags":F}`, or `null` with the stock player. |
| `GET api/party`, `POST api/party` with any of `on`, `name`, `vol`, `maxvol=1..100`, `np`, `queue`, `skip`, `total=1..50`, `consec=1..10`, `repeat`, `radio`, `names` (0/1 unless stated) | Party mode settings (saved on the SD card): `{"ok":true,"settings":{…,"name":"…","own_player":…},"guests":[{"name":"…","queued":N,"seen_s":N}]}`. |
| `POST api/queue` with `action=add&kind=file&path=…` | Adds a card file (all its tracks). |
| `POST api/outputs` with one of `aux=0/1`, `bt=0/1`, `vol_aux=0..120`, `vol_bt=0..120`, `auto=0..2`, `codec=0..4` (0 SBC, 1 AAC, 2 LDAC, 3 aptX, 4 aptX HD) | Settings > Audio > Outputs (own player only; refused with the reason otherwise). `api/state` reports `"outputs":{"aux","bt","vol_aux","vol_bt","bt_ready","auto","jack","bal","codec" (-1 none),"codecs":[ids offered],"switching"}`, or `null` with the stock player. |
| `POST api/seek` with `position_ms=N` | Seek. |
| `POST api/play_mode` with `mode=0..4` | 0 Sequential, 1 Shuffle, 2 Repeat one, 3 Repeat all, 4 Single. |
| `GET api/library/<list>` | `albums`, `artists`, `playlists`, `favorites`, `songs`, `radio` (items with `pos`, `name`, `url`, `codec`, `bitrate`, `country`, `fav`), `radio_search` (the last station search: also `"search":"idle"|"busy"|"done"|"failed"`); `album` and `artist` with `name=`, `playlist` with `id=`, `folder` with `path=` (relative to the card, empty for the top). All take `q=` (search), `offset=` and `limit=` (default 200, max 500). Answer: `{"ok":true,"total":N,"offset":N,"items":[…]}`. |
| `POST api/play_item` | `kind=album` + `name` + `song_id`; `kind=artist` + `name` + `song_id`; `kind=playlist` + `id` + `pos`; `kind=favorites` + `pos`; `kind=song` + `song_id`; `kind=file` + `path`; `kind=folder` + `path` (empty: the whole card; subfolders included, path order); `kind=all`; `kind=radio` + `pos` (+ `url`: refused if that favourite has changed); `kind=radio_hit` + `pos` + `url` (a search hit). `pos` counts from 1. |
| `POST api/radio_search` with `q` | Start a station search (`q` empty: popular stations); then poll `GET api/library/radio_search` until `search` is not `busy`. |
| `POST api/radio_fav_add`, `POST api/radio_fav_remove` | Save or remove a station: `kind=radio_hit` or `kind=radio`, with `pos` and `url`; or `kind=radio_now` (the station playing). |
| `POST api/radio_stop` | Stop the web radio and return to local playback. |

The guests' link `http://<Disc address>:8088/<party name>/` takes `GET api/state` (the guests' view: `"party":{"np","queue","vol","skip","radio","names","total","consec","repeat","mine","left","you"}` and only what they may see; `volume` on their 0-100 scale), `GET api/cover`, `GET api/queue`, `POST api/queue` (`action=add&kind=song&song_id=N` or `kind=file&path=…`; answers `"place"` and `"left"`), `POST api/volume` (`value=0..100`), `POST api/next`, `GET api/library/<list>` (not `radio` or `favorites`), `POST api/radio_search` and their own `POST api/radio` with `name`, `url` (http or https) and `now=0|1` (0: the station when the queue is empty; 1: play it now, needs skipping too). The state's `party` also has `radio_end` and `radio_now` (what picking a station may do). Each request carries the guest's id `g=` (8-24 letters and digits; required to add) and optionally their name `n=`.

Status codes: 200 done, 400 missing or invalid value, 404 wrong link code or path, 405 wrong method, 409 refused
(`message` says why), 503 another command is still running, 504 the UI did not answer in time.

Example (Home Assistant `rest_command`, a shell, a Stream Deck):

```sh
curl -X POST "http://192.168.1.50:8088/<link code>/api/volume" -d step=-5
curl "http://192.168.1.50:8088/<link code>/api/state"
```

## Source

| File | Role |
|---|---|
| `ui/queueview.c` | The queue list and its jump checks, shared with the MPD server. |
| `ui/remote_core.c` | Request parsing, routing, link code, JSON (pure, host-tested). |
| `ui/party.c` | Party mode: the settings on the card, the guests and their rules, what guests see. |
| `ui/remote_page/` | The pages' source: `remote.html` (control page), `system.html` (system page), `party.html` (the guests' page), shared `style.css` and `common.js`. `python3 ui/tools/gen_remote_page.py` writes them into `ui/remote_page.c` (generated, committed; `--check` tells whether it is up to date). |
| `ui/remote_srv.c` | The server thread and the one-slot command mailbox to the UI (no LVGL). |
| `ui/remote.c` | Settings, the Show Link overlay, Wi-Fi address tracking, and running commands on the main thread through `ui_transport_command`, `ui_set_volume`, `ui_seek_to`, `ui_set_workmode`, the `mdb_*` library queries, the `ui_play_*` entry points and the Radio app's `radio.h`. |
| `ui/tests/remote_test.c` | Unit tests and the HTTP round trip (`make -C ui/tests remote_test`); `./remote_test --serve PORT` serves the page with a stand-in player for browser checks. |
| `ui/tests/remote_library_test.py` | The library API against a test `song.db` and card through `host-render` (build it first). |
