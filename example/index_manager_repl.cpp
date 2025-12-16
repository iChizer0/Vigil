#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "vigil/storage.hpp"

using namespace vigil;
using namespace vigil::storage;
using vigil::types::CacheFileInfo;
using vigil::types::FileSize;

namespace {

vg_sta_e log_cb(vg_log_level_e level, const char *msg, size_t bytes_len)
{
    std::cout << static_cast<int>(level) << ": ";
    std::cout.write(msg, bytes_len);
    std::cout << std::endl;
    return 0;
}

void printHelp()
{
    std::cout << "Commands:\n"
              << "  help                 Show this message\n"
              << "  pwd                  Show the active index root\n"
              << "  sync                 Rebuild the in-memory index from disk\n"
              << "  ls                   List first-level entries tracked by IndexManager\n"
              << "  used                 Print total tracked bytes\n"
              << "  reset                Clear the in-memory index state\n"
              << "  mkdir <dir>          Create (or ensure) a first-level directory under the root\n"
              << "  mkfile <dir> <file> <bytes>  Create/overwrite a file with the requested size\n"
              << "  cachemove <dir> <bytes>      Create a temp file via dd and move it into the index\n"
              << "  rm <path>            Remove a file or directory via IndexManager\n"
              << "  free <bytes>         Ask IndexManager to free least-recent directories\n"
              << "  exit|quit            Terminate the REPL\n";
}

std::vector<std::string> tokenize(const std::string &line)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    return tokens;
}

void printStats(IndexManager &manager)
{
    const auto stats = manager.getFirstLevelStats();
    if (stats.empty())
    {
        std::cout << "(index is empty)" << std::endl;
        return;
    }
    std::cout << "tracked directories:" << std::endl;
    for (const auto &[index, size]: stats)
    {
        const auto sys_time = std::chrono::file_clock::to_sys(index.creation_time);
        const std::time_t c_time = std::chrono::system_clock::to_time_t(sys_time);
        std::tm tm = *std::localtime(&c_time);
        std::cout << "  " << std::setw(24) << std::left << index.name << " | size: " << std::setw(12)
                  << std::right << size << " bytes | created: " << std::put_time(&tm, "%F %T") << std::endl;
    }
}

