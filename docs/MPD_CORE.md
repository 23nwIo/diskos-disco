# MPD as the own player's audio core

Decided 2026-11-15 (Jo): the own player (ui/player, docs/MQ_PLAYER_REPLACEMENT.md) keeps the FiiO protocol, the DAC,
volume, balance, gain, keys and power; MPD only decodes, seeks and plays gapless. The EQ is on todo.md. The library
stays diskOS's own (musicdb); MPD runs without a database and gets songs as `file://` URIs over its local socket.

## Build

`mpd/build.sh <toolchain-with-c++> <work-dir>`: MPD 0.23.17 static for MIPS32r2/FP64/NaN2008 (3.3 MB stripped), with
alsa-lib 1.2.12, FLAC 1.4.3, a small FFmpeg 6.1 (AAC, ALAC, APE, WavPack, Vorbis, Opus, MP3, WMA, PCM), fmt 10.2.1.
The diskOS toolchain is C only; the script header says how to add g++ 11.2 with the same ABI. Our MPD patches are in
`mpd/patches/`.

## Device findings (2026-11-15, Jo's runs)

- Memory: VmRSS 6.6 MB, VmHWM 6.8 MB while playing FLAC (MemAvailable on the Disc is about 75 MB).
- `hw:0,3` (the stock player's PCM) refuses every `buffer_time` at 44.1 kHz when it is set before the period, and
  accepts it the other way round (`mpd/alsaprobe.c`). Patch `0001-alsa-period-first-fallback.patch`.
- The stock player always opens `hw:0,3` as **S32_LE** (trace: format mask bit 10; `/proc/asound/card0/pcm3p/sub0/hw_params`
  while it plays: S32_LE, 48000 Hz, period 1920, buffer 3840 frames). It played a 44.1 kHz FLAC at 48 kHz, so it seems
  to resample to 48 kHz (to be checked with other files). MPD config: `format "*:32:*"`.
- The mixer does not change between idle and playing (`LO3_MUX` = LI11 stays); no mixer write is needed for the DAC.
- **The DAC must be started and its devices kept open.** Without the stock start sequence the output is silent; with
  it but the devices closed again, there is only a hum and the DMA runs far faster than real time (no I2S clock). With
  the sequence and `/dev/cs43131` + `/dev/cs43131c` held open (`mpd/dacstart.c`), MPD plays in real time and is
  audible. Sequence (trace run 1, 578 s): `_IO('M',48)` on A once, A M56 = 3, C M60 = 3, 0.4 s, both 0, A M52 / C M54 =
  rate code (1 = 44.1 kHz, 2 = 48, 4 = 96, 6 = 192), A M41 / C M50 = 0, volume A M45 + M66 / C M47 + M68 (attenuation
  byte), A `_IO('M',46)`; then the PCM is opened.
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
- Not yet: CUE and ISO tracks (a part of a bigger file; skipped with a log line), resume after a restart
  (`MEMORY_PLAY`), Previous restarting the song after a few seconds (stock behaviour not measured).

## Next

Resume (`MEMORY_PLAY`), CUE/ISO tracks, then web radio via MPD streams. Bluetooth: bluealsa plugin built into MPD, fallback MPD pipe output into `aplay -D bluealsa` (Jo 2026-11-15).
