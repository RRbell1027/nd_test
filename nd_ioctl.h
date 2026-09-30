#ifndef ND_IOCTL_H
#define ND_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ND_IOCTL_MAGIC 'N'

struct nd_read_request {
    __u64 sector;
    __u32 length;
};

#define ND_IOCTL_READ \
    _IOW(ND_IOCTL_MAGIC, 0x01, struct nd_read_request)

#endif /* ND_IOCTL_H */