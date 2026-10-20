
#ifndef ND_H
#define ND_H

#include <stddef.h>
#include <stdint.h>
#include "nd_device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nd_file nd_file;

typedef struct nd_extent {
    uint64_t logical_offset;
    uint64_t physical_offset;
    uint64_t length;
    uint32_t flags;
} nd_extent;

/* Open a regular file read-only and obtain its FIEMAP extents. */
nd_file *nd_open(const char *path);

/* Read file bytes directly from exported SD into a public nd_alloc buffer.
 * Returns actual bytes (clamped at EOF), or -1 with errno. First extent only;
 * unsupported FIEMAP mappings are rejected. offset is a file byte offset.
 * The destination allocation identifies the shared device session.
 */
int64_t nd_read(nd_file *file, uint64_t offset, size_t count, nd_device_ptr destination);
/* Legacy SD -> host-buffer probe, retained under an explicit legacy name. */
int64_t nd_read_host_legacy(nd_file *file, void *buffer, size_t count);

/* Close the file and release its resources. */
int nd_close(nd_file *file);

/* Inspect the file's FIEMAP extents. */
size_t nd_extent_count(const nd_file *file);

int nd_get_extent(const nd_file *file,
                  size_t index,
                  nd_extent *out);

uint64_t nd_file_size(const nd_file *file);

#ifdef __cplusplus
}
#endif

#endif
