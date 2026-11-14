# Tracing the stock player (step 0)

Step 0 of [MQ_PLAYER_REPLACEMENT.md](MQ_PLAYER_REPLACEMENT.md): record what the stock FiiO player
(`/usr/bin/mq_player`) does to the hardware, so an own player can do the same. Nothing about the player changes; it
only runs with a recording library preloaded. Built 2026-11-14. First device run 2026-11-14 (build 4cefebb7): loads
and records; argument bytes were missing in that build (`in ?`), fixed since, the fix is not device-tested yet.

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
  until the card is mounted (the stock player mounts it some seconds after its start). Every `popen`/`system` first
  writes what is buffered, so everything up to the player's own `umount` of the card is on the card. When the card
  goes away after it was written (power off: the player runs `umount` and then `poweroff -f`; diskOS's "Shut down
  player" unmounts it earlier; USB storage mode), the following lines go to
  `/usr/data/diskos-trace-tail-<YYYYmmdd-HHMMSS>.log` in internal flash instead, at most 64 KiB per start, so the end
  of a power off is kept. In that state every line is written and fsynced at once (`poweroff -f` does not sync), not
  once a second.
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
- `close` of `/dev` files.

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
- Battery and charger: `/dev/sgm41513` `_IO('I',40)` and `/dev/aw35615` are opened and closed several times a second.
- Backlight: `fwrite` of the brightness value ("31", "0") to the pwm-backlight sysfs file.

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
