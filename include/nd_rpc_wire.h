#ifndef ND_RPC_WIRE_H
#define ND_RPC_WIRE_H
/* Shared by the runtime and kernel transports; fields are little-endian. */
#define ND_RPC_MAGIC 0x3152444eU
#define ND_RPC_HEADER_SIZE 24U
#define ND_RPC_MAX_BODY (16U * 1024U * 1024U)
#define ND_RPC_HELLO 1U
#endif
