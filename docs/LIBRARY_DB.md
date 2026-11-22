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

- **Own player switched on** (`/usr/data/diskos-own-player`), a card mounted at `/tmp/sdcard`, and the card has
  `library.db`: the card DB is used.
- **In every other case**, the flash DB is used, the same as before.

**Status:** the card DB is switched off for now (`LIBDB_CARD_ENABLED 0`), so `libdb_path()` always gives the flash DB.
It goes on once the UI closes its long-lived connections when the card goes away (USB storage, format, removal).
An open card DB would otherwise keep the card busy.

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

`libdb_migrate()` in `ui/libdb_migrate.c` does the copy. The UI part around it is still to do:

1. The migration starts only when the own player runs, the card has no `library.db`, and external power is connected.
   Without power, a notice asks you to plug it in. Until then, everything stays on the flash DB.
2. Playback stops. A full-screen overlay says that the migration is running, that the power must stay connected and
   that the card must not be removed.
3. The flash DB is copied with SQLite's backup API, as one consistent snapshot, to `library.db.tmp`.
4. The stock player's queue tables (`LIST_SONG_0`, `LIST_SONG_3`) are dropped. The own player keeps its list in RAM.
5. The tables above are added, then `integrity_check` and `foreign_key_check` run.
6. The file is fsynced, renamed to `library.db`, and the folder is fsynced.
7. The overlay is removed. A short "done" message shows, then the Disc reboots.

If anything fails, `library.db.tmp` is removed and the flash DB is untouched, and the next start tries again.

A card whose files are not in the flash DB gets the copy too, so playlists and favourites come along. A rescan then
brings its library up to date (still to do).

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

- `ui/tests/libdb_test.c` covers the copy, the connection settings, SOURCE_ID and the refusals.
- `ui/tests/scanner_srcid_test.c` covers reading the tags and writing SOURCE_ID.

Both run in `make -C ui/tests run`.
