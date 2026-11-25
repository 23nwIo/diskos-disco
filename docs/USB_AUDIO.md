# USB audio output (own player) - assessment and agreed design

Status (2026-11-25): **implemented for one USB sound card (the newest plugged), host-tested, not yet tested on a
Disc.** Own player (MPD core) only; the stock mq_player route in `ui/modes.c` (0666 3 + 0642 5) is not used by it.

## Device facts (verified on a Disc, 2026-11-25)

Probe: `tools/usbaudio-probe.sh`. Test device: a USB-C multifunction dock (VIA hub 2109:2817 + Billboard 2109:8888,
inner hub 1a40:0801, card reader 05e3:0751, ASIX AX88179A Ethernet, C-Media 0d8c:0014 sound card).

- The kernel (4.4.94) has `snd-usb-audio`, `usb-storage`, `usbhid` built in. No `cdc_ncm`/`ax88179` (Ethernet has no
  driver), no `/lib/modules`.
- **Host mode must be switched on by us**: `/sys/class/usb_role/13500000.otg_new-role-switch/role` stays `none` when
  a device is attached; writing `host` enumerates it (~3 s). It stays `host`, and a re-plug is picked up without
  writing it again. The own player ignores `0642 5` today (`ui/player/core.c`), so Files > USB drive needs this too.
- About 9 s after switching, the USB-C controller re-negotiated once (`port->Registers.Control.MODE=1`) and the whole
  USB tree disconnected and enumerated again. The player must survive this.
- A dock with its own power supply powers the Disc (`typec_state` `4 AttachedSink`, `cc_term` `R3p0`).
- The C-Media chip enumerates only while headphones are plugged into it; pulling them is a USB disconnect.
  So a card can appear and disappear at any time after plug-in: hotplug is required.
- C-Media 0d8c:0014: full speed (12 Mbit) behind a high-speed hub (split transactions) - plays fine on the Disc's
  DWC2 (audible test). ALSA card id `Device`, card 1. Playback S16_LE only, 2 ch, 44.1/48 kHz. Mixer
  `Speaker Playback Volume` 0..37 = -37..0 dB (came up at 27 = -10 dB), `Speaker Playback Switch`; no serial number.
- Card reader and sound card work at the same time on the same hub.

## Agreed design

**Universal**: no per-device configuration. Every USB sound card with a playback PCM is offered.

- **Identity**: key = VID:PID + USB serial number. A device without a serial number has no stored settings and is
  treated as new every time it is plugged in.
- **Outputs list**: every plugged USB card is its own switch under Outputs (next to Bluetooth / AUX analog /
  AUX digital). First version may support one active USB card (the newest plugged); several cards follow soon after.
- **New / unknown devices start OFF**, also with Auto Outputs = Newest. Known devices (with serial) follow Auto Outputs.
- **MPD**: never restarted for a card (better than agreed): MPD's USB output plays to the ALSA name `diskos_usb`,
  defined in `/tmp/diskos-mpd/asound.conf`, which MPD's alsa-lib reads besides its own `alsa.conf`
  (`ALSA_CONFIG_PATH`). alsa-lib reads its files again when one is replaced, so another card only needs the file
  rewritten and the output opened again (checked with alsa-lib 1.2.11/1.2.12 on a PC). No fixed format: MPD negotiates
  with the card and converts only what the card refuses. DoP later.
- **Host port**: shared by USB audio and USB drive (later Ethernet); released only when no user needs it. USB storage
  mode (Disc as gadget) excludes host mode.

### Volume (applies to all outputs, also Bluetooth per MAC)

- One **main volume** plus a stored **offset per device**; effective = clamp(main + offset, 0, cap). Only main and
  offsets are stored, never the effective value. Offsets are on the common 0..100 scale; each device maps it with its
  own curve. Volume keys and headphone (AVRCP) volume buttons move the main volume.
- **New devices: offset -120 and off** (silent at any main volume until raised on purpose; Jo asked for -20 or -100,
  -120 is the bottom of the 0..120 scale). Reason (Jo): use on concert-size PA systems - a loud surprise can injure
  people. New Bluetooth headphones also start at -120 (they are switched on by Auto Outputs as before, but silent).
- **Max volume cap per device** (adjustable).
- **Fade-in per device**: default ON, default 4 s (duration adjustable in the config file for now, no UI), from 0 to the target volume when the output is switched on and when
  playback starts after a pause; not on normal volume changes.
- The offset of a plugged but switched-off device can be changed. Hardware mixer: written directly via ALSA, no MPD.
- **Order of operations**: on plug-in the card's hardware mixer is set to the computed level at once, before any stream
  opens. Cards without a hardware mixer get MPD's software volume set before the output is enabled.
- Mixer choice: the first `Playback Volume` control of the card (Speaker, PCM, Headphone, Master, ...). A card without
  one gets its own: alsa-lib's `softvol` with a control `diskOS USB Playback Volume` on that card (0..120, -60..0 dB,
  0 = silence). The player creates the control itself at silence before MPD opens the card, because softvol would
  create it at 0 dB. No output shares a volume with another one (MPD's software volume is S/PDIF's alone).
- Stored in `/usr/data/diskos-own/volumes` (as the volumes were before), written only 3 s after a change and at
  power off, never periodically. At most 48 devices; the least recently used one goes.

## Implementation (2026-11-25)

- `ui/player/usbaudio.c`: the host role (`ua_role_tick`, every 2 s: data role DFP -> `host`; after we set it and the
  data role is no longer DFP -> `none`; left alone in USB storage mode), the card scan (`/proc/asound/cardN/usbid` +
  `pcmNp`, serial and product from sysfs), the volume control (control ioctls, no alsa-lib) and the ALSA file.
- `ui/player/main.c`: `usb_tick` once a second (newest card; a new card's control is set before anything can play),
  fade in (`before_play` hook from engine.c, outputs coming on in the route change), `apply_volumes`.
- `ui/player/core.c`: main + offset per device, limits, fade flags, `pl_core_usb`, 0d08/0d09/0d0a, 0642 5.
- `ui/player/engine.c`: output `usb` (no MPD mixer; the volume is the card's control or our softvol one), `eng_usb`.
- UI: Settings > Audio > Outputs (USB audio, the device sliders, Limits and Fade In), Remote Control `api/outputs`.
  **Device Sliders** (cfg `out_vol_view` in `/usr/data/diskos.conf`, also under Outputs and on the Remote Control
  page): Offset (default, like a mixing desk: the device sliders show their offset -120..+120 and the level in
  brackets, and stay put when the main volume moves) or Level (they show the level, 0..120, and move with main).
- Fade length: `fade_s=N` in `/usr/data/diskos-own/audio.conf` (0..30, default 4), read at player start.
- Known: the C-Media test card has no serial number and disconnects whenever its headphones are pulled, so it comes
  back off and silent every time (as agreed for cards without a serial).

## Out of scope for now

Ethernet over USB (needs kernel modules), the card's microphone and HID buttons, several USB mass storage devices.
