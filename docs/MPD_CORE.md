# MPD as the own player's audio core

Decided 2026-11-15 (Jo): the own player (ui/player, docs/MQ_PLAYER_REPLACEMENT.md) keeps the FiiO protocol, the DAC,
volume, balance, gain, keys and power; MPD only decodes, seeks and plays gapless. The EQ is on todo.md. The library
stays diskOS's own (musicdb); MPD runs without a database and gets songs as `file://` URIs over its local socket.

## Build

`mpd/build.sh <toolchain-with-c++> <work-dir>`: MPD 0.23.17 static for MIPS32r2/FP64/NaN2008 (4.3 MB stripped), with
alsa-lib 1.2.12, FLAC 1.4.3, a small FFmpeg 6.1 (AAC, ALAC, APE, WavPack, Vorbis, Opus, MP3, WMA, PCM), fmt 10.2.1,
and since 2026-11-16 libcurl 8.11.1 (HTTP/HTTPS only) over mbedTLS 3.6.2 for web radio (no CA bundle built in).
The diskOS toolchain is C only; the script header says how to add g++ 11.2 with the same ABI. Our MPD patches are in
`mpd/patches/`.

## Device findings (2026-11-15, Jo's runs)

- Memory: VmRSS 6.6 MB, VmHWM 6.8 MB while playing FLAC (MemAvailable on the Disc is about 75 MB).
- `hw:0,3` (the stock player's PCM) refuses every `buffer_time` at 44.1 kHz when it is set before the period, and
  accepts it the other way round (`mpd/alsaprobe.c`). Patch `0001-alsa-period-first-fallback.patch`.
- The stock player always opens `hw:0,3` as **S32_LE** (trace: format mask bit 10; `/proc/asound/card0/pcm3p/sub0/hw_params`
  while it plays: S32_LE, 48000 Hz, period 1920, buffer 3840 frames). It played a 44.1 kHz FLAC at 48 kHz, so it seems
  to resample to 48 kHz (to be checked with other files). MPD config: `format "*:32:*"`.
- **The mixer route `LO3_MUX` = 12 (item `LI11`) must be set**, or `hw:0,3` has no backend: the kernel logs
  "DMA3 playback: ASoC: no backend DAIs enabled for DMA3 playback" and `snd_pcm_hw_params()` fails with EINVAL (also
  for `aplay`). The stock player sets it (mixer_set_dma) at start and around USB storage mode, and it then stays set
  between idle and playing; after a boot straight into the own player it is unset. Confirmed 2026-11-15:
  `amixer -c 0 cset name='LO3_MUX' 12` made aplay and the own player audible. The player sets it itself (`mixer.c`,
  at start and before every stream).
- **The DAC must be started and its devices kept open.** Without the stock start sequence the output is silent; with
  it but the devices closed again, there is only a hum and the DMA runs far faster than real time (no I2S clock). With
  the sequence and `/dev/cs43131` + `/dev/cs43131c` held open (`mpd/dacstart.c`), MPD plays in real time and is
  audible. Sequence (trace run 1, 578 s): `_IO('M',48)` on A once, A M56 = 3, C M60 = 3, 0.4 s, both 0, A M52 / C M54 =
  rate code (1 = 44.1 kHz, 2 = 48, 4 = 96, 6 = 192), A M41 / C M50 = 0, volume A M45 + M66 / C M47 + M68 (attenuation
  byte), A `_IO('M',46)`; then the PCM is opened.
- MPD closes a client connection that has been silent for 60 s (`connection_timeout`; a connection in `idle` is
  exempt; checked against MPD 0.23.14 on a PC). The player's command connection is reopened whenever it is found
  closed (engine.c `xcmd`). Before that fix (7372509) the player lost MPD a minute after boot and never started the
  DAC: no sound (Jo, 2026-11-15).
- The DAC start sequence runs before MPD opens `hw:0,3` (an EINVAL from `snd_pcm_hw_params()` seen on 2026-11-15
  was first put down to a missing start; the real cause was the missing mixer route above). The player reads the song's rate from MPD (`playlistid`, `Format`) and starts the DAC right before `playid`
  (engine.c `eng_set_prepare`). A next song with another rate is not queued in MPD; it starts after the current one
  ends, with the DAC started for its rate (no gapless change across rates).
- The volume is the DAC's: the stock UI's volume does nothing to MPD's stream, the own player writes the volume bytes.

## In the own player (ui/player)

- **Engine** (`engine.c`): the player starts MPD as a child with a generated config (`/tmp/diskos-mpd`), owns the DAC
  (`dac.c`: start sequence with the stream's rate code, volume/balance/gain as attenuation bytes) and reads MPD's
  changes from a second connection that sits in `idle`.
- **Queue** (`queue.c`, `queuedb.c`, `playback.c`; RAM only since 2026-11-22): on `0100` the player reads the list's
  SONG.IDs (`qd_list`: `SONG`, `MY_LOVE` or `CUSTOM_PLAYLIST` with the stock filter and `ORDER BY`; favourite and
  playlist rows are matched to their library song by PATH, TRACK, IS_CUE, IS_ISO) into RAM. Nothing is written to
  song.db: no `LIST_SONG_0`, no `LIST_SONG_3` (the stock player wrote the whole list, 5,000+ rows for All Songs, to the
  internal flash at every list start). Positions are 1-based (the `a2` `pos_id`, the `0100` type 0 jump); in shuffle
  the play order is a permutation of the positions, the tapped song first. The player publishes the list to
  `/tmp/diskos-list` (own_player.h `OPL_LIST`, tmpfs) at every new list, shuffle change and its start; ui/musicdb.c
  reads it for Up Next, the current song and jumps while it exists. The list is lost at a restart. The orderings must
  stay identical to ui/musicdb.c's (`ui/tests/playback_test.c` checks them).
- **MPD holds two songs only**: the current one and the one after it (for gapless). The queue itself stays the
  player's, so a library of tens of thousands of songs is never copied into MPD. When MPD moves on to the queued
  song, the player queues the next one by the play mode: in order (stop after the last), shuffle and repeat all (wrap
  around), repeat one (the same song again), single (stop after it). Next/Previous always move one song, wrapping.
- **Frames**: every track start sends the stock sequence (`a102` mode on a play command or `{"state":1}` on a natural
  change, `aa1c`, the full `a2` with the song JSON and `pos_id` = the list position, `aa05`, `aa1c`, `{"state":0}`
  twice); `0202` with a track loaded is answered with the full `a2`.
- **Resume** (`MEMORY_PLAY`, mode `0684`): switched off for now (no flash writes, 2026-11-16); see
  docs/MQ_PLAYER_REPLACEMENT.md, "Resume (MEMORY_PLAY)".
- **CUE tracks** (2026-11-15): a row with `IS_CUE=1` is the part of its file (`PATH`) from `OFFSET` for `DURATION`
  (both ms, ui/scanner.c "CUE sheets"); the engine adds the file and limits it with `rangeid ID start:end` (open end
  when the duration is 0). MPD then counts elapsed time, duration and seeks from the part's start (checked with MPD
  0.23 on a PC), so `a1`, `a2` and `0103` need no conversion. Consecutive tracks of one file queue gaplessly like
  any two songs.
- **SACD ISO tracks** (`IS_ISO=1`) are skipped with a log line: MPD has no decoder for disc images.
- **Bluetooth output** (2026-11-15, plays on a Disc with volume: Jo, build ebab3fe). Our MPD is static (musl) and can't load the
  device's BlueALSA ALSA plugin (a glibc .so), so the first way is the fallback of Jo's plan: three `pipe` outputs
  (`bt44`, `bt48`, `bt96`, 16 bit stereo at that rate) whose command is `/tmp/diskos-mpd/bt-out.sh <rate>`, which
  runs the device's `aplay -t raw -D "plug:'bluealsa:DEV=<MAC>,PROFILE=a2dp'"` (MAC from `/tmp/diskos-mpd/bt-dev`,
  aplay's messages in `/tmp/diskos-mpd/bt.log`). The UI's route (ui/main.c `ui_route_bt`: `0666 2`, then `06b3`
  with the codec and the headphones' MAC) makes the player write the MAC, read the link's rate with
  `bluealsa-cli info` and enable the matching output (any other rate: `bt44`, and `plug` converts); `0666 6`
  switches back to the DAC. While on Bluetooth only, the DAC and its mixer route are left alone, the Bluetooth volume
  goes to the headphones with `bluealsa-cli volume` (0..127), and `06b3` codecs 1..4 are asked of BlueALSA with
  `bluealsa-cli codec` (AAC, LDAC; the LDAC quality not yet). MPD has no pause for a pipe output, so a pause closes
  it and play starts aplay again.
  Device check (Jo, 2026-11-15, LDAC headphones): BlueALSA v4.0.0; `bluealsa-cli info` of the PCM prints
  `Format: S32_LE`, `Sampling: 96000 Hz`, `Selected codec: LDAC`, `SoftVolume: Y`, `Volume: L: 4 R: 4`; `aplay` with
  16 bit/44.1 kHz into `bluealsa:DEV=<MAC>,PROFILE=a2dp` and into `plug:'bluealsa:...'` both open and play (the
  plugin prints `D:` debug lines on stderr), so the link's format and rate are converted either way.
- **Outputs** (2026-11-15, host-tested, not yet on a Disc; Settings > Audio > Outputs, own player only; AUX digital
  added 2026-11-25, see "S/PDIF"). MPD plays to every output that is on: AUX (the DAC, i.e. both jacks), Bluetooth, or both at once (Bluetooth is a few hundred
  ms behind; MPD can't delay one output). What connecting does is the **Auto Outputs** setting (`0d05`, cfg
  `out_auto`, sent again on every player start; 2026-11-16):
  - *Newest* (default, the behaviour before 0d05): a plug going in (3.5 mm or 4.4 mm, none was in) turns AUX on and
    Bluetooth off; the UI's Bluetooth route (`0666 2` + `06b3` for new headphones) turns Bluetooth on and AUX off.
  - *All connected*: a plug adds AUX, headphones add Bluetooth (and AUX goes off if no plug is in); choosing it turns
    on what is connected right away.
  - *Off*: connecting changes nothing; the player bumps `notice=` in its state file and the UI says "AUX connected,
    but not enabled. See Audio > Outputs" (or Bluetooth).
  Disconnecting, *Newest* and *All*: the last plug out turns AUX off when headphones are connected (they play on;
  if they were off, Bluetooth takes over paused), else it pauses. Headphones gone (`0666 6`: the UI routed away, or
  their link went, which the UI now tells the own player after three empty probes, ui/main.c `ui_route_forget`) turn
  Bluetooth off when AUX is on (AUX plays on); when Bluetooth was the only output the music pauses and AUX takes
  over if a plug is in, else Bluetooth stays chosen and waits for the headphones while the DAC stands in. *Off*:
  the choice stays; the music pauses when nothing that is on is left (the plug out with AUX playing, the only
  headphones gone).
  By hand: `0d01` (docs/PLAYER_PROTOCOL.md). Each output has its own volume (`0d02`, `0d03`),
  kept in `/usr/data/diskos-own/volumes`; the volume the UI shows (`a714`) is the lead output's (Bluetooth when on,
  else AUX), and the volume keys and `0715` move both volumes (AUX and Bluetooth, on or off) by the same step. When the DAC joins a
  running stream, its start sequence and mixer route go first. The player writes the state for the UI to
  `/tmp/diskos-player-outputs` (ui/own_player.h `opl_outputs_t`).
- **Codec switch by hand** (`0d04`, Outputs > Bluetooth Codec): pause, close the Bluetooth output (aplay lets go of
  the PCM), `bluealsa-cli codec <pcm> <codec>` as a child (at most 10 s), then read the link again (`bluealsa-cli
  info`: rate, `Selected codec`, `Available codecs`), open the output at the new rate and play on. Only for this
  connection; new connections use Connectivity > Bluetooth Codec as before. The list shows only the codecs the
  headphones offer (`codecs=` in the state file) and the one in use. Newer BlueALSA writes each codec with its
  configuration (`SBC:ffff0235`, `Selected codec: SBC:21150235`): only the name counts (fixed 2026-11-16; before,
  every choice said "These headphones don't offer it").
- **Volume log** (2026-11-16): every change of a volume adds `2026-11-16 01:30:12 Volume: Main 42, AUX 23, BT 5` to
  `/tmp/diskos-volume.log` (`tail -f`); Main is what the UI shows, `(off)` marks an output that does not play. The
  UI's log upkeep (ui/main.c `log_maintenance`) checks the logs in /tmp every 10 minutes: one past 2 MB keeps its
  last 2 MB as `<log>.1` and starts empty (player, volume, MPD, bt.log, bluealsa, bluetoothd, patchram, Boomshine).
- **Volume curve** (2026-11-15): the stock player's own curves, read from mq_player (0x4e7800, 0x4e78c0, 0x4e7980,
  0x4e7a40; ui/player/dac.c `dac_curve`, every value checked in ui/tests/player_test.c). 3.5 mm, high gain: 218 at
  volume 1, then steps of 8 (from 2), 6 (4), 4 (11), 2 (23) and 1 (37) down to 0 (0 dB) at 120. Low gain is 12 more
  (6 dB) everywhere; the 4.4 mm balanced curve 4 more up to volume 36, then 3, 2, 1 and the same from 40. The stock
  player uses the high-gain curve only while a plug is in. The earlier estimate (156 - 2 x volume) matched below 37
  but reached 0 dB at 78, so it was much too loud above 37.
