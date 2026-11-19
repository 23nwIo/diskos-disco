# What diskOS keeps where: SD card, RAM, internal flash

Rule (Jo, 2026-11-16): nothing recurring goes to the internal NAND (`/usr/data`). Logs go to `/tmp` (RAM, gone after
a reboot). Data that belongs with the music goes onto the SD card. Not yet device-tested.

## On the SD card: `/tmp/sdcard/.diskos/state/`

The other folders in `.diskos/` (`artcache`, `netart`, `album-roulette`) are caches and may be deleted. `state/` is
not: it holds data that has to stay. Every access holds an SD I/O lease (ui/sdstate.c, ui/sdio.c) and only happens
while the card is mounted at `/tmp/sdcard`, never during USB storage mode or a format. Files are written whole (temp
file, fsync, rename).

| File | What | Written | Before 2026-11-16 |
| --- | --- | --- | --- |
| `books.tsv` | audiobook bookmarks: book path, member path, position ms, updated (unix s), completed; tab separated (ui/bookmarks.c) | every 30 s while a book plays, at each pause, at the end of a book session and at power off | `BOOK_PROGRESS` in song.db, every 10 s. Moved over once, the first time a card without `books.tsv` is seen, then deleted from song.db |
| `usage.bin`, `usage_charge.bin` | Battery & usage: one sample a minute for the last ~25 h, and the totals since the last full charge (ui/usage.c) | every 10 minutes, at a full charge and before power off; without a card the data stays in RAM until one is there | `/usr/data/usage.bin`, `/usr/data/usage.bin_charge`: read at start, deleted once the card has a copy |
| `autotag.log` | Auto-tag: files where nothing was found, retried at most once a day (ui/tagfix.c) | per auto-tag attempt | `/usr/data/autotag.log`: dropped at the first write |
| `party.conf` | Remote Control party mode settings: `key=value` lines (name, on, vol, maxvol, np, queue, skip, total, consec, repeat, radio, names; ui/party.c) | only when the host changes a setting on System > Party | new (2026-11-16) |
| `hitguess.conf` | Hit&Guess setup: `key=value` lines (dir, pl and the game keys; ui/hitguess.c) | only when the host changes the setup on the game page | new (2026-11-18) |
| `song-fixes.tsv` | Hit&Guess corrections for the tags: a `#` header, then one tab-separated line per song (path on the card, title, artist, the library's date, right artist, right title, right year, leave out (1), by, when); a newer one replaces the older | when a player or the host corrects a song | new (2026-11-19; replaces `year-fixes.tsv`, which is still read) |

A different card brings its own files. Paths in `books.tsv` are as the Disc sees them (`/tmp/sdcard/...`).

## In RAM: `/tmp`

`/tmp/diskos_boot.log` (mq_ui's stdout and stderr; touch positions only with `/usr/data/touch_dbg`),
`/tmp/coldplug.log`, `/tmp/sdformat.log`, `/tmp/usbdrive.log`, `/tmp/album-roulette.log`, and the player logs
(`/tmp/diskos-player.log`, `/tmp/diskos-volume.log`, `/tmp/diskos-mpd/`). mq_ui rotates them.

## Still on the internal flash

Fatal signals of mq_ui and its start lines go to `/usr/data/diskos_crash.log` (rare; they must survive a reboot).
Everything else written there only on an event (settings, Wi-Fi, a song played, ...) is listed in todo.md
("Flash writes"). The own player no longer writes the resume bookmark (todo.md, "Resume Playback").
