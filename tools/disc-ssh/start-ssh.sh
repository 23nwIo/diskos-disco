#!/bin/sh
# start-ssh.sh - optional boot hook for Debug Mode SSH. Lives at /usr/data/sshd/start-ssh.sh (writable
# partition, survives reboots and flashes); no reflash is needed. mq_ui runs it in the background at every
# start if it is executable (ui/main.c: "[ -x /usr/data/sshd/start-ssh.sh ] && ... &").
# Installed by tools/disc-ssh-setup.sh; see docs/SSH_ACCESS.md.
#
# What it does:
#   1. Public-key login: bind-mounts /usr/data/sshd/home over root's home directory so dropbear finds
#      /usr/data/sshd/home/.ssh/authorized_keys. (The bundled dropbear 2022.83 only reads
#      <root home>/.ssh/authorized_keys, and root's home is on the read-only rootfs.)
#   2. Reboot-proof Debug Mode: if Debug Mode was ON when the device went down, turn it back on with the
#      SAME password. "ON" is taken from mq_ui's own state: /usr/data/sshd/current_pw exists while Debug
#      Mode is on and is deleted when it is turned off (ui/debug_ui.c). The matching sha512 hash is the
#      root line of the private shadow that diskos-debug.sh wrote on the last enable
#      (/usr/data/sshd/shadow). SSH is started through the normal fail-closed path
#      (/usr/project/diskos-debug.sh ssh-on), so turning Debug Mode OFF in the UI still stops dropbear
#      and drops the overlay exactly as before - and then nothing comes back on the next boot.
#
# Usage:  start-ssh.sh          boot mode: keys + restore Debug Mode if it was on
#         start-ssh.sh keys     only (re)do the key mount
#         start-ssh.sh status   print what is mounted / running
#
# Never prints secrets. Every step is best-effort and bounded; a failure leaves SSH OFF, never on the
# stock /etc/shadow (diskos-debug.sh refuses to start dropbear without its own overlay).

SELF=/usr/data/sshd
DBG=/usr/project/diskos-debug.sh
KHOME="$SELF/home"
LOG="$SELF/start-ssh.log"

log() { echo "$(date 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

root_home() { awk -F: '$1=="root"{print $6; exit}' /etc/passwd 2>/dev/null; }

# Is <dir> already a mount point of ours? (exact match on the mount-point field)
mounted_at() { awk -v d="$1" '$2==d{f=1} END{exit f?0:1}' /proc/mounts 2>/dev/null; }

keys_mount() {
    [ -s "$KHOME/.ssh/authorized_keys" ] || { log "keys: no $KHOME/.ssh/authorized_keys, skipped"; return 0; }
    # dropbear rejects keys if home, .ssh or the file are group/other-writable or not root-owned.
    chown root:root "$KHOME" "$KHOME/.ssh" "$KHOME/.ssh/authorized_keys" 2>/dev/null
    chmod 755 "$KHOME" 2>/dev/null; chmod 700 "$KHOME/.ssh" 2>/dev/null
    chmod 600 "$KHOME/.ssh/authorized_keys" 2>/dev/null
    H=$(root_home)
    case "$H" in /?*) ;; *) log "keys: root home '$H' unusable, skipped"; return 1 ;; esac
    H=${H%/}
    # Already done (either variant)? Check BEFORE picking a target: once the home is bound, <home>/.ssh
    # exists (it is ours), and binding again would stack a second mount on top.
    if mounted_at "$H" || mounted_at "$H/.ssh"; then log "keys: already mounted"; return 0; fi
    # Prefer the narrowest target: an existing <home>/.ssh; otherwise the home dir itself.
    if [ -d "$H/.ssh" ]; then T="$H/.ssh"; S="$KHOME/.ssh"; else T="$H"; S="$KHOME"; fi
    [ -d "$T" ] || { log "keys: $T does not exist, skipped"; return 1; }
    mount -o bind "$S" "$T" 2>/dev/null || mount --bind "$S" "$T" 2>/dev/null
    if mounted_at "$T"; then log "keys: bound $S over $T"; return 0; fi
    log "keys: bind $S -> $T FAILED"; return 1
}

restore_debug() {
    # Not on before the reboot -> nothing to do (the normal, default case).
    [ -s "$SELF/current_pw" ] || { log "debug: was off"; return 0; }
    # Already running (e.g. mq_ui was restarted, not the device): never kill live sessions.
    if pgrep -f "dropbearmulti dropbear" >/dev/null 2>&1; then log "debug: dropbear already running"; return 0; fi
    if grep -q ' /etc/shadow ' /proc/mounts 2>/dev/null; then log "debug: overlay already present"; return 0; fi
    [ -x "$DBG" ] || { log "debug: no $DBG"; return 1; }
    hash=$(awk -F: '$1=="root"{print $2; exit}' "$SELF/shadow" 2>/dev/null)
    case "$hash" in
        '$6$'*'$'*) ;;
        *) log "debug: no usable hash in $SELF/shadow - leaving Debug Mode OFF"; return 1 ;;
    esac
    r=$("$DBG" ssh-on "$hash" 2>&1)
    log "debug: ssh-on -> $r"
    "$DBG" serial-on >/dev/null 2>&1   # same as the UI's enable; a no-op without /dev/ttyGS0
    return 0
}

status() {
    H=$(root_home)
    echo "root home: ${H:-?}"
    awk -v h="${H%/}" '$2==h || $2==h"/.ssh" || $2=="/etc/shadow" {print "mount: " $2}' /proc/mounts 2>/dev/null
    if pgrep -f "dropbearmulti dropbear" >/dev/null 2>&1; then echo "dropbear: running"; else echo "dropbear: stopped"; fi
    [ -s "$SELF/current_pw" ] && echo "Debug Mode: on (restored on boot)" || echo "Debug Mode: off"
}

mkdir -p "$SELF" 2>/dev/null
case "$1" in
    keys)   keys_mount ;;
    status) status ;;
    ""|boot)
        log "boot hook start"
        keys_mount
        restore_debug ;;
    *) echo "usage: $0 [boot|keys|status]" >&2; exit 2 ;;
esac
