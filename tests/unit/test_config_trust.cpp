// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "app_constants.h"
#include "config.h"
#include "system/config_trust.h"
#include "system/update_checker.h"
#include "test_helpers/unique_temp_dir.h"

#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace {

// Points the trusted update-URL file at a scratch state dir so the tests see
// real ownership/mode state and never the developer machine's
// /var/lib/helixscreen.
class StateDirGuard {
  public:
    StateDirGuard()
        : prev_(helix::app_constants::update::detail::state_dir_ref()),
          dir_(helix::test::unique_temp_dir("helix_ct_state")) {
        std::filesystem::create_directories(dir_);
        // The trust gate refuses update_urls.json in a group/world-writable
        // directory, so pin the mode rather than trusting the umask.
        REQUIRE(::chmod(dir_.c_str(), 0755) == 0);
        helix::app_constants::update::detail::state_dir_ref() = dir_;
    }
    ~StateDirGuard() {
        helix::app_constants::update::detail::state_dir_ref() = prev_;
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::string& dir() const {
        return dir_;
    }
    std::string file() const {
        return dir_ + "/update_urls.json";
    }
    void write(const std::string& body, mode_t mode) {
        std::ofstream f(file(), std::ios::trunc);
        f << body;
        f.close();
        REQUIRE(chmod(file().c_str(), mode) == 0);
    }

  private:
    std::string prev_;
    std::string dir_;
};

// /var/tmp is outside the rule's /tmp and /var/log roots, so a scratch dir
// there isolates the install-root branch of log_path_allowed.
class InstallRootGuard {
  public:
    InstallRootGuard()
        : prev_(helix::config_trust::detail::install_root_override_ref()),
          dir_("/var/tmp/helix_ct_root_" + helix::test::unique_suffix()) {
        std::filesystem::create_directories(dir_);
        REQUIRE(chmod(dir_.c_str(), 0755) == 0);
        helix::config_trust::detail::install_root_override_ref() = dir_;
    }
    ~InstallRootGuard() {
        helix::config_trust::detail::install_root_override_ref() = prev_;
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::string& dir() const {
        return dir_;
    }

  private:
    std::string prev_;
    std::string dir_;
};

} // namespace

TEST_CASE("config_trust: update_urls.json absent yields defaults", "[config-trust][update]") {
    StateDirGuard guard;
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());
}

TEST_CASE("config_trust: update_urls.json supplies both URLs", "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": \"https://mirror.example.com/rel\", \"dev_url\": "
                "\"https://dev.example.com\"}",
                0644);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url == "https://mirror.example.com/rel");
    REQUIRE(urls.dev_url == "https://dev.example.com");
}

TEST_CASE("config_trust: group-writable update_urls.json is refused", "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": \"https://mirror.example.com\"}", 0664);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());
}

TEST_CASE("config_trust: world-writable update_urls.json is refused", "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"dev_url\": \"https://dev.example.com\"}", 0666);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());
}

TEST_CASE("config_trust: update_urls.json must be a JSON object", "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("[\"https://mirror.example.com\"]", 0644);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());

    guard.write("{\"r2_url\": \"https://mirror.example.com\"", 0644);
    urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());
}

TEST_CASE("config_trust: non-string URL fields are ignored", "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": 1, \"dev_url\": true}", 0644);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url.empty());

    // A valid field beside a wrong-typed one still reads.
    guard.write("{\"r2_url\": 1, \"dev_url\": \"https://dev.example.com\"}", 0644);
    urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    REQUIRE(urls.dev_url == "https://dev.example.com");
}

TEST_CASE_METHOD(HelixTestFixture, "config trust: UpdateChecker ignores a settings.json r2_url",
                 "[config-trust][update]") {
    StateDirGuard guard;
    auto* config = helix::Config::get_instance();
    config->set<std::string>("/update/r2_url", "https://mirror.example.com/rel");
    REQUIRE(UpdateChecker::effective_r2_base_url() ==
            std::string(UpdateChecker::kDefaultR2BaseUrl));
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker: trusted update_urls.json overrides r2 base",
                 "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": \"https://mirror.example.com/rel///\"}", 0644);
    REQUIRE(UpdateChecker::effective_r2_base_url() == "https://mirror.example.com/rel");
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker: untrusted update_urls.json falls to default",
                 "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": \"https://mirror.example.com\"}", 0666);
    REQUIRE(UpdateChecker::effective_r2_base_url() ==
            std::string(UpdateChecker::kDefaultR2BaseUrl));
}

