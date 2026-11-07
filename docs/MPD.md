# MPD Server (mpc, ncmpcpp and other MPD clients)

The Disc can answer the [MPD protocol](https://mpd.readthedocs.io/en/latest/protocol.html), so MPD clients such as
`mpc`, `ncmpcpp`, Cantata or phone apps like MALP can control it over Wi-Fi: play/pause, skip, seek, volume, play
mode, browse and search the library, and build a list of songs to play. Off by default.

> **Not yet tested on a Disc.** The protocol layer and the server thread are tested on a host build
> (`ui/tests/mpd_test.c`, and `ui/tests/mpd_client_test.py` with the python-mpd2 client library), and the commands run
> against a test `song.db` through the production code (`ui/tests/mpd_library_test.py`). A device test (MIPS build,
> real Wi-Fi, mpc and ncmpcpp against the real player) is still open.

This is stage 1: the commands `mpc` uses day to day and what ncmpcpp needs to browse and play. It is not a full MPD:
the Disc's player keeps its own play list, and the MPD view of it is built on top (see "The queue" below).

## Turn it on

1. Connect the Disc to Wi-Fi.
2. Settings > Connectivity > Remote Control > **MPD Server**: On.
3. **MPD Access** shows the Disc's address, the port (6600) and the password.

The password is the Remote Control link code (the part after the address in Show Link). **New Link** changes it for
both. MPD clients send it with the `password` command; everything except `ping`, `password`, `commands` and `close`
is refused until they do.

```sh
export MPD_HOST=<password>@<Disc address>      # mpc and ncmpcpp read MPD_HOST (and MPD_PORT, default 6600)
mpc status
mpc ls
mpc search title nuit
mpc clear; mpc add "Music/Blue Album"; mpc play
mpc volume 40
```

For ncmpcpp, set `mpd_host = "<password>@<Disc address>"` in `~/.config/ncmpcpp/config` (or use `MPD_HOST`).

The server listens only on the Disc's Wi-Fi address (never on every interface) and follows a new address within a
second or two. Up to 8 clients at once. The protocol is unencrypted, like MPD itself: anyone who can watch the Wi-Fi
traffic can read the password. Use it on trusted networks only.

## Paths

Song paths (MPD "URIs") are relative to the SD card root, for example `Music/Blue Album/01 Uno.flac`. A track of a
CUE sheet or an ISO image is `<file>/trackNNNN` (as MPD names CUE tracks), for example `Music/live.flac/track0002`.
Only songs the Disc's library has scanned are listed; `mpc update` starts a library rescan (as Settings > Rescan).

## The queue

MPD has one queue that clients edit freely. On the Disc the player owns the list it plays, and diskOS adds its own
Queue (songs that play right after the current one). An MPD client sees:

| Position | What |
|---|---|
| 0 | the song playing now |
| then | the songs in the diskOS Queue (what `add` puts there while something plays) |
| then | Up Next: the rest of the list the player plays, in play order (the shuffled order in Shuffle), at most 500 |

- **add** while a song is loaded puts the songs in the diskOS Queue: they play after the current song (as the Disc's
  own "Add to Queue"). Adding at a position is not supported.
- **clear** empties the diskOS Queue, pauses, and starts a new list: `add` then collects songs (at most 2000; a song is
  only added once), `status` says `stop`, and **play** (or `play N`) hands the whole list to the player, which plays it
  in your play mode. Anything started on the Disc itself in the meantime ends the new list.
- **delete**, **move** and **shuffle** work on the diskOS Queue and on a new list. Up Next belongs to the player: its
  songs can't be removed or moved (the Disc says so with an error), and neither can the song playing now.
- **play N** / **playid** jump to any song in the view; **next** / **previous** are the Disc's buttons.
- **save NAME** saves the view as a Disc playlist. `listplaylists`, `listplaylist(info)`, `load`, `rm` and `rename` work
  on the Disc's playlists.

Song ids: Up Next songs use the player's list row id; queued songs and new-list songs get ids from 1000000 and
2000000 up that change when the list changes (the playlist version changes with them, so clients re-read it).

## Web radio

The Disc's radio favourites (the ones held on the Radio screen, `/usr/data/radio_favorites.txt`) are the stored
playlist **Radio**. It is listed first by `listplaylists` and in the root of `lsinfo`; it hides a Disc playlist of the
same name.

- `listplaylist Radio` gives the stream URLs; `listplaylistinfo Radio` adds the station name (`Name`, `Title`) and
  codec, bit rate and country (`Comment`).
- A stream can't wait in a queue on the Disc, so a station **plays at once**: `add URL` / `addid URL` with an
  `http://` or `https://` URL, and `load Radio` (the first station) or `load Radio N:N+1` (station N, from 0). A URL
  that is a favourite plays under its name. While the radio plays, the MPD queue is that one stream (`currentsong`:
  the URL, the song the stream names as `Title`, the station as `Artist` and `Name`).
- `stop` and `clear` end the radio and return to local playback (paused). `pause` pauses the stream; `next`,
  `previous` and seeking are refused.
- `playlistadd Radio URL` saves a favourite (named after the station if it is the one playing, else the URL);
  `playlistdelete Radio N` removes station N. `rm` / `rename` / `save` refuse the name Radio.

ncmpcpp: Playlist editor (`5`), choose **Radio** on the left, Enter on a station on the right. mpc:
`mpc lsplaylists`, `mpc playlist Radio`, `mpc clear; mpc load Radio` (or `mpc add https://...`).

## Commands

| Group | Commands |
|---|---|
| Status | `status`, `currentsong`, `stats`, `idle` / `noidle` (player, mixer, options, playlist, stored_playlist, database, update), `ping`, `commands`, `notcommands`, `tagtypes`, `outputs`, `decoders`, `urlhandlers`, `replay_gain_status` |
| Playback | `play`, `playid`, `pause`, `stop` (pauses: the player has no stop; ends the radio), `next`, `previous`, `seek`, `seekid`, `seekcur` (only the song playing now) |
| Options | `random`, `repeat`, `single` (mapped to the Disc's play modes, below), `setvol`, `volume`, `getvol`; `consume 0` and `crossfade 0` are accepted |
| Queue | `playlistinfo`, `playlistid`, `playlist`, `plchanges`, `plchangesposid`, `add`, `addid` (appends only), `clear`, `delete`, `deleteid`, `move`, `moveid`, `shuffle` |
| Library | `lsinfo`, `listall`, `listallinfo`, `find`, `search`, `findadd`, `searchadd`, `count`, `searchcount`, `list` (with `group`), `update`, `rescan` |
| Playlists | `listplaylists`, `listplaylist`, `listplaylistinfo`, `load`, `save`, `rm`, `rename`; `playlistadd` / `playlistdelete` on Radio only |
| Connection | `password`, `close`, `binarylimit`, `command_list_begin` / `command_list_ok_begin` / `command_list_end` |

Filters take both forms: `find artist "Ana"` and `find "((artist == 'Ana') AND (track != '3'))"`, with `==`, `!=`,
`contains`, `starts_with`, `base`, a leading `!` and `AND`. Regular expressions (`=~`) and `OR` are not supported.
Tags: Artist, Album, AlbumArtist, Title, Track, Disc, Genre, Date, file, any (and the Sort variants, which map to their
base tag).

### Play modes and MPD's switches

| Disc play mode | random | repeat | single |
|---|---|---|---|
| Sequential | 0 | 0 | 0 |
| Shuffle | 1 | 0 | 0 |
| Repeat one | 0 | 1 | 1 |
| Repeat all | 0 | 1 | 0 |
| Single | 0 | 0 | 1 |

A combination the Disc has no mode for (random together with repeat or single, `single oneshot`) is refused with an
error that says so. The volume is the Disc's 0-120 scale shown as 0-100 %.

## Not supported

Album art (`albumart`, `readpicture`), stickers, partitions, client-to-client channels, consume and crossfade, replay
gain changes, more outputs, streams waiting in the queue (a stream plays at once, see Web radio), editing Disc playlists
(`playlistadd` and the like work on Radio only), `listfiles`, `prio`, `rangeid`. Music Assistant and the other working
modes show the one thing playing; transport works as on the Remote Control page.

Commands run on the Disc's UI thread, like the Remote Control page's. A whole-library answer (`listallinfo` of a big
card) takes a moment and holds the screen while it is built.

## Files

`ui/mpd_core.c` (protocol: lines, filters, sessions, command lists; pure), `ui/mpd_srv.c` (the network thread),
`ui/mpd.c` (the commands, the queue view, settings), `ui/tests/mpd_test.c`, `ui/tests/mpd_client_test.py`,
`ui/tests/mpd_library_test.py`.
