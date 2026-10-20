#include "nd_device.h"
#include <errno.h>
nd_device *nd_device_open(const char *path) { return nd_rt_open(path); }
void nd_device_close(nd_device *d) { nd_rt_close(d); }
const char *nd_device_error(nd_device *d) { return nd_rt_error(d); }
int nd_device_bits(nd_device *d) { return nd_rt_device_bits(d); }
int nd_alloc(nd_device *d, size_t n, nd_device_ptr *p) {
 uint64_t address;
 if (!p) { errno = EINVAL; return -1; }
 *p = NULL;
 if (nd_rt_alloc(d, n, &address)) return -1;
 *p = (void *)(uintptr_t)address;
 return 0;
}
int nd_free(nd_device *d, nd_device_ptr p) {
 return nd_rt_free(d, (uint64_t)(uintptr_t)p);
}
int nd_copy(nd_device *d, void *destination, const void *source,
            size_t n, nd_copy_kind direction) {
 switch (direction) {
 case ND_HOST_TO_DEVICE:
  return nd_rt_submit(d, (uint64_t)(uintptr_t)destination, source, n);
 case ND_DEVICE_TO_HOST:
  return nd_rt_retrieve(d, destination, (uint64_t)(uintptr_t)source, n);
 default:
  errno = EINVAL;
  return -1;
 }
}
