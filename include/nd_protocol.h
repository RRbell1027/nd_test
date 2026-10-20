/* nd_protocol.h */

#ifndef ND_PROTOCOL_H
#define ND_PROTOCOL_H

#include <linux/types.h>

#define ND_PROTOCOL_MAGIC 0x4e44
#define ND_RESPONSE_VERSION 1
#define ND_MAX_READ 4096

enum nd_opcode {
    ND_CMD_READ = 0x01,
};

struct nd_command {
    __le16 magic;       /* offset 0 */
    __le16 opcode;      /* offset 2 */
    __le32 length;      /* offset 4 */
    __le64 sector;      /* offset 8 */
};                      /* total: 16 bytes */

/* Minimal sequential response; all fields little-endian, followed by length bytes.
 * status is a signed Linux errno (0 for success). Request stays 16-byte v0.
 */
struct nd_response {
    __le16 magic;
    __le16 version;
    __le32 status;
    __le32 length;
    __le32 reserved;
};

#endif