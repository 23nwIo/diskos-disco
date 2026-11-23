# The own player's library on the SD card

Decided with Jo on 2026-11-22. Work in progress, not device-tested.

## Why

The stock player keeps one library, `/usr/data/fiio/db/song.db`, on the internal flash. When you put in another card,
that library does not know the songs on it. It is also written on every play (LIST_SONG_0), which wears the flash.

The plan is that the own player gets its own library on the card, `<card>/.diskos/state/library.db`. Each card then
carries the library of its own files. The stock player keeps the flash DB as it is and keeps updating it. That is the
fallback when the Disc boots to the stock player (Settings > Maintenance > Boot to Stock Player).

## Which file is used

`ui/libdb.h` decides this in one place. Every component that opens the library asks `libdb_path()` and opens it with
`libdb_open_v2()`. That covers musicdb, the scanner, the own player (`player/queuedb.h`), Listening Stats, Album
Roulette, SongSphere and Disc Shelf.

- **The own player runs**, a card is mounted at `/tmp/sdcard`, and the card has `library.db`: the card DB is used.
- **The launcher hasn't decided yet** and the own player is switched on (`/usr/data/diskos-own-player`): the card DB
  too. mq_ui starts about 2 s before the launcher, and this way the boot never opens the flash DB for a library that
  will come from the card. Until the card is mounted and SD access opens, every open of it fails, and the library
  loads once when it can (`libdb_watch` in `ui/main.c`).
- **The own player runs and no card is mounted yet**: the card DB as well. The card is mounted only after the launch
  verdict, seconds after the player started; answering the flash DB in that gap loaded the flash library first, and
  musicdb parks a replaced connection without closing it, so `song.db` stayed open in mq_ui (Jo's device test,
  2026-11-23).
- **In every other case** (stock player, or a mounted card not moved yet), the flash DB is used, the same as before.

**Status (2026-11-23):** switched on (`LIBDB_CARD_ENABLED 1`), migration device-verified by Jo. Build with
`-DLIBDB_CARD_ENABLED=0` to keep the flash DB only.

Which player runs is told by the launcher (`player_launch_prepare` in `ui/main.c`). Before it execs the player, it
writes `/tmp/.diskos_libdb_own_pid` (tmpfs): its own pid for the own player (the pid stays the same across the exec,
and the player writes it again at start), `stock` otherwise. `libdb_player()` reads it:

| Marker | Meaning |
| --- | --- |
| missing | pending: the launcher hasn't decided yet |
| `stock` | the stock player |
| pid, `/proc/<pid>/exe` = `/usr/data/diskos-own/mq_player` | the own player |
| pid, exe = mq_ui itself | pending: the launcher, just before its exec |
| pid of anything else, or gone | the stock player |

If no marker appears within 15 s after the launch verdict (a launcher older than this change), the UI writes `stock`
itself. The switch file is not used to decide this: it names the next boot's player, and the stock player may run
although it exists (after the own player failed).

## When the card goes away

mq_ui keeps connections open for a long time: the writer, the accent writer, a reader per thread and the Up Next
reader. Any of them can be in use on another thread at any moment. An open file on the card would make the plain
umount fail in USB storage mode and in Format SD Card, and closing a connection from outside is not safe.

So in mq_ui the card DB opens through a small pass-through VFS (`ui/libdb_vfs.c`, registered in `storage_init`).

1. **SD access held** (`sd_io_hold`: USB storage, format, card pulled, Restart). `libdb_vfs_detach()` releases the
   locks and closes the real files under every connection. The connections stay valid objects, and every further
   read or write on them fails with an I/O error. New opens are refused.
2. **SD access resumed** (`sd_io_resume`). New opens work again. A detached file is never reopened, because the card
   may be a different one by then and SQLite's page cache would not notice.
3. **Replacing connections.** musicdb replaces a shared connection on its next use when the detach generation
   (`libdb_gen()`) or `libdb_path()` has changed. It retires the old one (frees its memory and parks the handle) rather
   than closing it, because another thread may still hold it. Per-thread readers are closed by their own thread.
4. **Reloading the library.** `libdb_watch()` in `ui/main.c` reloads the in-memory library when the library in use
   changes or comes back.

The own player opens the DB per operation, using `libdb_path()` each time (`player/playback.c`), and closes it again
before it unmounts the card.

## Connection settings on the card DB

The flash DB is left exactly as the stock player has it. The card DB gets these settings on every connection
(`libdb_setup`):

| Setting | Why |
| --- | --- |
| `foreign_keys=ON` | SQLite only enforces `SOURCE_ID`'s foreign key on connections that switch it on. |
| `journal_mode=TRUNCATE` | A rollback journal, not WAL. WAL needs a shared-memory file that every process maps, which is risky on FAT and exFAT. TRUNCATE empties the journal instead of deleting it, which means fewer directory writes on FAT. |
| `synchronous=FULL` | The journal reaches the card before the DB changes. If the card is pulled or the power is lost, the last transaction is rolled back the next time the DB is opened. |

