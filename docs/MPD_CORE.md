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

## Next

The own player starts MPD as a child, owns the DAC devices (start sequence with the track's rate code before each
stream, volume, balance, gain), translates `0100`/`0201`/`0103` into MPD commands and MPD's status into `a1`/`a2`.
Bluetooth: bluealsa plugin built into MPD, fallback MPD pipe output into `aplay -D bluealsa` (Jo 2026-11-15).
