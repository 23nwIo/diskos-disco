#!/bin/sh
# diskos-update - install a signed SD update from an SSH shell on the Disc.
#
# Does exactly what Settings > System > Maintenance > Update from SD Card does (ui/ota.c ota_stage_dir):
# copy the six files of <SD>/diskos-update into /usr/data/updates/incoming, check them with the read-only
# verifier (/etc/diskos-ota/diskos-verify.sh, floor = max(MIN_UI_EPOCH, accepted + 1)), write `ready` and
# rename the folder to pending/. Nothing is installed here: the next boot verifies pending/ again
# (payload/S97diskos_install) and runs it as a trial with automatic rollback, as after a menu update.
#
# Shows the running and the new version, asks before staging and again before restarting.
#
# Installed by tools/disc-ssh-setup.sh as /usr/data/sshd/bin/diskos-update. Run it with a terminal:
#   ssh -t root@<ip> /usr/data/sshd/bin/diskos-update [folder]
# folder defaults to /tmp/sdcard/diskos-update (the card's top level).
# Exit: 0 staged (and restarted, or restart declined), 1 error/rejected, 2 cancelled.

SRC=${1:-/tmp/sdcard/diskos-update}
RO=${DISKOS_OTA_RO:-/etc/diskos-ota}          # overridable for host tests only
U=${DISKOS_UPDATES:-/usr/data/updates}
RUN=${DISKOS_RUN:-/tmp/.diskos_run}
FILES="keyauthz keyauthz.sig leafpub.pem update.manifest update.manifest.sig mq_ui"

kv() { grep "^$2=" "$1" | head -n 1 | cut -d= -f2; }
die() { echo "ERROR: $*"; exit 1; }
ask() {   # ask <question>: 0 only for y/yes
    printf '%s [y/N] ' "$1"
    read -r _a || { echo; return 1; }
    case "$_a" in y|Y|yes|YES|Yes) return 0;; esac
    return 1
}

[ -f "$RO/root.pub.pem" ] && [ -f "$RO/epochs" ] && [ -f "$RO/diskos-verify.sh" ] \
    || die "no update key on this Disc ($RO); reflash once with --ota-key"
[ -d "$SRC" ] || die "update folder not found: $SRC"
for f in $FILES; do
    [ -f "$SRC/$f" ] || die "missing $SRC/$f (all six files of one signing run are needed)"
done

MINKEY=$(kv "$RO/epochs" MIN_KEY_EPOCH)
FL=$(kv "$RO/epochs" MIN_UI_EPOCH)
ACC=
if [ -f "$U/accepted" ]; then
    ACC=$(kv "$U/accepted" epoch)
    [ -n "$ACC" ] && [ $((ACC + 1)) -gt "$FL" ] && FL=$((ACC + 1))
fi

# --- running build ---------------------------------------------------------------------------------
echo "Running now:"
if [ -f "$RUN" ]; then
    echo "  version $(kv "$RUN" VERSION) ($(kv "$RUN" BUILD)), epoch $(kv "$RUN" EPOCH), sha256 $(kv "$RUN" SHA256 | cut -c1-16)..."
else
    echo "  unknown ($RUN missing: stock UI or legacy launcher)"
fi
[ -n "$ACC" ] && echo "  last accepted: version $(kv "$U/accepted" version), epoch $ACC"
[ -d "$U/active" ] && echo "  NOTE: a trial is still running; keep or revert it first if you can"
[ -f "$U/pending/ready" ] && echo "  NOTE: already staged: version $(kv "$U/pending/ready" version) (will be replaced)"

# --- check the new one first, from a private copy (nothing in /usr/data is touched yet) ------------
T=/tmp/.diskos_cli.$$
rm -rf "$T"
mkdir -m 700 "$T" || die "cannot create $T"
trap 'rm -rf "$T"' EXIT
for f in keyauthz keyauthz.sig leafpub.pem update.manifest update.manifest.sig; do
    cp "$SRC/$f" "$T/$f" || die "cannot read $SRC/$f"
done
echo
echo "Checking $SRC ..."
OUT=$(/bin/sh "$RO/diskos-verify.sh" "$RO/root.pub.pem" "$MINKEY" "$FL" "$T" "$SRC/mq_ui") || die "update rejected (see REJECT above)"
NVER=$(echo "$OUT" | sed -n 's/.* version=\([^ ]*\).*/\1/p')
NEP=$(echo "$OUT" | sed -n 's/.* epoch=\([^ ]*\).*/\1/p')
NSHA=$(kv "$T/update.manifest" sha256)
if [ -f "$U/rejected" ] && grep -qx "$NSHA" "$U/rejected"; then
    die "this exact build was rolled back before (listed in $U/rejected); boot would drop it"
fi
echo "New (signature OK):"
echo "  version $NVER, epoch $NEP, sha256 $(echo "$NSHA" | cut -c1-16)..."
[ -f "$RUN" ] && [ "$(kv "$RUN" SHA256)" = "$NSHA" ] && echo "  NOTE: this is the build that is running now"
echo

ask "Stage version $NVER for the next start?" || { echo "Cancelled, nothing changed."; exit 2; }

# --- stage: same steps and order as ota_stage_dir ---------------------------------------------------
mkdir -p "$U" && chmod 700 "$U" || die "cannot prepare $U"
rm -rf "$U/incoming" "$U/pending.old" || die "cannot clear old staging folders"
mkdir -m 700 "$U/incoming" || die "cannot create $U/incoming"
for f in $FILES; do
    cp "$SRC/$f" "$U/incoming/$f" || die "copy of $f failed (data partition full? df -h /usr/data)"
done
chmod 600 "$U"/incoming/*
# verify the copy that boot will read (the card could have changed since the check above)
OUT2=$(/bin/sh "$RO/diskos-verify.sh" "$RO/root.pub.pem" "$MINKEY" "$FL" "$U/incoming" "$U/incoming/mq_ui") \
    || { rm -rf "$U/incoming"; die "staged copy failed verification"; }
[ "$OUT2" = "$OUT" ] || { rm -rf "$U/incoming"; die "the files changed while copying; run again"; }
printf 'version=%s\nepoch=%s\n' "$NVER" "$NEP" > "$U/incoming/ready" || die "cannot write ready"
sync || die "sync failed"
rm -rf "$U/pending" || die "cannot remove the old pending folder"
mv "$U/incoming" "$U/pending" || die "cannot rename incoming to pending"
sync || die "sync failed"
echo "Staged $NVER. It starts as a trial after the next restart."
echo

if ask "Restart now?"; then
    echo "Restarting..."
    sync && reboot
    exit $?
fi
echo "Not restarted. Restart later with: sync; reboot  (or Settings > System > Maintenance > Restart)"
exit 0
