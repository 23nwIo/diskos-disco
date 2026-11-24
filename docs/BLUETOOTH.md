# Bluetooth audio (experimental)

[Back to the README](../README.md) · [Settings](DISPLAY_SETTINGS.md) · [3.4.0 notes](releases/v3.4.0.md)

> [!WARNING]
> **Experimental.** Bluetooth audio, the codec choice, aptX, the automatic switching between Bluetooth stacks, the
> crash guard and Quiet Wi-Fi for Bluetooth are all experimental. They were tested on one Disc with two pairs of
> headphones. Wired listening is the dependable way to use the Disc.

Pair and connect headphones or speakers in **Settings › Connectivity › Bluetooth**. Audio follows the connected
headphones automatically.

## Codecs

**Settings › Connectivity › Bluetooth › Bluetooth Codec** applies from the next connection:

| Choice | What the headphones get |
|---|---|
| **aptX/LDAC Mobile / Standard / High** | LDAC or aptX, whichever they have, then AAC, then SBC: LDAC → aptX HD → aptX → AAC → SBC. Mobile, Standard and High are LDAC's quality (aptX has none). |
| **AAC** | AAC, else SBC. Never higher than AAC (useful if LDAC stutters). |
| **SBC** | SBC, which every pair supports. |

Each pair of headphones gets its own best codec: an LDAC pair gets LDAC and an aptX pair aptX, with the same
setting. Disco learns each pair's codecs at its first connection. **Song Info** shows the codec really in use, for
example *Bluetooth · LDAC*.

## aptX and aptX HD

FiiO's Bluetooth audio on the Disc has SBC, AAC and LDAC only. Disco carries its own: BlueALSA 4.0.0 with the
open-source libfreeaptx encoder, built for the Disc and stored inside the interface, so an SD update brings it.

1. Turn on **Settings › Connectivity › aptX (experimental)**. Disco puts its files in place and says when it's ready.
2. Choose a **Bluetooth Codec** starting with *aptX/LDAC*.
3. Turn Bluetooth off and on.

The first connection of new headphones can still use SBC while Disco learns their codecs; from then on they get their
best one. Off (the default), Bluetooth is FiiO's, exactly as in 3.3.1.

- **LDAC always runs on FiiO's Bluetooth audio**, aptX on Disco's: LDAC through Disco's build froze the Disc in testing.
  When a pair's best codec needs the other one, Disco pauses the music and reconnects the headphones once (*Reconnecting
  for LDAC…* / *…for aptX…*). Press Play again afterwards.
- **aptX Adaptive and aptX Lossless are not supported:** there is no open-source encoder for them. Headphones with
  those (and classic aptX) get classic aptX.

## When a codec doesn't work

- A codec that doesn't come up steps down to the next one for **that connection only**. The next connection tries
  the best one again. Nothing is remembered as broken.
- **Crash guard:** if the Disc restarts just as music starts on a codec that hasn't played on those headphones before,
  that codec is skipped for them and Disco says so. Choose Bluetooth Codec again to give it another try.

## Quiet Wi-Fi for Bluetooth

The Disc's Wi-Fi and Bluetooth share one 2.4 GHz radio and take turns. **Settings › Connectivity › Quiet Wi-Fi for
Bluetooth** (on by default): while audio goes to Bluetooth, Wi-Fi saves power and stops searching for networks when it
isn't connected, so the headphones get more airtime. LDAC needs the most, so it gains the most. Wi-Fi still works;
everything goes back to normal when audio leaves Bluetooth.

## Headphone battery, buttons and volume

**Battery.** Many headphones report their battery over the hands-free profile (HFP), the way they do to a phone.
Disco connects that profile too (**Settings › Connectivity › Headphone Battery**, on by default; no calls, no
microphone) and shows the level in the device list ("Connected - SBC - 70%"), on the headphones' details screen
(Settings › Connectivity › Bluetooth, tap the connected headphones) and in Remote Control's Bluetooth tab. At 20 % and at 10 % the Disc says so once. Headphones
report in 10 % steps. At boot the stock player starts BlueALSA itself, without that profile; Disco restarts it with
the profile as soon as Bluetooth is up and no headphones are connected yet (also when headphones disconnect). If
headphones were already connected by then, the battery shows after their next connection.
Headphones that connect themselves sometimes open only the audio link (A2DP) and not the hands-free one, and the
Disc's BlueZ 5.54 never adds it. When the level is still unknown 6 s after the headphones connected, Disco asks BlueZ
for the hands-free link (`Device1.ConnectProfile`, at most twice per connection; logged as `bt battery:` in
/tmp/diskos_boot.log).

**Buttons** (own player only). Play/Pause, Next and Previous on the headphones control the Disc. Headphones that send
"Play" for both play and pause (the Austrian Audio Hi-X25BT does) toggle. The stock player handles the buttons itself.

**Volume** (own player only). Headphones with AVRCP absolute volume get the Disc's Bluetooth volume as their own level,
and their volume buttons move the Disc's Bluetooth volume (Settings › Audio › Outputs), like on a phone or a Linux PC.
The Disc's level is sent to them as soon as they connect, so nothing gets louder. Headphones without it keep their own
volume, and the Disc's Bluetooth volume scales the sound as before. To keep the Disc's volume separate even on
headphones that have absolute volume: `touch /usr/data/diskos-own/no-abs-volume`. Whether a device has absolute
volume is decided per connection: a link without it (a receiver such as the XQ-50 PRO2 may offer it on one connection
and not on the next) is set back to the Disc's own volume on every new Bluetooth route. **Settings › Audio › Outputs ›
Headphone Volume** sets what their volume buttons move: *Bluetooth only* (default) or *All outputs*, where the AUX volume
moves by the same step (player command `0d06`, docs/PLAYER_PROTOCOL.md).

How it works (verified on a Disc with firmware V2.57, 2026-11-16): FiiO's BlueZ 5.54 hands AVRCP button presses and
the headphones' volume only to a media player registered over D-Bus (`org.bluez.Media1.RegisterPlayer`); without one
they would go to a uinput device, and the Disc has no `/dev/uinput`. The own player registers one
(`ui/player/btavrcp.c`, with a small D-Bus client of its own, `ui/dbusmini.c`) and switches BlueALSA's
`SoftVolume` off for headphones that report a volume. The battery comes from BlueALSA's `org.bluealsa.RFCOMM1.Battery`
(`ui/btbatt.c`); BlueZ 5.54 is too old to show it itself. Log lines start with `avrcp:` in /tmp/diskos-player.log.

## Known limits

- After changing headphones, press **Play** again.
- Dropouts on LDAC: keep Quiet Wi-Fi on, or try **aptX/LDAC Mobile** (about half LDAC Standard's bitrate).
- Bluetooth codec choice applies to firmware V2.57 only.
