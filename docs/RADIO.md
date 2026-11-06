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
   screen.

While the radio plays, the Music screen shows the station name and "Web radio". The elapsed and total time are the
player's own and mean nothing for a live stream; pause works, skip and seek do nothing useful. **Stop radio** on the
Radio screen, Playback under Working mode, or starting any song from your library returns the Disc to normal
playback. Working mode shows "Web radio" while it is active.

## How it works

The stock player (`mq_player`) has a DLNA/UPnP renderer that plays network streams with its own decoders (ffmpeg).
Radio switches the player to that work mode (source mode 6, frame `0657000C000C`, see [COMMAND_MAP](COMMAND_MAP.md))
and hands it the station with the standard UPnP `SetAVTransportURI` and `Play` actions, on the Disc's own address. No
audio passes through the UI and no decoder was added.

Stations come from [radio-browser.info](https://www.radio-browser.info/) (`url_resolved`, broken stations hidden),
fetched with `wget` over HTTPS. Favourites live in `/usr/data/radio_favorites.txt`, one per line:
`name<TAB>url<TAB>codec<TAB>bitrate<TAB>country`. You can edit it over SSH; only `http://` and `https://` addresses are
accepted.

## Limits

- Wi-Fi only, and 2.4 GHz only (the Disc's radio chip). A weak signal means dropouts.
- The output stays as it was in Playback: headphones, or Bluetooth headphones that were already playing (tested with
  dlnatest over Bluetooth). Headphones that connect only while the radio plays are not switched to automatically;
  connect them first.
- The current song title from the stream (ICY metadata) is not shown yet; the station name is.
- As in the other working modes, a microSD card inserted while the radio plays is not mounted automatically.
- Playlists (`.pls`, `.m3u`) are resolved by the directory; a bare playlist URL in the favourites file may not play.

For a front end such as Remote Control, `ui/radio.h` offers `radio_favorites`, `radio_play`, `radio_play_url`,
`radio_stop`, `radio_state`, `radio_station_name`, the directory search (`radio_search`, `radio_search_result`) and
`radio_fav_add` / `radio_fav_remove` (main thread only). [Remote Control](REMOTE_CONTROL.md) uses them for
its Radio tab.
