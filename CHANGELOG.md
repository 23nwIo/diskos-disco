# Changelog

User-facing changes in diskOS Disco!. Details of each release are in [docs/releases](docs/releases/); everything else is in the Git history. diskOS is beta software.

## Unreleased

- diskOS player: the playing list lives in RAM only. Starting an album, a playlist or All Songs no longer writes the
  whole list (5,000+ songs for All Songs) to the library database on the internal flash, and lists of any length work.
  After a restart the list is gone; a resumed song plays on its own. Not yet tested on a Disc.

- Hit&Guess (own player): each song plays on its own by its ID, so the player no longer copies the whole library (5,000+
  rows on the internal flash) for every song of a round. Not yet tested on a Disc.

- Remote Control: the favourite symbol and its colour are chosen separately, on the system page and on the party
  page. The host sets which symbols and colours (name and hex) there are, in `.diskos/state/look.conf` on the card or
  on the system page; defaults ❤️ ⭐ ☭ 𖤐 𐕣 and nine colours including Black. Not yet tested on a Disc.

- Party mode: the queue no longer goes back to the list that played before it. After Hit&Guess that was the whole
  library, so whenever the queue ran empty the Disc went back to the song after the last game song (the same few songs
  again and again) instead of **When the queue is empty** (a radio station, for example). The guests' order is never
  shuffled. With a playing list of more than 4,000 songs, queued songs now also start at the next song change (they
  waited for Next or the end of the list). `/tmp/diskos-queue.log` says why each song plays. Not yet tested on a Disc.

- Queue with the own player: the player keeps the queue in its memory and plays it before its list goes on (party
  mode: then After the List Ends), so queuing no longer writes to the internal flash (no copy of the playing list in
  the music database per queued song, no queue file) and works with libraries of any size. The queue is empty after a
  restart. Repeat One and Single keep their song until Next. Not yet tested on a Disc.

- Music (Stone and Ring): the blurred cover behind Now Playing is now washed with the theme's background colour just
  enough that title, artist, album and times stay readable over any cover: in light themes a dark or colourful cover
  becomes a light tint of its colour (light Stone showed dark text on a darkened cover before), in dark themes a light
  cover becomes a dark one. The queue icon there uses the text colour. Not yet tested on a Disc.

- Disco, Squash and Null: the "Tap title for more" hint now counts as learned once the title opens the song menu (it
  kept coming back on the next starts). Settings › Audio no longer leaves the Outputs rows Auto Outputs and Headphone
  Volume reachable through the settings list when the stock player runs. Not yet tested on a Disc.

