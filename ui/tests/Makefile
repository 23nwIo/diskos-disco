# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Off-device host tests for diskOS pure-logic modules (arch#5 harness).
# Native x86-64 build with sanitizers + CFG_* paths pointed at a temp dir. Does NOT touch the
# device build (separate dir, separate Makefile). See docs/TESTING.md.
#   make -C ui/tests test      everything below, in this order (stops at the first failing group)
#   make -C ui/tests run       the C unit tests
#   make -C ui/tests review    fileops + app shortcut tests
#   make -C ui/tests extra     further C tests (shuffle history, log rotation, song.db reads, cover upscale, Sendspin crypto)
#   make -C ui/tests pytests   every *_test.py through run_pytests.py (builds host-render, sendspin-host, mpd_test first)
CC ?= cc
CFLAGS := -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
TDIR := /tmp/diskos_cfgtest
DEFS := -DCFG_PATH='"$(TDIR)/diskos.conf"' -DCFG_DIR='"$(TDIR)"' \
        -DLEGACY_CONF='"$(TDIR)/nolegacy1"' -DLEGACY_SWIPE='"$(TDIR)/nolegacy2"'

config_test: ../config.c config_test.c ../config.h
	$(CC) $(CFLAGS) $(DEFS) -I.. ../config.c config_test.c -o $@

playstate_test: ../playstate.c playstate_test.c ../playstate.h
	$(CC) $(CFLAGS) -I.. ../playstate.c playstate_test.c -o $@

fileops_test: fileops_test.c ../fileops.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -ffunction-sections -fdata-sections -I.. $< -Wl,--gc-sections -pthread -o $@

apps_shortcut_test: apps_shortcut_test.c ../apps.c ../config.c ../md5.c
	$(CC) $(CFLAGS) $(subst diskos_cfgtest,diskos_shortcuttest,$(DEFS)) -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE -ffunction-sections -fdata-sections -I.. $< ../config.c ../md5.c -Wl,--gc-sections -o $@

remote_test: remote_test.c ../remote_core.c ../remote_page.c ../remote_srv.c ../hitguess_core.c ../remote_core.h ../remote_srv.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. remote_test.c ../remote_core.c ../remote_page.c ../remote_srv.c ../hitguess_core.c -pthread -o $@

mpd_test: mpd_test.c ../mpd_core.c ../mpd_srv.c ../remote_core.c ../hitguess_core.c ../mpd_core.h ../mpd_srv.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. mpd_test.c ../mpd_core.c ../mpd_srv.c ../remote_core.c ../hitguess_core.c -pthread -o $@

wifi_reco_test: wifi_reco_test.c ../wifi_reco.h
	$(CC) $(CFLAGS) -I.. wifi_reco_test.c -o $@

wifi_override_test: wifi_override_test.c ../wifi_override.h
	$(CC) $(CFLAGS) -I.. wifi_override_test.c -o $@

wd_override_test: wd_override_test.c ../wd_override.h
	$(CC) $(CFLAGS) -I.. wd_override_test.c -o $@
trace_launch_test: trace_launch_test.c ../trace_launch.h ../wd_override.h
	$(CC) $(CFLAGS) -I.. trace_launch_test.c -o $@
own_player_test: own_player_test.c ../own_player.h
	$(CC) $(CFLAGS) -I.. own_player_test.c -o $@

PLAYER_SRCS := ../player/core.c ../player/proto.c ../player/sdcard.c ../player/usbstorage.c ../player/dac.c ../player/mpdcli.c ../player/engine.c ../player/queue.c ../player/queuedb.c ../player/playback.c ../player/mixer.c ../player/btout.c ../player/btavrcp.c ../dbusmini.c
# SQLite for the player, without sanitizers (instrumenting the amalgamation only slows the tests)
sqlite3_host.o: ../sqlite3.c
	$(CC) -O1 -g -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c $< -o $@
diskos-player-host: ../player/main.c ../player/hw.c ../player/hw.h $(PLAYER_SRCS) sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/core.h ../player/proto.h ../own_player.h ../watchdog/watchdog.c ../watchdog/watchdog.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. ../player/main.c ../player/hw.c $(PLAYER_SRCS) ../watchdog/watchdog.c sqlite3_host.o -lrt -lpthread -lm -o $@
playback_test: playback_test.c ../player/core.c ../player/proto.c ../player/queue.c ../player/queuedb.c ../player/playback.c sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/engine.h ../player/core.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DPB_RESUME=1 -DPB_UP_STREAMS=\"/tmp/pb_test_up_streams\" -I.. playback_test.c ../player/core.c ../player/proto.c ../player/queue.c ../player/queuedb.c ../player/playback.c sqlite3_host.o -lpthread -lm -o $@
player_test: player_test.c $(PLAYER_SRCS) ../player/hw.c sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/core.h ../player/proto.h ../player/hw.h ../player/sdcard.h ../player/usbstorage.h ../player/dac.h ../player/mpdcli.h ../player/engine.h diskos-player-host
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. player_test.c $(PLAYER_SRCS) ../player/hw.c ../watchdog/watchdog.c sqlite3_host.o -lrt -lpthread -lm -o $@

# the D-Bus client and the headphones' buttons / absolute volume against a real dbus-daemon (skipped without one)
btavrcp_test: btavrcp_test.c ../dbusmini.c ../dbusmini.h ../player/btavrcp.c ../player/btavrcp.h ../btbatt.c ../btbatt.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. btavrcp_test.c ../dbusmini.c ../player/btavrcp.c ../btbatt.c -o $@

