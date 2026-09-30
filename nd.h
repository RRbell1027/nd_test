
#ifndef ND_H
#define ND_H

#include <stddef.h>
#include <stdint.h>

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

/*
 * Initial implementation: reads from the Host filesystem.
 * Returns bytes read, 0 on EOF, or -1 on error.
 */
int64_t nd_read(nd_file *file, void *buffer, size_t count);

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
