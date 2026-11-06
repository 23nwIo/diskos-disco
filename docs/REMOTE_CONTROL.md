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
someone it should not have.

The page and the link survive restarts. If the Disc gets a new Wi-Fi address (another network, a new DHCP lease), the
server follows it within a second or two; scan Show Link again for the new address.

## What it can do

| Control | Notes |
|---|---|
| Play/Pause, Next, Previous | Exactly what the Music screen's buttons do: audiobooks skip 30 s / back 15 s, fast repeated skips are paced, Music Assistant (MA Sendspin) is controlled when it plays. |
| Position slider | Seeks the current song. |
| Volume | 0-120, the player's own scale (the same as the hardware keys). |
| Play mode | Sequential, Shuffle, Repeat one, Repeat all, Single. Saved like the Disc's own setting. Not available while Music Assistant plays. |

Commands that need local playback are refused while the Disc is in another working mode (USB DAC, Bluetooth DAC,
AirPlay, USB storage) and the page says why.

### Library

The page's other tabs list **Albums, Artists, Playlists, Favourites, Songs** and **Folders** (the card's own folders,
audio files only). Each list has a search field and loads 100 entries at a time. Tap an album, artist, playlist or
folder to open it; tap a song to play it in that context (the album, the artist's songs, the playlist, favourites or
the whole library), or press **Play all**. A file in Folders plays only if the library already knows it (rescan the
card on the Disc after copying new music). Folders never leave the card: `..` and links pointing elsewhere are refused.

### Radio

The **Radio** tab lists the favourites of the Disc's [Radio app](RADIO.md). Tap one to play it; the star removes it.
**Find stations** searches the same station directory as the Radio app (radio-browser.info, through the Disc's own
search): an empty field shows the popular stations, typing searches by name. The Disc downloads the answer, which takes
a few seconds. Tap a hit to play it, or its star to save it as a favourite. The Disc and the page share one search, so
a search started on the Disc at the same moment replaces the page's (search again). While the radio plays, Now playing shows
the station and a **Stop radio** button; play/pause and volume work, while the position, Next, Previous and Play mode
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
| `GET api/state` | `{"ok":true,"has_track":…,"playing":…,"title":"…","artist":"…","album":"…","position_ms":…,"duration_ms":…,"volume":…,"volume_max":120,"play_mode":0..4 or -1,"source":"local"\|"music_assistant"\|"radio"\|"other","controllable":…,"radio_state":"off"\|"starting"\|"playing"\|"error","station":"…","radio_error":"…"}` |
| `POST api/play_pause` | Toggle. |
| `POST api/play`, `POST api/pause` | Only acts if needed. |
| `POST api/next`, `POST api/previous` | Skip. |
| `POST api/volume` with `value=0..120` or `step=-120..120` | Absolute or relative volume. |
| `POST api/seek` with `position_ms=N` | Seek. |
| `POST api/play_mode` with `mode=0..4` | 0 Sequential, 1 Shuffle, 2 Repeat one, 3 Repeat all, 4 Single. |
| `GET api/library/<list>` | `albums`, `artists`, `playlists`, `favorites`, `songs`, `radio` (items with `pos`, `name`, `url`, `codec`, `bitrate`, `country`, `fav`), `radio_search` (the last station search: also `"search":"idle"|"busy"|"done"|"failed"`); `album` and `artist` with `name=`, `playlist` with `id=`, `folder` with `path=` (relative to the card, empty for the top). All take `q=` (search), `offset=` and `limit=` (default 200, max 500). Answer: `{"ok":true,"total":N,"offset":N,"items":[…]}`. |
| `POST api/play_item` | `kind=album` + `name` + `song_id`; `kind=artist` + `name` + `song_id`; `kind=playlist` + `id` + `pos`; `kind=favorites` + `pos`; `kind=song` + `song_id`; `kind=file` + `path`; `kind=all`; `kind=radio` + `pos` (+ `url`: refused if that favourite has changed); `kind=radio_hit` + `pos` + `url` (a search hit). `pos` counts from 1. |
| `POST api/radio_search` with `q` | Start a station search (`q` empty: popular stations); then poll `GET api/library/radio_search` until `search` is not `busy`. |
| `POST api/radio_fav_add`, `POST api/radio_fav_remove` | Save or remove a station: `kind=radio_hit` or `kind=radio`, with `pos` and `url`. |
| `POST api/radio_stop` | Stop the web radio and return to local playback. |

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
| `ui/remote_core.c` | Request parsing, routing, link code, JSON, the control page (pure, host-tested). |
| `ui/remote_srv.c` | The server thread and the one-slot command mailbox to the UI (no LVGL). |
| `ui/remote.c` | Settings, the Show Link overlay, Wi-Fi address tracking, and running commands on the main thread through `ui_transport_command`, `ui_set_volume`, `ui_seek_to`, `ui_set_workmode`, the `mdb_*` library queries, the `ui_play_*` entry points and the Radio app's `radio.h`. |
| `ui/tests/remote_test.c` | Unit tests and the HTTP round trip (`make -C ui/tests remote_test`); `./remote_test --serve PORT` serves the page with a stand-in player for browser checks. |
| `ui/tests/remote_library_test.py` | The library API against a test `song.db` and card through `host-render` (build it first). |
