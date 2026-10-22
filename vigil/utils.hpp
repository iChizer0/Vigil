#pragma once
#ifndef VIGIL_UTILS_HPP
#define VIGIL_UTILS_HPP

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <mntent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern "C" {
extern char **environ;
}

namespace vigil::utils {

class Execute
{
public:
    Execute(const std::string &program, const std::vector<std::string> &args) noexcept
        : _args(nullptr), _args_data(nullptr)
    {
        size_t argc = 1;
        size_t args_size = program.size() + 1;
        for (const auto &arg: args)
        {
            ++argc;
            args_size += arg.size() + 1;
        }
        const ptrdiff_t args_offset = (argc + 1) * sizeof(char **);
        _args_data = new (std::nothrow) char[args_offset + args_size];
        if (!_args_data)
            return;

        size_t i = 0;
        char *p = static_cast<char *>(_args_data) + args_offset;
        reinterpret_cast<char **>(_args_data)[i++] = p;
        std::memcpy(p, program.c_str(), program.size() + 1);
        p += program.size() + 1;
        for (const auto &arg: args)
        {
            reinterpret_cast<char **>(_args_data)[i++] = p;
            std::memcpy(p, arg.c_str(), arg.size() + 1);
            p += arg.size() + 1;
        }
        reinterpret_cast<char **>(_args_data)[i] = nullptr;
        _args = reinterpret_cast<char **>(_args_data);
    }

    Execute(const Execute &) = delete;
    Execute &operator=(const Execute &) = delete;

    Execute(Execute &&) = delete;
    Execute &operator=(Execute &&) = delete;

    ~Execute() noexcept
    {
        _args = nullptr;
        delete[] static_cast<char *>(_args_data);
        _args_data = nullptr;
    }

    [[nodiscard]] int operator()(const std::atomic<int> *signal = nullptr) const
    {
        if (!_args || !_args[0])
            return -EFAULT;

        ::pid_t pid;
        ::posix_spawn_file_actions_t file_actions;
        int status;

        if ((status = ::posix_spawn_file_actions_init(&file_actions)) != 0) [[unlikely]]
            return -status;

        if ((::posix_spawn_file_actions_addclosefrom_np(&file_actions, 3)) != 0) [[unlikely]]
            goto l_check_status;

        if ((status
                = ::posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null", O_RDWR | O_CLOEXEC, 0))
                == 0
            && (status = ::posix_spawn_file_actions_adddup2(&file_actions, STDIN_FILENO, STDOUT_FILENO)) == 0
            && (status = ::posix_spawn_file_actions_adddup2(&file_actions, STDIN_FILENO, STDERR_FILENO)) == 0)
            [[likely]]
            status = ::posix_spawnp(&pid, _args[0], &file_actions, nullptr, _args, environ);

    l_check_status:
        ::posix_spawn_file_actions_destroy(&file_actions);
        if (status != 0)
            return -status;

        if (signal == nullptr)
        {
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                    return -1;
            }
        }
        else
        {
            int last_sig = 0;
            while (true)
            {
                pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
                if (wait_result == pid)
                    break;
                if (wait_result == -1)
                {
                    if (errno == EINTR)
                        continue;
                    break;
                }

                int cur_sig = signal->load(std::memory_order_acquire);
                if (cur_sig != last_sig)
                {
                    last_sig = cur_sig;
                    if (cur_sig > 0)
                        ::kill(pid, cur_sig);
                }

                const ::timespec interval { 0, 10L * 1000L * 1000L };
                static_cast<void>(::nanosleep(&interval, nullptr));
            }
        }

        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            return -WTERMSIG(status);
        if (WIFSTOPPED(status))
            return -WSTOPSIG(status);

        return -1;
    }

private:
    char **_args;
    void *_args_data;
};

class FSCK
{
public:
    enum Type {
        Ext,
        Fat,
        Unknown,
    };

    enum Result : int {
        Clean = 0,
        Dirty,
        ErrorIO,
        ErrorMagic,
        ErrorInvalid,
        ErrorUnsupported,
        ErrorMounted,
        ErrorUnknown,
    };

    FSCK(const std::filesystem::path &dev_path, std::string_view fs_type) noexcept
        : _dev_path(dev_path), _fd(-1), _fs_type(Type::Unknown)
    {
        if (fs_type == "ext" || fs_type == "ext2" || fs_type == "ext3" || fs_type == "ext4")
            _fs_type = Type::Ext;
        else if (fs_type == "fat" || fs_type == "fat12" || fs_type == "fat16" || fs_type == "fat32"
                 || fs_type == "msdos" || fs_type == "vfat")
            _fs_type = Type::Fat;
    }

