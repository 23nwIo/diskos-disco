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

## Engineering validation

- Measure screen-off current, large-library memory, scrolling frame pacing and Bluetooth reconnect behaviour on hardware before claiming gains.
- Continue bounded/background database and artwork work where profiling identifies foreground stalls.
- Reconcile legacy minute-based signing epochs with timestamp-floor installations before changing the bundled signing tool; preserve monotonicity and signature/trial protections.
- Keep focused test harnesses aligned with production interfaces; distinguish test failures from device bugs.
- Revisit decoder cancellation only with a reproducible crash trace and device verification.

## For the next rootfs (flash) change

Rootfs changes are deferred for now (Jo, 2026-11-06). Collect here what a future flash should take along.

- Replace `/usr/bin/cmd_watchdog` with `diskos-watchdog` (ui/watchdog) once it has run stably as the bind-mount override from mq_ui (ui/wd_override.h, since 2026-11-14). Caution: the rootfs is also the Vol-Up stock fallback, so after the swap the stock firmware runs our tool too; only do it with a long clean record. See docs/MQ_PLAYER_REPLACEMENT.md.

No dates, performance improvements or completion status are promised by this list. Previous checkpoint lists are available in Git history.
