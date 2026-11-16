# Development targets after Disco 3.3.0

Targets below are proposals or verification work, not shipped features. Work belongs on dev; changes, builds and releases follow maintainer direction.

## Current UI targets

- Make Shade follow the finger during the pull; 3.3.0 animates after the gesture.
- Extend artwork coverage in Wheel album/playlist lists; preserve smooth motion and bounded decoding work.
- Refresh native 360 × 360 screenshots after layout approval, including long labels, empty states and all supported appearances.
- Review secondary-screen theme consistency: Null Stats typography, file dialogs, Apps headers, Date & Time and Weather. These are audit proposals, not approved redesigns.

## Feature proposals

- Local song ratings and smart playlists using reliable listening data; not implemented as a 3.3.0 feature.
- Optional dyslexia-friendly text font with preserved icon/clock/non-Latin rendering; requires layout and device evaluation.
- Own player (docs/MQ_PLAYER_REPLACEMENT.md): make the headphone-unplug behaviour a setting (today: unplug pauses, re-plug stays paused, as stock). Not urgent (Jo, 2026-11-15).
- Own player: the EQ presets (0689, software EQ in the stock player). Planned as an ALSA plugin behind the MPD audio core, about 3-5 days; not important for now (Jo, 2026-11-15).
- Battery: the Charging Limit (stop at about 80%, Settings > Battery & power) does not work on the Disc (Jo, 2026-11-16); to be looked at later.
- Radio: copy the favourites between the SD card and the player from the UI (today the setting only chooses the file; copy it by hand, docs/RADIO.md) (Jo, 2026-11-16).
- Radio, own player: send the song title to the UI as a frame over the player queue instead of /tmp/diskos-player-stream (Jo, 2026-11-16).
- Playlists: optionally keep them on the SD card (like the radio favourites) instead of only in song.db on the player; check what that means for the stock and the own player (Jo, 2026-11-16, not urgent).
- Own player: Resume Playback (switched off 2026-11-16, `PB_RESUME 0` in ui/player/playback.c; Jo: unimportant, no flash writes for it). It wrote `MEMORY_PLAY` in song.db (internal NAND) at every track start, every 30 s while playing, on pause and at power off. To bring it back without periodic flash writes: keep the position in RAM and write the row only at track start, pause/stop and an orderly power off (menu, sleep timer, power key), optionally plus a rare fallback (every 10 min) against crashes and an empty battery; the price is an older position after a crash, a power loss or a `reboot` over SSH.
- Flash writes (Jo, 2026-11-16, unimportant, look again later): event-driven writes to the internal NAND that remain - PLAY_LOG/PLAY_STATS in song.db per song played (Listening Stats), the ACCENT colour cache in song.db per new cover, the own player's queue tables (LIST_SONG_0/3) per new list, diskos.conf per settings change, wpa_supplicant `save_config` per Wi-Fi connect, lastfm.queue per scrobble, diskos_crash.log lines (start, crash, stall, low memory), and bluetoothd's own files (pairing/device cache, not checked). The stock player's writes are not analysed: it is being replaced.
- Own player: check an MPD-own library next to musicdb (only for ncmpcpp and other clients to browse natively; the UI stays on SQLite). Decide once playback runs over MPD and its RAM use on the device is known; costs a second scan, no SACD ISO in upstream MPD (Jo, 2026-11-15).

## Engineering validation

- Device tests waiting for material Jo does not have yet (2026-11-16): the 4.4 mm balanced jack (no plug), CUE playback in the own player (no CUE files), audiobook bookmarks on the card (`.diskos/state/books.tsv`, no .m4b; docs/SD_STATE.md).
- Measure screen-off current, large-library memory, scrolling frame pacing and Bluetooth reconnect behaviour on hardware before claiming gains.
- Continue bounded/background database and artwork work where profiling identifies foreground stalls.
- Reconcile legacy minute-based signing epochs with timestamp-floor installations before changing the bundled signing tool; preserve monotonicity and signature/trial protections.
- Keep focused test harnesses aligned with production interfaces; distinguish test failures from device bugs.
- Revisit decoder cancellation only with a reproducible crash trace and device verification.

## For the next rootfs (flash) change

Rootfs changes are deferred for now (Jo, 2026-11-06). Collect here what a future flash should take along.

- Replace `/usr/bin/cmd_watchdog` with `diskos-watchdog` (ui/watchdog) once it has run stably as the bind-mount override from mq_ui (ui/wd_override.h, since 2026-11-14). Caution: the rootfs is also the Vol-Up stock fallback, so after the swap the stock firmware runs our tool too; only do it with a long clean record. See docs/MQ_PLAYER_REPLACEMENT.md.

- Early-boot clock is ahead by the UTC offset: the RTC holds local time but the kernel loads it as UTC (hctosys), and nothing corrects it before mq_ui/mq_player run `hwclock -s`, so S96/S97 log stamps are 1 h (CET) / 2 h (CEST) late. No fix is possible without a flash: `/var/lib/hwclock` is on the read-only rootfs and no early hook runs from `/usr/data`. Options: (a) run `hwclock -s` (local time) as the first init step before S96, keeping the stock convention, with no migration; (b) switch the RTC to UTC by shipping `/var/lib/hwclock/adjtime` (or a symlink into `/usr/data`) with `UTC`. Stock mq_player's flagless `hwclock` calls and the Vol-Up fallback then follow it, and the kernel is right from the first second, but mq_ui needs a one-time migration (rewrite the RTC as UTC on the first boot of the new rootfs) or the clock is off by the offset once. See docs/HARDWARE.md (Misc, RTC).

No dates, performance improvements or completion status are promised by this list. Previous checkpoint lists are available in Git history.

- Own player, SD hot-plug (side note, Jo 2026-11-15, not urgent): the first uevent listener (d342e43) received nothing
  on the Disc; b3822cf (256 KiB receive buffer, POLLERR also read) works. Which of the two changes fixed it was not
  investigated, since swapping the card is not a usual scenario.
