# Web radio

Play internet radio stations over Wi-Fi: search the community station directory, open the most popular stations, and
keep favourites on the Disc.

> **Status: not yet tested as a build on a Disc.** The playback path itself was tested by hand on a Disc on
> 2026-11-06 (the stock player's DLNA renderer, driven over UPnP): MP3, AAC, MP3 over HTTPS, Ogg Vorbis, Ogg Opus and
> FLAC streams played and the pause key worked. The Radio app, the directory parser and the UPnP messages are tested
> on a host build (`ui/tests/radio_test.c`) and the screens are rendered in every theme.

## Use it

1. Connect the Disc to Wi-Fi.
2. Open **Radio** from Apps (or from the app menu of your theme).
3. **Search stations** (type part of a name) or **Popular stations**. Tap a station to play it.
4. **Hold** a station to save it as a favourite; hold a favourite to remove it. Favourites are listed on the Radio
   screen (up to 1000).

### Where the favourites are kept

**Settings > System > Library > Radio Favourites** chooses the file:

| Setting | File |
|---|---|
| **On SD Card** (default) | `radio_favorites.txt` in the root of the SD card (`/tmp/sdcard/radio_favorites.txt` on the Disc) |
| **On Player** | `/usr/data/radio_favorites.txt` inside the player (where favourites were kept before) |

Switching copies nothing: each place keeps its own file, and the Radio app shows the one the setting names. To take
your list along, copy the file yourself, for example over SSH (`cp /usr/data/radio_favorites.txt /tmp/sdcard/`) or
with a computer. Without an SD card the card list is empty ("No SD card") and saving a favourite is refused; the file
on the card is never overwritten then. The Disc reads the file again when the setting changes, when a card is
inserted or removed, and when the file changes (an edit on a computer or over SSH), at most once a second.

While the radio plays, the Music screen shows the station name and "Web radio". The elapsed and total time are the
player's own and mean nothing for a live stream; pause works, skip and seek do nothing useful. **Stop radio** on the
Radio screen, Playback under Working mode, or starting any song from your library returns the Disc to normal
playback. Working mode shows "Web radio" while it is active.

## How it works

The stock player (`mq_player`) has a DLNA/UPnP renderer that plays network streams with its own decoders (ffmpeg).
Radio switches the player to that work mode (source mode 6, frame `0657000C000C`, see [COMMAND_MAP](COMMAND_MAP.md))
and hands it the station with the standard UPnP `SetAVTransportURI` and `Play` actions, on the Disc's own address. No
audio passes through the UI and no decoder was added.

**With the own player** ([MPD_CORE.md](MPD_CORE.md#web-radio)) there is no DLNA: the Radio app sees that the own
player runs (`opl_running()`), switches to the same work mode and sends the station as one command, `0d10` (name and
URL), and MPD plays the stream through its curl input (HTTP and HTTPS). Leaving the radio stops the station and
loads the local song again, paused where it was. HTTPS needs a CA bundle: the player gives MPD the first of
`/usr/data/diskos-own/cacert.pem`, `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`,
`/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem` that exists (the Disc has the second).

Stations come from [radio-browser.info](https://www.radio-browser.info/) (`url_resolved`, broken stations hidden),
fetched with `wget` over HTTPS. The favourites file (see [above](#where-the-favourites-are-kept)) holds one station per
line, `name<TAB>url<TAB>codec<TAB>bitrate<TAB>country` (UTF-8, at most 1000 lines). You can edit it in a text editor
or over SSH; only `http://` and `https://` addresses are accepted, other lines are skipped. A save writes a temporary
file next to it and renames it over the old one.

## Limits

- Wi-Fi only, and 2.4 GHz only (the Disc's radio chip). A weak signal means dropouts.
- The output stays as it was in Playback: headphones, or Bluetooth headphones that were already playing (tested with
  dlnatest over Bluetooth). Headphones that connect only while the radio plays are not switched to automatically;
  connect them first.
- The song the station names is shown on the Music screen (title, artist, the station as album): ICY metadata for
  MP3/AAC from Icecast or SHOUTcast, the comment header (TITLE, ARTIST) for Ogg Vorbis and Opus. With the own player
  (docs/MPD_CORE.md) the title comes from MPD, which reads it from the stream it plays anyway: the player writes it to
  `/tmp/diskos-player-stream` and the Radio app reads that file every 2 seconds, with no extra connection. The stock
  player does not pass it on, so with it the Disc opens a second, short connection to the station every 20 seconds (a
  few seconds of extra data); a station that names nothing is asked again only every 5 minutes, and a song change can
  take up to 20 seconds to show. Without a title the station name is shown. FLAC streams are not read for titles.
  Not yet tested on a Disc.
- **Settings > System > Library > Radio Certificates** (own player only): On (default), an HTTPS station needs a
  certificate the Disc's CA list (`/etc/ssl/certs/ca-certificates.crt`) trusts. Off, HTTPS stations play without that
  check (for stations with an expired or self-made certificate; anyone in the network path could then pose as the
  station). The setting is a file, `/usr/data/diskos-own/radio-no-cert-check`; MPD reads it when it starts, so the
  player restarts MPD before the next station after a change.
- As in the other working modes, a microSD card inserted while the radio plays is not mounted automatically.
- Playlists (`.pls`, `.m3u`) are resolved by the directory; a bare playlist URL in the favourites file may not play.

For a front end such as Remote Control, `ui/radio.h` offers `radio_favorites`, `radio_play`, `radio_play_url`,
`radio_stop`, `radio_state`, `radio_station_name`, `radio_stream_title` ("Artist - Title", "" none), the directory search (`radio_search`, `radio_search_result`) and
`radio_fav_add` / `radio_fav_remove`, `radio_fav_count` / `radio_fav_get` / `radio_fav_has` (main thread only). [Remote Control](REMOTE_CONTROL.md) uses them for
its Radio tab, the [MPD Server](MPD.md#web-radio) for the stored playlist "Radio".
`radio_end_set` / `radio_end_get` / `radio_end_sync` (`RADIO_END_STOP`, `_SHUFFLE`, `_STATION`, `_PLAYLIST` with
`radio_end_set_playlist` / `radio_end_playlist`) set what plays after the
list ends with the own player, the station included ([MPD_CORE.md](MPD_CORE.md#after-the-list-ends)); the last station
played becomes that station.
