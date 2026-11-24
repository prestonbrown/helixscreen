// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_update_rollback.cpp
 * @brief Automatic-update rollback (#1304): which installs support it, the
 *        marker files shared with helix-launcher.sh, and what a just-started
 *        version reports to the launcher's guard.
 *
 * The launcher side (restoring the tree, the window, the start count) is
 * pinned by tests/shell/test_helix_launcher_rollback.bats.
 *
 * Run with: ./build/bin/helix-tests "[rollback]"
 */

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "system/update_rollback.h"
#include "test_helpers/scoped_env.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::update;

namespace {

RollbackFacts supported_facts() {
    RollbackFacts f;
    f.platform_key = "pi";
    f.guard_dir = true;
    f.supervised = true;
    return f;
}

/// A private HELIX_ROLLBACK_DIR for one test.
struct RollbackDirFixture : public LVGLTestFixture {
    std::string dir;
    helix::ScopedEnv env;

    RollbackDirFixture() : dir(make_dir()), env(kRollbackDirEnv, dir.c_str()) {}

    ~RollbackDirFixture() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    static std::string make_dir() {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "helix-rollback-XXXXXX").string();
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        return tmpl;
    }

    bool has(const char* name) const {
        return std::filesystem::exists(dir + "/" + name);
    }

    std::string read(const char* name) const {
        std::string out;
        read_rollback_file(dir, name, out);
        return out;
    }

    void arm() {
        RollbackMarker m{"/opt/helixscreen", "/opt/helixscreen.old", "1.0.0", "1.1.0"};
        REQUIRE(write_rollback_file(dir, kRollbackArmedFile, format_rollback_marker(m)));
    }
};

} // namespace

TEST_CASE("Rollback support: every fact must hold, and some platforms never can",
          "[auto_update][rollback]") {
    CHECK(rollback_support(supported_facts()) == RollbackSupport::Supported);
    for (const char* key : {"pi", "pi32", "k1", "mips", "ad5m", "cc1", "snapmaker-u1", "x86"}) {
        auto f = supported_facts();
        f.platform_key = key;
        CHECK(rollback_support(f) == RollbackSupport::Supported);
    }
    for (const char* key : {"k2", "x86-desktop", "android", "esp32"}) {
        auto f = supported_facts();
        f.platform_key = key;
        CHECK(rollback_support(f) == RollbackSupport::Platform);
    }

    auto f = supported_facts();
    f.guard_dir = false;
    CHECK(rollback_support(f) == RollbackSupport::NoGuard);

    f = supported_facts();
    f.supervised = false;
    CHECK(rollback_support(f) == RollbackSupport::NotSupervised);

    f = supported_facts();
    f.install_suppressed = true;
    CHECK(rollback_support(f) == RollbackSupport::InstallSuppressed);

    f = supported_facts();
    f.mod_payload = true;
    CHECK(rollback_support(f) == RollbackSupport::ModPayload);
}

TEST_CASE("Rollback support: the capability follows the rule over this process's facts",
          "[auto_update][rollback]") {
    {
        helix::ScopedEnv dir(kRollbackDirEnv, "/tmp/helix-rollback-guard");
        helix::ScopedEnv supervised("HELIX_SUPERVISED", "1");
        const RollbackFacts facts = current_rollback_facts();
        INFO("support: " << rollback_support_name(rollback_support(facts)) << " platform "
                         << facts.platform_key);
        REQUIRE(facts.guard_dir);
        REQUIRE(facts.supervised);
        CHECK(auto_rollback_supported() == (rollback_support(facts) == RollbackSupport::Supported));
        // On a host the rule accepts, the guard and the watchdog are all it takes.
        if (rollback_support(facts) != RollbackSupport::Platform && !facts.install_suppressed &&
            !facts.mod_payload) {
            CHECK(auto_rollback_supported());
        }
    }
    helix::ScopedEnv no_dir(kRollbackDirEnv, nullptr);
    CHECK_FALSE(auto_rollback_supported());
}

TEST_CASE("Rollback marker: the launcher's key=value lines round-trip", "[auto_update][rollback]") {
    RollbackMarker m{"/usr/data/helixscreen", "/usr/data/helixscreen.old.17", "1.2.0", "1.3.0"};
    const std::string text = format_rollback_marker(m);
    CHECK(rollback_value(text, "install_dir") == m.install_dir);
    CHECK(rollback_value(text, "previous_dir") == m.previous_dir);
    CHECK(rollback_value(text, "from_version") == m.from_version);
    CHECK(rollback_value(text, "to_version") == m.to_version);
    CHECK(rollback_value(text, "missing").empty());
    // A key is matched whole, never as a prefix of another.
    CHECK(rollback_value("to_version_x=9\nto_version=2\n", "to_version") == "2");

    auto result = parse_rollback_result("from_version=1.0\r\nto_version=1.1\r\nreason=timeout\r\n");
    REQUIRE(result.has_value());
    CHECK(result->from_version == "1.0");
    CHECK(result->to_version == "1.1");
    CHECK(result->reason == "timeout");
    CHECK_FALSE(parse_rollback_result("reason=timeout\n").has_value());
}

TEST_CASE("Startup health: only an armed rollback is reported, and a crash loop asks for it",
          "[auto_update][rollback]") {
    CHECK(decide_startup_health(false, false) == StartupHealth::NotArmed);
    CHECK(decide_startup_health(false, true) == StartupHealth::NotArmed);
    CHECK(decide_startup_health(true, true) == StartupHealth::RequestRollback);
    CHECK(decide_startup_health(true, false) == StartupHealth::ReportWhenSettled);
}

TEST_CASE_METHOD(RollbackDirFixture, "On UI ready: a crash-looping new version asks for rollback",
                 "[auto_update][rollback]") {
    arm();
    on_ui_ready(/*safe_mode=*/true);
    CHECK(rollback_value(read(kRollbackRequestFile), "reason") == "crash_loop");
    CHECK_FALSE(has(kRollbackHealthyFile));
}

TEST_CASE_METHOD(RollbackDirFixture,
                 "On UI ready: a healthy new version reports only after it has run a while",
                 "[auto_update][rollback]") {
    arm();
    on_ui_ready(/*safe_mode=*/false);
    process_lvgl((kRollbackHealthySeconds - 5) * 1000);
    CHECK_FALSE(has(kRollbackHealthyFile));
    process_lvgl(10 * 1000);
    CHECK(has(kRollbackHealthyFile));
    CHECK_FALSE(has(kRollbackRequestFile));
}

TEST_CASE_METHOD(RollbackDirFixture, "On UI ready: nothing armed, nothing reported",
                 "[auto_update][rollback]") {
    on_ui_ready(/*safe_mode=*/true);
    process_lvgl((kRollbackHealthySeconds + 5) * 1000);
    CHECK_FALSE(has(kRollbackRequestFile));
    CHECK_FALSE(has(kRollbackHealthyFile));
}

TEST_CASE_METHOD(RollbackDirFixture,
                 "On UI ready: a rollback the launcher made is consumed and its version dismissed",
                 "[auto_update][rollback]") {
    auto* config = helix::Config::get_instance();
    const std::string before = config->get<std::string>("/update/dismissed_version", "");
    REQUIRE(write_rollback_file(dir, kRollbackResultFile,
                                "from_version=1.0.0\nto_version=9.8.7\nreason=timeout\n"));

    on_ui_ready(/*safe_mode=*/false);

    CHECK_FALSE(has(kRollbackResultFile));
    CHECK(config->get<std::string>("/update/dismissed_version", "") == "9.8.7");

    config->set<std::string>("/update/dismissed_version", before);
    config->save();
}