bandcamp_test: bandcamp_test.c ../bandcamp_core.c ../bandcamp_core.h ../jsmn.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. bandcamp_test.c ../bandcamp_core.c -o $@
# bandcamp.c with a stand-in wget (fixtures/bandcamp/wget) and stubs for the UI around it
bandcamp_app_test: bandcamp_app_test.c ../bandcamp.c ../bandcamp.h ../bandcamp_core.c ../bandcamp_core.h ../remote_core.c ../hitguess_core.c ../command_spawn.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DBC_OPL_RUNNING=test_opl_running -DBC_CARD_MOUNTED=test_card_mounted -DBC_GUEST_SAVED=test_guest_saved -DBC_CARD_ROOT='"/tmp/bc_test_card"' -I.. bandcamp_app_test.c ../bandcamp.c ../bandcamp_core.c ../remote_core.c ../hitguess_core.c ../command_spawn.c -pthread -o $@
radio_test: radio_test.c ../radio_core.c ../radio_core.h ../jsmn.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. radio_test.c ../radio_core.c -o $@

.PHONY: review
review: fileops_test apps_shortcut_test
	./fileops_test
	./apps_shortcut_test

.PHONY: test extra pytests
test: run review extra pytests

EXTRA := shufhist_test logrotate_test db_read_test soft_backdrop_test
extra: $(EXTRA)
	./shufhist_test
	./logrotate_test
	./db_read_test
	./soft_backdrop_test
	$(MAKE) -C .. CROSS= sendspin-crypto-test

shufhist_test: shufhist_test.c ../shufhist.c ../shufhist.h
	$(CC) $(CFLAGS) -I.. shufhist_test.c ../shufhist.c -o $@
logrotate_test: logrotate_test.c ../logrotate.h
	$(CC) $(CFLAGS) -I.. logrotate_test.c -o $@
# musicdb.c is #included by the test; jsmn_impl.c supplies the JSON parser it is built against (JSMN_HEADER)
db_read_test: db_read_test.c ../musicdb.c ../musicdb.h ../bookmarks.c ../sdstate.c jsmn_impl.c sqlite3_host.o
	$(CC) $(CFLAGS) -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function -D_GNU_SOURCE -DSDS_NO_DEVICE -I.. db_read_test.c jsmn_impl.c ../bookmarks.c ../sdstate.c sqlite3_host.o -lpthread -lm -ldl -o $@
soft_backdrop_test: soft_backdrop_test.c ../tools/diskos_artdec.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. soft_backdrop_test.c -lm -lpthread -o $@

# Python tests: the binaries they expect, then all of them, one after another (run_pytests.py)
pytests: mpd_test
	$(MAKE) -C .. CROSS= host-render sendspin-host
	python3 run_pytests.py

.PHONY: run clean
run: btavrcp_test config_test playstate_test remote_test mpd_test radio_test bandcamp_test bandcamp_app_test wifi_reco_test wifi_override_test wd_override_test trace_launch_test own_player_test player_test playback_test usbsync_core_test bookmarks_test playlog_test scanner_date_test hitguess_test
	./config_test
	@echo "--- playstate ---"
	./playstate_test
	@echo "--- remote ---"
	./remote_test
	@echo "--- mpd ---"
	./mpd_test
	@echo "--- radio ---"
	./radio_test
	./bandcamp_test
	./bandcamp_app_test
	@echo "--- wifi boot hold ---"
	./wifi_reco_test
	@echo "--- wifi_up.sh override ---"
	./wifi_override_test
	@echo "--- cmd_watchdog override ---"
	./wd_override_test
	./trace_launch_test
	./own_player_test
	./btavrcp_test
	@echo "--- own player skeleton ---"
	./player_test ./diskos-player-host
	@echo "--- own player queue and playback ---"
	./playback_test
	@echo "--- USB import/export (USC_TEST_RSYNC=path also runs a real rsync) ---"
	./usbsync_core_test
	@echo "--- audiobook bookmarks on the card ---"
	./bookmarks_test
	./playlog_test
	@echo "--- scanner date tags ---"
	./scanner_date_test
	@echo "--- hitguess rules ---"
	./hitguess_test

clean:
	rm -f btavrcp_test config_test playstate_test fileops_test apps_shortcut_test remote_test mpd_test radio_test bandcamp_test bandcamp_app_test wifi_reco_test wifi_override_test wd_override_test trace_launch_test own_player_test player_test playback_test diskos-player-host sqlite3_host.o usbsync_core_test bookmarks_test playlog_test scanner_date_test hitguess_test $(EXTRA)

usbsync_core_test: usbsync_core_test.c ../usbsync_core.c ../usbdrive_core.c ../usbsync_core.h ../usbdrive_core.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. usbsync_core_test.c ../usbsync_core.c ../usbdrive_core.c -o $@

hitguess_test: hitguess_test.c ../hitguess_core.c ../hitguess_core.h
	$(CC) $(CFLAGS) -I.. hitguess_test.c ../hitguess_core.c -o $@

scanner_date_test: scanner_date_test.c ../scanner.c ../scanner.h sqlite3_host.o
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. scanner_date_test.c sqlite3_host.o -pthread -ldl -lm -o $@

playlog_test: playlog_test.c ../playlog.c ../playlog.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. playlog_test.c ../playlog.c -o $@
bookmarks_test: bookmarks_test.c ../bookmarks.c ../sdstate.c ../bookmarks.h ../sdstate.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DSDS_NO_DEVICE -I.. bookmarks_test.c ../bookmarks.c ../sdstate.c -lpthread -o $@