    ~FSCK() noexcept
    {
        if (_fd >= 0)
        {
            ::close(_fd);
            _fd = -1;
        }
    }

    FSCK(const FSCK &) = delete;
    FSCK &operator=(const FSCK &) = delete;
    FSCK(FSCK &&other) = delete;
    FSCK &operator=(FSCK &&other) = delete;

    Result dirty() noexcept
    {
        if (_fd < 0)
        {
            _fd = ::open(_dev_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (_fd < 0)
                return Result::ErrorIO;
        }
        {
            // Resolve the target device to its underlying block device identity so
            // that mount detection is robust against non-canonical names (symlinks
            // such as /dev/disk/by-uuid/..., /dev/mapper/x vs /dev/dm-0, etc.). stat
            // follows symlinks, so st_rdev identifies the same device regardless of
            // the path spelling.
            struct ::stat dev_st;
            const bool dev_is_blk = ::stat(_dev_path.c_str(), &dev_st) == 0 && S_ISBLK(dev_st.st_mode);

            ::FILE *mount_table = ::setmntent("/proc/mounts", "r");
            if (mount_table == nullptr)
                return Result::ErrorIO;
            bool found = false;
            while (struct ::mntent *mount_entry = ::getmntent(mount_table))
            {
                if (mount_entry->mnt_fsname == nullptr)
                    continue;
                if (mount_entry->mnt_fsname == _dev_path)
                {
                    found = true;
                    break;
                }
                if (dev_is_blk)
                {
                    struct ::stat ent_st;
                    if (::stat(mount_entry->mnt_fsname, &ent_st) == 0 && S_ISBLK(ent_st.st_mode)
                        && ent_st.st_rdev == dev_st.st_rdev)
                    {
                        found = true;
                        break;
                    }
                }
            }
            ::endmntent(mount_table);
            if (found) // If the device is currently mounted, we cannot reliably check
                       // dirty status, return error to indicate this
                return Result::ErrorMounted;
        }
        switch (_fs_type)
        {
            case Type::Ext:
                return checkExt();
            case Type::Fat:
                return checkFat();
            default:
                return Result::ErrorUnsupported;
        }
    }

private:
    static constexpr inline const uint64_t kMaxOffset = static_cast<uint64_t>(std::numeric_limits<::off_t>::max());

private:
    bool read(void *buffer, size_t size, uint64_t offset) const noexcept
    {
        if (size != 0 && (offset > kMaxOffset || static_cast<uint64_t>(size - 1) > kMaxOffset - offset))
            return false;
        for (uint8_t *p = static_cast<uint8_t *>(buffer); size > 0;)
        {
            if (const ::ssize_t n = ::pread(_fd, p, size, static_cast<::off_t>(offset)); n > 0)
            {
                p += n;
                size -= static_cast<size_t>(n);
                offset += static_cast<uint64_t>(n);
                continue;
            }
            else if (n == 0)
                return false;
            if (errno == EINTR)
                continue;
            return false;
        }
        return true;
    }

    template<size_t Offset, size_t Size>
    static inline uint8_t u8(const uint8_t (&buffer)[Size]) noexcept
    {
        static_assert(Offset < Size, "u8 offset exceeds buffer");
        return buffer[Offset];
    }

    template<size_t Offset, size_t Size>
    static inline uint16_t u16le(const uint8_t (&buffer)[Size]) noexcept
    {
        static_assert(Offset <= Size && sizeof(uint16_t) <= Size - Offset, "u16le offset exceeds buffer");
        const uint8_t *p = buffer + Offset;
        return static_cast<uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
    }

    template<size_t Offset, size_t Size>
    static inline uint32_t u32le(const uint8_t (&buffer)[Size]) noexcept
    {
        static_assert(Offset <= Size && sizeof(uint32_t) <= Size - Offset, "u32le offset exceeds buffer");
        const uint8_t *p = buffer + Offset;
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
               | (static_cast<uint32_t>(p[3]) << 24);
    }

    template<typename T>
    static inline bool isPowerOfTwo(T value) noexcept
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    Result checkExt() const noexcept
    {
        // https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git
        uint8_t sb[0x160 /* superblock size */];
        if (!read(sb, sizeof(sb), 1024 /* superblock offset */))
            return Result::ErrorIO;

        if (u16le<0x38>(sb) != 0xEF53 /* ext magic */)
            return Result::ErrorMagic;

        const auto inodes_count = u32le<0x00>(sb);
        uint64_t blocks_count = u32le<0x04>(sb);
        uint64_t free_blocks_count = u32le<0x0C>(sb);
        const auto free_inodes_count = u32le<0x10>(sb);
        const auto first_data_block = u32le<0x14>(sb);
        const auto log_block_size = u32le<0x18>(sb);
        const auto log_cluster_size = u32le<0x1C>(sb);
        const auto blocks_per_group = u32le<0x20>(sb);
        const auto clusters_per_group = u32le<0x24>(sb);
        const auto inodes_per_group = u32le<0x28>(sb);
        const auto rev_level = u32le<0x4C>(sb);
        const auto incompat_features = u32le<0x60>(sb);
        const auto ro_compat_features = u32le<0x64>(sb);
        const bool has_64bit = (incompat_features & 0x0080 /* ext feature 64-bit */) != 0;
        const bool has_bigalloc = (ro_compat_features & 0x0200 /* ext feature ro compat bigalloc */) != 0;

        if (has_64bit)
        {
            blocks_count |= static_cast<uint64_t>(u32le<0x150>(sb)) << 32;
            free_blocks_count |= static_cast<uint64_t>(u32le<0x158>(sb)) << 32;
        }

        if (inodes_count == 0 || blocks_count == 0 || blocks_per_group == 0 || clusters_per_group == 0
            || inodes_per_group == 0 || free_blocks_count > blocks_count || free_inodes_count > inodes_count
            || log_block_size > 6 /* max log block size */ || log_cluster_size > 29 /* max log cluster size */
            || log_cluster_size < log_block_size || rev_level > 1 /* dynamic revision */)
            return Result::ErrorInvalid;

        if (!has_bigalloc && log_cluster_size != log_block_size)
            return Result::ErrorInvalid;

        const uint64_t block_size = 1024ULL << log_block_size;
        if (first_data_block != (block_size == 1024 ? 1U : 0U) || blocks_count <= first_data_block)
            return Result::ErrorInvalid;

        uint64_t inode_size = 128;
        if (rev_level == 1 /* dynamic revision */)
        {
            const auto first_ino = u32le<0x54>(sb);
            inode_size = u16le<0x58>(sb);
            if (first_ino == 0 || inode_size < 128 /* min inode size */ || inode_size > block_size
                || !isPowerOfTwo(inode_size))
                return Result::ErrorInvalid;
        }

        const auto cluster_ratio_bits = log_cluster_size - log_block_size;
        const uint64_t expected_blocks_per_group = static_cast<uint64_t>(clusters_per_group) << cluster_ratio_bits;
        if (blocks_per_group != expected_blocks_per_group || blocks_per_group < 8
            || clusters_per_group > block_size * 8U
            || clusters_per_group > ((1U << 16U) - 8U) /* max clusters per group */)
            return Result::ErrorInvalid;

        if (has_64bit)
        {
            const auto desc_size = u16le<0xFE>(sb);
            if (desc_size < 64 /* min 64-bit descriptor size */ || desc_size > 1024 /* max 64-bit descriptor size */
                || !isPowerOfTwo(desc_size))
                return Result::ErrorInvalid;
        }

        const uint64_t block_groups = ((blocks_count - first_data_block - 1U) / blocks_per_group) + 1U;
        const uint64_t inodes_per_block = block_size / inode_size;
        const uint64_t max_inodes_per_group = (1ULL << 16U) - inodes_per_block;
        if (block_groups == 0 || inodes_per_group > block_size * 8U || inodes_per_group > max_inodes_per_group
            || block_groups > std::numeric_limits<uint64_t>::max() / inodes_per_group
            || static_cast<uint64_t>(inodes_per_group) * block_groups != inodes_count)
            return Result::ErrorInvalid;

        const auto state = u16le<0x3A>(sb);
        return (state & 0x0001 /* ext state valid */) == 0
                       || (state
                              & (0x0002 /* ext state error */ | 0x0004 /* ext state orphans */
                                  | 0x0020 /* ext state fast commit replay */))
                              != 0
                       || (incompat_features & 0x0004 /* ext feature recover */) != 0
                       || (ro_compat_features & 0x10000 /* ext feature ro compat orphan present */) != 0
                   ? Result::Dirty
                   : Result::Clean;
    }

    Result checkFat() const noexcept
    {
        // https://github.com/dosfstools/dosfstools.git
        uint8_t bs[512];
        if (!read(bs, sizeof(bs), 0))
            return Result::ErrorIO;

        if (u16le<510>(bs) != 0xAA55 /* fat signature */)
            return Result::ErrorMagic;

        const uint16_t bytes_per_sec = u16le<11>(bs);
        if (bytes_per_sec < 512 || bytes_per_sec > 4096 || !isPowerOfTwo(bytes_per_sec))
            return Result::ErrorInvalid;

        const uint8_t sec_per_cluster = u8<13>(bs);
        if (sec_per_cluster > 128 || !isPowerOfTwo(sec_per_cluster))
            return Result::ErrorInvalid;

        const uint16_t reserved_sec = u16le<14>(bs);
        if (reserved_sec == 0)
            return Result::ErrorInvalid;

        const uint8_t num_fats = u8<16>(bs);
        if (num_fats == 0)
            return Result::ErrorInvalid;

        const uint16_t root_entries = u16le<17>(bs);
        const uint16_t total_sec_16 = u16le<19>(bs);
        const uint16_t fat_size_16 = u16le<22>(bs);
        const uint32_t total_sec_32 = u32le<32>(bs);
        const uint32_t fat_size_32 = u32le<36>(bs);

        const uint64_t total_sec = total_sec_16 != 0 ? total_sec_16 : total_sec_32;
        const uint64_t fat_size = fat_size_16 != 0 ? fat_size_16 : fat_size_32;
        if (total_sec == 0 || fat_size == 0)
            return Result::ErrorInvalid;

        const uint64_t root_dir_sectors
            = ((static_cast<uint64_t>(root_entries) * 32U) + bytes_per_sec - 1U) / bytes_per_sec;
        const uint64_t first_data_sector
            = static_cast<uint64_t>(reserved_sec) + (static_cast<uint64_t>(num_fats) * fat_size) + root_dir_sectors;
        if (first_data_sector >= total_sec)
            return Result::ErrorInvalid;

        const uint64_t cluster_count = (total_sec - first_data_sector) / sec_per_cluster;
        if (cluster_count == 0)
            return Result::ErrorInvalid;

        enum class FatType { Fat12, Fat16, Fat32 };
        const FatType fat_type
            = cluster_count < 4085 ? FatType::Fat12 : (cluster_count < 65525 ? FatType::Fat16 : FatType::Fat32);

        size_t boot_flags_offset = 0;
        switch (fat_type)
        {
            case FatType::Fat32:
                if (root_entries != 0 || fat_size_16 != 0 || fat_size_32 == 0)
                    return Result::ErrorInvalid;
                boot_flags_offset = 0x41;
                break;
            case FatType::Fat16:
            case FatType::Fat12:
                if (root_entries == 0 || fat_size_16 == 0)
                    return Result::ErrorInvalid;
                boot_flags_offset = 0x25;
                break;
        }

        if (bs[boot_flags_offset] & 0x01 /* boot sector dirty */)
            return Result::Dirty;

        if (fat_type == FatType::Fat12)
            return Result::Clean;

        uint64_t fat_index = 0;
        if (fat_type == FatType::Fat32)
        {
            if (const uint16_t ext_flags = u16le<40>(bs); (ext_flags & 0x0080) != 0)
            {
                fat_index = ext_flags & 0x000F;
                if (fat_index >= num_fats)
                    return Result::ErrorInvalid;
            }
        }

        const uint64_t fat_offset = (static_cast<uint64_t>(reserved_sec) + (fat_index * fat_size)) * bytes_per_sec;
        const uint64_t entry_offset = fat_type == FatType::Fat32 ? 4U : 2U;

        if (fat_type == FatType::Fat32)
        {
            uint8_t fat_entry[4];
            if (!read(fat_entry, sizeof(fat_entry), fat_offset + entry_offset))
                return Result::ErrorIO;

            const uint32_t value = u32le<0>(fat_entry);
            return (value & 0x08000000 /* clean shutdown */) == 0 || (value & 0x04000000 /* no hard errors */) == 0
                       ? Result::Dirty
                       : Result::Clean;
        }
        else
        {
            uint8_t fat_entry[2];
            if (!read(fat_entry, sizeof(fat_entry), fat_offset + entry_offset))
                return Result::ErrorIO;

            const uint16_t value = u16le<0>(fat_entry);
            return (value & 0x8000 /* clean shutdown */) == 0 || (value & 0x4000 /* no hard errors */) == 0
                       ? Result::Dirty
                       : Result::Clean;
        }
    }

private:
    const std::filesystem::path _dev_path;
    int _fd;
    Type _fs_type;
};

// The device number of the filesystem that holds `path`, which keys the in-flight budgets.
inline std::optional<std::uintmax_t> FilesystemIdOf(const std::filesystem::path &path) noexcept
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return std::nullopt;
    return static_cast<std::uintmax_t>(st.st_dev);
}

} // namespace vigil::utils

#endif