Keep `library.db-journal` next to `library.db`. After a pulled card, that file holds what SQLite needs to roll back.

## Tables diskOS adds

The card DB has the stock player's schema (it starts as a copy), plus these tables:

```sql
CREATE TABLE SOURCE_ID(SOURCE TEXT NOT NULL, EXT_ID TEXT NOT NULL, ALBUM_ID TEXT,
                       SONG_ID INTEGER NOT NULL REFERENCES SONG(ID) ON DELETE CASCADE,
                       PRIMARY KEY(SOURCE, EXT_ID));
CREATE INDEX SOURCE_ID_SONG ON SOURCE_ID(SONG_ID);
-- DISKOS_META('LIBDB_VERSION') = 1
```

`SOURCE_ID` maps a song to its ID at an outside source. Today that is `SOURCE` = `bandcamp`. Later sources could be
`navidrome`, `rsync` and so on.

- **Deleting a song** also deletes its mappings, through the cascade.
- **A rescan** updates songs in place by PATH, so SONG.ID stays the same.
- **A moved or renamed file** gets a new SONG.ID. The scanner then fills its mapping again from the file's tags
  (`source_note` in `ui/scanner.c`).

The tags the scanner reads are the ones Bandcamp downloads write (docs/BANDCAMP.md). Values must be digits; anything
else is ignored.

| Format | Track ID → `EXT_ID` | Album ID → `ALBUM_ID` |
| --- | --- | --- |
| MP3 (ID3v2) | `TXXX:BANDCAMP_TRACK_ID` | `TXXX:BANDCAMP_ALBUM_ID` |
| FLAC, Ogg (Vorbis comments) | `BANDCAMP_TRACK_ID=` | `BANDCAMP_ALBUM_ID=` |

On the flash DB there is no `SOURCE_ID` table, and the scanner keeps no IDs there.

## Moving the library to the card

`libdb_migrate()` in `ui/libdb_migrate.c` does the copy; `ui/libdb_ui.c` runs it (checked every 5 s from main.c):

1. The migration starts only when the own player runs, the card is local and writable, no scan runs, the card has no
   `library.db`, and external power is connected. Without power, a notice asks for it, once per boot. Until then,
   everything stays on the flash DB.
2. It waits until nothing plays. A full-screen overlay says that the migration is running, that the power must stay connected and
   that the card must not be removed.
3. The flash DB is copied with SQLite's backup API, as one consistent snapshot, to `library.db.tmp`.
4. The stock player's queue tables (`LIST_SONG_0`, `LIST_SONG_3`) are dropped. The own player keeps its list in RAM.
5. The tables above are added, then `integrity_check` and `foreign_key_check` run.
6. The file is fsynced, renamed to `library.db`, and the folder is fsynced.
7. The overlay is removed. A short "done" message shows, then the Disc reboots.

If anything fails, `library.db.tmp` is removed and the flash DB is untouched, and the next start tries again.

**Manual start:** Settings > System > Maintenance > Move Library to SD Card. It checks the same conditions in turn and
shows the first one that fails as a toast (the full text goes to `/tmp/diskos_boot.log`, `libdb:`). An existing
`library.db` always stops it. When every check passes, a confirmation starts the same move; a failure then toasts the
first error with its L1xx code.

A card whose files are not in the flash DB gets the copy too, so playlists and favourites come along. The copy checks
up to 64 random card paths from it; when fewer than half are on this card, it is marked (`DISKOS_META`
`LIBDB_RESCAN` = 1), and after the restart the UI starts a rescan once.

### Error codes

| Code | Meaning |
| --- | --- |
| L101 | The flash DB can't be opened. |
| L102 | `library.db.tmp` can't be created on the card (the folder is missing, the card is read-only or full). |
| L103 | The copy failed (the DB stayed locked, I/O error, card full). |
| L104 | Dropping the queue tables or adding diskOS's tables failed. |
| L105 | `integrity_check` or `foreign_key_check` found a problem in the copy. |
| L106 | fsync or close failed, so the copy may not be on the card. |
| L107 | Renaming `library.db.tmp` to `library.db` failed. |
| L108 | `library.db` already exists. A migration never overwrites one. |

## Tests

- `ui/tests/libdb_test.c` covers the copy, the connection settings, SOURCE_ID, the refusals and the player marker.
- `ui/tests/scanner_srcid_test.c` covers reading the tags and writing SOURCE_ID.
- `ui/tests/libdb_vfs_test.c` covers closing the files under open connections, including four threads querying
  through a hold, and checks via `/proc/self/fd` that no file stays open on the card.

Both run in `make -C ui/tests run`.
