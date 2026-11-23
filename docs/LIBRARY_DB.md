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

- **The own player runs, or the launcher hasn't decided yet and the own player is switched on**: always the card DB
  (Jo, 2026-11-23). The flash DB lists the songs of whatever card the stock player scanned last, so it is never the
  own player's library, not even for a moment. Before the card is mounted (only after the launch verdict, seconds
  after the player started) every open fails and the library is empty until it loads (`libdb_watch` in
  `ui/main.c`). A card without `library.db` keeps an empty library until the move has put one there; the flash DB
  is only the move's source.
- **The stock player**: the flash DB, the same as before.

A scan is refused (`scanner_start` returns -2) while the card has no `library.db`: the UI makes it first, with every
table, and starts the scan itself.

**Status (2026-11-23):** switched on (`LIBDB_CARD_ENABLED 1`); the earlier copy-based move was device-verified by Jo, the new library + take-over (2026-11-23) is not yet. Build with
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

## A new library on the card

Decided with Jo on 2026-11-23: the card library is never a copy of the flash library's songs. It is made new and
filled by a scan of this card; afterwards the user's data from the flash library is taken over for the songs this
card has. `ui/libdb_migrate.c` does the work, `ui/libdb_ui.c` runs it (checked every 5 s from main.c). There is no
Maintenance entry.

1. **New library** (`libdb_create`). When the own player runs and the card has no `library.db`, and external power
   is connected and nothing plays (without power, a notice asks for it once per boot):
   - `library.db.tmp` gets the flash library's schema, without songs and without the stock queue tables
     (`LIST_SONG_*`). Without a flash library it gets the stock tables (V2.57 `.schema` from Jo's Disc, in
     `STOCK_SCHEMA`); diskOS's own tables are made on first use as before.
   - The tables below are added, `DISKOS_META` gets `LIBDB_RESCAN` = 1 and, with a flash library, `LIBDB_IMPORT` = 1.
   - `integrity_check` and `foreign_key_check` run, the file is fsynced, renamed to `library.db`, the folder fsynced.
   - A full-screen overlay ("Creating the library on the SD card", keep the power connected, do not remove the card)
     covers it and is removed when it ends, either way. Then the Disc restarts.
2. **Scan.** After the restart, `LIBDB_RESCAN` = 1 starts a scan of the card (once; the flag is cleared when it
   starts). Until the card has a `library.db`, `scanner_start()` refuses (-2): a scan would make a library with only
   the scanner's tables.
3. **Take-over** (`libdb_import`). When no scan is pending or running, the library has songs and `LIBDB_IMPORT` = 1:
   behind the overlay again ("Taking over favourites and playlists"), the flash library is attached read-only and,
   in one transaction, every table both have is copied, except the songs, the queue, resume and scan bookkeeping
   (`SONG`, `LIST_SONG_*`, `PLAY_LIST`, `MEMORY_PLAY`, `BOOKS`, `DISKOS_CUE_FILES`, `DISKOS_BOOK_MOVED`,
   `DISKOS_META`, `SOURCE_ID`):
   - tables with a `PATH` column (favourites `MY_LOVE`, playlist members `CUSTOM_PLAYLIST`, `RECORD_SONG`,
     `PLAY_STATS`, `PLAY_LOG`): only rows whose PATH is a song in the card library; their own `ID` is renumbered;
   - tables without one (playlist names `CUSTOM_PLAYLIST_INDEX` / `PLAYLIST_INFO`, `PEQ`, `BOOK_PROGRESS`, the
     playlist sync tables): whole, IDs kept, so playlist members still find their playlist;
   - `INSERT OR IGNORE`: nothing already in the card library is replaced. Then `LIBDB_IMPORT` = 0 and the library
     view reloads. The log lists what came over (`libdb: taken over from the flash library: MY_LOVE 12, ...`).

If anything fails, nothing half-made stays (`library.db.tmp` removed, the take-over rolled back), the flash DB is
untouched, and the next start tries again. Favourites and playlist members keep the tag values the flash library had
for them (as the stock player keeps them).

### Error codes

| Code | Meaning |
| --- | --- |
| L101 | The flash DB can't be opened or attached. |
| L102 | `library.db.tmp` can't be created on the card (the folder is missing, the card is read-only or full). |
| L103 | Reading the flash schema or copying the user's data failed (DB locked, I/O error, card full). |
| L104 | Adding the stock or diskOS's tables, or the meta values, failed. |
| L105 | `integrity_check` or `foreign_key_check` found a problem in the copy. |
| L106 | fsync or close failed, so the copy may not be on the card. |
| L107 | Renaming `library.db.tmp` to `library.db` failed. |
| L108 | `library.db` already exists. A new library never overwrites one. |

## Tests

- `ui/tests/libdb_test.c` covers the new library (flash schema and stock tables), the take-over by PATH, the
  connection settings, SOURCE_ID, the refusals and the player marker, with the card DB opened through libdb_vfs.
- `ui/tests/scanner_srcid_test.c` covers reading the tags and writing SOURCE_ID.
- `ui/tests/libdb_vfs_test.c` covers closing the files under open connections, including four threads querying
  through a hold, and checks via `/proc/self/fd` that no file stays open on the card.

Both run in `make -C ui/tests run`.
