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

## Known failures (2026-11-20)

All of the following already failed in upstream Disco 3.4.1 (`1cb1828`), where this fork started; they are listed in
`KNOWN_FAILURES` with the reason:

- `display_pass_test`, `review_ui_test`, `render_test` (immersive part): `host-render` assertions about the Display
  pages, Home after a preview and the immersive cover no longer match the UI.
- `theme_efficiency_test`, `zune_scroll_test`: pixel baselines in `ui/tests/fixtures/` older than the rendering.
- `settings_alignment_test`: Display rows changed upstream; since our Outputs work (bb8c0d2, 06b4c07) the Outputs page
  is also listed without the own player and has a Headphone Volume row the test does not expect.

## Writing a test

- A C test with its own `main`: add a target to `ui/tests/Makefile` and to `run` (or `extra`), and the binary name to
  `clean`. Test binaries end in `_test` and are ignored by git (`ui/tests/.gitignore`).
- A Python test: name it `ui/tests/<something>_test.py`; `run_pytests.py` finds it. Exit non-zero on failure; print a
  line starting with `SKIP` and exit 0 when an optional tool is missing. Build what it needs itself, or say in its
  docstring which make target it expects.
- A test that cuts a slice out of production code breaks when that code calls something new. Then add a stand-in for
  the new call in the test (see `launcher_test.py`, `idle_shutdown_test.py`), never weaken the checks.
- Run `make -C ui/tests test` before committing a change to code that a test covers.