- Remote Control party mode: guests get their own link (`/<party name>/`) to a small page where they see what plays,
  see the queue and who queued what, browse the whole library and add single songs; set up on the system page's
  Party tab: volume (scaled to a maximum), what plays now, the queue, skip, songs per guest, guests taking turns,
  repeats, names, and a radio station guests may pick for when the queue is empty. Settings are kept on the SD card.
  After the list ends (own player) can now also play a playlist, once, again and again or shuffled. Not yet tested on
  a Disc. [Guide](docs/REMOTE_CONTROL.md#party-mode)

- Remote Control party mode: guests can upload songs (System › Party › Upload songs to the SD card, off by default,
  with a size limit); they go to `PartyUploads/<party name>/` on the card and into the library at once, not into the
  queue. The host sees each guest's saved radio stations in the guests list and in `/tmp/party-stations.txt`. Not yet
  tested on a Disc. [Guide](docs/REMOTE_CONTROL.md#party-mode)

- Remote Control radio: **⋯ › Add a stream URL…** saves a stream address that the station directory does not have
  (http:// or https://) to the radio favourites; party guests can add one to their own stations in their browser. Not
  yet tested on a Disc.

- Bluetooth headphones: their battery level shows in the device list ("Connected - SBC - 70%"), on their details
  screen and in Remote Control, with a note at 20 %
  and 10 % (Settings > Connectivity > Headphone Battery, on by default; over the hands-free profile). With the own
  player, their Play/Pause, Next and Previous buttons control the Disc, and headphones with absolute volume share the
  Bluetooth volume with the Disc both ways (Settings > Audio > Outputs > Headphone Volume: Bluetooth only, the
  default, or all outputs). Battery checked on a Disc by hand (Hi-X25BT); the build is not yet tested
  on a Disc. [Details](docs/BLUETOOTH.md#headphone-battery-buttons-and-volume)
- Settings > System > Maintenance > Format SD Card: erase the microSD card as FAT32 (one MBR partition, as cards come
  from the factory; for other players and computers) or as ext4 on the whole card without a partition table (diskOS
  only). Shows the card size, needs Erase held for 3 seconds, closes and unmounts the card first (nothing is written if
  it stays in use) and restarts the Disc afterwards. ext4 is made by the firmware's own mke2fs; FAT32 and its
  partition table are written by diskOS itself. Log: /usr/data/sdformat.log. FAT32 checked on card images; not yet
  tested on a Disc.
- Settings > System > Maintenance now also offers Restart and Shut down player (still under Battery & power too). Both
  ask first and close the card safely before the Disc restarts or switches off.
- Radio (in Apps and the app menus): web radio over Wi-Fi. Search thousands of stations (radio-browser.info) or open
  Popular, hold a station to save it as a favourite. The Disc's own player plays the stream (MP3, AAC, Ogg Vorbis,
  Opus, FLAC; HTTP and HTTPS) and the usual Music screen shows it, with the song the station names (ICY, or the Ogg
  comments) checked every 20 seconds. Any song you start returns to normal playback. Not
  yet tested as a build on a Disc. [Guide](docs/RADIO.md)
- Remote Control (Settings > Connectivity, off by default): control playback from a phone or computer browser on the
  same Wi-Fi through a private link (QR code on the Disc): play/pause, skip, seek, volume, balance, outputs (own player) and play mode, see and edit the queue, browse
  albums, artists, playlists, favourites, songs, card folders and radio stations (favourites and the station search) to start playback; switch Bluetooth on/off, search, connect, disconnect, rename and forget devices; plus a small JSON API for scripts. Plain HTTP on your network; New Link revokes old links. Not yet tested on a Disc.
  [Guide](docs/REMOTE_CONTROL.md)
- Bluetooth: give a device its own name on the Disc (hold its row, or Rename on its details screen); it is kept per
  address and never sent to the device. Two devices with the same name show the end of their address, e.g.
  "BT Receiver (A1:B2)". Forgetting a device drops its name.
- microSD cards formatted ext4 (also ext2/ext3), with or without a partition table, mount at start-up (firmware V2.40 and V2.57). The stock
  player cannot mount ext4 itself, so a card inserted while the Disc is on stays unmounted until the next start.
  Leaving USB storage, diskOS mounts an ext4 card itself; only if that fails does the Disc restart. Windows and macOS
  cannot read ext4 in USB storage. After start-up the stock player unmounts the card and fails to mount ext4 again; diskOS then mounts it
  again within a few seconds. Cover files are found ignoring upper/lower case on ext4 cards (COVER.JPG). Tested on a
  Disc (V2.57): start-up mount, USB storage and return.

## 3.4.1

**MA Sendspin speaks Sendspin 1.0, with code pairing, album art and FLAC** (experimental; needs Music Assistant 2.10.7
or newer). Fresh Bluetooth and Wi-Fi screens for Squash and Disco, and fixes for shuffle. [Release notes](docs/releases/v3.4.1.md)

- MA Sendspin: end-to-end encrypted (Noise; Monocypher); pairing with the 6-digit code or QR code the Disc shows, or the
  Pairing Token QR in Settings; Allow Unpaired and Forget Servers; album artwork in Music; FLAC first, PCM still offered.
- Sync Delay: 10 ms steps, − / + buttons, double-tap the slider for 0.
- Squash and Disco: new Bluetooth and Wi-Fi screens (the icon's shade says on or off, a ring that spins while scanning,
  big "Scanning…", taller rows). Disco Quick Settings bubbles sit on an oval.
- A real AirPlay icon instead of the Wi-Fi fan; Working mode shows the same icons in every theme. Sendspin on the Zune
  home dial.
- Fixed: opening Up Next on a large shuffled queue froze the Disc (a 28,619-song shuffle took it down).
- Fixed: Previous on shuffle played some other song: it now goes back to what played, and Next retraces it.
- A stall reporter in the crash log, for hangs that never end.

## 3.4.0

**aptX and aptX HD on the Disc** (experimental). [Release notes](docs/releases/v3.4.0.md) · [Bluetooth guide](docs/BLUETOOTH.md)

- aptX / aptX HD through Disco's own open-source BlueALSA build; Settings › Connectivity › aptX (experimental).
- Bluetooth Codec: SBC, AAC or aptX/LDAC Mobile / Standard / High (LDAC → aptX HD → aptX → AAC → SBC, by what each pair has).
- LDAC always on FiiO's Bluetooth audio; a pair is reconnected once when its best codec needs the other stack.
- Codec badge in Song Info; crash guard; Quiet Wi-Fi for Bluetooth (fewer dropouts).
- Fixed: headphones stuck on AAC after one slow codec switch.
- New Linux app with Fedora and Arch packages; the Windows app redesigned with tabs (both experimental).

## 3.3.1

- Fixed main Settings retaining Wheel geometry after changing List motion in Squash, Disco and Null; ordinary returns preserve scroll position.
- Corrected Braun 2.0/Disc Shelf documentation, package links and licences, and screenshot provenance.
- Fresh matching MIPS UI build. [Release notes](docs/releases/v3.3.1.md)

## 3.3.0

**Braun 2.0**, a new light and dark theme. [Release notes](docs/releases/v3.3.0.md)

- Warm panels, shaded keys with orange icons, Inter type that follows Font Size.
- Home: a wall clock with date, weather, battery and Wi-Fi/Bluetooth glyphs.
- Music: artwork beside or above the text, a radio-dial seek scale with scrubbing, an optional gesture zone.
- Quick Settings with battery and brightness scales; matching Working mode, Shortcuts, Weather, menus and EQ.
- Braun's volume dial with a high/low gain switch.
- Fixed: the colon in Braun's large times and the A–Z wheel's large letter.

## 3.2.2

- Default artwork for songs without a cover.
- Wheel lists open albums and artists with Play and Shuffle in view.
- Fixed: Queue always starts and returns to the list it interrupted; Now Playing with quotes in tags; Squash wheel pills; Zune search and shortcuts (#22).

## 3.2.1

- Smoother Wheel lists; Disco's buttons get some depth.
- Fixed: idle power-off on the charger, power-button shutdown saving stats, Queue under Wheel mode.

## 3.2.0

- Wheel lists, the QS style *Shade*, a letter wheel and A–Z keypad, round track menus, Auto-tag in Song info.
- One header line on every screen; tidier Queue, EQ, Search and volume.
- Fixed: idle power-off, vertical text scrolling, battery use while idle.

## 3.1.0

- Automatic light/dark appearance, menu-edge fades, idle timer and queue/history fixes.

## 3.0.1

- A–Z Focus wheel; Settings reorganised the same way in every theme; idle shutdown fixes.

## 3.0.0

- The **Zune** theme (by namastecoconut-imagined), Immersive 3.0, USB-C audio output in Zune, USB recovery, a new website.

## Older releases

- **2.7.0** — Shuffle! in Disco, Null and Squash; Navidrome downloads.
- **2.6.x** — Fast skipping, Bluetooth headphone handling, Help, crash log.
- **2.5.x** — Windows app, bulk queue and folder queueing, playlists, Immersive and battery improvements; Squash (2.5.0).
- **2.4.0** — The **Null** theme, clearer settings, calmer scrolling.
- **2.3.0 – 2.0.0** — Library, Queue and display additions; the Windows app can flash.
- **1.5.0** — Disc Shelf.
- **1.4.x** — Quick Settings, faster track changes, optional apps; security fixes.
- **1.3.x** — Left-handed mode, list scroll styles, album hold menu, Song info.
- **1.2.x** — Album Roulette built in, smoother scrolling, navigation drawer.
- **1.1.x** — Folder A–Z, MA Sendspin (experimental), Opus, AirPlay and Bluetooth streaming.
- **1.0** — The fork becomes **diskOS Disco!** with the Disco theme.
- **Fork 1.2.x** — SD card updates, hold menus, Ring fixes.
- **diskOS 1.2.0 and earlier** (upstream) — V2.57 support, signed updates, Books view, weather and Last.fm.
