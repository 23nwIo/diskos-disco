# Tracing the stock player (step 0)

Step 0 of [MQ_PLAYER_REPLACEMENT.md](MQ_PLAYER_REPLACEMENT.md): record what the stock FiiO player
(`/usr/bin/mq_player`) does to the hardware, so an own player can do the same. Nothing about the player changes; it
only runs with a recording library preloaded. Built 2026-11-14. First device run 2026-11-14 (build 4cefebb7): loads
and records; argument bytes were missing in that build (`in ?`), fixed since and device-checked in the second and third runs.

## Switch it on and off

Over SSH on the Disc:

```sh
touch /usr/data/diskos-trace-player     # on, log on the SD card; takes effect at the next start of the player
echo tmp > /usr/data/diskos-trace-player   # or: on, log in /tmp (RAM, gone after a reboot)
reboot
rm /usr/data/diskos-trace-player        # off again
reboot
```

There is nothing in the UI. On every start with the switch file the launcher (inside mq_ui, `/usr/data/mq_player`)
writes the library carried in mq_ui to `/usr/data/diskos-trace.so` and starts the stock player with
`LD_PRELOAD=/usr/data/diskos-trace.so` (ui/trace_launch.h, called from `player_launch_prepare()` in ui/main.c). One
line per start goes to `/usr/data/diskos_boot.log`:

```sh
grep "player trace" /usr/data/diskos_boot.log | tail -3
```

`launcher: player trace on` is the normal case. If a traced start never got as far as loading the library (the glibc
loader refused it, or the player died before), the next start renames the switch file to
`diskos-trace-player.failed`, logs `previous traced start did not load the library - switched off` and starts the
player untraced. The library proves it loaded by writing `/usr/data/diskos-trace.ok`. It also needs the device's
`libdl.so.2` in `/lib` or `/usr/lib` and is not started without it.

A player that the firmware watchdog restarts directly (not through our launcher) runs untraced.

## The log

One file per start of the player, named by its start time on the Disc's clock (the launcher passes the name in
`DISKOS_TRACE_FILE`; helper programs inherit it and write into the same file):

- default: `/tmp/sdcard/diskos-trace/player-trace-<YYYYmmdd-HHMMSS>.log` on the SD card. Lines are held in memory
  while the card is not mounted (before the first mount, during the stock player's own unmount and remount at start,
  in USB storage mode) and written once it is back. Every `popen`/`system`/exec first writes what is buffered. Only
  when the end is near (a `poweroff`/`reboot`/`halt` command, or the process exits) and the card is gone after it had
  been written (diskOS's "Shut down player" unmounts it before the player powers off) do the held lines go to
  `/usr/data/diskos-trace-tail-<YYYYmmdd-HHMMSS>.log` in internal flash, at most 64 KiB per start, each line then
  written and fsynced at once (`poweroff -f` does not sync). In the stock idle power off (device run 2026-11-14) the
  card stays mounted to the end and no tail file is needed.
- switch file starting with `tmp`: `/tmp/diskos-trace/player-trace-<YYYYmmdd-HHMMSS>.log`, meant to be RAM (not yet
  checked on the Disc that `/tmp` is a tmpfs: `grep " /tmp " /proc/mounts`); nothing is written to flash or the card,
  and a reboot clears it, so copy the file first.

Files of earlier starts can be deleted at any time (on the card with `busybox rm`); the running start only appends to
its own file. Lines are appended about once a second with open/append/close, so no file stays open and USB storage
mode can still unmount the card. The file stops growing at 16 MiB. If lines cannot be written for a while, they wait in
a 512 KiB memory buffer, and the file says how many were lost. Every file starts with a `=== trace start` block.

Each line: seconds since boot, pid (`pid/thread` for other threads), the caller's return address, the event (the
values below show the format only, they are not from a device):

```
12.345 812 0x0 === trace start "mq_player" ppid 1 realtime 1794695236
12.345 812 0x0 map 00400000-00a5c000 r-xp 00000000 1f:02 123 /usr/bin/mq_player
14.002 812/830 0x4e7a1c open "/dev/cs43131" flags 0x2 -> 7
14.010 812/830 0x4e7b40 ioctl 7 "/dev/cs43131" 0x80014d29 _IOW('M',41,sz1) in 01 -> 0
15.100 812 0x4d1230 popen "cmd_watchdog start 10000" "r"
16.200 812 0x4d2000 fopen "/sys/class/power_supply/cw221X-bat/capacity" "r" (first read) -> 0
```

