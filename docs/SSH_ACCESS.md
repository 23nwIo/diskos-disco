# SSH access: keys, reboot-proof Debug Mode, rsync

Optional, for developers. Nothing here changes the flashed image: everything lives on the writable
`/usr/data` partition and is installed once over Debug Mode SSH. **Not yet tested on a Disc.**

## What you get

- **Public-key login** for Debug Mode SSH (the random password keeps working too).
- **Reboot-proof Debug Mode:** if Debug Mode was on when the device restarted, it comes back on by itself
  with the same password. Turning Debug Mode **off** in Settings > System > Debug Mode stops the SSH
  server as before, and it then stays off after the next reboot.
- **rsync** over SSH (incremental copies, resumable), optionally.

The Disc's SSH server is Dropbear 2022.83 (`payload/dropbearmulti`): it has no `scp` and no SFTP server,
so `scp`, `sftp` and SFTP apps (Cyberduck, FileZilla, WinSCP) do not work. Use `rsync` or
`ssh root@IP 'cat > /path/file' < file`.

## Setup (once)

1. On the Disc: Settings > System > Debug Mode > Enable. Note the IP and password.
2. Optional, for rsync: `build/build-rsync-static.sh`. It uses the project toolchain (the one `mq_ui` is
   built with, `ui/Dockerfile`) from `/opt/mipsel-n2008-musl-cross` or `CROSS=<prefix>`, and falls back
   to `gcc-mipsel-linux-gnu`. Output: `build/out/rsync-mipsel` (static, about 0.8 MB with musl) and the
   source tarball. It needs `git`, `make`, `awk`, `autoconf` and `automake` (for `aclocal`; a git checkout of rsync has no
   generated `configure.sh`) and network access to github.com.
3. On your computer:

   ```
   DISKOS_IP=<ip> tools/disc-ssh-setup.sh [--key ~/.ssh/id_ed25519.pub] [--rsync build/out/rsync-mipsel]
   ```

   You are asked for the Debug Mode password once. The script ends with a key-login test.

Files on the device:

| Path | Purpose |
|---|---|
| `/usr/data/sshd/start-ssh.sh` | boot hook ([`tools/disc-ssh/start-ssh.sh`](../tools/disc-ssh/start-ssh.sh)); `mq_ui` runs it at every start if it is executable (`ui/main.c`, search `start-ssh.sh`) |
| `/usr/data/sshd/home/.ssh/authorized_keys` | your public keys (one per line) |
| `/usr/data/sshd/bin/rsync` | static MIPS rsync (optional) |
| `/usr/data/sshd/start-ssh.log` | what the hook did on each start |

## Use

```
ssh root@<ip>
rsync -rtv --progress --rsync-path=/usr/data/sshd/bin/rsync Music/ root@<ip>:/tmp/sdcard/Music/
```

The card is mounted at `/tmp/sdcard` (`ui/fileops.c:31`). Use `-rt` rather than `-a`: an exFAT card stores no owners or permissions (an ext4 card does, but the Disc runs everything as root). Dropbear's sessions use
`PATH=/usr/bin:/bin`, which is on the read-only rootfs, so `--rsync-path` is needed. A shell alias helps:
`alias drsync='rsync -rt --rsync-path=/usr/data/sshd/bin/rsync'`.

Avoid writing to the card while it is exported over USB mass storage.

## How it works

- **Keys:** Dropbear reads only `<root's home>/.ssh/authorized_keys`, and root's home is on the read-only
  rootfs. The hook bind-mounts `/usr/data/sshd/home/.ssh` over `<home>/.ssh` if that exists, otherwise
  `/usr/data/sshd/home` over the home directory. If root's home is `/`, keys are skipped (see the log).
- **Restore:** `mq_ui` keeps `/usr/data/sshd/current_pw` while Debug Mode is on and deletes it when you
  turn it off (`ui/debug_ui.c`). If it exists at boot, the hook takes the sha512 hash from the private
  shadow `diskos-debug.sh` wrote on the last enable (`/usr/data/sshd/shadow`) and calls the normal
  `/usr/project/diskos-debug.sh ssh-on <hash>`, which is fail-closed: no valid hash or no overlay means
  no SSH. If dropbear is already running (an `mq_ui` restart, not a reboot) the hook does nothing, so
  live sessions are not dropped.

Status on the device: `/usr/data/sshd/start-ssh.sh status`.

## Security

Debug Mode gives root over Wi-Fi. With the hook installed it no longer switches itself off on reboot:
turn it off when you do not need it. Anyone with a listed key, or the password, has root while it is
on. Dropbear 2022.83 predates the Terrapin (CVE-2023-48795) mitigation. See [SECURITY.md](../SECURITY.md).

## Troubleshooting and removal

- Key refused: check `/usr/data/sshd/start-ssh.log` and `start-ssh.sh status` (is the mount there?).
  Dropbear refuses keys if the home, `.ssh` or `authorized_keys` are group/other-writable.
- Remove: `DISKOS_IP=<ip> tools/disc-ssh-setup.sh --uninstall` (deletes the hook and the key mount;
  keys and rsync stay until you delete `/usr/data/sshd/home` and `/usr/data/sshd/bin`).

rsync is GPL-3.0-or-later. `build/build-rsync-static.sh` also writes the matching source tarball; pass it
along with the binary if you give the binary to anyone.
