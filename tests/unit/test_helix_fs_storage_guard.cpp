// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/text_io.h"
#include "helix_fs.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

// A thread barred from storage (the ESP32 HTTP lane, whose PSRAM stack cannot
// survive a flash access) gets a refusal from every entry point instead of
// reaching the filesystem; every other thread is unaffected.
TEST_CASE("helix_fs: a thread barred from storage is refused, others are not",
          "[helix_fs][storage_guard]") {
    char tmpl[] = "/tmp/helix_storage_guard_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const std::string dir = tmpl;
    const std::string file = dir + "/f.txt";
    REQUIRE(helix::text_io::write_file(file, "hello"));
    const std::string new_dir = dir + "/made/here";

    const std::string exe = dir + "/run.sh";
    REQUIRE(helix::text_io::write_file(exe, "#!/bin/sh\n"));
    REQUIRE(::chmod(exe.c_str(), 0755) == 0);
    const std::string link = dir + "/link";
    REQUIRE(::symlink(file.c_str(), link.c_str()) == 0);
    const std::string renamed = dir + "/renamed.txt";
    const std::string copied = dir + "/copy.txt";
    const std::string written = dir + "/written.txt";

    // Every entry point, each answered on the barred thread. A true here means
    // the call reached the filesystem.
    struct Seen {
        bool exists = true, is_dir = true, is_regular = true, is_symlink = true, owner_exec = true,
             mtime = true, space = true, canonical = true, created = true, removed = true,
             renamed = true, copied = true, listed = true, read = true, sized = true,
             written = true, written_atomic = true;
        int exists_errno = 0;
    } seen;
    char cwd[4096];
    REQUIRE(::getcwd(cwd, sizeof(cwd)) != nullptr);
    REQUIRE(::chdir(dir.c_str()) == 0);
    std::thread barred([&] {
        namespace fs = helix::fs;
        namespace tio = helix::text_io;
        fs::forbid_storage_on_this_thread("test_lane");
        seen.exists = fs::exists(file);
        seen.exists_errno = errno;
        seen.is_dir = fs::is_directory(dir);
        seen.is_regular = fs::is_regular_file(file);
        seen.is_symlink = fs::is_symlink(link);
        seen.owner_exec = fs::is_owner_executable(exe);
        seen.mtime = fs::mtime_ns(file).has_value();
        seen.space = fs::space_available(dir).has_value();
        seen.canonical = fs::canonical(file).has_value();
        // Relative, so a missing guard would create the first component even
        // though every later stat is refused.
        seen.created = fs::create_directories("made/here");
        seen.removed = fs::remove(file);
        seen.renamed = fs::rename(file, renamed);
        seen.copied = fs::copy_file(file, copied);
        seen.listed = fs::list_dir(dir).has_value();
        seen.read = tio::read_file(file).has_value();
        seen.sized = tio::file_size(file).has_value();
        seen.written = tio::write_file(written, "x");
        seen.written_atomic = tio::write_file_atomic(written, "x");
    });
    barred.join();
    REQUIRE(::chdir(cwd) == 0);

    CHECK_FALSE(seen.exists);
    CHECK(seen.exists_errno == EPERM);
    CHECK_FALSE(seen.is_dir);
    CHECK_FALSE(seen.is_regular);
    CHECK_FALSE(seen.is_symlink);
    CHECK_FALSE(seen.owner_exec);
    CHECK_FALSE(seen.mtime);
    CHECK_FALSE(seen.space);
    CHECK_FALSE(seen.canonical);
    CHECK_FALSE(seen.created);
    CHECK_FALSE(seen.removed);
    CHECK_FALSE(seen.renamed);
    CHECK_FALSE(seen.copied);
    CHECK_FALSE(seen.listed);
    CHECK_FALSE(seen.read);
    CHECK_FALSE(seen.sized);
    CHECK_FALSE(seen.written);
    CHECK_FALSE(seen.written_atomic);

    // Nothing reached the disk, and this thread still can.
    CHECK_FALSE(helix::fs::exists(new_dir));
    CHECK_FALSE(helix::fs::exists(dir + "/made"));
    CHECK_FALSE(helix::fs::exists(renamed));
    CHECK_FALSE(helix::fs::exists(copied));
    CHECK_FALSE(helix::fs::exists(written));
    CHECK(helix::fs::exists(file));
    CHECK(helix::fs::is_symlink(link));
    CHECK(helix::fs::space_available(dir).has_value());
    CHECK(helix::text_io::read_file(file) == std::optional<std::string>("hello"));

    for (const auto& p : {link, exe, file})
        REQUIRE(helix::fs::remove(p));
    REQUIRE(helix::fs::remove(dir));
}