What is recorded:

- `open`, `open64`, `openat`, `openat64`, `fopen`, `fopen64` of paths under `/dev`, `/sys` and `/proc`. Read-only
  opens of `/sys` and `/proc` only the first time per path (they are polled); everything else every time.
- Every `ioctl`, with the request decoded (`_IO/_IOR/_IOW/_IOWR`, type, number, size) and up to 64 argument bytes
  before (`in`) and after (`out`) the call. FiiO drivers also pass structs with plain `_IO` requests (`/dev/gpio`
  `'G'` 122): when such an argument looks like a pointer, 16 bytes behind it are shown (`mem`, and `mem-after` when
  the call changed them). The bytes are copied through a short-lived pipe, so a bad pointer gives `?` instead of a
  crash. The hot ALSA PCM calls (status, delay, sync, read/write frames) are left out.
- `write`, `fwrite`, `fputs`, `fputc`, `fprintf`, `vfprintf` to files opened under `/dev`, `/sys` and `/proc`
  (first 48 bytes).
- `system`, `popen`, `execl`, `execv`, `execvp`, `execve` (the shell commands the player runs). Programs started
  this way (helpers) inherit `LD_PRELOAD` and record too, under their own pid, but less: no `/proc`, no `ioctl` on
  sockets, no exec of their own (the player's line already has the command), and their `=== trace start` line only
  once they record something. The once-a-second `cmd_watchdog feed` is recorded three times, then not any more.
- Never recorded: `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`, `/dev/tty`, `/dev/console`, `/dev/pts/*`.
- `close` of `/dev` files; `sync`. `dup2`/`dup3` carry a file's path to the new descriptor, so the shell's
  `echo 0 > /sys/...` in a helper shows the written value.

The same event more than 4 times in 10 s is counted instead of written: the next one that passes says
`[+N like this in 10 s]`. `ioctl` calls on `/dev/cs43131*` (the DACs) are always written.

## First findings (device run 2026-11-14, build 4cefebb7)

Not yet with argument bytes, so values are still open. Times are seconds since boot in that run.

- Only `/dev/cs43131` and `/dev/cs43131c` are opened; `/dev/cs43131b` and `/dev/cs43131d` are not.
- Polled about once a second: `_IO('M',40)` on cs43131 returns 11, `_IO('M',43)` on cs43131c returns 0.
- Headphone jack: thread polls `/dev/gpio` `_IO('G',122)` with a pointer argument (0x6e16a0) about three times a
  second; the return value changes between 0 and 1 when the plug goes in or out. After a change the player issues
  `_IOW('M',45)` + `_IOW('M',66)` on cs43131 and `_IOW('M',47)` + `_IOW('M',68)` on cs43131c (one byte each).
- Volume keys: another thread reads `/dev/gpio` `'G'` 122 with two arguments (0x6db930, 0x6db938, probably the two
  keys) and then sends the same four ioctls (45/66, 47/68). So these four carry the volume; the values are needed.
- Start of playback: `_IOW('M',56)` + `_IOW('M',60)` (twice), `_IOW('M',52)`, `_IOW('M',54)`, `_IOW('M',41)`,
  `_IOW('M',50)`, the volume group, `_IO('M',46)`, then ALSA `/dev/snd/pcmC0D3p` (hw_params, sw_params, start).
  The second `_IOW('M',56)` returns an odd value with errno 2 (both runs).
- At start: `_IOW('M',62)` / `_IOW('M',64)` and `_IOW('M',70)`; ALSA controls on `/dev/snd/controlC0`.

Second run (2026-11-14/15, build 19ca546, with argument bytes; idle, then the player's own idle power off):

- Volume group at start: `_IOW('M',45)`, `('M',66)` on cs43131 and `('M',47)`, `('M',68)` on cs43131c, all with
  0x72 (114).
- Start sequence values: `M,62`=0, `M,64`=0, `M,70`=1, `M,56`=0 and 3, `M,60`=0 and 3 (one call each), `M,52`=2, `M,54`=2,
  `M,41`=0, `M,50`=0.
- `/dev/gpio` `'G'` 122: the argument points to a 16-byte buffer that starts with text (`pb20`, `po_add:%`), so the
  pin is probably named by a string (`pb20`, port B pin 20, for the jack; unverified); the return value is the level.
- Charger: `/dev/sgm41513` `_IO('I',40)` fills a text buffer (`98\n`, `97\n`), apparently the battery percent.
- Stock idle power off (thread 1441, about 5 min after start): Bluetooth off (dbus Powered false, `hciconfig hci0
  down`, kill BT daemons), `ifconfig wlan0 down`, kill `udhcpc`/`wpa_supplicant`/`curl`/`wget`, backlight 0,
  `/dev/lcd_st77916` `_IO(0xef,1)` (display off), **`killall -9 mq_ui`**, rfkill 0, kill `bluetoothd`/`bluealsa`/
  `brcm_patchram_plus`, `poweroff -f`. No DAC ioctls, no unmount of the card. The `killall -9 mq_ui` explains a killed
  diskOS UI before power off.
- Battery and charger: `/dev/sgm41513` `_IO('I',40)` and `/dev/aw35615` are opened and closed several times a second.
- Backlight: `fwrite` of the brightness value ("31", "0") to the pwm-backlight sysfs file.

Third run (2026-11-15, build dd5de8e, the test programme with time marks; log `player-trace-20261115-010953.log`).
`A` = `/dev/cs43131`, `C` = `/dev/cs43131c`. Marks were written by hand right after each step, so a value belongs to
the mark that follows it.

- **Volume** (`A` `M45`+`M66`, `C` `M47`+`M68`, one byte, the same value in both requests of a chip) is an
  attenuation: smaller = louder. One key press = 2 units (0x66 → 0x64 → 0x62 → 0x60 louder). If a unit is the CS43131's
  0.5 dB step, a key press is 1 dB (unverified). Playback started at 0x66.
- **Balance** changes only one chip, 1 unit per balance step: balance left 1..10 raises `A` from 0x66 to 0x70, balance
  right 1..10 raises `C` from 0x66 to 0x70. So `A` drives the right channel and `C` the left one.
- **Gain** is not a hardware switch: "low" adds 12 units (6 dB if 0.5 dB steps) to both chips' volume values
  (0x68/0x66 → 0x74/0x72), "high" takes them off again. No other call.
- **Sample rate**: `A` `M52` + `C` `M54` before every stream: 44.1 kHz = 1, 48 kHz = 2, 96 kHz = 4, 192 kHz = 6
  (88.2/176.4 kHz probably 3/5, not tested). Then `M41`/`M50` = 0, the volume group, then ALSA `pcmC0D3p`
  (hw_params, sw_params, start). sw_params: start threshold 2 periods; period 3528 frames at 44.1 kHz, 960 at 48 kHz,
  1920 at 96 kHz, 3840 at 192 kHz.
- First stream after boot only: `M56`/`M60` = 3, then `M56`/`M60` = 0 (the `A` call returns an odd value with errno 2,
  as in run 1), then `_IO('M',46)` on `A`. The same block runs when USB storage mode starts and ends. Before the very first
  stream `_IO('M',48)` on `A`; the menu power off sends it too.
- **DAC filter** (`A` `M62` + `C` `M64`, same value): fast roll-off II = 0x00 (also the boot value), slow II = 0x80,
  slow PC = 0xc0, fast PC = 0x40, NOS = 0x20, wideband = 0x02. The first change also restarted the stream.
- **DRE** (`A` `M70` only): 0 = on, 1 = off (checked 2026-11-15 with a mark right after each toggle); the player
  writes the current setting at start.
- **Headphone jack**: `/dev/gpio` `'G'` 122 (`pb20`) returns 1 with the plug in, 0 without. After plugging in again only
  the volume group was written (no stream restart); Jo heard no sound after the re-insert.
- **Power key**: screen off = backlight "0", `/dev/cst816t` `_IO(0xef,3)` (touch off), `/dev/lcd_st77916`
  `_IO(0xef,1)`; screen on = lcd `_IO(0xef,2)`, backlight "31", touch `_IO(0xef,4)`. The player does this itself.
- **USB storage mode**: stop the stream, mixer write (control `LO3_MUX`), the DAC block above, `sync`, `umount
  /tmp/sdcard`, `rm -rf /tmp/sdcard`, `echo device > .../13500000.otg_new-role-switch/role`, configfs gadget
  `storage_demo` (idVendor 0x0525, idProduct 0xa4a5, mass_storage lun.0 file `/dev/mmcblk0`, removable 1, UDC
  `13500000.otg_new`). Leaving it: the DAC block, empty UDC, `umount /sys/kernel/config`, `mkdir -p /tmp/sdcard`,
  `blkid`.
- **Charger in/out**: no calls besides the reads of `/dev/aw35615` and `/dev/sgm41513` (reads are not recorded).
- **Power off from the menu** (with charger): Bluetooth off, `ifconfig wlan0 down`, kill net tools, `_IO('M',48)` on
  `A`, `cmd_watchdog stop`, **`killall -9 fiio_init.sh`**, backlight 0, lcd `_IO(0xef,1)`, `killall -9 mq_ui`, rfkill 0,
  kill BT daemons, `poweroff -f`. The card stayed mounted; the log on the card reaches `poweroff -f`.
- Not yet explained: `ioctl` with request 0x467f (`_IO('F',127)`, pointer argument) on a descriptor the library did not
  see being opened (`?`), at every stream start and stop.
- At boot, the player itself switches Bluetooth on (`/usr/project/bluetoothd`, bluealsa, bt-agent, D-Bus calls). In the
  boot after this run it crashed right after (SIGSEGV in libdbus at 26.7 s), and fiio_init's loop then killed the diskOS
  UI and started the stock UI. Fix attempt 54359e6 (not device-tested): diskOS leaves the boot Bluetooth bring-up to the player.

The caller address points into the code mapping listed in the `map` lines of the same start block. For
`/usr/bin/mq_player` it is the plain address in the binary (not position independent), so it can be looked up in a
disassembly directly.

## Test programme

Run each step with a short pause (5 s) in between and write down the time of each step (`cat /proc/uptime` over
SSH gives the seconds the log uses), so the blocks in the log can be told apart.

1. Boot with the switch file, wait until the player is idle (1 minute).
2. Play PCM 44.1 kHz, then 96 kHz, then 192 kHz (headphones on the 3.5 mm jack).
3. Play DSD64 and DSD128 (native and DoP if the setting exists).
4. Each DAC filter once.
5. Volume: five steps up and down with the keys; gain switch; balance; mute if available.
6. Screen off and on; power key short press.
7. Charger in and out.
8. USB storage mode on and off; USB DAC mode on and off.
9. Bluetooth output on, play, off.
10. Power off from the menu (with the log on the card). Afterwards copy the log from the card and
    `/usr/data/diskos-trace-tail-*.log` (the part after the card was unmounted). What can still be missing: lines
    from up to one second between the `umount` and the player's next `popen`/`system` if the power is cut in that
    second, and calls a thread is in the middle of when the power goes.

## Limits

- Calls glibc makes internally (for example `fopen` opening its file inside glibc) are seen at the outer call only;
  direct `syscall()` use is not recorded.
- Reads are not recorded except the first open of a polled file.
- Writing the log (about once a second) happens on whichever player thread made the hooked call at that moment; it
  is short (one append), but on a slow card that thread waits for it.
- A forked child that exits with `_exit` without exec loses its unwritten lines.

## Files

- `ui/trace/trace.c` (MIT): the library. Built by `ui/Makefile` with the static musl toolchain as a self-contained
  shared object: no musl code is linked in, it only asks the device's glibc for `dlsym` and `__errno_location`
  (linked against the stand-ins in `ui/trace/stub.c` to name them), and uses raw system calls for everything it does
  itself. Same MIPS ABI flags as the stock player (o32, nan2008, FP64).
- `ui/trace_blob.c`: carries the library inside mq_ui. `ui/trace_launch.h`: the switch, the fail-safe and the
  environment for the exec.
- Tests: `python3 -m unittest tests.test_trace` (builds the library for the host glibc and preloads it into a
  small program named mq_player) and `make -C ui/tests trace_launch_test`.
