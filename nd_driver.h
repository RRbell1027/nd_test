#ifndef ND_DRIVER_H
#define ND_DRIVER_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u16 nd_u16;
typedef __u32 nd_u32;
typedef __u64 nd_u64;
#else
#include <stdint.h>
typedef uint16_t nd_u16;
typedef uint32_t nd_u32;
typedef uint64_t nd_u64;
#endif

#define ND_DRIVER_MAGIC 0x4E44

enum nd_driver_command_type {
    ND_DRIVER_CMD_READ  = 1,
};

struct nd_driver_command {
    nd_u16 magic;
    nd_u16 command;
    nd_u32 length;
    nd_u64 sector;
};

#endif /* ND_DRIVER_H */