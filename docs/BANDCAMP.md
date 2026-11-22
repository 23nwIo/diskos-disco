# Bandcamp

diskOS can search Bandcamp and play what artists let anyone stream, without an account (step 1, 2026-11-22). Step 2,
a login for purchased music, and downloads to the SD card are planned (see "Next" below).

Bandcamp plays only with the **diskOS player** (Settings > About > Player, docs/MQ_PLAYER_REPLACEMENT.md); the stock
player is not supported (Jo, 2026-11-22: new features need not support it).

## Using it

For now on the Remote Control page (docs/REMOTE_CONTROL.md, tab **Bandcamp**): search, open an artist or a release,
tap a track or **Play all**. The Disc's own screens show the track like any other (title, artist, album, time) and
its Previous / Next / seek work; the Disc's own Bandcamp screens follow after the first device tests (Jo, 2026-11-22).

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

## Files

- `ui/bandcamp_core.[ch]`: the pure part (URLs, search reply, release page, music page, the player's list). Host test
  `ui/tests/bandcamp_test.c`.
- `ui/bandcamp.[ch]`: the worker, playing, the JSON for the Remote Control page. Host test
  `ui/tests/bandcamp_app_test.c` with a stand-in `wget` (`ui/tests/fixtures/bandcamp/wget`).
- `ui/player/playback.c` `pb_slist_*`, `core.c` `0d14`: the player's side. Host test `ui/tests/playback_test.c`
  `test_slist`.
- `ui/remote_page/remote.html`: the Bandcamp tab.

## Next

- Downloads to the card (Jo, 2026-11-22): `BandcampDownloads/<Artist>/<Album>/` with the cover, then a library
  rescan, also from the Disc. The best quality available unless chosen otherwise: Bandcamp's own download for free /
  name-your-price releases, otherwise the 128 kbit/s stream with tags written by diskOS.
- The Disc's own Bandcamp screens.
- Step 2: a login (the `identity` cookie, kept in `/usr/data`, never on the card) for the collection and purchased
  music in full quality.
