
#include "nd.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

int main(int argc, char **argv)
{
    if (argc != 2) {

        std::fprintf(
            stderr,
            "Usage: %s FILE\n",
            argv[0]
        );

        return 2;
    }

    nd_file *file = nd_open(argv[1]);

    if (!file) {

        std::fprintf(
            stderr,
            "nd_open: %s\n",
            std::strerror(errno)
        );

        return 1;
    }

    std::printf(
        "file size: %" PRIu64 " bytes\n",
        nd_file_size(file)
    );

    for (size_t i = 0;
         i < nd_extent_count(file);
         ++i) {

        nd_extent e {};

        if (nd_get_extent(file, i, &e) != 0) {
            break;
        }

        std::printf(
            "extent %zu: "
            "logical=%" PRIu64 " "
            "physical=%" PRIu64 " "
            "length=%" PRIu64 " "
            "flags=0x%08" PRIx32 "\n",

            i,
            e.logical_offset,
            e.physical_offset,
            e.length,
            e.flags
        );
    }

    char buffer[256];

    const int64_t n =
        nd_read(file, buffer, sizeof(buffer));

    if (n < 0) {

        std::fprintf(
            stderr,
            "nd_read: %s\n",
            std::strerror(errno)
        );

        nd_close(file);

        return 1;
    }

    std::printf(
        "read %" PRId64 " bytes (HOST-LOCAL)\n",
        n
    );

    std::printf("buffer: %.*s\n", (int)n, buffer);

    nd_close(file);

    return 0;
}
