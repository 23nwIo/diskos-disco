/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "usbstorage.h"
#include "sdcard.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define CFS      "/sys/kernel/config"
#define GADGET   CFS "/usb_gadget/storage_demo"
#define UDC_NAME "13500000.otg_new"
#define ROLE     "/sys/class/usb_role/13500000.otg_new-role-switch/role"

static int g_active;

int us_active(void){ return g_active; }

static int put(const char *path, const char *val, int dry, us_log_fn log){
    if(dry){ log("usb: (dry run) %s = %s", path, val); return 0; }
    FILE *f = fopen(path, "we");
    if(!f){ log("usb: open %s: %s", path, strerror(errno)); return -1; }
    int ok = fputs(val, f) >= 0;
    int e = errno;
    if(fclose(f) != 0){ ok = 0; e = errno; }      /* sysfs/configfs report a refused value on the write at close */
    if(!ok){ log("usb: write \"%s\" to %s: %s", val, path, strerror(e)); return -1; }
    return 0;
}
static int mk(const char *path, int dry, us_log_fn log){
    if(dry) return 0;
    if(mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    log("usb: mkdir %s: %s", path, strerror(errno));
    return -1;
}
static int configfs_mount(int dry, us_log_fn log){
    if(dry) return 0;
    if(mount("none", CFS, "configfs", 0, NULL) == 0 || errno == EBUSY) return 0;   /* EBUSY: already mounted */
    log("usb: mount configfs on %s: %s", CFS, strerror(errno));
    return -1;
}

int us_start(int dry, us_log_fn log){
    if(g_active){ log("usb: storage already active"); return 0; }
    /* the card must not be mounted here while the host writes to it */
    if(!dry && sd_is_mounted()){
        sync();
        if(umount(SD_MOUNTPOINT) != 0){
            log("usb: umount %s: %s - storage mode not started", SD_MOUNTPOINT, strerror(errno));
            return -1;
        }
        log("usb: card unmounted");
    }
    int bad = 0;
    bad |= put(ROLE, "device", dry, log);
    bad |= configfs_mount(dry, log);
    /* the values of the stock player (trace run 5) */
    static const char *const dirs[] = {
        GADGET, GADGET "/strings/0x409", GADGET "/configs/c.1", GADGET "/configs/c.1/strings/0x409",
        GADGET "/functions/mass_storage.0",
    };
    for(unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) bad |= mk(dirs[i], dry, log);
    static const char *const kv[][2] = {
        { GADGET "/bDeviceClass", "0x00" }, { GADGET "/bDeviceSubClass", "0x00" },
        { GADGET "/bDeviceProtocol", "0x00" }, { GADGET "/bcdUSB", "0x200" }, { GADGET "/bcdDevice", "0x100" },
        { GADGET "/idVendor", "0x0525" }, { GADGET "/idProduct", "0xa4a5" },
        { GADGET "/strings/0x409/manufacturer", "ingenic" }, { GADGET "/strings/0x409/product", "composite-storage" },
        { GADGET "/strings/0x409/serialnumber", "ingenic-storage" },
        { GADGET "/configs/c.1/MaxPower", "50" }, { GADGET "/configs/c.1/bmAttributes", "0x80" },
        { GADGET "/configs/c.1/strings/0x409/configuration", "storage" },
        { GADGET "/functions/mass_storage.0/stall", "0" },
        { GADGET "/functions/mass_storage.0/lun.0/cdrom", "0" },
        { GADGET "/functions/mass_storage.0/lun.0/nofua", "0" },
        { GADGET "/functions/mass_storage.0/lun.0/removable", "1" },
        { GADGET "/functions/mass_storage.0/lun.0/ro", "0" },
        { GADGET "/functions/mass_storage.0/lun.0/file", "/dev/mmcblk0" },
    };
    for(unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++) bad |= put(kv[i][0], kv[i][1], dry, log);
    if(!dry && symlink(GADGET "/functions/mass_storage.0", GADGET "/configs/c.1/mass_storage.0") != 0 && errno != EEXIST){
        log("usb: link mass_storage.0 into c.1: %s", strerror(errno)); bad = 1;
    }
    if(!bad) bad |= put(GADGET "/UDC", UDC_NAME, dry, log);
    if(bad){
        log("usb: storage mode failed, card back to the player");
        g_active = 1;              /* so us_stop undoes whatever part did happen */
        us_stop(dry, log);
        return -1;
    }
    g_active = 1;
    log("usb: storage mode on, card exported as /dev/mmcblk0");
    return 0;
}

void us_stop(int dry, us_log_fn log){
    if(!g_active) return;
    /* A write of "\n" to UDC unbinds the gadget (the kernel strips the newline; an empty name means unbind). The
     * stock player's trace shows only an fopen of UDC for writing and no write; we unbind explicitly, then release
     * the card from the LUN, so the host is gone before the card is mounted here again. */
    put(GADGET "/UDC", "\n", dry, log);
    put(GADGET "/functions/mass_storage.0/lun.0/file", "\n", dry, log);
    if(!dry && umount(CFS) != 0) log("usb: umount %s: %s", CFS, strerror(errno));
    g_active = 0;
    log("usb: storage mode off");
    if(!dry) sd_mount(log);
}
