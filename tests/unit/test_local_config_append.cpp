// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_local_config_append.cpp
 * @brief Appending an [include] to a config file HelixScreen edits directly
 *
 * The K2 fallback edits Moonraker's own moonraker.conf on the local filesystem,
 * which is the most destructive thing in this feature: the file belongs to the
 * vendor firmware, and a truncated or half-written one leaves the printer with a
 * Moonraker that will not start. So: read-modify-write, never truncate, and land
 * the result with a rename rather than an in-place rewrite.
 */

#include "system/moonraker_local_probe.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::diag::append_include_to_local_config;

namespace fs = std::filesystem;

namespace {

/// A throwaway directory, removed when the test ends however it ends.
class TempDir {
  public:
    TempDir() {
        static int counter = 0;
        dir_ = fs::temp_directory_path() /
               ("helix-cfgappend-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] fs::path path() const {
        return dir_;
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (dir_ / name).string();
    }

  private:
    fs::path dir_;
};

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Files in the directory, so a stray temp file shows up as a failure.
size_t file_count(const fs::path& dir) {
    size_t n = 0;
    for (auto it = fs::directory_iterator(dir); it != fs::directory_iterator(); ++it)
        ++n;
    return n;
}

/// What the plan hands the writer, and the line it must produce from it.
const char* TARGET = "/mnt/UDISK/printer_data/config/helixscreen.conf";
const char* INCLUDE = "[include /mnt/UDISK/printer_data/config/helixscreen.conf]";

/// A trimmed-down but real stock K2 moonraker.conf.
const char* VENDOR = "[server]\n"
                     "host: 0.0.0.0\n"
                     "port: 7125\n"
                     "\n"
                     "[file_manager]\n"
                     "config_path: /mnt/UDISK/printer_data/config\n"
                     "\n"
                     "[authorization]\n"
                     "trusted_clients:\n"
                     "  127.0.0.1\n";

} // namespace

TEST_CASE("append preserves every byte of the vendor config", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));
    CHECK(err.empty());

    const std::string after = read_file(path);
    // Everything that was there is still there, unmodified, at the front.
    CHECK(after.rfind(VENDOR, 0) == 0);
    CHECK(after.find(INCLUDE) != std::string::npos);
    CHECK(after.size() > std::string(VENDOR).size());
}

TEST_CASE("append is idempotent — a second call changes nothing", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));
    const std::string once = read_file(path);

    REQUIRE(append_include_to_local_config(path, TARGET, err));
    CHECK(read_file(path) == once);
    CHECK(err.empty());
}

TEST_CASE("append recognises the include even with odd spacing", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, std::string("   ") + INCLUDE + "   \n[server]\n");
    const std::string before = read_file(path);

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));
    CHECK(read_file(path) == before);
}

TEST_CASE("a different include target is not mistaken for ours", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, "[include helixscreen.conf]\n[server]\n");

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));

    const std::string after = read_file(path);
    CHECK(after.find(INCLUDE) != std::string::npos);
    CHECK(after.find("[include helixscreen.conf]") != std::string::npos);
}

TEST_CASE("append leaves no temp file behind", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);
    REQUIRE(file_count(tmp.path()) == 1);

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));
    CHECK(file_count(tmp.path()) == 1);
}

TEST_CASE("a config with no trailing newline still gets a well-formed include", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, "[server]\nhost: 0.0.0.0");

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));

    const std::string after = read_file(path);
    // The include must start its own line, or it becomes part of "host: 0.0.0.0".
    CHECK(after.find(std::string("\n") + INCLUDE) != std::string::npos);
    CHECK(after.rfind("[server]\nhost: 0.0.0.0", 0) == 0);
}

TEST_CASE("a missing config is an error, not a file we create", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("nope.conf");

    std::string err;
    CHECK_FALSE(append_include_to_local_config(path, TARGET, err));
    CHECK_FALSE(err.empty());
    // Creating Moonraker's config from nothing would replace a file we failed to
    // read with one that defines no [server] at all.
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("an empty include target is refused", "[local_append]") {
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);

    std::string err;
    CHECK_FALSE(append_include_to_local_config(path, "", err));
    CHECK(read_file(path) == VENDOR);
}

