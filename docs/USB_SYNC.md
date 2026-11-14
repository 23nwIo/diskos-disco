# USB drive: import and export music

Copy music between the Disc's SD card and a USB drive: a USB stick, an SD card reader, or a second Disc in mass
storage mode. Nothing is ever deleted on either side.

Status (2026-11-14): implemented, host-tested (unit tests and real rsync runs). On the device so far: host port and drive detection; import/export not yet.

## Requirements

- rsync at `/usr/data/sshd/bin/rsync` (the one shipped with the SSH bundle). Without it the copy fails with
  "rsync not found".
- A USB-C OTG cable or adapter. The Disc acts as USB host.
- The source mode must be local playback or Bluetooth (modes 0 and 6). Switching to another mode while a drive is on
  ejects the drive first.

## Using it

1. **Files** → **USB drive** (first row on the SD card root). Tapping it turns on the host port and looks for drives.
   The row says "N drives connected" once a drive is mounted.
2. Every partition shows as one row, for example "DISC – ext4 · 127 GB · sdb2". Unpartitioned disks show as the whole
   disk. Drives are mounted **read-only** under `/tmp/usbdrive/<dev>`.
3. Browse a drive like the SD card. Files on a drive can't be played: import them first.
4. **Import**: long-press a folder or file on the drive, or long-press the drive itself for **Import everything**. The
   target is the same path on the SD card.
5. **Export**: long-press a folder on the SD card → **Export to USB** (the item shows only while a drive is mounted), or
   long-press a drive → **Export everything**. The drive is remounted read-write for the copy and read-only afterwards,
   after a warning.
6. **Eject** (last row of the drive list) unmounts all drives, syncs, and releases the host port.

### Options

| Option | rsync | Meaning |
|---|---|---|
| Only missing (default) | `--ignore-existing` | Copy files the target doesn't have. Existing files are never touched. |
| Update | (quick check: size and time) | Also replace files that differ. |
| Compare thoroughly | `--checksum` | With Update: compare file contents, not size and time. Slow. |
| Keep backups (default on) | `--backup --backup-dir=` | With Update: every replaced file is moved to `<target root>/_changedN/<path>`; N is the next free number per run. |

Before the copy, a dry run counts new and changed files and their size and compares this with the free space. The copy
shows overall progress and can be cancelled or hidden.

Never copied in either direction: `_changed<N>/`, `synclog/`, `.diskos/`, `.diskos-*`, `lost+found/`. The library
scanner ignores `synclog/` and `_changed<N>/` on the SD card root.

### Logs

- Every run writes a log on the **target**: `<target root>/synclog/YYYY-MM-DD_HHMMSS-import.log` (or `-export`).
- Drive detection and mounting logs to `/usr/data/usbdrive.log` (including the `/proc/mounts` lines for `/dev/sd*`).

## File systems

ext4 is the main target (whole disk or any partition). exFAT, FAT and NTFS are mounted too, with the warning "Windows
file systems not fully supported, use at your own risk": names and sizes the file system can't store end up as rsync
exit 23 ("Some files could not be copied"). Times are compared with a 2 s window, so FAT targets don't look changed on
every run.

The stock player has its own USB disk automount (mq_player V2.57, disassembled): on every uevent
`SUBSYSTEM=scsi_device ACTION=add` (one per LUN; a card reader sends four) it unmounts every mount whose source
contains `/dev/sd`, runs `rm -rf` on that mount point, and mounts each `/dev/sd*` at `/tmp/udisk<N>` with
`iocharset=utf8`, which ext4 rejects. diskOS therefore mounts from its own block node `/tmp/usbdrive/.blk/<dev>`,
which that sweep does not match, and takes a FAT drive back if the player mounted it first.

## Host port

diskOS requests the host port with the player IPC frame `0642000C0005` and releases it with `0642000C0000` (not while
the USB audio output is selected). Device-verified 2026-11-14: `0642 5` alone takes the host port (card reader behind
a hub enumerated as sdb, output route stayed on the headphones).
If no drive appears, check `/usr/data/usbdrive.log` and `dmesg`.

## Not yet

- Network source via an rsync daemon (`rsync://host/module/`).
- Bluetooth OBEX (phone ↔ Disc).
