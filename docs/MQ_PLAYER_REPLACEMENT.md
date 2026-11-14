# Replacing the stock mq_player with our own code: assessment

Status: assessment only (2026-11-14). Nothing here is implemented. Facts marked **(binary)** were read from the stock
`/usr/bin/mq_player` that Jo uploaded on 2026-11-14 (MIPS32r2, glibc, stripped, 4 601 368 bytes, SHA-256
`a5a6740435758bb3…`, presumably V2.57); facts marked **(repo)** cite this repository. Everything marked
**unverified** still needs a check on a Disc.

## Short answer

Yes, it is possible, and it does **not** need a new flash: the boot hook already starts `/usr/data/mq_player`, and
that path is ours. The protocols (decoding, ALSA, Bluetooth, AirPlay, DLNA, USB gadgets) are all covered by existing
open-source software. The work that only exists inside FiiO's binary is the hardware control: the four CS43131 DACs,
the hardware watchdog, power/charging, the power key and screen sleep. The DAC interface
turned out to be small (19 ioctl codes), which makes a replacement much more realistic than earlier notes suggested.

Rough effort: a first own player that plays local files on headphones with volume, DAC filter and EQ is about
**4 to 6 weeks** of work plus Jo's device tests. Parity with everything diskOS uses today (Bluetooth both ways, USB
modes, AirPlay, DLNA/radio, DSD) is about **3 to 4 months**. Roon, QPlay and FiiO Link would be dropped.

## What mq_player does

Source file names in the binary **(binary)** group into these subsystems. "diskOS" says whether diskOS already does
the job itself, so the stock part is no longer needed.

