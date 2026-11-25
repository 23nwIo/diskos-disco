#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# usbaudio-probe.sh - on-device: collect what the Disc knows about a USB sound card (USB host port, USB-C state,
# snd-usb-audio, ALSA card, stream formats/rates, mixer controls), for the "USB audio output" assessment.
#
# Run over SSH, once WITHOUT the card and once WITH it plugged in (give each run a label):
#   sh /usr/data/usbaudio-probe.sh without
#   sh /usr/data/usbaudio-probe.sh with
#   sh /usr/data/usbaudio-probe.sh with --host     # also set the OTG role to "host" if the card did not show up
#   sh /usr/data/usbaudio-probe.sh with --play     # also play 2 s of silence to the card (checks that it opens)
# Output: /tmp/usbaudio-probe-<label>.txt (RAM; copy it off the Disc). Errors are kept in the output on purpose.
# Nothing is written to flash. --host writes the role switch (undone by a reboot or by writing "device" back).

LABEL=${1:-run}
OUT=/tmp/usbaudio-probe-$LABEL.txt
ROLE=/sys/class/usb_role/13500000.otg_new-role-switch/role
CC=/sys/bus/i2c/devices/2-0022/control
HOST=0; PLAY=0
for a in "$@"; do
  case "$a" in --host) HOST=1 ;; --play) PLAY=1 ;; esac
done

sec(){ echo; echo "===== $*"; }
run(){ echo "\$ $*"; "$@" 2>&1; echo "[exit $?]"; }

{
sec "label $LABEL, $(date)"
run uname -a
run cat /proc/version
sec "who plays"
ps | grep -E "mq_player|diskos-player|mpd|aplay" | grep -v grep

sec "USB-C controller (AW35615) and OTG role"
for f in typec_state cc_term data_role power_role; do
  [ -e "$CC/$f" ] && { echo "$f: $(cat "$CC/$f" 2>&1)"; }
done
run ls "$CC"
run cat "$ROLE"
run ls /sys/class/usb_role
run ls /sys/class/typec
if [ "$HOST" = 1 ]; then
  sec "--host: writing host to $ROLE"
  echo host > "$ROLE"; echo "[exit $?]"
  sleep 3
  run cat "$ROLE"
fi

sec "kernel drivers and modules"
run ls /sys/bus/usb/drivers
run ls /sys/bus/usb/drivers/snd-usb-audio
run ls /sys/module/snd_usb_audio/parameters
run lsmod
run ls /lib/modules

sec "USB devices"
for d in /sys/bus/usb/devices/*; do
  [ -e "$d/idVendor" ] || continue
  echo "-- $d"
  for f in idVendor idProduct manufacturer product speed bMaxPower bDeviceClass version; do
    [ -e "$d/$f" ] && echo "  $f: $(cat "$d/$f" 2>&1)"
  done
  for i in "$d"/*:*; do
    [ -e "$i/bInterfaceClass" ] || continue
    echo "  if $(basename "$i"): class $(cat "$i/bInterfaceClass") sub $(cat "$i/bInterfaceSubClass") proto $(cat "$i/bInterfaceProtocol") driver $(basename "$(readlink "$i/driver" 2>&1)")"
  done
done

sec "ALSA"
run cat /proc/asound/cards
run cat /proc/asound/devices
run cat /proc/asound/pcm
run ls -l /dev/snd
for c in /proc/asound/card[1-9]; do
  [ -d "$c" ] || continue
  sec "ALSA $c"
  run cat "$c/id"
  run cat "$c/usbid"
  run cat "$c/usbbus"
  run cat "$c/usbmixer"
  for s in "$c"/stream*; do [ -e "$s" ] && run cat "$s"; done
  n=${c#/proc/asound/card}
  run amixer -c "$n" scontents
  run amixer -c "$n" contents
  if [ "$PLAY" = 1 ]; then
    sec "--play: 2 s of silence to hw:$n,0"
    run aplay -D "hw:$n,0" --dump-hw-params -d 2 -t raw -f S16_LE -r 44100 -c 2 /dev/zero
    run aplay -D "hw:$n,0" -d 2 -t raw -f S24_3LE -r 96000 -c 2 /dev/zero
    run aplay -D "hw:$n,0" -d 2 -t raw -f S32_LE -r 96000 -c 2 /dev/zero
  fi
done

sec "power"
for p in /sys/class/power_supply/*; do
  echo "-- $p"
  for f in status online current_now voltage_now capacity; do [ -e "$p/$f" ] && echo "  $f: $(cat "$p/$f" 2>&1)"; done
done

sec "dmesg (USB / sound)"
dmesg | grep -i -E "usb|snd|audio|dwc|otg|typec|vbus|over.?current" | tail -n 80
} > "$OUT" 2>&1

echo "written: $OUT"
