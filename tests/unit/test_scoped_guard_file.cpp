// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/text_io.h"
#include "helix_fs.h"
#include "scoped_guard_file.h"
#include "test_helpers/unique_temp_dir.h"

#include <filesystem>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

namespace sfs = std::filesystem;

namespace {

struct ScratchDir {
    std::string path = helix::test::unique_temp_dir("scoped_guard_file");
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

} // namespace

TEST_CASE("ScopedGuardFile: the file exists exactly while the guard lives", "[scoped_guard_file]") {
    ScratchDir dir;
    const std::string path = dir.at("gpu_3d_guard");

    std::optional<helix::ScopedGuardFile> guard;
    guard.emplace(path);
    CHECK(guard->armed());
    CHECK(guard->path() == path);
    REQUIRE(helix::fs::exists(path));
    CHECK(helix::text_io::read_file(path).value_or("") == "1");

    guard.reset();
    CHECK_FALSE(helix::fs::exists(path));
}

TEST_CASE("ScopedGuardFile: an early return removes the file", "[scoped_guard_file]") {
    ScratchDir dir;
    const std::string path = dir.at("gpu_blur_guard");

    auto init_that_fails_gracefully = [&]() -> bool {
        const helix::ScopedGuardFile guard(path);
        REQUIRE(helix::fs::exists(path));
        return false;
    };

    CHECK_FALSE(init_that_fails_gracefully());
    CHECK_FALSE(helix::fs::exists(path));
}

TEST_CASE("ScopedGuardFile: an unwritable path reports unarmed and creates nothing",
          "[scoped_guard_file]") {
    ScratchDir dir;
    const std::string path = dir.at("missing_dir/gpu_3d_guard");

    {
        const helix::ScopedGuardFile guard(path);
        CHECK_FALSE(guard.armed());
        CHECK_FALSE(helix::fs::exists(path));
    }
    CHECK_FALSE(helix::fs::exists(dir.at("missing_dir")));
}