TEST_CASE("config_trust: log path roots", "[config-trust][log-path]") {
    REQUIRE(helix::config_trust::log_path_allowed("/tmp/helix-screen.log"));
    REQUIRE(helix::config_trust::log_path_allowed("/var/log/helix-screen.log"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("helix-screen.log"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/tmp/helix-screen.txt"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/tmp/../etc/helix-screen.log"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/tmp/./helix-screen.log"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/proc/helix-screen.log"));
    // Short values must be refused, never throw: a 2-3 char path wraps the
    // ".log" suffix arithmetic and would boot-loop the app.
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(""));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/ab"));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed("/.log"));
}

TEST_CASE("config_trust: log path subdirectory must be owner-locked", "[config-trust][log-path]") {
    const std::string dir = "/tmp/helix_ct_sub_" + helix::test::unique_suffix();
    std::filesystem::create_directories(dir);
    REQUIRE(chmod(dir.c_str(), 0755) == 0);
    REQUIRE(helix::config_trust::log_path_allowed(dir + "/helix-screen.log"));
    REQUIRE(chmod(dir.c_str(), 0777) == 0);
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(dir + "/helix-screen.log"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("config_trust: log path may sit in the install dir", "[config-trust][log-path]") {
    InstallRootGuard guard;
    const std::string sub = guard.dir() + "/logs";
    std::filesystem::create_directories(sub);
    REQUIRE(chmod(sub.c_str(), 0755) == 0);
    REQUIRE(helix::config_trust::log_path_allowed(guard.dir() + "/helix-screen.log"));
    REQUIRE(helix::config_trust::log_path_allowed(sub + "/helix-screen.log"));
    REQUIRE(chmod(sub.c_str(), 0777) == 0);
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(sub + "/helix-screen.log"));
}

TEST_CASE("config_trust: an existing log path must be a single-link regular file",
          "[config-trust][log-path]") {
    const std::string dir = "/tmp/helix_ct_hard_" + helix::test::unique_suffix();
    std::filesystem::create_directories(dir);
    // 0755 so only the hard link, not a loose parent, can be the refusing
    // condition.
    REQUIRE(chmod(dir.c_str(), 0755) == 0);
    const std::string log = dir + "/helix-screen.log";
    { std::ofstream f(log); }
    REQUIRE(chmod(log.c_str(), 0644) == 0);
    REQUIRE(helix::config_trust::log_path_allowed(log));

    // A second name for the same inode means an attacker can still reach the
    // file the app appends to as root.
    std::error_code ec;
    std::filesystem::create_hard_link(log, dir + "/twin.log", ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(log));
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(dir + "/twin.log"));
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("config_trust: update_urls.json in a loose directory is refused",
          "[config-trust][update]") {
    StateDirGuard guard;
    guard.write("{\"r2_url\": \"https://mirror.example.com\"}", 0644);
    REQUIRE(chmod(guard.dir().c_str(), 0777) == 0);
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url.empty());
    // The same file in a locked directory reads fine, so the directory was
    // the refusing condition.
    REQUIRE(chmod(guard.dir().c_str(), 0755) == 0);
    urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.r2_url == "https://mirror.example.com");
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker: settings.json dev_url is ignored",
                 "[config-trust][update]") {
    StateDirGuard guard;
    auto* config = helix::Config::get_instance();
    config->set<std::string>("/update/dev_url", "https://dev.example.com");
    helix::config_trust::UpdateUrls urls = helix::config_trust::read_update_urls();
    REQUIRE(urls.dev_url.empty());
    REQUIRE(urls.r2_url.empty());
}

TEST_CASE("config_trust: log path may not be a symlink", "[config-trust][log-path]") {
    const std::string dir = "/tmp/helix_ct_link_" + helix::test::unique_suffix();
    std::filesystem::create_directories(dir);
    // 0755 so only the symlink, not a loose parent, can be the refusing
    // condition.
    REQUIRE(chmod(dir.c_str(), 0755) == 0);
    const std::string target = dir + "/real.log";
    { std::ofstream f(target); }
    std::error_code ec;
    std::filesystem::create_symlink(target, dir + "/link.log", ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(helix::config_trust::log_path_allowed(dir + "/link.log"));
    std::filesystem::remove_all(dir, ec);
}
