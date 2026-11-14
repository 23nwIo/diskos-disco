# Tracing the stock player (step 0)

Step 0 of [MQ_PLAYER_REPLACEMENT.md](MQ_PLAYER_REPLACEMENT.md): record what the stock FiiO player
(`/usr/bin/mq_player`) does to the hardware, so an own player can do the same. Nothing about the player changes; it
only runs with a recording library preloaded. Built 2026-11-14, **not device-tested yet**.

## Switch it on and off

Over SSH on the Disc:

```sh
touch /usr/data/diskos-trace-player     # on, takes effect at the next start of the player (reboot)
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

`<card>/diskos-trace/player-trace.log` on the SD card (`/tmp/sdcard/diskos-trace/player-trace.log` on the Disc).
Lines are held in memory until the card is mounted and then appended about once a second, each time with
open/append/close, so no file stays open on the card and USB storage mode can still unmount it. If the card is away
for long enough that the memory buffer (512 KiB) fills, the file says how many lines were lost. The file stops
growing at 256 MiB. Every start appends a new `=== trace start` block.

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
  before (`in`) and after (`out`) the call. The hot ALSA PCM calls (status, delay, sync, read/write frames) are left
  out.
- `write`, `fwrite`, `fputs`, `fputc`, `fprintf`, `vfprintf` to files opened under `/dev`, `/sys` and `/proc`
  (first 48 bytes).
- `system`, `popen`, `execl`, `execv`, `execvp`, `execve` (the shell commands the player runs). Programs started
  this way inherit `LD_PRELOAD` and record too, under their own pid. The once-a-second `cmd_watchdog feed` is
  recorded three times, then not any more.
- `close` of `/dev` files.

The same event more than 4 times in 10 s is counted instead of written: the next one that passes says
`[+N like this in 10 s]`.

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
10. Power off from the menu (the last lines before power off are lost if the player is killed before its next
    write; the log is written about once a second).

Then copy `diskos-trace/player-trace.log` from the card.

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
  small program named mq_player; needs root for a tmpfs as the card) and `make -C ui/tests trace_launch_test`.