| Subsystem | Stock (source files in the binary) | diskOS today | Replacement | Needs RE |
|---|---|---|---|---|
| UI IPC | `mq_player_server*.c`, `/player` + `/ui` mqueues | sends ~33 command tags (ui/*.c) | own backend API in diskOS (see Strategy) | no |
| Decoding | `decoder.c`, FFmpeg 58 libs, `libfiio_decoder.so` (proprietary) | none | FFmpeg (the same libs are on the rootfs) | no |
| SACD ISO / DST | `libsacd/scarletbook_read.c`, `dstdec` (sacd-ripper code) | scanner expands ISO tracks (ui/scanner.c) | sacd-ripper / FFmpeg `dst` decoder | no |
| DSD (native, DoP) | `player_digital.c`, `dac_control.c` | none | DoP is plain PCM framing; native DSD needs the DAC ioctls | **yes** |
| PCM output | `pcm_control*.c`, `audio_router_manager.c`, libasound + libtinyalsa | none | alsa-lib / tinyalsa on card 0 | no |
| DAC control | `dac_control.c`: `cs43131_init` per L+/L-/R+/R-, `cs43131_filter_set`, `cs43131_sample_write`, `dac_power_up/down` | none | own code using the RE'd ioctls | **yes** |
| Volume, balance, gain | `vol_manager.c`, `hw_set_balance`; `ICODEC HPOUTL GAIN` and `/sys/.../i2c-4/4-0034/vol` strings | sends 0715/0713/0645 | own code; which path the Disc really uses is **unverified** | **yes** |
| EQ / PEQ, bass/treble | `eq.c` (biquad cascade, RE_CATALOGUE §3) | custom EQ UI + PEQ table | own biquad code (well documented maths) | no |
| Gapless, fade, ReplayGain, CUE, M3U, lyrics | `fade.c`, `cue_parse.c`, `m3u_parse.c`, `lrc_paser.c` | CUE/ISO in scanner, own play history | own code or MPD | no |
| Library DB + scanner | `db_*.c`, `table_*.c`, `file_scanner.c` | **done**: own scanner (ui/scanner.c), musicdb | keep diskOS | no |
| Bluetooth source (to headphones) | `bt_source_control.c`, BlueZ + BlueALSA, `brcm_patchram_plus` | own BlueALSA 4.0 with aptX (BLUETOOTH.md) | BlueZ + BlueALSA (+ libldac for LDAC) | no |
| Bluetooth sink (phone to Disc) | `bt_sink_server.c`, `bt_sink_control.c` | none | BlueALSA a2dp-sink + `bluealsa-aplay` | no |
| AirPlay receiver | `fiio_airplay.c` + embedded shairport-sync (`src/rtsp.c` …) | uses it for the radio helper (ui/ma.c) | upstream shairport-sync | no |
| DLNA renderer | `dlna_player.c` | radio uses it (RADIO.md) | upmpdcli + MPD, or gmrender-resurrect | no |
| Roon Ready | `roon_player.c`, `libroon.so` | none | **dropped** (closed SDK; Jo does not need it) | - |
| QPlay, FiiO Link | `mq_player_server_qplay.c`, `fiio_link.c` | none | **dropped** | - |
| NAS | `nas_control.c` (`mount -t cifs` / NFS) | none | shell `mount`, trivial | no |
| HTTP server, online cover/lyrics | `mongoose.c`, `http_server_*.c`, `online_*.c` | Remote Control, SSH/rsync | **dropped** | - |
| OTA | `ota_update.c`, `udisk_update.c` | signed SD updates (UPDATING.md) | **dropped** | - |
| USB storage mode | `all_mode_control.c`, configfs `storage_demo` | mode switch via 0642 | configfs `mass_storage`, documented (RE_CATALOGUE §6) | no |
| USB DAC mode (Disc as DAC) | configfs `uac_demo` (uac1/uac2), `/dev/usb_dac`, `uac_srate_change_handler` | mode switch via 0642/0657 | configfs UAC2 + capture-to-playback loop | small |
| USB host, external USB DAC | `uevent_handler.c` (`on_usb_audio_add`), role switch sysfs | USB drive (USB_SYNC.md) | snd-usb-audio is plain ALSA | no |
| USB drive automount | `mount_storage_dev.c` | **done**, and works around the stock one (USB_SYNC.md) | keep diskOS | no |
| SD card events | `uevent_listener.c` (`on_sdcard_event`) | ext4 mount/remount in diskOS | netlink uevents, simple | no |
| Wi-Fi status | `disc_wifi_control.c` (`wpa_cli status`) | diskOS manages Wi-Fi state | keep diskOS | no |
| Power key, screen sleep | `echo_key_handler.c` (`/dev/key_ioctl`), `lcd_handler.c` (`/dev/lcd_st77916`), touch sleep (`/dev/cst816t`) | power key comes from the player (ui/main.c:1961, 4201) | own code, ioctls known by number | **yes** (small) |
| Power off | `echo_sys_control.c`: `cmd_watchdog stop`, kill `fiio_init.sh` and `mq_ui`, backlight 0, `poweroff -f` | none | own code, sequence is visible in strings | small |
| Battery, charger, USB-C PD | `adc_control.c`, `/dev/sgm41513`, `/dev/aw35615`, `/dev/usbcc_ioctl`, cw221X sysfs | reads cw221X; uses cc_term for idle (memory note) | own code | **yes** |
| Headphone / balanced detect | `adc_bal_det_handler`, `po_handle` (SAR ADC `/dev/jz_adc_aux_*`) | none | own code; thresholds need RE | **yes** |
| Watchdog | `watchdog_control.c`: `cmd_watchdog start 10000`, `cmd_watchdog feed` | none | run the same command | small |
| MCU over SPI | `spi_handler.c`, `/dev/spidev0.0`, framed packets with checksum; commands `SET_DEVICE_VOL`, `SET_DAC_FILTER`, `SET_USB_MODE`, `SET_MCU_POWER`, `ENTER_MCU_UPDATE_MODE` … | none | **not present on the Disc**: no `/dev/spidev*` node and no device under `/sys/bus/spi/devices` (checked on Jo's Disc 2026-11-14); the code is shared with other FiiO models | no |
| RTC | `hwclock -w/-s` | diskOS loads the clock from the RTC (ui/main.c:238) | keep | no |

## Key findings from the binary

**Launch path, no flash needed (repo).** The patched `fiio_init.sh` starts `/usr/data/mq_player`, both at boot and in
the watchdog respawn (diskos_installer/imagebuild.py:439, :460, :480). Today that is a symlink to our `mq_ui`, which
prepares the launch and then execs `/usr/bin/mq_player` (ui/main.c:3169-3175, :3498-3520). A replacement player only
has to be exec'd there instead, with `argv[0]` = `mq_player`, so the stock watchdog's `pgrep -x mq_player` still
matches. The fail-closed fallback (stock player when ours is missing or fails) can stay in the same launcher. A
rootfs change is only needed if we ever want the stock watchdog script itself changed.

**Watchdog (binary).** `watchdog_control.c` runs `cmd_watchdog start 10000` and then `cmd_watchdog feed` in a loop
(`loop_watchdog`). That fits the known "killing mq_player reboots the Disc after about 10 s" (tools/diskos-deploy.sh:33).
A replacement must feed it the same way; the ioctls behind it are now known (see open question 1).

**DAC interface is small (binary).** All ioctls in `dac_control.c` (0x4e7000-0x4e9800) use type `'M'` (0x4d):

- 6 without argument: nr 40, 43, 45, 46, 47, 48
- 13 with a 1-byte argument (`_IOW('M', nr, u8)`): nr 41, 45, 47, 50, 52, 54, 56, 60, 62, 64, 66, 68, 70

They go to the four nodes `/dev/cs43131`, `cs43131b`, `cs43131c`, `cs43131d`. The log strings name the operations
(init per channel half, filter, sample format `bits_per_sample/sample_rate/is_dsd`, power up/down, and "power down
deferred (CS43131 I2C config in progress)"). What each number does, the order, and the timing still have to be
traced, but this is days of work, not weeks. Because the binary is dynamically linked against glibc, an
`LD_PRELOAD` logger can record every `ioctl` with its argument while the stock player runs normally (no `strace`,
which is known to trip the reboot). diskOS already builds glibc binaries against the Disc's libraries
(build/bluealsa-aptx).

**Other ioctl surfaces (binary, device mapping by nearest string reference, so treat as a guess).** Charger
SGM41513 type `'I'` nr 40-44 (log strings `SGM41513_OTG_ON`, `SGM41513_CHARGE_OFF`); USB-C AW35615 type `'N'` nr
38/39 (`SUORCE_SET`, `SINK_SET`); `/dev/usbcc_ioctl` type 0xEF nr 1-8; LCD and touch sleep/wake type 0xEF; spidev
uses the standard `SPI_IOC_*` codes.

**Bundled open source (binary).** shairport-sync, mongoose, zlog, yabmp, uchardet and sacd-ripper's libsacd are
compiled in. FFmpeg 58, ALSA, tinyalsa, BlueZ/D-Bus, Avahi, SQLite, curl and OpenSSL are linked from the rootfs.
`libfiio_decoder.so` and `libroon.so` are FiiO's or Roon's own.

## Can it run next to the stock player?

Only partly. The audio device, the DACs, the power key (`event0` is grabbed by the player, ui/main.c:1961) and the
watchdog can only have one owner. So the audio engine switches as a whole, per boot. What can be taken over while
the stock player keeps running has mostly been done already (library scanner, USB drive, Wi-Fi state, updates).
Stock behaviour that gets in the way can be switched off without a full replacement through the same `LD_PRELOAD`
route, for example by making the stock USB automount's `system("mount …")` calls fail.

## Incremental strategy

0. **Measure, change nothing (1 week).** Build an `LD_PRELOAD` tracer for the stock player that logs `open`,
   `ioctl` (with the argument bytes), `write` to sysfs and every `system()`/`popen()` command. Record: boot, play
   PCM at 44.1/96/192 kHz, DSD64/128 native and DoP, each DAC filter, volume steps, gain, balance, mute, screen
   off/on, power key, charging on/off, USB storage and USB DAC mode, power off. Check on the device what
   `cmd_watchdog` is and whether `/dev/spidev0.0` talks to anything. Result: a documented hardware interface in
   HARDWARE.md and RE_CATALOGUE.md. This step is useful even if we never replace the player.
1. **Backend seam in diskOS (1 week).** Put the ~33 command tags diskOS sends behind one player-backend interface,
   so the UI can talk to either the stock player or ours. The stock path stays the default.
2. **Own player, local playback only (3 to 4 weeks).** A new `diskos-player` daemon in `/usr/data`, started by the
   launcher instead of the stock player when an experimental setting is on, with automatic fallback to stock after
   a failed start or a crash loop (same fail-closed idea as the UI). Scope: FFmpeg decode, ALSA output, DAC
   init/rate/filter/mute, volume, gapless, EQ, resume, watchdog, power key, screen sleep, battery, power off.
   Headphones only.
3. **Parity, one subsystem at a time (2 to 3 months), each behind its own test:**
   Bluetooth source with BlueZ + BlueALSA (diskOS already has most of this) · DSD native/DoP and SACD ISO ·
   balanced output detection · USB storage mode · USB DAC mode (UAC2) · external USB DAC · Bluetooth sink ·
   AirPlay (shairport-sync) · DLNA renderer for the radio (upmpdcli) · charger/PD details · NAS.
4. **Switch the default** once a release has run on the own player for a while. The stock player stays on the
   rootfs as the fallback; no flash is ever needed.

### Engine choice

Two ways to build step 2. Recommendation: decide after step 0.

- **MPD as the audio engine, plus a small own hardware daemon.** MPD brings decoders, gapless, ReplayGain, CUE,
  resampling, stream playback and, with upmpdcli, a DLNA renderer. diskOS already speaks the MPD protocol (its MPD
  server, MPD.md). Costs: the DAC has to be configured when MPD opens a stream (an ALSA hook or ioplug plugin calling
  the ioctls), EQ needs a filter plugin, and MPD has to be cross-built for MIPS.
- **Own small C player on FFmpeg + ALSA.** Full control over DAC timing, DSD switching and memory, and less to build.
  But gapless, seeking, CUE and stream handling are ours to get right, and those are where players usually break.

## Effort

Estimates are focused work days for code written in the cloud; Jo's device tests come on top and are the real
bottleneck, because almost every step touches hardware.

| Step | Days |
|---|---|
| 0 Tracer and hardware documentation | 4-6 |
| 1 Backend seam in diskOS | 4-6 |
| 2 Own player, local playback on headphones | 15-25 |
| DSD native + DoP, SACD ISO | 6-10 |
| Bluetooth source (+ codec handling) | 5-10 |
| Bluetooth sink | 3-5 |
| USB storage / USB DAC (UAC2) / external USB DAC | 1-2 / 5-8 / 2-3 |
| AirPlay / DLNA renderer / NAS | 3-5 / 5-10 / 1-2 |
| Charger, PD, balanced detect, power details | 3-8 |
| **Local playback ready (0-2)** | **about 25-35** |
| **Parity with today's diskOS use** | **about 60-100** |

## Risks

- **Undocumented hardware.** The DAC, charger and USB-C ioctls still have to be traced (step 0). There is no SPI
  MCU on the Disc (checked 2026-11-14), so nothing beyond those drivers is expected.
- **Pops, clicks and DAC damage.** Wrong power or mute order on the DACs can be loud on headphones. Test with low
  volume and without headphones first.
- **Battery and charging.** The stock player sets charger and USB-C PD state. A missing call could stop charging or
  leave OTG power on. Fall back to stock if the battery state looks wrong.
- **Reboot loops.** A player that does not feed the watchdog reboots the Disc every 10 s. The launcher must stop
  trying our player after a failed start; recovery otherwise needs the Vol-Up stock boot (SWITCHING.md).
- **RAM.** 120 MB total, about 64 MB available with stock running. MPD + FFmpeg fit, but the library and art caches
  in diskOS must stay small.
- **Lost features.** Roon, QPlay, FiiO Link, the stock web transfer and the stock OTA would be gone. None of them is
  used by diskOS today.
- **Firmware updates.** The traced interface belongs to the kernel drivers. A new FiiO kernel could change ioctl
  numbers; the player would then need a version check, like the IPC tags today.

## First building block: diskos-watchdog

`src/watchdog/` holds an open-source replacement for `cmd_watchdog` (2026-11-14, not yet device-tested):

- `watchdog.c` / `watchdog.h`: `wd_open`, `wd_start`, `wd_stop`, `wd_feed`, `wd_reset` with the exact request numbers
  of the stock `libhardware2.so`. An own player links this file directly.
- `main.c`: the command line tool `diskos-watchdog` with the stock commands (`start <ms>`, `stop`, `feed`, `reset`,
  `-h`) plus `run <ms> [<every_ms>]`, which arms, feeds until SIGTERM/SIGINT/SIGHUP and then disarms (for headless
  use). It prints nothing on success. Exit status 0 on success and 255 on any failure, like the stock tool; its only
  caller (mq_player, through `popen`/`pclose`, feeding once per second) ignores it anyway. The kernel refuses `start`
  while the watchdog runs (EPERM, dmesg `WDT: watchdog is running !`, device-checked 2026-11-14, same for the stock
  tool); `run` then keeps feeding the running watchdog. `feed` was device-checked with exit 0.
- Build: `CROSS=<toolchain>/bin/mipsel-linux-musl- build/build-watchdog.sh` (static, about 42 KB). Host tests:
  `python3 -m unittest tests.test_watchdog`.

Using it for the stock player needs one more step in the launcher: put a directory holding it as `cmd_watchdog`
first in the stock player's `PATH` (the player calls the tool without a path). Not done yet.

## Open questions for a device session

1. `cmd_watchdog` is `/usr/bin/cmd_watchdog` (5304 bytes): a thin wrapper around `watchdog_start/feed/stop/reset` in
   Ingenic's `libhardware2.so` (both checked 2026-11-14). The library opens `/dev/jz_watchdog` and uses four ioctls:
   start `_IOW('W', 13, int ms)` (0x8004570d), stop `_IO('W', 14)`, feed `_IO('W', 15)`, reset `_IO('W', 16)`. So the
   watchdog needs no further RE. A second `start` while it runs fails with EPERM, so it is not armed before mq_player starts
   it (device-checked 2026-11-14). The library also has an `/dev/mcu` "shutdown" call; that node does not exist on the
   Disc (checked 2026-11-14). Still open: whether `stop` really disarms it.
2. Resolved 2026-11-14: no `/dev/spidev*` and no SPI devices, so no MCU. While idle the stock player holds
   `/dev/key_ioctl`, `/dev/cs43131` and `/dev/cs43131c` open; the other nodes are opened on demand.
3. Which volume path does the Disc use: CS43131 registers, `ICODEC HPOUTL GAIN`, or `i2c-4/4-0034/vol`?
4. Is the physical output balanced, single-ended, or both with detection (`adc_bal_det_handler`)?
