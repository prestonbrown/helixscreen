// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/text_io.h"
#include "helix_fs.h"
#include "test_helpers/unique_temp_dir.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace hfs = helix::fs;
namespace sfs = std::filesystem;

namespace {

struct ScratchDir {
    std::string path = helix::test::unique_temp_dir("helix_fs");
    ScratchDir() {
        sfs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        sfs::remove_all(path, ec);
    }
    std::string at(const std::string& name) const {
        return path + "/" + name;
    }
};

void touch(const std::string& p, const std::string& body = "x") {
    REQUIRE(helix::text_io::write_file(p, body));
}

// Path shapes the tree actually builds: sysfs and /proc paths, asset and cache
// files, config backups, and the degenerate cases around separators and dots.
const std::vector<std::string> kPaths = {
    "",
    "/",
    "//",
    "///",
    "///a",
    "a",
    "/a",
    "//a",
    "a/",
    "/a/",
    "a/b",
    "a//b",
    "a/b/",
    "a/b//",
    "/sys/class/net/wlan0/operstate",
    "/proc/1234/cmdline",
    "assets/images/printers/voron.png",
    "cache/thumbs/abc123.bin",
    "config/printer.cfg.bak",
    "settings.json.pre-migration",
    ".bashrc",
    "dir/.hidden",
    "archive.tar.gz",
    "noext.",
    ".",
    "..",
    "a/.",
    "a/..",
    "./a",
    "../a",
    "a/./b",
    "a/b/../c",
    "a/b/../../c",
    "../../x",
    "/..",
    "/../a",
    "/a/b/..",
    "a/../..",
    "a/b/../",
    "./",
    "a//./b/",
};

} // namespace

namespace {
// POSIX leaves a leading run of two or more slashes implementation-defined, and libc++
// and libstdc++ disagree on these inputs (libc++ keeps the run in parent_path and
// collapses it in lexically_normal). helix::fs follows libstdc++, the Linux target, so
// these inputs are pinned to literal values instead of compared against the std::
// result of whichever library builds the test.
struct LeadingSlashRunCase {
    const char* path;
    const char* parent;
    const char* normal;
};
const LeadingSlashRunCase kLeadingSlashRuns[] = {
    {"//", "//", "//"},
    {"///", "///", "///"},
    {"///a", "/", "/a"},
    {"//a", "/", "/a"},
};

bool has_leading_slash_run(std::string_view p) {
    return p.size() >= 2 && p[0] == '/' && p[1] == '/';
}
} // namespace

TEST_CASE("helix::fs path helpers agree with std::filesystem::path", "[helix_fs]") {
    for (const std::string& p : kPaths) {
        CAPTURE(p);
        const sfs::path sp(p);
        CHECK(hfs::filename(p) == sp.filename().string());
        CHECK(hfs::stem(p) == sp.stem().string());
        CHECK(hfs::extension(p) == sp.extension().string());
        if (has_leading_slash_run(p)) {
            continue; // pinned in the next case
        }
        CHECK(hfs::parent_path(p) == sp.parent_path().string());
        CHECK(hfs::lexically_normal(p) == sp.lexically_normal().string());
    }
}

TEST_CASE("helix::fs parent_path and lexically_normal pin leading slash runs", "[helix_fs]") {
    for (const LeadingSlashRunCase& c : kLeadingSlashRuns) {
        CAPTURE(c.path);
        CHECK(hfs::parent_path(c.path) == c.parent);
        CHECK(hfs::lexically_normal(c.path) == c.normal);
    }
}

TEST_CASE("helix::fs join_path agrees with path operator/", "[helix_fs]") {
    const std::vector<std::string> lhs = {"", "/", "a", "a/", "/sys/class/net", "cache//"};
    const std::vector<std::string> rhs = {"b", "b/c", "/abs", "wireless", "cmdline", ".."};
    for (const auto& a : lhs) {
        for (const auto& b : rhs) {
            CAPTURE(a, b);
            CHECK(hfs::join_path(a, b) == (sfs::path(a) / b).string());
        }
    }
}

TEST_CASE("helix::fs queries read a failed stat as no", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("f"));
    sfs::create_directory(d.at("sub"));

    CHECK(hfs::exists(d.at("f")));
    CHECK(hfs::is_regular_file(d.at("f")));
    CHECK_FALSE(hfs::is_directory(d.at("f")));
    CHECK(hfs::is_directory(d.at("sub")));
    CHECK_FALSE(hfs::is_regular_file(d.at("sub")));

    CHECK_FALSE(hfs::exists(d.at("missing")));
    CHECK_FALSE(hfs::is_directory(d.at("missing")));
    CHECK_FALSE(hfs::mtime_ns(d.at("missing")).has_value());
    // A path through a regular file fails with ENOTDIR, not ENOENT: still "no".
    CHECK_FALSE(hfs::exists(d.at("f/child")));
}

TEST_CASE("helix::fs symlinks: queries follow them, is_symlink and remove do not", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("target"));
    REQUIRE(::symlink(d.at("target").c_str(), d.at("link").c_str()) == 0);
    REQUIRE(::symlink(d.at("nowhere").c_str(), d.at("dangling").c_str()) == 0);

    CHECK(hfs::is_symlink(d.at("link")));
    CHECK(hfs::is_regular_file(d.at("link")));
    CHECK(hfs::is_symlink(d.at("dangling")));
    CHECK_FALSE(hfs::exists(d.at("dangling")));
    CHECK_FALSE(hfs::is_symlink(d.at("target")));

    CHECK(hfs::remove(d.at("dangling")));
    CHECK(hfs::remove(d.at("link")));
    CHECK(hfs::exists(d.at("target")));
}

