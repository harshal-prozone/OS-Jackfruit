/* monitor_ioctl.h - Shared interface between engine.c and monitor.c */
#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_MAGIC    'M'
#define MONITOR_NAME_LEN  64

/* Used by MONITOR_REGISTER */
struct container_reg {
    pid_t  pid;
    char   id[MONITOR_NAME_LEN];
    long   soft_limit_bytes;
    long   hard_limit_bytes;
};

/* Used by MONITOR_UNREGISTER */
struct container_unreg {
    pid_t  pid;
    char   id[MONITOR_NAME_LEN];
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_reg)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct container_unreg)

#endif /* MONITOR_IOCTL_H */