- **4.4 mm balanced plug**: detected like the stock player does it (`adc_bal_det_handler`): `/dev/jz_adc_aux_2`, enabled
  with `_IO('A',11)`, one 32-bit value every 330 ms; plugged at >= 1501 or 801..949, not at 1151..1349, anything else
  keeps the state; a change counts after 4 reads in a row. Same audio path as 3.5 mm (the stock player only switches
  the volume curve). Not checked on a Disc (no 4.4 mm plug at hand); the player logs the first three values it reads.
- Not yet: Previous restarting the song after a few seconds (stock behaviour not measured).

## Web radio

Since 2026-11-16 (Jo asked for it): the Radio app (RADIO.md) switches the player to work mode 0C, as for the stock
player's DLNA renderer, and then sends the own player the station as one diskOS command, `0d10` (name length, name,
URL; no URL = stop; ui/radio_core.c `rc_station_frame`). It knows which player runs from `opl_running()`; the stock
player still gets SetAVTransportURI + Play over UPnP. The first version (98e361f) had the own player answer that UPnP
on `127.0.0.1:49494`; Jo chose the direct command (2026-11-16), so the renderer (`ui/player/upnp.c`) is gone. The
player plays the URL in MPD (`playback.c` `pb_stream_*`): the local song is paused and its place kept; a new `0d10`
replaces the station; leaving the mode stops the station and loads the local song again, paused. The Play key pauses
and plays on; skip and seek do nothing. The UI gets one track frame for the station (`song_name` = the station,
`work_mode` "DLNA"). Host end-to-end (player + the MIPS MPD under qemu): an ICY stream over HTTP (its title reached
`/tmp/diskos-player-stream`) and a self-signed HTTPS stream with Radio Certificates off played; stop removed the
title file.
- MPD's curl input plays `http://` and `https://` streams; HTTPS checks the certificate against `cacert` in mpd.conf,
  the first of `/usr/data/diskos-own/cacert.pem`, `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`,
  `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem` (engine.c `eng_ca_file`). The Disc has
  `/etc/ssl/certs/ca-certificates.crt` (Jo, 2026-11-16); without one, HTTPS stations fail ("not correctly signed by
  the trusted CA") and the player logs it. Settings > Library > Radio Certificates off puts
  `/usr/data/diskos-own/radio-no-cert-check` in place: mpd.conf then gets `verify_peer "no"` and `verify_host "no"`
  instead of `cacert`, and the player restarts MPD before the next station when the file came or went
  (`eng_cert_stale`, `eng_restart`; in radio mode the local song is reloaded from the player's queue anyway). Checked
  under qemu with the real MIPS MPD: a self-signed station plays with the check off and is refused with it on.
- Host-tested 2026-11-16: playback_test (the actions and the mode change) and the real MIPS MPD under qemu-mipsel with a
  null output, driven by the host player over UPnP: an MP3 over HTTP and over HTTPS (own CA) played; with another CA
  bundle the HTTPS stream was refused.
- The stream's rate is only known once MPD has opened it: the DAC is started for 44.1 kHz and started again for the
  stream's rate when MPD reports it (as for a song whose header has no rate). Not checked on a Disc.
- Song titles (2026-11-16): MPD's `Title` (ICY StreamTitle, "Artist - Title") and `Artist` (Ogg comments) of the
  station go to `/tmp/diskos-player-stream` (`uri=`, `artist=`, `title=`) whenever MPD reports a change (idle
  `player`); ui/radio.c reads it every 2 s instead of its own ICY connection (only when the own player runs). Checked
  under qemu: MPD names the ICY title of a test stream.

## After the list ends

Since 2026-11-16 (Jo, for the party mode of Remote Control): Settings > Playback > After the List Ends (cfg
`end_mode`, own player only) says what plays when the last song of a list has ended and the UI queue is empty:
**Stop** (as before), **Shuffle All**, **Last Station** (the radio station played last; cfg `end_st_name`,
`end_st_url`, kept by `radio_play`) or **Playlist** (cfg `end_pl_id`, `end_pl_flags` Repeat 1 / Shuffle 2; set from
Remote Control). ui/radio.c `radio_end_sync` tells the player `0d12` (the station) and `0d11` at its start and after
each change: 0 stop, 1 the UI picks, 2 the station (PLAYER_PROTOCOL.md). When MPD stops after the last song with
nothing next, the player (`playback.c` `list_ended`):
- 1 (Shuffle All, Playlist): stops as usual and sends `ad11`. 2.5 s later, if nothing has started meanwhile (the queue
  going back to a list it interrupted, the user), the UI (`radio_end_list_ended`) plays queued songs first, else
  Shuffle All (play mode Shuffle, all songs from a random one) or the playlist: play mode Shuffle with the Shuffle
  flag (it goes on endlessly, as that play mode always does), else Repeat All with Repeat, else in order, once (when
  that same playlist ends, nothing follows). The UI starts these lists itself so that its queue knows which list it
  plays: songs queued meanwhile play after the current song, and the list goes on after them.
- 2 (Last Station): MPD plays the URL in local work mode (`g_tail`), and the UI gets a local track frame: title = the
  stream's title (ICY), else the station; artist = the station; album "Web radio". A new stream title sends a new
  frame. Play/pause work, seek is refused, Next/Prev start the list again from its first song; any new list ends the
  station. Songs added to the UI queue interrupt the station (ui/queue.c; it would never end), and when they have
  ended the list ends again and the station comes back.
- Streams are not counted in history, statistics or Last.fm (`mdb_is_stream_path`).
- Host-tested (playback_test `test_list_end`); not checked on a Disc.

## S/PDIF (coaxial out)

2026-11-25 (Jo: a TOSLINK cable to an external DAC was silent; the own player ignored 0666 4 and always played to
the analog DAC on hw:0,3). The stock player plays S/PDIF on **hw:0,4** (DMA4) and sets **`LO2_MUX` = 13**
(mq_player `set_out_device` 0x47bf24, out mode 4: the card is found in /proc/asound/cards, device 4; read from the
binary, not measured). The DAC route is DMA3 -> `LO3_MUX` = 12 the same way. The own player now has a fifth MPD
output `spdif` (hw:0,4, 24 bit: the S/PDIF driver refuses S32_LE with "ingenic_spdif_params_check" -22 and takes S24_LE and S16_LE at 44.1 and 48 kHz but not 96 or 192 kHz, Jo's aplay runs 2026-11-25; so two outputs `spdif44` / `spdif48`, picked by the song's rate family in `prepare`, MPD converts 88.2/96/176.4/192 kHz down): 0666 4 sets `spdif` in the core, and AUX then plays on that output with
`LO2_MUX` = 13 instead of starting the DAC (`playback.c` `prepare`, `engine.c` `eng_set_spdif`); 0666 6 goes back.
Bluetooth is left as it is. The AUX volume works as MPD's software volume on that output (`mixer_type "software"`, `setvol` = AUX volume / 120, MPD's curve; set again after an MPD restart), since an external DAC may have none (Jo's has none). The switch is Settings > Audio >
Outputs > Coaxial (S/PDIF) (cfg `spdif`, through the stock form in modes.c: pause, PCM quiet, 0666, 0657 8, play on);
it is off again after a player restart, as the route always was. Host-tested; not yet heard on a Disc.

The Disc's S/PDIF is **coaxial** (electrical) on the 3.5 mm jack: FiiO lists "coaxial output", and a TOSLINK cable stayed dark with hw:0,4 running at 44.1 kHz / 24 bit and `LO2_MUX` = 13 (Jo, 2026-11-25). A 3.5 mm to RCA cable goes to the external DAC's coaxial input. The stock player's S/PDIF switch (`set_spdif_mode_link` 0x4f0ee0) also powers the DAC down (`dac_power_down` 0x4e83b8: `_IO('M',47)` on /dev/cs43131) and up again on leaving it (`_IO('M',48)`); the own player powers the DAC down whenever AUX analog is off (`dac.c` `dac_power_down`); for the coaxial signal itself it is not needed (see "Cable").

**Cable (device-checked 2026-11-25).** The coaxial signal is on the jack's third contact from the tip (ring 2 of a 4-pole TRRS jack). A normal 3-pole stereo plug shorts it to ground with its long sleeve, so the external DAC stays silent; with the plug pulled out a little, Jo's coaxial DAC played from the own player (f21be5e) and from FiiO's stock firmware. A 4-pole 3.5 mm to RCA cable with ring 2 on an RCA centre pin and the sleeve on its shield is needed (check with a continuity tester; AV cables differ). Our earlier stock check with `0666000C0001` sent by hand hung the stock player; its value 1 = S/PDIF, any other = DAC (`player_handle_set_spdif_mode_link`; 4 is only its internal `SPDIF_TX` number). The own player's coax output, with its volume, played on Jo's DAC (f21be5e, 2026-11-25).

**Three outputs (2026-11-25, Jo).** Settings > Audio > Outputs has three independent switches: Bluetooth, AUX analog (the DAC) and AUX digital (S/PDIF), in any combination, none included (`0d01` bits 1/2/4; the stock-form `0666 4` = digital instead of analog). MPD plays to every output that is on (`engine.c` `eng_output(dac, dig, bt, rate)`). AUX analog off powers the DAC down (`_IO('M',47)`); on again, `dac_start` powers it up (`_IO('M',48)`) and starts it, also into a running stream. AUX digital off closes the S/PDIF output and then sets `LO2_MUX` back to 0; on, it sets the route and the output for the rate family before MPD opens it. With no output at all MPD is stopped and the song waits as a paused start at its place (`g_armed`); every later play is held back and bumps `eng_blocked_seq`, which becomes `notice=… 3` in the state file and the toast "No output is on. See Audio > Outputs". AUX digital has its own volume (`0d07`, `vol_dig` in `/usr/data/diskos-own/volumes`, MPD's software volume); the volume keys move all three volumes by the same step, and the volume shown is Bluetooth's when it plays, else analog's, else digital's. Both AUX outputs are on the jack: a plug going in brings back the last AUX choice that had something on (`mem_aux` / `mem_dig`: analog, digital or both; analog until one is made), and the last plug out pauses either. AUX digital is off after a player restart. Analog and digital at the same time is untested on a Disc (both DMAs at once; possible interference of the coax signal on ring 2 with the analog contacts).

**DAC changes under running S/PDIF (device 2026-11-25).** Analog and digital played together, but switching analog off (DAC power-down) left the coax silent with the S/PDIF output still open; analog on again did not bring it back, and digital off then hung MPD (closing the stalled output) until the watchdog rebooted the Disc. The stock player too reopens its PCM after `dac_power_down` (`set_spdif_mode_link`: `close_player`, `open_player`). Since then `playback.c` closes the S/PDIF output in MPD before any DAC power change or DAC start while it runs and opens it again afterwards with its route set anew (`dig_hold` / `dig_release`, `engine.c` `eng_hold_spdif`). And an MPD that gives no answer within 3 s is taken as stuck: `engine.c` kills it at once and starts it anew (`hung`), instead of waiting 3 s for every further command until the 10 s watchdog fires. A slow command (a seek in a slow web stream) can trigger that too; the log says `MPD gave no answer`.

## Next

Web radio: use MPD's ICY title instead of the UI's poll. Bluetooth: bluealsa plugin built into MPD as the primary way (Jo 2026-11-15; needs a static
alsa-lib with the BlueALSA PCM plugin built in and libdbus for musl), the pipe output above stays as its fallback.
