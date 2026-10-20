#include "nd_device.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv) {
 if (argc != 2) { fprintf(stderr, "usage: %s TRANSPORT\n", argv[0]); return 2; }
 nd_device *d = nd_device_open(argv[1]);
 if (!d) { perror("nd_device_open"); return 1; }
 nd_device_ptr p = 0;
 int32_t input[3] = {7, 19, 42}, output[3] = {0};
 int result = 1;
 if (nd_alloc(d, sizeof input, &p) ||
     nd_copy(d, p, input, sizeof input, ND_HOST_TO_DEVICE) ||
     nd_copy(d, output, p, sizeof output, ND_DEVICE_TO_HOST)) goto done;
 for (int i = 0; i < 3; ++i) if (input[i] != output[i]) goto done;
 errno = 0;
 if (nd_copy(d, output, p, sizeof output, (nd_copy_kind)0) != -1 || errno != EINVAL)
  goto done;
 /* Device reports an invalid range; the session must remain usable. */
 errno = 0;
 if (nd_copy(d, output, (void *)((uintptr_t)p + sizeof input), sizeof output, ND_DEVICE_TO_HOST) != -1 || errno != EFAULT)
  goto done;
 if (nd_copy(d, output, p, sizeof output, ND_DEVICE_TO_HOST)) goto done;
 result = 0;
 puts("PASS: public void-pointer nd_copy roundtrip, invalid direction and range recovery");
done:
 if (result) fprintf(stderr, "device API failure: %s\n", nd_device_error(d));
 if (p && nd_free(d, p)) result = 1;
 nd_device_close(d);
 return result;
}
