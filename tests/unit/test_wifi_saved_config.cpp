// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_saved_config.h"

#include <climits>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

/**
 * Keeping wpa_supplicant's saved config off volatile storage.
 *
 * wpa_supplicant writes its config atomically — temp file plus rename() — and
 * that rename replaces a persistence SYMLINK with a regular file. On a firmware
 * that points `-c` at /var/run and symlinks it to persistent storage, the first
 * SAVE_CONFIG destroys the link and every credential afterwards is written to
 * RAM, then lost at power-off.
 *
 * Device-verified on a Snapmaker U1 (2026-07-29): restored the symlink, ran one
 * save_config, and it came back a 166-byte regular file on tmpfs while the
 * persistent file still had zero network blocks. Seeding the persistent file by
 * hand and rebooting — with the vendor's own restore path disabled — brought
 * WiFi up, proving boot reads that file.
 *
 * These tests pin the two halves that make the repair safe:
 *   - we only mirror when there is genuinely somewhere durable to mirror TO
 *   - a healthy platform (persistent config, no symlink) is left completely alone
 */

using helix::wifi::detail::is_volatile_path;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

constexpr const char* CONFIG_BODY = "ctrl_interface=/var/run/wpa_supplicant\n"
                                    "update_config=1\n"
                                    "\n"
                                    "network={\n"
                                    "\tssid=\"TestNet\"\n"
                                    "\tpsk=\"testpass\"\n"
                                    "}\n";

} // namespace

TEST_CASE("Volatile filesystems are recognised", "[network][wifi][savedconfig]") {
    SECTION("/dev/shm is tmpfs") {
        // Present on every Linux CI box and on the printers.
        struct stat st {};
        if (::stat("/dev/shm", &st) == 0) {
            CHECK(is_volatile_path("/dev/shm"));
        }
    }

    SECTION("an ordinary on-disk path is not volatile") {
        // The repo itself lives on real storage.
        CHECK_FALSE(is_volatile_path("."));
    }

    SECTION("a nonexistent path is treated as persistent") {
        // statfs fails; assuming persistent means we do nothing, which is the
        // safe direction — we never rewrite a file we failed to understand.
        CHECK_FALSE(is_volatile_path("/nonexistent/helix/path/xyz"));
    }
}

TEST_CASE("A healthy platform is left alone", "[network][wifi][savedconfig][regression]") {
    // THE REGRESSION: on the K2 (and every other printer whose wpa_supplicant
    // persists correctly) this feature must do nothing at all.
    SECTION("a plain persistent config yields no mirror target") {
        const std::string conf = "/tmp/helix_plain_conf_test.conf";
        {
            std::ofstream out(conf);
            out << CONFIG_BODY;
        }

        helix::wifi::remember_persistent_target(conf);
        CHECK(helix::wifi::persistent_target().empty());
        CHECK_FALSE(helix::wifi::mirror_to_persistent(conf));

        ::unlink(conf.c_str());
    }

    SECTION("an empty path yields no target") {
        helix::wifi::remember_persistent_target("");
        CHECK(helix::wifi::persistent_target().empty());
        CHECK_FALSE(helix::wifi::mirror_to_persistent(""));
    }
}

TEST_CASE("A symlink to persistent storage is remembered and mirrored",
          "[network][wifi][savedconfig][regression]") {
    // Reproduces the U1 shape: a config path that is a symlink onto durable
    // storage, which wpa_supplicant's rename() will later replace.
    const std::string durable = "/tmp/helix_durable_wpa.conf";
    const std::string link = "/tmp/helix_link_wpa.conf";
    ::unlink(durable.c_str());
    ::unlink(link.c_str());

    {
        std::ofstream out(durable);
        out << "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n";
    }
    REQUIRE(::symlink(durable.c_str(), link.c_str()) == 0);
    REQUIRE(::chmod(durable.c_str(), 0644) == 0);

    helix::wifi::remember_persistent_target(link);

    SECTION("the link's target is captured") {
        // remember_persistent_target() stores the realpath(), which also resolves
        // symlinked *prefixes* — macOS has /tmp -> /private/tmp. Resolve the
        // expectation the same way instead of comparing to the literal we wrote.
        char resolved[PATH_MAX];
        REQUIRE(::realpath(durable.c_str(), resolved) != nullptr);
        CHECK(helix::wifi::persistent_target() == std::string(resolved));
    }

    SECTION("mirroring copies the saved config onto the durable file") {
        // Simulate what wpa_supplicant does: replace the symlink with a real
        // file holding the new credentials.
        ::unlink(link.c_str());
        {
            std::ofstream out(link);
            out << CONFIG_BODY;
        }

        REQUIRE(helix::wifi::mirror_to_persistent(link));

        const std::string durable_contents = read_file(durable);
        CHECK(durable_contents.find("ssid=\"TestNet\"") != std::string::npos);
        CHECK(durable_contents.find("psk=\"testpass\"") != std::string::npos);
    }

    SECTION("the durable file keeps its permissions") {
        ::unlink(link.c_str());
        {
            std::ofstream out(link);
            out << CONFIG_BODY;
        }
        REQUIRE(helix::wifi::mirror_to_persistent(link));

        // Tightening could lock a vendor UI running as another user out of its
        // own config.
        struct stat st {};
        REQUIRE(::stat(durable.c_str(), &st) == 0);
        CHECK((st.st_mode & 07777) == 0644);
    }

    SECTION("no temp file is left behind") {
        ::unlink(link.c_str());
        {
            std::ofstream out(link);
            out << CONFIG_BODY;
        }
        REQUIRE(helix::wifi::mirror_to_persistent(link));

        struct stat st {};
        CHECK(::stat((durable + ".helix.tmp").c_str(), &st) != 0);
    }

    ::unlink(link.c_str());
    ::unlink(durable.c_str());
}

TEST_CASE("An empty saved config is not mirrored over a good one",
          "[network][wifi][savedconfig][regression]") {
    // Copying an empty file onto the durable target would erase the user's
    // saved network — worse than doing nothing.
    const std::string durable = "/tmp/helix_durable_guard.conf";
    const std::string link = "/tmp/helix_link_guard.conf";
    ::unlink(durable.c_str());
    ::unlink(link.c_str());

    {
        std::ofstream out(durable);
        out << CONFIG_BODY;
    }
    REQUIRE(::symlink(durable.c_str(), link.c_str()) == 0);
    helix::wifi::remember_persistent_target(link);

    ::unlink(link.c_str());
    { std::ofstream out(link); } // empty

    CHECK_FALSE(helix::wifi::mirror_to_persistent(link));
    CHECK(read_file(durable).find("ssid=\"TestNet\"") != std::string::npos);

    ::unlink(link.c_str());
    ::unlink(durable.c_str());
}
