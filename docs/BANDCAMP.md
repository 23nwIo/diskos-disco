# Bandcamp

diskOS can search Bandcamp and play what artists let anyone stream, without an account (step 1, 2026-11-22), and save
releases or single tracks to the SD card ("Downloads" below). Step 2, a login for purchased music, is planned (see "Next").

Bandcamp plays only with the **diskOS player** (Settings > About > Player, docs/MQ_PLAYER_REPLACEMENT.md); the stock
player is not supported (Jo, 2026-11-22: new features need not support it).

## Using it

On the Remote Control page (docs/REMOTE_CONTROL.md, tab **Bandcamp**): search, open an artist or a release, tap a
track or **Play all**. The Disc's own screens show the track like any other (title, artist, album, time) and its
Previous / Next / seek work.

On the Disc: the app **Bandcamp** (Apps, `builtin:bandcamp`, `ui/bandcamp_ui.c`, SCR_BANDCAMP), a plain list in the
Radio screen's look:
- **Search Bandcamp** (the keyboard), **Favourites**, **Downloads**, **Download quality**, **Stop Bandcamp** (while it plays),
  **Last search**.
- Results: an artist or album opens its page; a track plays. Hold anything: its actions (Open, Play, Save to the SD
  card, Add to / Remove from favourites).
- An artist: the releases. A release: **Play all**, **Add to favourites**, **Save the album**, the tracks (tap: play
  from it, hold: its actions).
- Downloads: waiting / saving n/N / saved / failed with the reason, **Stop the downloads**.
- Back goes one view up; pages already seen are kept. Playing opens Now Playing once it starts.

## How it works

Bandcamp has no public API for listeners. diskOS makes the same requests Bandcamp's web pages make, only when you ask
(no crawling, no background polling), with the Disc's own `wget` over HTTPS:

| What | Request | Parsed by |
|---|---|---|
| search | `POST https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic` with `{"search_text":…,"search_filter":"","full_page":false,"fan_id":null}`: up to ~30 artists, albums, tracks (fans are left out) | `bandcamp_core.c` `bc_parse_search` |
| an artist's releases | `GET <artist site>/music`: the release grid (`li data-item-id`) and the rest in `ol data-client-items`; an artist with one release gets that release's page instead | `bc_parse_music` |
| a release | `GET <album or track page>`: `<script data-tralbum="{…}">`, `trackinfo[]` with `title`, `duration`, `track_num` and `file["mp3-128"]` | `bc_parse_release` |

The stream (`file["mp3-128"]`, `https://t4.bcbits.com/stream/…`) is a 128 kbit/s MP3, the whole track, signed with a
token that is valid for about 24 hours (`ts=` in the URL; checked on the Disc 2026-11-22). A track without `file` is
one the artist doesn't let anyone stream ("not streamable").

Playing: `ui/bandcamp.c` downloads the release page (or reuses the one just opened, if less than 10 minutes old),
writes its streamable tracks to `/tmp/diskos-stream-list` (`own_player.h` `OPL_BC_LIST`), switches to source mode 7
("Bandcamp": the player's work mode 12, the same as the web radio's) and sends `0d14` with the line to start at
(docs/PLAYER_PROTOCOL.md). The player plays the URLs in MPD one after another (`playback.c` `pb_slist_*`), with real
track frames (title, artist, album, duration, `k/n`), Next, Previous and seek. MPD streams the HTTPS URL itself
(libcurl + mbedTLS, the CA bundle as for the radio): **nothing is saved**, neither on the card nor in the internal
flash. Only the pages and the list are kept, in `/tmp` (RAM), and the downloaded pages are deleted at once.

When a track stops within 5 s of its start (its link expired, the network is gone), the player stops and sends `ad14`
with its line; the UI downloads the release page again for fresh links and starts that line once more. The same line
failing again within a minute is an error ("Bandcamp didn't send this track").

Any local play, the radio or another working mode ends Bandcamp (as with the radio).

## Terms and limits

- Bandcamp's Terms of Use (effective 2026-05-07) allow the service "only for your own personal, non-commercial use";
  they have no clause on unofficial clients or the requests above. Bandcamp's robots.txt reportedly closes `/api/` to
  crawlers; diskOS is not a crawler (one request per tap). Bandcamp can change these pages at any time: the parsers are
  host-tested against real pages (`ui/tests/fixtures/bandcamp`, fetched on the Disc 2026-11-22), so a change shows up
  as a failing `bandcamp_test` once new pages are saved there.
- Artists can cap how often a fan may stream a track (`play_cap_data`); diskOS doesn't count plays.
- Without an account the quality is 128 kbit/s MP3.

## Favourites

