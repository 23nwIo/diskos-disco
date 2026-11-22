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
| `party.conf` | Remote Control public mode settings (`on` = mode 0 Off, 1 Jukebox (was party mode), 2 Hit&Guess): `key=value` lines (name, on, vol, maxvol, np, queue, skip, total, consec, repeat, radio, names; ui/party.c) | only when the host changes a setting on System > Public or ends it on the Disc | new (2026-11-16) |
| `hitguess.conf` | Hit&Guess setup: `key=value` lines (dir, pl and the game keys; ui/hitguess.c) | only when the host changes the setup on the game page | new (2026-11-18) |
| `song-fixes.tsv` | Hit&Guess corrections for the tags: a `#` header, then one tab-separated line per song (path on the card, title, artist, the library's date, right artist, right title, right year, leave out (1), by, when); a newer one replaces the older | when a player or the host corrects a song | new (2026-11-19; replaces `year-fixes.tsv`, which is still read) |
| `library.db` (+ `library.db-journal`) | the own player's library, a copy of the flash song.db plus SOURCE_ID (docs/LIBRARY_DB.md); never delete the journal | by the scanner, the UI and the own player | new (2026-11-22, not switched on yet) |

A different card brings its own files. Paths in `books.tsv` are as the Disc sees them (`/tmp/sdcard/...`).

## In RAM: `/tmp`

`/tmp/diskos_boot.log` (mq_ui's stdout and stderr; touch positions only with `/usr/data/touch_dbg`),
`/tmp/coldplug.log`, `/tmp/sdformat.log`, `/tmp/usbdrive.log`, `/tmp/album-roulette.log`, and the player logs
(`/tmp/diskos-player.log`, `/tmp/diskos-volume.log`, `/tmp/diskos-mpd/`). mq_ui rotates them.

## Still on the internal flash

Fatal signals of mq_ui and its start lines go to `/usr/data/diskos_crash.log` (rare; they must survive a reboot).
Everything else written there only on an event (settings, Wi-Fi, a song played, ...) is listed in todo.md
("Flash writes"). The own player no longer writes the resume bookmark (todo.md, "Resume Playback").

## A card that comes back read-only

Switching from USB storage back to Playback while the computer still has the card mounted leaves the file system
"in use" (2026-11-20, an ext4 card from a Linux PC: journal needs recovery and the newer kernel's `orphan_present`
feature, which the Disc's kernel refuses to mount writable). diskOS handles this in two places:

- **Leaving USB storage** reads the start of the shared file system (`ui/sdfscheck.c`, read from `/dev/mmcblk0` while
  it stays exported) and asks first when it is marked in use: "Eject on the computer first", with Cancel or Switch.
  Marked in use means: ext2/3/4 needs recovery, `orphan_present` or not "clean"; FAT/FAT32 dirty bit; exFAT
  VolumeDirty. Linux sets these while a card is mounted (checked for ext4 with a loop mount). Whether Windows or macOS
  set them is not checked, so no warning does not prove the computer let go.
- **Emergency read-only mount**: an ext card the kernel will not mount writable is mounted read-only (as `mount -o ro`,
  which replays the journal) at start-up and after USB storage, by mq_ui (`sd_ext_mount` in `ui/main.c`) and by the
  own player (`ui/player/sdcard.c`). Before, a failed mount after USB storage restarted the Disc and the card then
  stayed unmounted. While read-only, diskOS shows "SD card is read-only" once and makes no writes of its own to the
  card (`sd_write_begin`). USB storage works again, so the card can be repaired on a computer:

```
sudo e2fsck -f /dev/sdX
sudo tune2fs -O ^orphan_file /dev/sdX    # only if the card has orphan_file (formatted by e2fsprogs 1.47 or newer)
```

Back on Playback, the repaired card is mounted writable again.
