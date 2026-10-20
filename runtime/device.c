#include "nd_device.h"
nd_device *nd_device_open(const char *path) { return nd_rt_open(path); }
void nd_device_close(nd_device *d) { nd_rt_close(d); }
const char *nd_device_error(nd_device *d) { return nd_rt_error(d); }
int nd_device_bits(nd_device *d) { return nd_rt_device_bits(d); }
int nd_alloc(nd_device *d, size_t n, nd_device_ptr *p) { return nd_rt_alloc(d,n,p); }
int nd_free(nd_device *d, nd_device_ptr p) { return nd_rt_free(d,p); }
int nd_submit(nd_device *d, nd_device_ptr p, const void *s, size_t n) {
 return nd_rt_submit(d,p,s,n);
}
int nd_retrieve(nd_device *d, void *p, nd_device_ptr s, size_t n) {
 return nd_rt_retrieve(d,p,s,n);
}
