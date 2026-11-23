# Wi-Fi start and DHCP hostname (advanced)

For users with shell access (SSH, see [SSH_ACCESS.md](SSH_ACCESS.md)). There is no setting for this on the Disc or in
the web interface. Not yet tested on a Disc (dev a374d02, 2026-11-23).

## Set the hostname

The Disc sends a hostname to the DHCP server (your router, dnsmasq, ...) each time it gets an IP address. Set it with
one line in `/usr/data/diskos-hostname`:

```
echo IoMusic > /usr/data/diskos-hostname
```

Rules: letters `A-Z a-z`, digits and `-`, at most 63 characters, not starting or ending with `-`. Only the first line
counts. A name that breaks these rules is ignored and the default is used.

Default without the file: `Disc-` plus the last four hex digits of the Wi-Fi MAC address (for example `Disc-1A2B`), so
two Discs never share a name.

The new name is used at the next Wi-Fi start, so after a reboot, after switching Wi-Fi off and on, or when you connect
to a network on the Wi-Fi screen. Check it:

```
cat /tmp/diskos-hostname
ps w | grep [u]dhcpc
grep "wifi:" /tmp/diskos_boot.log
```

Remove the file to go back to the default. Updates never touch `/usr/data/diskos-hostname`.

## How Wi-Fi is started

FiiO's `/usr/bin/wifi_up.sh` lives on the read-only system partition. It sends the hostname `FIIO_` and starts DHCP
together with `wpa_supplicant` on BusyBox's defaults: 3 requests 3 s apart, then a 20 s pause. If joining the network
takes longer than those 9 s, the IP address comes about 20 s late.

So mq_ui carries its own script and uses it instead of FiiO's, at every start and after every update:

- it writes the script to `/usr/data/diskos-wifi_up.sh` (only when it changed, so once per update), checks it, and
  bind-mounts it over `/usr/bin/wifi_up.sh` (in RAM; the system partition stays as it is);
- the script starts `wpa_supplicant`, then `udhcpc -i wlan0 -x hostname:NAME -t 10 -T 2 -A 3` (10 requests 2 s
  apart, then a 3 s pause);
- mq_ui puts NAME in `/tmp/diskos-hostname` before every Wi-Fi start; connecting on the Wi-Fi screen sends the same
  name.

Editing `/usr/data/diskos-wifi_up.sh` has no lasting effect: mq_ui writes it back at the next start. If anything goes
wrong, FiiO's script stays in use and `/tmp/diskos_boot.log` says why (`wifi: diskOS script ...`).

Source: `ui/wifi_script.h` (script and hostname rules), `ui/wifi.c` (`wifi_apply_script_override`), test
`ui/tests/wifi_script_test.c`.

## Coming from an own /usr/data/wifi_up.sh

Before this change, a copy of FiiO's script in `/usr/data/wifi_up.sh` was used instead of the original (to change the
hostname). At the first start with the new mq_ui:

- the hostname from its `udhcpc ... -x hostname:NAME` line becomes `/usr/data/diskos-hostname`, unless that file
  already exists (a name built from a variable, like FiiO's `FIIO_$product_model`, is not taken over);
- the copy is renamed to `/usr/data/wifi_up.sh.old`.

## Your own script

To keep using a script of your own, create the switch file and put the script in `/usr/data/wifi_up.sh`:

```
touch /usr/data/diskos-wifi-custom
```

Then mq_ui binds `/usr/data/wifi_up.sh` as before and migrates nothing. Your script is used only if it is a regular
file (no symlink), executable (`chmod +x`), at most 64 KiB and starts with `#!`; otherwise FiiO's script runs. With your
own script, `/usr/data/diskos-hostname` only affects connecting on the Wi-Fi screen.
