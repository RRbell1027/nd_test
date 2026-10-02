#ifndef ND_COMMAND_H
#define ND_COMMAND_H

#include <stdint.h>

#define ND_CMD_MAGIC 0x4E44

enum nd_command_type {
    ND_CMD_READ = 1,
};

struct nd_command {
    uint16_t magic;
    uint16_t command;
    uint32_t length;
    uint64_t sector;
};

#endif /* ND_COMMAND_H */