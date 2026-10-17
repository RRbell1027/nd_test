
#include "nd.h"
#include "nd_driver.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include <fcntl.h>
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

extern "C" int64_t nd_read(
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

    /*
     * 第一階段限制：
     * - 只從邏輯位移 0 讀（使用第一個 extent）
     * - 刻意不寫入 buffer
     * - Luckfox 把讀到的資料印到 dmesg
     */
    if (file->extents.empty()) {
        errno = EIO;
        return -1;
    }

    const nd_extent &extent = file->extents[0];

    constexpr uint64_t sector_size = 512;
    constexpr uint64_t partition_start_sector = 4096;

    /*
     * FIEMAP 的 physical_offset 是相對於 /dev/vda2。
     *
     * 轉換：
     *   vda2 實體位元組位移
     *          ↓
     *   vda2 sector
     *          ↓
     *   SD 卡絕對 sector
     */
    if (extent.physical_offset % sector_size != 0) {
        errno = EIO;
        return -1;
    }

    const uint64_t sector =
        extent.physical_offset / sector_size
        + partition_start_sector;

    if (count > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    nd_driver_command  cmd {};
    cmd.magic   = ND_DRIVER_MAGIC;
    cmd.command = ND_DRIVER_CMD_READ;
    cmd.length  = static_cast<uint32_t>(count);
    cmd.sector  = sector;

    int nd_fd = ::open("/dev/nd0", O_RDWR | O_CLOEXEC);
    if (nd_fd < 0) {
        return -1;
    }

    ssize_t ret = ::write(nd_fd, &cmd, sizeof(cmd));
    if (ret < 0) {
        const int saved_errno = errno;
        ::close(nd_fd);
        errno = saved_errno;
        return -1;
    }

    if (ret != sizeof(cmd)) {
        ::close(nd_fd);
        errno = EIO;
        return -1;
    }

    ::close(nd_fd);

    /*
     * 第一階段不會把資料寫進 buffer。
     * 成功只代表請求已送到 Luckfox。
     */
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
