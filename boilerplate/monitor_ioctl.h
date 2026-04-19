#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 0xCE

struct monitor_request {
    char          container_id[64];
    int           pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

/* Used by engine.c to open the device */
#define DEVICE_NAME "container_monitor"

#endif /* MONITOR_IOCTL_H */