TEST_CASE("helix::fs mtime_ns reads the file's modification time", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("f"));
    const struct timespec times[2] = {{1'700'000'000, 123'456'789}, {1'700'000'000, 123'456'789}};
    REQUIRE(::utimensat(AT_FDCWD, d.at("f").c_str(), times, 0) == 0);
    CHECK(hfs::mtime_ns(d.at("f")) == std::optional<std::int64_t>(1'700'000'000'123'456'789));
}

TEST_CASE("helix::fs create_directories nests, tolerates existing, refuses a file", "[helix_fs]") {
    ScratchDir d;
    CHECK(hfs::create_directories(d.at("a/b/c")));
    CHECK(sfs::is_directory(d.at("a/b/c")));
    CHECK(hfs::create_directories(d.at("a/b/c")));  // already there
    CHECK(hfs::create_directories(d.at("a/b/c/"))); // trailing separator

    touch(d.at("file"));
    errno = 0;
    CHECK_FALSE(hfs::create_directories(d.at("file/sub")));
    CHECK(errno == ENOTDIR);
}

TEST_CASE("helix::fs remove distinguishes absent from failed", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("f"));
    CHECK(hfs::remove(d.at("f")));
    CHECK_FALSE(sfs::exists(d.at("f")));

    errno = 0;
    CHECK_FALSE(hfs::remove(d.at("f")));
    CHECK(errno == ENOENT);

    sfs::create_directory(d.at("empty"));
    CHECK(hfs::remove(d.at("empty")));

    sfs::create_directory(d.at("full"));
    touch(d.at("full/x"));
    errno = 0;
    CHECK_FALSE(hfs::remove(d.at("full")));
    CHECK(errno != ENOENT);
    CHECK(sfs::exists(d.at("full/x")));
}

TEST_CASE("helix::fs rename replaces an existing target", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("from"), "new");
    touch(d.at("to"), "old");
    CHECK(hfs::rename(d.at("from"), d.at("to")));
    CHECK_FALSE(sfs::exists(d.at("from")));
    CHECK(helix::text_io::read_file(d.at("to")) == "new");
}

TEST_CASE("helix::fs copy_file honours the overwrite flag", "[helix_fs]") {
    ScratchDir d;
    std::string big(20000, '\0');
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>(i * 7);
    }
    touch(d.at("src"), big);

    CHECK(hfs::copy_file(d.at("src"), d.at("dst")));
    CHECK(helix::text_io::read_file(d.at("dst")) == big);

    touch(d.at("dst"), "keep");
    errno = 0;
    CHECK_FALSE(hfs::copy_file(d.at("src"), d.at("dst")));
    CHECK(errno == EEXIST);
    CHECK(helix::text_io::read_file(d.at("dst")) == "keep");

    CHECK(hfs::copy_file(d.at("src"), d.at("dst"), /*overwrite=*/true));
    CHECK(helix::text_io::read_file(d.at("dst")) == big);

    CHECK_FALSE(hfs::copy_file(d.at("missing"), d.at("dst2")));
    CHECK_FALSE(sfs::exists(d.at("dst2")));
}

TEST_CASE("helix::fs list_dir skips dot entries and types every entry", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("a.bin"));
    touch(d.at("b.png"));
    sfs::create_directory(d.at("sub"));
    REQUIRE(::symlink(d.at("sub").c_str(), d.at("sublink").c_str()) == 0);
    REQUIRE(::symlink(d.at("nowhere").c_str(), d.at("dangling").c_str()) == 0);

    const auto entries = hfs::list_dir(d.path);
    REQUIRE(entries.has_value());
    std::vector<std::string> names;
    for (const auto& e : *entries) {
        names.push_back(e.name);
        CHECK(e.path == d.at(e.name));
        CHECK(e.is_dir == sfs::is_directory(e.path));
        CHECK(e.is_regular == sfs::is_regular_file(e.path));
    }
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"a.bin", "b.png", "dangling", "sub", "sublink"});

    errno = 0;
    CHECK_FALSE(hfs::list_dir(d.at("missing")).has_value());
    CHECK(errno == ENOENT);
}

TEST_CASE("helix::fs canonical and space_available on a real directory", "[helix_fs]") {
    ScratchDir d;
    touch(d.at("f"));
    const auto c = hfs::canonical(d.path + "/./f");
    REQUIRE(c.has_value());
    CHECK(*c == sfs::canonical(d.at("f")).string());
    CHECK_FALSE(hfs::canonical(d.at("missing")).has_value());

    // Free space moves between two calls; bound it by the volume instead.
    const auto avail = hfs::space_available(d.path);
    REQUIRE(avail.has_value());
    CHECK(*avail > 0);
    CHECK(*avail <= sfs::space(d.path).capacity);

    CHECK(hfs::is_owner_executable("/bin/sh"));
    CHECK_FALSE(hfs::is_owner_executable(d.at("f")));
}
