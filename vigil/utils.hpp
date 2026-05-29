#pragma once
#ifndef VIGIL_UTILS_HPP
#define VIGIL_UTILS_HPP

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace vigil::utils {

class Execute
{
public:
    Execute(std::string program, std::vector<std::string> args) : _program(std::move(program)), _args(std::move(args))
    {
        _argv.reserve(_args.size() + 2);
        _argv.push_back(const_cast<char *>(_program.c_str()));
        for (const auto &arg: _args)
            _argv.push_back(const_cast<char *>(arg.c_str()));
        _argv.push_back(nullptr);
    }

    Execute(const Execute &) = delete;
    Execute &operator=(const Execute &) = delete;

    Execute(Execute &&) = delete;
    Execute &operator=(Execute &&) = delete;

    ~Execute() = default;

    [[nodiscard]] int operator()(const std::atomic<int> *signal = nullptr) const
    {
        pid_t pid = ::fork();
        if (pid == -1)
        {
            return -1;
        }
        else if (pid == 0)
        {
            int dev_null = ::open("/dev/null", O_WRONLY);
            if (dev_null == -1)
                ::_exit(1);
            ::dup2(dev_null, STDOUT_FILENO);
            ::dup2(dev_null, STDERR_FILENO);
            if (dev_null > STDERR_FILENO)
                ::close(dev_null);

            ::execvp(_argv[0], _argv.data());

            ::_exit(127);
        }
        else
        {
            int status;
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
                while (true)
                {
                    pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
                    if (wait_result == pid)
                        break;
                    if (wait_result == -1)
                    {
                        if (errno == EINTR)
                            continue;
                        return -1;
                    }

                    int sig = 0;
                    int cur_sig = signal->load(std::memory_order_acquire);
                    if (cur_sig != sig)
                    {
                        sig = cur_sig;
                        if (cur_sig > 0)
                        {
                            ::sigset_t set;
                            if (::sigemptyset(&set) == 0 && ::sigaddset(&set, cur_sig) == 0)
                                static_cast<void>(::kill(pid, cur_sig));
                        }
                    }

                    const ::timespec interval { 0, 10L * 1000L * 1000L };
                    static_cast<void>(::nanosleep(&interval, nullptr));
                }
            }

            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            else
                return -1;
        }
    }

private:
    std::string _program;
    std::vector<std::string> _args;
    mutable std::vector<char *> _argv;
};

class FSCK
{
public:
    enum Type {
        Ext,
        Fat,
        Unknown,
    };

    enum Result {
        Clean = 0,
        Dirty = 1,
        ErrorIO = 2,
        ErrorMagic = 3,
        ErrorInvalid = 4,
        ErrorUnsupported = 5,
    };

    FSCK(const std::filesystem::path &dev_path, std::string_view fs_type) noexcept : _fd(-1), _fs_type(Type::Unknown)
    {
        _fd = ::open(dev_path.c_str(), O_RDONLY | O_CLOEXEC);
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

    Result dirty() const noexcept
    {
        if (_fd < 0)
            return Result::ErrorIO;
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

    static inline uint16_t u16le(const uint8_t *p) noexcept
    {
        return static_cast<uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
    }

    static inline uint32_t u32le(const uint8_t *p) noexcept
    {
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
        uint8_t sb[0x64];
        if (!read(sb, sizeof(sb), 1024))
            return Result::ErrorIO;

        if (u16le(sb + 0x38) != 0xEF53 /* ext magic number */)
            return Result::ErrorMagic;

        const auto state = u16le(sb + 0x3A);
        const auto incompat_features = u32le(sb + 0x60);
        return (state & 0x0001 /* valid fs */) == 0 || (state & 0x0002 /* error fs */) != 0
                       || (incompat_features & 0x0004 /* needs recovery */) != 0
                   ? Result::Dirty
                   : Result::Clean;
    }

    Result checkFat() const noexcept
    {
        uint8_t bs[512];
        if (!read(bs, sizeof(bs), 0))
            return Result::ErrorIO;

        if (u16le(bs + 510) != 0xAA55 /* fat signature */)
            return Result::ErrorMagic;

        const uint16_t bytes_per_sec = u16le(bs + 11);
        if (bytes_per_sec < 512 || bytes_per_sec > 4096 || !isPowerOfTwo(bytes_per_sec))
            return Result::ErrorInvalid;

        const uint8_t sec_per_cluster = bs[13];
        if (sec_per_cluster > 128 || !isPowerOfTwo(sec_per_cluster))
            return Result::ErrorInvalid;

        const uint16_t reserved_sec = u16le(bs + 14);
        if (reserved_sec == 0)
            return Result::ErrorInvalid;

        const uint8_t num_fats = bs[16];
        if (num_fats == 0)
            return Result::ErrorInvalid;

        const uint16_t root_entries = u16le(bs + 17);
        const uint16_t total_sec_16 = u16le(bs + 19);
        const uint16_t fat_size_16 = u16le(bs + 22);
        const uint32_t total_sec_32 = u32le(bs + 32);
        const uint32_t fat_size_32 = u32le(bs + 36);

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

        enum StateOffset : size_t {
            Fat12 = 0,
            Fat16 = 0x25,
            Fat32 = 0x41,
        };
        const StateOffset so = cluster_count < 4085 ? StateOffset::Fat12
                                                    : (cluster_count < 65525 ? StateOffset::Fat16 : StateOffset::Fat32);
        switch (so)
        {
            case StateOffset::Fat32:
                if (root_entries != 0 || fat_size_16 != 0 || fat_size_32 == 0)
                    return Result::ErrorInvalid;
                break;
            case StateOffset::Fat16:
                if (root_entries == 0 || fat_size_16 == 0)
                    return Result::ErrorInvalid;
                break;
            default:
                return Result::ErrorUnsupported;
        }

        if (bs[static_cast<size_t>(so)] & 0x01 /* boot sector dirty */)
            return Result::Dirty;

        uint64_t fat_index = 0;
        if (so == StateOffset::Fat32)
        {
            if (const uint16_t ext_flags = u16le(bs + 40); (ext_flags & 0x0080) != 0)
            {
                fat_index = ext_flags & 0x000F;
                if (fat_index >= num_fats)
                    return Result::ErrorInvalid;
            }
        }

        const uint64_t fat_offset = (static_cast<uint64_t>(reserved_sec) + (fat_index * fat_size)) * bytes_per_sec;
        const uint64_t entry_offset = so == StateOffset::Fat32 ? 4U : 2U;

        if (so == StateOffset::Fat32)
        {
            uint8_t fat_entry[4];
            if (!read(fat_entry, sizeof(fat_entry), fat_offset + entry_offset))
                return Result::ErrorIO;

            const uint32_t value = u32le(fat_entry);
            return (value & 0x08000000 /* clean shutdown */) == 0 || (value & 0x04000000 /* no hard errors */) == 0
                       ? Result::Dirty
                       : Result::Clean;
        }
        else
        {
            uint8_t fat_entry[2];
            if (!read(fat_entry, sizeof(fat_entry), fat_offset + entry_offset))
                return Result::ErrorIO;

            const uint16_t value = u16le(fat_entry);
            return (value & 0x8000 /* clean shutdown */) == 0 || (value & 0x4000 /* no hard errors */) == 0
                       ? Result::Dirty
                       : Result::Clean;
        }
    }

private:
    int _fd;
    Type _fs_type;
};

} // namespace vigil::utils

#endif