TEST_CASE("an unwritable directory fails without damaging the original", "[local_append]") {
    if (::geteuid() == 0) {
        SUCCEED("running as root — directory permissions do not apply");
        return;
    }
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);
    fs::permissions(tmp.path(), fs::perms::owner_write, fs::perm_options::remove);

    std::string err;
    CHECK_FALSE(append_include_to_local_config(path, TARGET, err));
    CHECK_FALSE(err.empty());

    fs::permissions(tmp.path(), fs::perms::owner_all, fs::perm_options::add);
    CHECK(read_file(path) == VENDOR);
}

TEST_CASE("append keeps the original file's permission bits", "[local_append]") {
    // The replacement arrives by rename, so it carries the temp file's mode, not
    // the original's. Moonraker's config can hold an API key: silently widening
    // 0600 to whatever the umask says is a real disclosure, and narrowing it can
    // lock the vendor's own tooling out of its file.
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    const auto before = fs::status(path).permissions();

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));

    CHECK(fs::status(path).permissions() == before);
    CHECK(read_file(path).rfind(VENDOR, 0) == 0);
}

TEST_CASE("a symlinked config is followed, not replaced", "[local_append]") {
    // Embedded firmware routinely reaches a config through a link. rename() replaces
    // whatever sits at the path, so following the link first is the difference
    // between editing Moonraker's config and quietly severing it.
    TempDir tmp;
    const std::string real = tmp.file("real-moonraker.conf");
    const std::string link = tmp.file("moonraker.conf");
    write_file(real, VENDOR);

    std::error_code ec;
    fs::create_symlink(real, link, ec);
    if (ec)
        SKIP("cannot create symlinks here: " + ec.message());

    std::string err;
    REQUIRE(append_include_to_local_config(link, TARGET, err));
    CHECK(err.empty());

    // The link survives as a link, and the file it names got the include.
    CHECK(fs::is_symlink(fs::symlink_status(link)));
    CHECK(fs::read_symlink(link).string() == real);
    CHECK(read_file(real).find(INCLUDE) != std::string::npos);
    CHECK(read_file(real).rfind(VENDOR, 0) == 0);

    // And no temp file was left in either name's directory.
    CHECK(file_count(tmp.path()) == 2);
}

TEST_CASE("a symlinked config is still idempotent through the link", "[local_append]") {
    TempDir tmp;
    const std::string real = tmp.file("real-moonraker.conf");
    const std::string link = tmp.file("moonraker.conf");
    write_file(real, VENDOR);

    std::error_code ec;
    fs::create_symlink(real, link, ec);
    if (ec)
        SKIP("cannot create symlinks here: " + ec.message());

    std::string err;
    REQUIRE(append_include_to_local_config(link, TARGET, err));
    const std::string after_first = read_file(real);
    REQUIRE(append_include_to_local_config(link, TARGET, err));
    CHECK(read_file(real) == after_first);
}

TEST_CASE("a dangling symlink is refused rather than materialised", "[local_append]") {
    // Creating the file would replace a config we could not read with one defining
    // no [server] — the same reason a missing config is an error, not a create.
    TempDir tmp;
    const std::string link = tmp.file("moonraker.conf");

    std::error_code ec;
    fs::create_symlink(tmp.file("nowhere.conf"), link, ec);
    if (ec)
        SKIP("cannot create symlinks here: " + ec.message());

    std::string err;
    CHECK_FALSE(append_include_to_local_config(link, TARGET, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(tmp.file("nowhere.conf")));
}

TEST_CASE("the appended config is readable back in full after the rename", "[local_append]") {
    // Guards the fsync-then-rename rewrite: the replacement must be complete and
    // byte-identical to what was intended, not merely present.
    TempDir tmp;
    const std::string path = tmp.file("moonraker.conf");
    write_file(path, VENDOR);

    std::string err;
    REQUIRE(append_include_to_local_config(path, TARGET, err));

    const std::string expect = std::string(VENDOR) + INCLUDE + "\n";
    CHECK(read_file(path) == expect);
    CHECK(fs::file_size(path) == expect.size());
}
