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
/* Opaque device address, never dereference on the host. Byte offsets may be
 * added within the allocation; nd_free requires the allocation base address.
 */
typedef uint64_t nd_device_ptr;
nd_device *nd_device_open(const char *transport_path);
void nd_device_close(nd_device *device);
const char *nd_device_error(nd_device *device);
int nd_device_bits(nd_device *device);
/* Synchronous operations: 0 on success, -1 with errno on failure.
 * submit copies host -> device; retrieve copies device -> host.
 * Addresses belong to this session and become invalid after device reset.
 */
int nd_alloc(nd_device *device, size_t bytes, nd_device_ptr *address);
int nd_free(nd_device *device, nd_device_ptr address);
int nd_submit(nd_device *device, nd_device_ptr destination,
              const void *source, size_t bytes);
int nd_retrieve(nd_device *device, void *destination,
                nd_device_ptr source, size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
