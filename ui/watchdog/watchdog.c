/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 diskOS contributors */
#include "watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* Tests replace the ioctl with a recorder (see tests/test_watchdog.py). */
#ifdef WD_TEST_IOCTL
int wd_test_ioctl(int fd, unsigned long req, void *arg);
#define WD_IOCTL(fd, req, arg) wd_test_ioctl((fd), (req), (arg))
#else
#define WD_IOCTL(fd, req, arg) ioctl((fd), (req), (arg))
#endif

int wd_open(void)
{
    int fd;
    do {
        fd = open(WD_DEV_PATH, O_RDWR | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd < 0 ? -errno : fd;
}

void wd_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

static int wd_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    if (fd < 0)
        return -EBADF;
    do {
        r = WD_IOCTL(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : 0;
}

int wd_start(int fd, int timeout_ms)
{
    if (timeout_ms <= 0)
        return -EINVAL;
    return wd_ioctl(fd, WD_IOC_START, &timeout_ms);
}

int wd_stop(int fd)  { return wd_ioctl(fd, WD_IOC_STOP, 0); }
int wd_feed(int fd)  { return wd_ioctl(fd, WD_IOC_FEED, 0); }
int wd_reset(int fd) { return wd_ioctl(fd, WD_IOC_RESET, 0); }
