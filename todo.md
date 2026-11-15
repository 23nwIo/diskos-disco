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
- Own player: check an MPD-own library next to musicdb (only for ncmpcpp and other clients to browse natively; the UI stays on SQLite). Decide once playback runs over MPD and its RAM use on the device is known; costs a second scan, no SACD ISO in upstream MPD (Jo, 2026-11-15).

## Engineering validation

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
