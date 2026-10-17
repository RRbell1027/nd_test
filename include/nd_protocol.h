/* nd_protocol.h */

#ifndef ND_PROTOCOL_H
#define ND_PROTOCOL_H

#include <linux/types.h>

#define ND_PROTOCOL_MAGIC 0x4e44

enum nd_opcode {
    ND_CMD_READ = 0x01,
};

struct nd_command {
    __le16 magic;       /* offset 0 */
    __le16 opcode;      /* offset 2 */
    __le32 length;      /* offset 4 */
    __le64 sector;      /* offset 8 */
};                      /* total: 16 bytes */

#endif