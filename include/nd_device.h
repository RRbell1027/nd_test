#ifndef ND_DEVICE_H
#define ND_DEVICE_H
#include "nd_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A session owns one exclusive RPC stream. Do not open a second session on
 * the same device: HELLO resets device resources. Borrow an existing runtime
 * session when integrating with another client (including OpenMP).
 * Session close must not race operations; free device allocations first.
 */
typedef nd_runtime nd_device;
/* Opaque device address carried by a host pointer; never dereference on host.
 * This public API requires a 64-bit host. Use uintptr_t arithmetic for byte
 * offsets within an allocation; nd_free requires its base address.
 */
#if UINTPTR_MAX != UINT64_MAX
#error "The ND device pointer API requires a 64-bit host"
#endif
typedef void *nd_device_ptr;
typedef enum nd_copy_kind {
 ND_HOST_TO_DEVICE = 1,
 ND_DEVICE_TO_HOST = 2
} nd_copy_kind;
nd_device *nd_device_open(const char *transport_path);
void nd_device_close(nd_device *device);
const char *nd_device_error(nd_device *device);
int nd_device_bits(nd_device *device);
/* Synchronous operations: 0 on success, -1 with errno on failure.
 * Addresses belong to this session and become invalid after device reset.
 */
int nd_alloc(nd_device *device, size_t bytes, nd_device_ptr *address);
int nd_free(nd_device *device, nd_device_ptr address);
/* Direction identifies the device operand. HOST_TO_HOST and DEVICE_TO_DEVICE
 * are not implemented. Invalid directions fail with EINVAL before any RPC.
 */
int nd_copy(nd_device *device, void *destination, const void *source,
            size_t bytes, nd_copy_kind direction);
#ifdef __cplusplus
}
#endif
#endif
