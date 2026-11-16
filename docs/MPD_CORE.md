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
- **Queue** (`queue.c`, `queuedb.c`, `playback.c`, 2026-11-15): on `0100` the player rebuilds `LIST_SONG_0` in
  song.db exactly like the stock player (DROP and CREATE with the stock schema, so the IDs start at 1;
  `INSERT ... SELECT` from `SONG`, `MY_LOVE` or `CUSTOM_PLAYLIST` with the stock filter and `ORDER BY`), and in shuffle
  writes `LIST_SONG_3` (one `POS_ID` per song, the tapped song first). The UI's Up Next and positions read these
  tables, so they must stay identical (`ui/tests/playback_test.c` checks the orderings against ui/musicdb.c).
- **MPD holds two songs only**: the current one and the one after it (for gapless). The queue itself stays the
  player's, so a library of tens of thousands of songs is never copied into MPD. When MPD moves on to the queued
  song, the player queues the next one by the play mode: in order (stop after the last), shuffle and repeat all (wrap
  around), repeat one (the same song again), single (stop after it). Next/Previous always move one song, wrapping.
- **Frames**: every track start sends the stock sequence (`a102` mode on a play command or `{"state":1}` on a natural
  change, `aa1c`, the full `a2` with the song JSON and `pos_id` = `LIST_SONG_0.ID`, `aa05`, `aa1c`, `{"state":0}`
  twice); `0202` with a track loaded is answered with the full `a2`.
- **Resume** (`MEMORY_PLAY`, mode `0684`): see docs/MQ_PLAYER_REPLACEMENT.md, "Resume (MEMORY_PLAY)".
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
- **Outputs** (2026-11-15, host-tested, not yet on a Disc; Settings > Audio > Outputs, own player only). MPD plays
  to every output that is on: AUX (the DAC, i.e. both jacks), Bluetooth, or both at once (Bluetooth is a few hundred
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
  else AUX), and the volume keys and `0715` move every output that is on by the same step. When the DAC joins a
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

Since 2026-11-16 (Jo asked for it): the Radio app (RADIO.md) switches the player to work mode 0C and hands it the
station over UPnP AVTransport on `127.0.0.1:49494`, as it does with the stock player's DLNA renderer. The own player
opens that port only in work mode 0C (`ui/player/upnp.c`, local only, no SSDP) and plays the URL in MPD
(`playback.c` `pb_stream_*`): the local song is paused and its place kept; leaving the mode stops the station and loads
the local song again, paused. The Play key pauses and plays on; skip and seek do nothing. The UI gets one track frame
for the station (`song_name` = the station, `work_mode` "DLNA").
- MPD's curl input plays `http://` and `https://` streams; HTTPS checks the certificate against `cacert` in mpd.conf,
  the first of `/usr/data/diskos-own/cacert.pem`, `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`,
  `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/ca-bundle.pem` (engine.c `eng_ca_file`). Which one the Disc has is not
  checked yet; without one, HTTPS stations fail ("not correctly signed by the trusted CA") and the player logs it.
- Host-tested 2026-11-16: playback_test (the actions and the mode change) and the real MIPS MPD under qemu-mipsel with a
  null output, driven by the host player over UPnP: an MP3 over HTTP and over HTTPS (own CA) played; with another CA
  bundle the HTTPS stream was refused.
- The stream's rate is only known once MPD has opened it: the DAC is started for 44.1 kHz and started again for the
  stream's rate when MPD reports it (as for a song whose header has no rate). Not checked on a Disc.
- The song titles still come from the UI's own ICY poll (RADIO.md); MPD has them too (`Title` from ICY), not used yet.

## Next

Web radio: use MPD's ICY title instead of the UI's poll. Bluetooth: bluealsa plugin built into MPD as the primary way (Jo 2026-11-15; needs a static
alsa-lib with the BlueALSA PCM plugin built in and libdbus for musl), the pipe output above stays as its fallback.
