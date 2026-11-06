#!/usr/bin/env bash
# disc-ssh-setup.sh - one-time setup for convenient SSH on a diskOS Disc, WITHOUT reflashing.
#
# Installs onto the writable /usr/data partition (survives reboots and flashes):
#   - /usr/data/sshd/start-ssh.sh            boot hook (tools/disc-ssh/start-ssh.sh): restores Debug Mode
#                                            after a reboot if it was on, and enables public-key login
#   - /usr/data/sshd/home/.ssh/authorized_keys  your public key (appended once, never duplicated)
#   - /usr/data/sshd/bin/rsync               optional static MIPS rsync (build/build-rsync-static.sh)
# then activates the key mount immediately. See docs/SSH_ACCESS.md.
#
# Usage (Debug Mode ON; you are asked for the Debug Mode password once):
#   DISKOS_IP=<device-ip> tools/disc-ssh-setup.sh [--key ~/.ssh/id_ed25519.pub] [--rsync path/to/rsync]
#   DISKOS_IP=<device-ip> tools/disc-ssh-setup.sh --uninstall
#
# Requires: OpenSSH client (ssh), md5sum (or md5 on macOS). The device has no scp/sftp; files are streamed
# over `ssh 'cat > file'` and verified by md5 before they are moved into place.
set -eu

IP="${DISKOS_IP:?set DISKOS_IP to the device IP (shown in Debug Mode)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
HOOK="$HERE/disc-ssh/start-ssh.sh"
KEY=""; RSYNC=""; UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --key)       KEY="${2:?--key needs a .pub file}"; shift 2 ;;
        --rsync)     RSYNC="${2:?--rsync needs a file}"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help)   sed -n 2,17p "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if [ -z "$KEY" ] && [ "$UNINSTALL" = 0 ]; then
    for k in ~/.ssh/id_ed25519.pub ~/.ssh/id_ecdsa.pub ~/.ssh/id_rsa.pub; do [ -f "$k" ] && { KEY="$k"; break; }; done
    [ -n "$KEY" ] || { echo "no public key found; pass --key <file.pub>" >&2; exit 1; }
fi

md5() { if command -v md5sum >/dev/null; then md5sum "$1" | cut -d' ' -f1; else md5 -q "$1"; fi; }

# One connection, one password prompt: OpenSSH connection sharing (client-side, works with dropbear).
CTL="$(mktemp -u "${TMPDIR:-/tmp}/disc-ssh.XXXXXX")"
SSH=(ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 -o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=60)
cleanup() { ssh -o "ControlPath=$CTL" -O exit "root@$IP" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# push <local> <remote> <mode>: stream to <remote>.new, verify md5, then rename into place.
push() {
    local want got
    want="$(md5 "$1")"
    got="$("${SSH[@]}" "root@$IP" "mkdir -p \"\$(dirname '$2')\" && cat > '$2.new' && md5sum '$2.new' | cut -d' ' -f1" < "$1")"
    [ "$got" = "$want" ] || { "${SSH[@]}" "root@$IP" "rm -f '$2.new'"; echo "md5 mismatch for $2 (device=$got) - aborted" >&2; exit 1; }
    "${SSH[@]}" "root@$IP" "chmod $3 '$2.new' && mv -f '$2.new' '$2'"
    echo ">> $2 ok ($want)"
}

if [ "$UNINSTALL" = 1 ]; then
    # Removes the boot hook and the key mount. Keys and rsync stay on /usr/data (delete by hand if wanted).
    "${SSH[@]}" "root@$IP" 'rm -f /usr/data/sshd/start-ssh.sh;
        H=$(awk -F: '"'"'$1=="root"{print $6; exit}'"'"' /etc/passwd); H=${H%/};
        for t in "$H/.ssh" "$H"; do awk -v d="$t" '"'"'$2==d{f=1} END{exit f?0:1}'"'"' /proc/mounts && umount "$t"; done; echo ">> hook removed"'
    exit 0
fi

[ -f "$HOOK" ] || { echo "missing $HOOK" >&2; exit 1; }
"${SSH[@]}" "root@$IP" true   # opens the shared connection (password prompt)

push "$HOOK" /usr/data/sshd/start-ssh.sh 755

# Append the key only if that exact key line is not there yet.
"${SSH[@]}" "root@$IP" 'mkdir -p /usr/data/sshd/home/.ssh && chmod 755 /usr/data/sshd/home && chmod 700 /usr/data/sshd/home/.ssh &&
    f=/usr/data/sshd/home/.ssh/authorized_keys; touch "$f" && chmod 600 "$f";
    read -r k; grep -qxF "$k" "$f" && echo ">> key already present" || { echo "$k" >> "$f"; echo ">> key added"; }' < "$KEY"

if [ -n "$RSYNC" ]; then
    push "$RSYNC" /usr/data/sshd/bin/rsync 755
    "${SSH[@]}" "root@$IP" '/usr/data/sshd/bin/rsync --version | head -1'
fi

"${SSH[@]}" "root@$IP" '/usr/data/sshd/start-ssh.sh keys; /usr/data/sshd/start-ssh.sh status; tail -3 /usr/data/sshd/start-ssh.log'
cleanup
echo ">> testing key login (no password should be asked):"
ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=8 "root@$IP" 'echo "   key login OK"' \
    || echo "   key login FAILED - see docs/SSH_ACCESS.md (Troubleshooting)"
