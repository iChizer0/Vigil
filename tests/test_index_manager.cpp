// The filesystem helpers of vigil/index_manager.hpp; IndexManager itself is not tested, since it needs fanotify, which
// needs elevated privileges.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "vigil/context.hpp"
#include "vigil/index_manager.hpp"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil;
using namespace vigil::index_manager;
using namespace vigil::tests;

namespace {

struct TempDir
{
    std::filesystem::path path;

    TempDir()
    {
        std::string p = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
        if (::mkdtemp(p.data()) == nullptr)
            throw std::runtime_error("mkdtemp failed");
        path = p;
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void Touch(const std::filesystem::path &path, const std::string &text = "x")
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
    if (!out)
        throw std::runtime_error("failed to write " + path.string());
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

} // namespace

VG_TEST_CASE(is_path_under_root_accepts_descendants_only)
{
    TempDir t;
    VG_CHECK(IsPathUnderRoot(t.path, t.path / "2026-10-19" / "clip.mp4"));
    VG_CHECK(IsPathUnderRoot(t.path, "2026-10-19/clip.mp4")); // relative: resolved against the root
    VG_CHECK(!IsPathUnderRoot(t.path, t.path));               // the root itself
    VG_CHECK(!IsPathUnderRoot(t.path, "../escape"));
    VG_CHECK(!IsPathUnderRoot(t.path, "2026-10-19/../../escape"));
    VG_CHECK(!IsPathUnderRoot(t.path, t.path.parent_path()));
    VG_CHECK(!IsPathUnderRoot("relative-root", "relative-root/a"));
}

VG_TEST_CASE(ensure_directory_exists_is_idempotent_and_never_removes_what_is_in_the_way)
{
    TempDir t;
    Context ctx(MakeFakeConfig());
    const auto dir = t.path / "2026-10-19" / ".thumb";
    VG_CHECK(EnsureDirectoryExists(dir, false, false, &ctx)); // creates the whole chain
    VG_CHECK(std::filesystem::is_directory(dir));
    VG_CHECK(EnsureDirectoryExists(dir, false, false, &ctx)); // and accepts it afterwards
    VG_CHECK(EnsureDirectoryExists(dir, true, true, &ctx));   // with or without re-applying permissions
    VG_CHECK(std::filesystem::is_directory(dir));

    // A regular file in the way is an error and is left alone.
    const auto file = t.path / "2026-10-20";
    Touch(file, "keep");
    VG_CHECK(!EnsureDirectoryExists(file, false, false, &ctx));
    VG_CHECK(std::filesystem::is_regular_file(file));
    VG_CHECK_EQ(Read(file), "keep");
    VG_CHECK(!EnsureDirectoryExists(file / "nested", false, false, &ctx)); // a file as a parent component
    VG_CHECK_EQ(Read(file), "keep");
}

VG_TEST_CASE(unique_publication_path_never_reuses_a_taken_name)
{
    TempDir t;
    const std::filesystem::path name = "2026-10-19 12.00.00 123456.mp4";
    VG_CHECK_EQ(UniquePublicationPath(t.path, name).string(), (t.path / name).string());
    Touch(t.path / name);
    const auto second = UniquePublicationPath(t.path, name);
    VG_CHECK_EQ(second.string(), (t.path / "2026-10-19 12.00.00 123456 (1).mp4").string());
    Touch(second);
    VG_CHECK_EQ(UniquePublicationPath(t.path, name).string(), (t.path / "2026-10-19 12.00.00 123456 (2).mp4").string());

    VG_CHECK_EQ(UniquePublicationPath(t.path, "noext").string(), (t.path / "noext").string());
    Touch(t.path / "noext");
    VG_CHECK_EQ(UniquePublicationPath(t.path, "noext").string(), (t.path / "noext (1)").string());

    // The thumbnail is named after the published file, so it cannot collide either.
    VG_CHECK_EQ(ThumbnailPathFrom(second).string(),
        (t.path / ".thumb" / "2026-10-19 12.00.00 123456 (1).mp4.thumb.jpg").string());
}

VG_TEST_MAIN()
