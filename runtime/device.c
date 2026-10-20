#include "nd_device.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
_Static_assert(sizeof(uintptr_t)==sizeof(uint64_t), "ND public API requires a 64-bit host");
nd_device *nd_device_open(const char *path) { return nd_rt_acquire(path); }
void nd_device_close(nd_device *d) { nd_rt_release(d); }
const char *nd_device_error(nd_device *d) { return nd_rt_error(d); }
int nd_device_bits(nd_device *d) { return nd_rt_device_bits(d); }
/* Registry is shared with nd_read. Each allocation retains its session until
 * freed, so a file read cannot accidentally use a reset or unrelated stream. */
struct allocation { nd_device *device; uintptr_t base; size_t size; struct allocation *next; };
static struct allocation *allocations;
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
int nd_alloc(nd_device *d, size_t n, nd_device_ptr *p) {
 uint64_t address;
 if (!d || !p) { errno = EINVAL; return -1; }
 *p = NULL;
 struct allocation *a = malloc(sizeof *a);
 if (!a) return -1;
 nd_device *held = nd_rt_acquire(NULL);
 if (held != d) { nd_rt_release(held); free(a); errno = EINVAL; return -1; }
 if (nd_rt_alloc(d, n, &address)) { int e = errno; nd_rt_release(held); free(a); errno = e; return -1; }
 a->device = d; a->base = (uintptr_t)address; a->size = n;
 pthread_mutex_lock(&allocation_lock);
 a->next = allocations; allocations = a;
 pthread_mutex_unlock(&allocation_lock);
 *p = (void *)(uintptr_t)address;
 return 0;
}
int nd_free(nd_device *d, nd_device_ptr p) {
 pthread_mutex_lock(&allocation_lock);
 struct allocation **link = &allocations;
 while (*link && ((*link)->device != d || (*link)->base != (uintptr_t)p)) link = &(*link)->next;
 if (!*link) { pthread_mutex_unlock(&allocation_lock); errno = EINVAL; return -1; }
 struct allocation *a = *link;
 int result = nd_rt_free(d, a->base);
 if (!result) *link = a->next;
 pthread_mutex_unlock(&allocation_lock);
 if (!result) { nd_rt_release(d); free(a); }
 return result;
}
/* Internal libnd entry: locate the owning session without a device argument. */
int nd_device_read_storage(void *destination, uint64_t offset, size_t bytes) {
 uintptr_t address = (uintptr_t)destination;
 pthread_mutex_lock(&allocation_lock);
 struct allocation *a = allocations;
 while (a && !(address >= a->base && address - a->base <= a->size &&
               bytes <= a->size - (address - a->base))) a = a->next;
 if (!a) { pthread_mutex_unlock(&allocation_lock); errno = EFAULT; return -1; }
 int result = nd_rt_read_storage(a->device, address, offset, bytes);
 pthread_mutex_unlock(&allocation_lock);
 return result;
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
