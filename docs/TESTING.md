# Host tests

All tests in `ui/tests/` run on a Linux PC (x86-64), not on the Disc. They compile production code natively (most
under AddressSanitizer/UBSan), often a slice cut out of `main.c` or `ui.c` with small stand-ins for the device, and
check it. A passing host test says nothing about the MIPS build or the device: see `docs/DEV_WORKFLOW.md`.

## Run everything

```sh
make -C ui/tests test
```

That runs, in this order (it stops at the first group that fails):

| Target | What |
|---|---|
| `make -C ui/tests run` | The C unit tests (config, playstate, Remote Control, MPD, radio, Wi-Fi/watchdog/trace/own player switches, own player skeleton and playback, USB import/export, bookmarks, play log, scanner dates, Hit&Guess). |
| `make -C ui/tests review` | File operations (folder replacement) and app shortcuts. |
| `make -C ui/tests extra` | Shuffle history, `/tmp` log rotation, `song.db` reads under a lock, cover upscale, Sendspin crypto. |
| `make -C ui/tests pytests` | Every `ui/tests/*_test.py`, one after another, through `ui/tests/run_pytests.py`. Builds `host-render`, `sendspin-host` and `mpd_test` first. Takes about 5 minutes. |

Only some Python tests: `python3 ui/tests/run_pytests.py queue history` runs those whose name contains one of the
words (build `host-render` first: `make -C ui CROSS= host-render`). A single test also runs directly, e.g.
`python3 ui/tests/queue_test.py`.

`run_pytests.py` prints one line per test, then the last lines of every failing test's output; the whole output of
each test is in `/tmp/diskos-tests/<name>.log` (`DISKOS_TEST_LOGS` changes the directory, `DISKOS_TEST_TIMEOUT` the
900 s limit per test). Results:

- `PASS`, `FAIL`, `TIMEOUT`
- `SKIP`: exit 0 with a line starting with `SKIP` (an optional Python module is missing).
- `KNOWN`: fails for a reason listed in `KNOWN_FAILURES` in `run_pytests.py`. It does not fail the run.
- `FIXED`: listed in `KNOWN_FAILURES` but passes now. Fails the run until it is taken off the list.

## What the PC needs

- gcc (or `CC=clang`), make, python3 with Pillow (`python3-pil`), e2fsprogs (`mke2fs`, `e2fsck`, `blkid`),
  `dbus-daemon` (`btavrcp_test` skips its D-Bus part without one), the DejaVu fonts (`fonts-dejavu-core`).
- Optional, else the test reports `SKIP`: `pip install python-mpd2` (`mpd_client_test.py`),
  `pip install "aiosendspin[server]"` (`sendspin_interop_test.py`). `USC_TEST_RSYNC=/path/to/rsync` also runs a real
  rsync in `usbsync_core_test`.
- **Root, in a container or VM.** `hitguess_e2e_test.py`, `mpd_library_test.py` and `remote_library_test.py` bind-mount
  `/tmp/sdcard` and replace `/usr/data/fiio/db/song.db`, the Disc's paths. `sdformat_test.py` and `sdfscheck_test.py`
  also loop-mount images when run as root. Do not run the suite as root on a PC where those paths mean something.

The tests share `/tmp/sdcard`, `ui/tests/host_build/` and a few `/tmp` paths, so never run two at once.

## Known failures

None (2026-11-20). When one is added to `KNOWN_FAILURES`, list it here with the reason.

## Pixel baselines

`theme_efficiency_test.py` (list and menu animations, Home, Music, Quick Settings and Settings in every theme) and
`zune_scroll_test.py` (Zune list scrolling) compare every frame bit for bit with a recorded run in
`ui/tests/fixtures/`. Any pixel change fails them, wanted or not. After an intended visual change:

1. Look at the changed screens (render them with `ui/tests/host_build/host-render PRESET VARIANT preview-home 0 out.ppm`
   and the other `preview-*` pages).
2. Re-record: `python3 ui/tests/theme_efficiency_test.py --record-baseline` and/or
   `python3 ui/tests/zune_scroll_test.py --record-baseline`.
3. Commit the fixtures together with the change and say why in the message.

Never re-record to make a difference go away that nobody can explain. Both were recorded on 2026-11-20 (after
`fc36fcf`), replacing baselines older than upstream 3.4.1; `theme_efficiency_test` again the same day for the washed
Now Playing backdrop.

## Readability

`np_contrast_test.py` renders Now Playing in every theme, light and dark, over a black, a white, an orange and a busy
multicolour cover, and measures the WCAG contrast of title, artist and album against the pixels behind them
(`REVIEW_NP_CONTRAST=1 host-render PRESET VARIANT preview-np 0 out.ppm` prints the values; the cover is `out.png`).
It fails below 4.5. When it fails, fix the colours or the backdrop wash (`np_backdrop_wash()` in `ui/ui.c`), not
the limit.

## Writing a test

- A C test with its own `main`: add a target to `ui/tests/Makefile` and to `run` (or `extra`), and the binary name to
  `clean`. Test binaries end in `_test` and are ignored by git (`ui/tests/.gitignore`).
- A Python test: name it `ui/tests/<something>_test.py`; `run_pytests.py` finds it. Exit non-zero on failure; print a
  line starting with `SKIP` and exit 0 when an optional tool is missing. Build what it needs itself, or say in its
  docstring which make target it expects.
- A test that cuts a slice out of production code breaks when that code calls something new. Then add a stand-in for
  the new call in the test (see `launcher_test.py`, `idle_shutdown_test.py`), never weaken the checks.
- Run `make -C ui/tests test` before committing a change to code that a test covers.
