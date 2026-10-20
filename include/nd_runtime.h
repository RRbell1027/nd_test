#ifndef ND_RUNTIME_H
#define ND_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct nd_runtime nd_runtime;
/* Owns fd. Stream must be exclusively reserved for framed binary RPC. */
nd_runtime *nd_rt_connect_fd(int fd);
nd_runtime *nd_rt_open(const char *path);
void nd_rt_close(nd_runtime *rt);
/* Process-wide single-device session. All users must link the same shared
 * runtime. NULL path selects ND_RUNTIME_DEVICE_PATH (default /dev/ttyS0).
 * A different path while active fails EBUSY; acquire/release are refcounted.
 */
nd_runtime *nd_rt_acquire(const char *path);
void nd_rt_release(nd_runtime *rt);
/* Direct storage byte range into a registered device allocation. */
int nd_rt_read_storage(nd_runtime *, uint64_t address, uint64_t offset, size_t bytes);
const char *nd_rt_error(nd_runtime *rt);
int nd_rt_device_bits(nd_runtime *rt);
/* ELF contents, never a host/device pathname. */
int nd_rt_load_image(nd_runtime *, const void *image, size_t bytes, uint64_t *id);
int nd_rt_unload_image(nd_runtime *, uint64_t id);
int nd_rt_alloc(nd_runtime *, size_t bytes, uint64_t *address);
int nd_rt_free(nd_runtime *, uint64_t address);
int nd_rt_submit(nd_runtime *, uint64_t address, const void *, size_t bytes);
int nd_rt_retrieve(nd_runtime *, void *, uint64_t address, size_t bytes);
/* size=0 resolves a function handle; size>0 resolves/registers a global range. */
int nd_rt_symbol(nd_runtime *, uint64_t image, const char *, size_t size, uint64_t *value);
/* Prototype supports only mapped pointer arguments, explicit uint64 wire slots. */
int nd_rt_launch(nd_runtime *, uint64_t function, const uint64_t *args, size_t count);
int nd_rt_shutdown(nd_runtime *);
#ifdef __cplusplus
}
#endif
#endif
