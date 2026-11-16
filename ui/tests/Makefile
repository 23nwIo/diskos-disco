# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# Off-device host tests for diskOS pure-logic modules (arch#5 harness).
# Native x86-64 build with sanitizers + CFG_* paths pointed at a temp dir. Does NOT touch the
# device build (separate dir, separate Makefile). Run: make -C tests run
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

remote_test: remote_test.c ../remote_core.c ../remote_page.c ../remote_srv.c ../remote_core.h ../remote_srv.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. remote_test.c ../remote_core.c ../remote_page.c ../remote_srv.c -pthread -o $@

mpd_test: mpd_test.c ../mpd_core.c ../mpd_srv.c ../remote_core.c ../mpd_core.h ../mpd_srv.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. mpd_test.c ../mpd_core.c ../mpd_srv.c ../remote_core.c -pthread -o $@

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

PLAYER_SRCS := ../player/core.c ../player/proto.c ../player/sdcard.c ../player/usbstorage.c ../player/dac.c ../player/mpdcli.c ../player/engine.c ../player/queue.c ../player/queuedb.c ../player/playback.c ../player/mixer.c ../player/btout.c
# SQLite for the player, without sanitizers (instrumenting the amalgamation only slows the tests)
sqlite3_host.o: ../sqlite3.c
	$(CC) -O1 -g -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c $< -o $@
diskos-player-host: ../player/main.c ../player/hw.c ../player/hw.h $(PLAYER_SRCS) sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/core.h ../player/proto.h ../own_player.h ../watchdog/watchdog.c ../watchdog/watchdog.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. ../player/main.c ../player/hw.c $(PLAYER_SRCS) ../watchdog/watchdog.c sqlite3_host.o -lrt -lpthread -lm -o $@
playback_test: playback_test.c ../player/core.c ../player/proto.c ../player/queue.c ../player/queuedb.c ../player/playback.c sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/engine.h ../player/core.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DPB_RESUME=1 -I.. playback_test.c ../player/core.c ../player/proto.c ../player/queue.c ../player/queuedb.c ../player/playback.c sqlite3_host.o -lpthread -lm -o $@
player_test: player_test.c $(PLAYER_SRCS) ../player/hw.c sqlite3_host.o ../player/queue.h ../player/queuedb.h ../player/playback.h ../player/core.h ../player/proto.h ../player/hw.h ../player/sdcard.h ../player/usbstorage.h ../player/dac.h ../player/mpdcli.h ../player/engine.h diskos-player-host
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. player_test.c $(PLAYER_SRCS) ../player/hw.c ../watchdog/watchdog.c sqlite3_host.o -lrt -lpthread -lm -o $@

radio_test: radio_test.c ../radio_core.c ../radio_core.h ../jsmn.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. radio_test.c ../radio_core.c -o $@

.PHONY: review
review: fileops_test apps_shortcut_test
	./fileops_test
	./apps_shortcut_test

.PHONY: run clean
run: config_test playstate_test remote_test mpd_test radio_test wifi_reco_test wifi_override_test wd_override_test trace_launch_test own_player_test player_test playback_test usbsync_core_test bookmarks_test
	./config_test
	@echo "--- playstate ---"
	./playstate_test
	@echo "--- remote ---"
	./remote_test
	@echo "--- mpd ---"
	./mpd_test
	@echo "--- radio ---"
	./radio_test
	@echo "--- wifi boot hold ---"
	./wifi_reco_test
	@echo "--- wifi_up.sh override ---"
	./wifi_override_test
	@echo "--- cmd_watchdog override ---"
	./wd_override_test
	./trace_launch_test
	./own_player_test
	@echo "--- own player skeleton ---"
	./player_test ./diskos-player-host
	@echo "--- own player queue and playback ---"
	./playback_test
	@echo "--- USB import/export (USC_TEST_RSYNC=path also runs a real rsync) ---"
	./usbsync_core_test
	@echo "--- audiobook bookmarks on the card ---"
	./bookmarks_test

clean:
	rm -f config_test playstate_test fileops_test apps_shortcut_test remote_test mpd_test radio_test wifi_reco_test wifi_override_test wd_override_test trace_launch_test own_player_test player_test playback_test diskos-player-host sqlite3_host.o usbsync_core_test bookmarks_test

usbsync_core_test: usbsync_core_test.c ../usbsync_core.c ../usbdrive_core.c ../usbsync_core.h ../usbdrive_core.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I.. usbsync_core_test.c ../usbsync_core.c ../usbdrive_core.c -o $@

bookmarks_test: bookmarks_test.c ../bookmarks.c ../sdstate.c ../bookmarks.h ../sdstate.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DSDS_NO_DEVICE -I.. bookmarks_test.c ../bookmarks.c ../sdstate.c -lpthread -o $@
