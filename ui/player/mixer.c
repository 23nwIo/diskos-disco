/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "mixer.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>  /* struct timespec, needed by sound/asound.h */
#include <sound/asound.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int mix_set_enum(const char *dev, const char *name, unsigned value, mix_log_fn log){
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if(fd < 0){ int e = errno; log("mixer: open %s: %s", dev, strerror(e)); return -e; }
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    v.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)v.id.name, name, sizeof v.id.name - 1);
    int rc = 0;
    if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) != 0){ rc = -errno; log("mixer: read %s: %s", name, strerror(-rc)); }
    else if(v.value.enumerated.item[0] != value){
        unsigned old = v.value.enumerated.item[0];
        v.value.enumerated.item[0] = value;
        if(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0){ rc = -errno; log("mixer: %s = %u: %s", name, value, strerror(-rc)); }
        else log("mixer: %s %u -> %u", name, old, value);
    }
    close(fd);
    return rc;
}