Artists, albums and tracks (Jo, 2026-11-22), kept apart from the radio's and the library's favourites:
`<card>/bandcamp_favorites.txt`, one per line, TAB-separated `type name artist album address cover` (`b` artist or
label, `a` album, `t` a track's own page; `#` lines are comments), at most 1000 (`bc_fav_parse` / `bc_fav_format`).
Only on the SD card for now (user data belongs on the card; a place on the player, as the radio's setting, can
follow): without the card there are none and nothing is written; a file that can't be read is never overwritten; an
edit on a computer is read again (checked at most once a second).

- Disc: **Favourites** on the Bandcamp screen (tap: open / play). Hold any artist, album or track for its actions:
  Open, Play, Save to the SD card, **Add to / Remove from favourites**. A release's page has its own row.
- Remote page: a heart next to every artist, album and track; the menu's **Favourites…** lists them, the release
  menu adds or removes the release. `GET api/bandcamp?favs=1`, `POST action=fav_add` / `fav_remove`.
- Later (Jo): one favourites list for every source (library, radio, Bandcamp) with sources chosen on and off. The
  file keeps a type per line so it can join that list.

## Downloads

On the Remote Control page: an album or track page's menu (⋯ at the top) > **Save the album / track to the SD card**,
or a track's ⋯ > **Save to the SD card**; menu > **Downloads…** shows what is waiting, saving, saved or failed and
stops the rest. They run on the Disc one at a time on their own worker (`ui/bandcamp.c` `job_download`), so searching
and playing go on meanwhile.

- Where: `<card>/BandcampDownloads/<Artist>/<Album>/NN Title.mp3` and `cover.jpg` (1200 px, `a<art_id>_10.jpg`). A
  track page without an album uses the track's title as the folder. Names are made safe for FAT32 and ext4
  (`bc_safe_name`: no `/ \ : * ? " < > |`, at most 100 bytes). A file already there is not downloaded again.
- Quality (Jo: the best unless chosen otherwise): **Download quality** on the Disc's Bandcamp screen (tap: the next
  one) or the Remote page's menu; diskos.conf `bc_quality` 0 Best (FLAC, else MP3 V0 / 320 / Vorbis / AAC), 1 MP3 V0,
  2 MP3 320, 3 Stream. A free / name-your-price release (the page's `freeDownloadPage`) comes from Bandcamp's own
  download in that format: the download page's `data-blob` `digital_items[0].downloads[enc].url`, then
  `/statdownload/...&.vrs=1&.rand=N` - its first answer is always `ExpiredFreeDownloadError` with a `retry_url`, the
  next one `"result":"ok"` with `download_url` (checked on the Disc 2026-11-22) - then that URL: a ZIP for an album
  (`application/zip`, Bandcamp's own names "Artist - Album - NN Title.flac", tags and cover inside), the file for a
  track. The ZIP is unpacked with `unzip -n` (BusyBox) into the album's folder and deleted; nothing there is
  overwritten. One track of a free album uses its track page's own free download.
- Everything else (paid releases, Jo: "Variante B"), the Stream setting, and whenever the free download fails: the
  128 kbit/s stream, its own ID3 tag (if any) replaced by an ID3v2.4 tag (UTF-8) with title, artist, album, album
  artist, track n/N, the release date and the cover (APIC).
- Then: each saved file goes into the library at once (`scanner_add_file`, as the party uploads); while a scan runs
  they wait for it.
- The card: the SD lease (`sd_io_begin`) is held while a track downloads and is written, as for an upload; at least
  64 MB stay free. wget writes to `.bandcamp-download` in the folder, the tagged file is written as `<name>.part`
  and renamed.
- Playing saves too (Jo, 2026-11-22): whatever you play (a release from any track, or a track page) is also saved
  by these rules, in the background (diskos.conf `bc_save_played=0` turns that off; nothing happens without a card).
  Tracks already in the release's folder play from the card instead of the stream: the stream list line then holds
  the file's path (`bc_local_match`: `NN Title.<ext>` as saved from the stream, or Bandcamp's ZIP name
  `Artist - Album - NN Title.<ext>`; flac, mp3, ogg, m4a). A track saved under another name (a title Bandcamp's ZIP
  writes differently) is still streamed.

## Jukebox guests

With **Pick songs from Bandcamp** ticked under System > Public (party.conf `bandcamp=1`, off by default), Jukebox guests
get a **Bandcamp** tab on their page: their own search and pages (up to 8 guests at a time keep theirs; the host's
search on the Remote page and the Disc stays the host's). A track a guest adds is saved like the host's downloads
(BandcampDownloads/<artist>/<album>/, the download quality chosen by the host), goes into the library and then into
the queue as the guest's song, by the Jukebox's rules (songs per guest, in a row, already queued). While it is on its
way it counts as one of the guest's songs; their **Your Bandcamp picks** menu shows how far it is, and the page tells
them once it is queued or why not. Guests' picks are saved before the host's own downloads waiting. They stay on the
card after the party.

## Files

- `ui/bandcamp_core.[ch]`: the pure part (URLs, search reply, release page, music page, the player's list). Host test
  `ui/tests/bandcamp_test.c`.
- `ui/bandcamp.[ch]`: the worker, playing, the JSON for the Remote Control page. Host test
  `ui/tests/bandcamp_app_test.c` with a stand-in `wget` (`ui/tests/fixtures/bandcamp/wget`).
- `ui/player/playback.c` `pb_slist_*`, `core.c` `0d14`: the player's side. Host test `ui/tests/playback_test.c`
  `test_slist`.
- `ui/remote_page/remote.html`: the Bandcamp tab.
- `ui/bandcamp_ui.[ch]`: the Disc's screen. Host renders: `host-render <preset> 0 preview-bandcamp|preview-bandcamp-results|preview-bandcamp-release 0 out.ppm`.

## Next

- Step 2: a login (the `identity` cookie, kept in `/usr/data`, never on the card) for the collection and purchased
  music in full quality.
