
#include "nd.h"
#include "nd_driver.h"
#include "nd_protocol.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include <fcntl.h>
#include <endian.h>
#include <poll.h>
#include <termios.h>
#include <algorithm>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

struct nd_file {
    int fd = -1;
    uint64_t size = 0;
    std::vector<nd_extent> extents;
};

namespace {

constexpr uint32_t kBatchSize = 128;

int collect_extents(nd_file &file)
{
    /*
     * FIEMAP physical offsets are not necessarily absolute
     * offsets on the SD card.
     *
     * If the filesystem is on LVM/device-mapper, additional
     * address translation is required.
     */

    const size_t bytes =
        sizeof(struct fiemap) +
        kBatchSize * sizeof(struct fiemap_extent);

    void *memory = std::calloc(1, bytes);

    if (!memory) {
        errno = ENOMEM;
        return -1;
    }

    auto *map = static_cast<struct fiemap *>(memory);

    uint64_t start = 0;
    int result = 0;

    while (start < file.size) {

        std::memset(memory, 0, bytes);

        map->fm_start = start;
        map->fm_length = FIEMAP_MAX_OFFSET;
        map->fm_flags = FIEMAP_FLAG_SYNC;
        map->fm_extent_count = kBatchSize;

        if (::ioctl(file.fd, FS_IOC_FIEMAP, map) < 0) {
            result = -1;
            break;
        }

        if (map->fm_mapped_extents == 0) {
            break;
        }

        uint64_t next_start = start;
        bool last = false;

        for (uint32_t i = 0;
             i < map->fm_mapped_extents;
             ++i) {

            const auto &e = map->fm_extents[i];

            if (e.fe_length == 0 ||
                e.fe_logical > UINT64_MAX - e.fe_length) {

                errno = EIO;
                result = -1;
                break;
            }

            file.extents.push_back({
                e.fe_logical,
                e.fe_physical,
                e.fe_length,
                e.fe_flags
            });

            const uint64_t end =
                e.fe_logical + e.fe_length;

            if (end > next_start) {
                next_start = end;
            }

            last =
                (e.fe_flags & FIEMAP_EXTENT_LAST) != 0;
        }

        if (result < 0) {
            break;
        }

        if (last || next_start >= file.size) {
            break;
        }

        if (next_start <= start) {
            errno = EIO;
            result = -1;
            break;
        }

        start = next_start;
    }

    const int saved_errno = errno;

    std::free(memory);

    if (result < 0) {
        errno = saved_errno;
    }

    return result;
}

} // namespace

extern "C" nd_file *nd_open(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }

    int fd = ::open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return nullptr;
    }

    struct stat st {};

    if (::fstat(fd, &st) < 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        return nullptr;
    }

    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return nullptr;
    }

    nd_file *file = new (std::nothrow) nd_file;

    if (!file) {
        ::close(fd);
        errno = ENOMEM;
        return nullptr;
    }

    file->fd = fd;
    file->size = static_cast<uint64_t>(st.st_size);

    try {

        if (collect_extents(*file) < 0) {

            const int error = errno;

            ::close(fd);
            delete file;

            errno = error;
            return nullptr;
        }

    } catch (const std::bad_alloc &) {

        ::close(fd);
        delete file;

        errno = ENOMEM;
        return nullptr;
    }

    return file;
}