bool ensureDirectory(const std::filesystem::path &root, const std::string &dir)
{
    std::error_code ec;
    std::filesystem::create_directories(root / dir, ec);
    if (ec)
    {
        std::cout << "Failed to create directory: " << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool createFile(const std::filesystem::path &root, const std::string &dir, const std::string &file, FileSize bytes)
{
    if (bytes < 0)
    {
        std::cout << "Byte size must be non-negative" << std::endl;
        return false;
    }
    if (!ensureDirectory(root, dir))
        return false;
    std::ofstream ofs(root / dir / file, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
    {
        std::cout << "Failed to open file for writing" << std::endl;
        return false;
    }
    static constexpr std::size_t kChunk = 4096;
    std::array<char, kChunk> buffer {};
    FileSize remaining = bytes;
    while (remaining > 0)
    {
        const auto to_write = static_cast<std::streamsize>(std::min<FileSize>(remaining, kChunk));
        ofs.write(buffer.data(), to_write);
        if (!ofs)
        {
            std::cout << "Failed to write file contents" << std::endl;
            return false;
        }
        remaining -= to_write;
    }
    return true;
}

std::filesystem::path resolveRoot(int argc, char **argv)
{
    if (argc > 1)
        return std::filesystem::absolute(argv[1]);
    return std::filesystem::temp_directory_path() / "vigil_index_manager";
}

std::string escapePathForShell(const std::filesystem::path &path)
{
    std::string raw = path.string();
    std::string escaped;
    escaped.reserve(raw.size() + 2);
    escaped.push_back('"');
    for (char ch: raw)
    {
        if (ch == '"')
            escaped += "\\\"";
        else
            escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

bool createTempCacheFileWithDD(const std::filesystem::path &path, FileSize bytes)
{
    if (bytes <= 0)
    {
        std::cout << "Byte size must be positive" << std::endl;
        return false;
    }
    if (path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            std::cout << "Failed to prepare cache temp directory: " << ec.message() << std::endl;
            return false;
        }
    }
    const FileSize bs = std::clamp<FileSize>(bytes, 1, 1024 * 1024);
    const FileSize count = (bytes + bs - 1) / bs;
    std::ostringstream cmd;
    cmd << "dd if=/dev/zero of=" << escapePathForShell(path) << " bs=" << bs << " count=" << count
        << " status=none";
    const auto exit_code = std::system(cmd.str().c_str());
    if (exit_code != 0)
    {
        std::cout << "dd failed with exit code " << exit_code << std::endl;
        return false;
    }
    return true;
}

std::filesystem::path makeTempCacheFilePath(const std::filesystem::path &root)
{
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return root / ".tmp_cache_files" / ("cache_" + std::to_string(stamp));
}

} // namespace

int main(int argc, char **argv)
{
    vg_config_t cfg {};
    cfg.basic.log_cb = log_cb;
    cfg.basic.log_level = VG_LOG_LEVEL_DEBUG;
    cfg.storage.relay_directory = "relay";

    Context ctx(cfg);

    const auto root = resolveRoot(argc, argv);
    if (!IndexManager::ensureDirectoryExist(root, ctx))
        return 1;

    IndexManager manager(ctx, root);

    std::cout << "IndexManager REPL using root: " << root << std::endl;
    printHelp();

    std::string line;
    while (std::cout << "vigil-index> " && std::getline(std::cin, line))
    {
        const auto tokens = tokenize(line);
        if (tokens.empty())
            continue;
        const auto &cmd = tokens.front();
        if (cmd == "help")
            printHelp();
        else if (cmd == "pwd")
            std::cout << root << std::endl;
        else if (cmd == "sync")
        {
            std::cout << (manager.trySync() ? "Index sync complete" : "Index sync failed") << std::endl;
        }
        else if (cmd == "ls")
            printStats(manager);
        else if (cmd == "used")
            std::cout << manager.usedBytes() << " bytes tracked" << std::endl;
        else if (cmd == "reset")
        {
            manager.reset();
            std::cout << "Index reset" << std::endl;
        }
        else if (cmd == "mkdir")
        {
            if (tokens.size() != 2)
            {
                std::cout << "Usage: mkdir <dir>" << std::endl;
                continue;
            }
            if (ensureDirectory(root, tokens[1]))
                std::cout << "Directory ready" << std::endl;
        }
        else if (cmd == "mkfile")
        {
            if (tokens.size() != 4)
            {
                std::cout << "Usage: mkfile <dir> <file> <bytes>" << std::endl;
                continue;
            }
            try
            {
                const auto bytes = static_cast<FileSize>(std::stoll(tokens[3]));
                if (createFile(root, tokens[1], tokens[2], bytes))
                    std::cout << "File written" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cout << "Invalid byte value: " << e.what() << std::endl;
            }
        }
        else if (cmd == "cachemove")
        {
            if (tokens.size() != 3)
            {
                std::cout << "Usage: cachemove <dir> <bytes>" << std::endl;
                continue;
            }
            try
            {
                const auto bytes = static_cast<FileSize>(std::stoll(tokens[2]));
                const auto temp_file = makeTempCacheFilePath(root);
                if (!createTempCacheFileWithDD(temp_file, bytes))
                    continue;
                CacheFileInfo cache_file_info {};
                cache_file_info.cache_file_path = temp_file;
                cache_file_info.target_dir_name = tokens[1];
                cache_file_info.creation_time = std::chrono::file_clock::now();
                if (auto added = manager.moveAndAddCacheFile(cache_file_info))
                    std::cout << "Moved cache file into '" << tokens[1] << "' (" << *added << " bytes)"
                              << std::endl;
                else
                {
                    std::cout << "moveAndAddCacheFile failed" << std::endl;
                    std::error_code ec;
                    std::filesystem::remove(temp_file, ec);
                }
            }
            catch (const std::exception &e)
            {
                std::cout << "Invalid byte value: " << e.what() << std::endl;
            }
        }
        else if (cmd == "rm")
        {
            if (tokens.size() != 2)
            {
                std::cout << "Usage: rm <relative-path>" << std::endl;
                continue;
            }
            std::cout << (manager.removeFileOrDirectory(tokens[1]) ? "Removed" : "Removal failed") << std::endl;
        }
        else if (cmd == "free")
        {
            if (tokens.size() != 2)
            {
                std::cout << "Usage: free <bytes>" << std::endl;
                continue;
            }
            try
            {
                const auto bytes = static_cast<FileSize>(std::stoll(tokens[1]));
                const auto freed = manager.freeUpStaledFiles(bytes);
                std::cout << "Freed " << freed << " bytes" << std::endl;
            }
            catch (const std::exception &e)
            {
                std::cout << "Invalid byte value: " << e.what() << std::endl;
            }
        }
        else if (cmd == "exit" || cmd == "quit")
            break;
        else
            std::cout << "Unknown command. Type 'help' for options." << std::endl;
    }

    return 0;
}