extern "C" int64_t nd_read_host_legacy(
    nd_file *file,
    void *buffer,
    size_t count)
{
    if (!file || (!buffer && count != 0)) {
        errno = EINVAL;
        return -1;
    }

    if (count >
        static_cast<size_t>(
            std::numeric_limits<ssize_t>::max())) {

        errno = EINVAL;
        return -1;
    }

    if (!count || !file->size)
        return 0;
    if (file->extents.empty()) {
        errno = EIO;
        return -1;
    }
    const nd_extent &extent = file->extents[0];
    count = std::min<uint64_t>(count, file->size);
    const uint32_t unsafe_flags = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
        FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED |
        FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
        FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN;
    /* Multi-extent slicing is LEO-18; fail before sending an invalid mapping. */
    if (extent.logical_offset != 0 || count > extent.length ||
        (extent.flags & unsafe_flags) || extent.physical_offset % 512) {
        errno = ENOTSUP;
        return -1;
    }
    uint64_t partition_start = 4096; // Verified Luckfox SD baseline; configurable.
    if (const char *value = std::getenv("ND_PARTITION_START_SECTOR")) {
        char *end = nullptr;
        errno = 0;
        partition_start = std::strtoull(value, &end, 10);
        if (errno || !*value || *value == '-' || !end || *end) {
            errno = EINVAL;
            return -1;
        }
    }
    const uint64_t base_sector = extent.physical_offset / 512;
    if (partition_start > UINT64_MAX - base_sector ||
        base_sector + partition_start > (INT64_MAX - count) / 512) {
        errno = EOVERFLOW;
        return -1;
    }
    const char *transport = std::getenv("ND_TRANSPORT_PATH");
    int nd_fd = ::open(transport ? transport : "/dev/nd0",
                       O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (nd_fd < 0)
        return -1;
    auto fail = [&](int error, size_t done) -> int64_t {
        ::close(nd_fd);
        errno = error;
        return done ? static_cast<int64_t>(done) : -1;
    };
    if (::isatty(nd_fd)) {
        struct termios settings {};
        if (::tcgetattr(nd_fd, &settings) < 0)
            return fail(errno, 0);
        ::cfmakeraw(&settings);
        settings.c_cflag |= CLOCAL | CREAD;
        ::cfsetispeed(&settings, B115200);
        ::cfsetospeed(&settings, B115200);
        if (::tcsetattr(nd_fd, TCSANOW, &settings) < 0)
            return fail(errno, 0);
        ::tcflush(nd_fd, TCIOFLUSH);
    }
    auto transfer = [&](void *data, size_t bytes, bool writing) -> bool {
        auto *cursor = static_cast<unsigned char *>(data);
        while (bytes) {
            struct pollfd event {nd_fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
            int ready;
            do { ready = ::poll(&event, 1, 5000); } while (ready < 0 && errno == EINTR);
            if (ready <= 0) {
                if (!ready) errno = ETIMEDOUT;
                return false;
            }
            if (!(event.revents & event.events)) {
                errno = EIO;
                return false;
            }
            ssize_t n = writing ? ::write(nd_fd, cursor, bytes) : ::read(nd_fd, cursor, bytes);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0) {
                if (!n) errno = EIO;
                return false;
            }
            cursor += n;
            bytes -= static_cast<size_t>(n);
        }
        return true;
    };
    static_assert(sizeof(nd_driver_command) == 16, "host command ABI");
    static_assert(sizeof(nd_response) == 16, "response ABI");
    size_t copied = 0;
    while (copied < count) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(count - copied, ND_MAX_READ));
        nd_driver_command command {};
        command.magic = htole16(ND_DRIVER_MAGIC);
        command.command = htole16(ND_DRIVER_CMD_READ);
        command.length = htole32(chunk);
        command.sector = htole64(base_sector + partition_start + copied / 512);
        if (!transfer(&command, sizeof(command), true))
            return fail(errno, copied);
        nd_response response {};
        if (!transfer(&response, sizeof(response), false))
            return fail(errno, copied);
        const int32_t status = static_cast<int32_t>(le32toh(response.status));
        const uint32_t length = le32toh(response.length);
        if (le16toh(response.magic) != ND_PROTOCOL_MAGIC ||
            le16toh(response.version) != ND_RESPONSE_VERSION || response.reserved ||
            status > 0 || status < -4095 || length > chunk || (status && length))
            return fail(EPROTO, copied);
        if (status)
            return fail(-status, copied);
        if (!transfer(static_cast<unsigned char *>(buffer) + copied, length, false))
            return fail(errno, copied);
        copied += length;
        if (length < chunk)
            break;
    }
    ::close(nd_fd);
    return static_cast<int64_t>(copied);
}

extern "C" int nd_device_read_storage(void *, uint64_t, size_t);
extern "C" int64_t nd_read(nd_file *file, uint64_t offset, size_t count,
                            nd_device_ptr destination)
{
    if (!file || (!destination && count) || count > INT64_MAX) {
        errno = EINVAL; return -1;
    }
    if (!count || offset >= file->size) return 0;
    count = std::min<uint64_t>(count, file->size - offset);
    if (file->extents.empty()) { errno = ENOTSUP; return -1; }
    const nd_extent &e = file->extents[0];
    const uint32_t unsafe = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
        FIEMAP_EXTENT_ENCODED | FIEMAP_EXTENT_DATA_ENCRYPTED |
        FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |
        FIEMAP_EXTENT_DATA_TAIL | FIEMAP_EXTENT_UNWRITTEN;
    if (offset < e.logical_offset || offset-e.logical_offset > e.length ||
        count > e.length-(offset-e.logical_offset) || (e.flags & unsafe)) {
        errno = ENOTSUP; return -1;
    }
    uint64_t start = 4096;
    if (const char *v = std::getenv("ND_PARTITION_START_SECTOR")) {
        char *end = nullptr; errno = 0;
        start = std::strtoull(v, &end, 10);
        if (errno || !*v || *v=='-' || !end || *end) { errno=EINVAL; return -1; }
    }
    if (start > UINT64_MAX/512 || e.physical_offset > UINT64_MAX-start*512 ||
        offset-e.logical_offset > UINT64_MAX-start*512-e.physical_offset) {
        errno=EOVERFLOW; return -1;
    }
    uint64_t physical = start*512+e.physical_offset+offset-e.logical_offset;
    if (nd_device_read_storage(destination, physical, count)) return -1;
    return static_cast<int64_t>(count);
}

extern "C" int nd_close(nd_file *file)
{
    if (!file) {
        errno = EINVAL;
        return -1;
    }

    const int result = ::close(file->fd);

    delete file;

    return result;
}

extern "C" size_t nd_extent_count(
    const nd_file *file)
{
    return file ? file->extents.size() : 0;
}

extern "C" int nd_get_extent(
    const nd_file *file,
    size_t index,
    nd_extent *out)
{
    if (!file ||
        !out ||
        index >= file->extents.size()) {

        errno = EINVAL;
        return -1;
    }

    *out = file->extents[index];

    return 0;
}

extern "C" uint64_t nd_file_size(
    const nd_file *file)
{
    return file ? file->size : 0;
}
