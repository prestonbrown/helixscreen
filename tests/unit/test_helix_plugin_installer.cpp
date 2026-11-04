// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_helix_plugin_installer.cpp
 * @brief Unit tests for HelixPluginInstaller
 *
 * Tests cover:
 * 1. Local moonraker detection (is_local_moonraker)
 * 2. Preference management (should_prompt_install, set_install_declined)
 * 3. Install script path resolution
 * 4. Remote install command generation
 * 5. Running install.sh: exit codes, timeout, arguments - always a stub in a
 *    temp dir (SandboxedInstaller), never the repository's script
 */

#include "../../include/config.h"
#include "../../include/helix_plugin_installer.h"
#include "../../include/helix_version.h"
#include "../test_helpers/helix_plugin_installer_test_access.h"
#include "../test_helpers/plugin_script_sandbox.h"
#include "../test_helpers/scoped_env.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct InstallerHolder {
    HelixPluginInstaller installer;
};

/// An installer whose install.sh lookup is confined to an empty temp dir, so
/// nothing a test does can reach the repository's moonraker-plugin/install.sh.
struct SandboxedInstaller : InstallerHolder, helix::test::PluginScriptSandbox {
    SandboxedInstaller() : PluginScriptSandbox(installer) {}
};

} // namespace

TEST_CASE("HelixPluginInstaller is_local_moonraker WSS handling", "[plugin_installer]") {
    SECTION("is_local_moonraker answers from the URL the installer was given") {
        // The predicate itself is tested once, in test_host_identity.cpp. What
        // belongs here is the wiring: that the installer names the host from its
        // own URL and refuses to claim locality when it cannot.
        helix::HelixPluginInstaller installer;

        installer.set_websocket_url("ws://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker());
        installer.set_websocket_url("ws://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker());
        installer.set_websocket_url("ws://[::1]:7125/websocket");
        REQUIRE(installer.is_local_moonraker());

        // 203.0.113.0/24 is TEST-NET-3 (RFC 5737) — reserved for documentation
        // and never assigned to a real interface, so this stays remote on any
        // machine. An RFC1918 literal would not: is_moonraker_on_same_host()
        // scans local interfaces, and a dev box or CI runner can legitimately
        // hold 192.168.x.x.
        installer.set_websocket_url("ws://203.0.113.7:7125/websocket");
        REQUIRE_FALSE(installer.is_local_moonraker());

        // No URL at all is not evidence of locality — auto-install must not fire.
        installer.set_websocket_url("");
        REQUIRE_FALSE(installer.is_local_moonraker());
    }

    SECTION("is_local_moonraker works with WSS URLs") {
        helix::HelixPluginInstaller installer;

        // WSS localhost should be detected as local
        installer.set_websocket_url("wss://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("wss://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        // WSS remote should not be detected as local
        installer.set_websocket_url("wss://192.168.1.100:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);
    }
}

// ============================================================================
// Test: HelixPluginInstaller Class
// ============================================================================

TEST_CASE("HelixPluginInstaller", "[plugin_installer]") {
    SECTION("is_local_moonraker detects localhost connections") {
        helix::HelixPluginInstaller installer;

        // Test with various URL patterns
        // Note: is_local_moonraker() uses the API's connected URL

        // When not connected, should return false
        REQUIRE(installer.is_local_moonraker() == false);

        // When set to localhost, should return true
        installer.set_websocket_url("ws://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("ws://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("ws://[::1]:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        // When set to remote, should return false
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);

        installer.set_websocket_url("ws://printer.local:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);
    }

    SECTION("get_remote_install_command pins the command to this app's release") {
        helix::HelixPluginInstaller installer;
        const std::string ref = plugin_release_ref(helix_version());

        CHECK(installer.get_remote_install_command() == plugin_remote_install_command(ref));
    }
}

TEST_CASE("plugin_release_ref names the release tag of an app version", "[plugin_installer]") {
    CHECK(plugin_release_ref("1.2.0") == "v1.2.0");
    CHECK(plugin_release_ref("1.1.0-beta.10") == "v1.1.0-beta.10");
    CHECK(plugin_release_ref("1.1.0-beta+abc1234") == "v1.1.0-beta");
    // A build that is no release reads its plugin from main.
    CHECK(plugin_release_ref("dev") == "main");
    CHECK(plugin_release_ref("") == "main");
}

TEST_CASE("parse_plugin_version reads helix_print.py's PLUGIN_VERSION line", "[plugin_installer]") {
    CHECK(parse_plugin_version("import os\n\nPLUGIN_VERSION = \"1.2.0\"\n") == "1.2.0");
    CHECK(parse_plugin_version("PLUGIN_VERSION = \"2.0.0-rc1\"\nx = 1\n") == "2.0.0-rc1");
    // Only an assignment at the start of a line counts, not a mention.
    CHECK(parse_plugin_version("log(f\"v{PLUGIN_VERSION}\")\n# PLUGIN_VERSION = \"9\"\n") == "");
    CHECK(parse_plugin_version("PLUGIN_VERSION = \"unterminated\nnext\n") == "");
    CHECK(parse_plugin_version("") == "");
}

TEST_CASE("decide_plugin_self_heal: what the startup check does about the plugin",
          "[plugin_installer]") {
    PluginSelfHealFacts f;
    f.local = true;
    f.bundled_version = "1.2.0";
    f.reported_version = "1.2.0";

    SECTION("a link this app re-pointed needs a restart to load the plugin") {
        f.link = PluginLinkCheck::Relinked;
        f.reported_version = ""; // a dangling link loaded nothing
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
    }
    SECTION("a current link running the version this app ships is left alone") {
        f.link = PluginLinkCheck::Current;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::Nothing);
    }
    SECTION("a current link running another version is reloaded") {
        f.link = PluginLinkCheck::Current;
        f.reported_version = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
    }
    SECTION("a restart waits while a job holds the machine") {
        f.link = PluginLinkCheck::Relinked;
        f.job_holds = true;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::WaitForJob);
    }
    SECTION("a plugin this app did not find installed is never touched, only reported") {
        f.link = PluginLinkCheck::NotOurs;
        f.reported_version = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::ReportOnly);
        f.job_holds = true;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::ReportOnly);
        f.reported_version = "1.2.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::Nothing);
    }
    SECTION("a failed relink restarts nothing") {
        f.link = PluginLinkCheck::Failed;
        f.reported_version = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::Nothing);
    }
    SECTION("an unknown version on either side is no mismatch") {
        f.link = PluginLinkCheck::Current;
        f.bundled_version = "";
        f.reported_version = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::Nothing);
    }
    SECTION("a mismatch an earlier start already restarted for is not restarted for again") {
        f.link = PluginLinkCheck::Current;
        f.reported_version = "1.1.0";
        f.memory.restarted_for_bundled = "1.2.0";
        f.memory.restarted_for_reported = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::GiveUp);
        // ... nor while a job holds the machine.
        f.job_holds = true;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::GiveUp);
    }
    SECTION("a new version pair gets one fresh attempt") {
        f.link = PluginLinkCheck::Current;
        f.reported_version = "1.1.0";
        f.memory.restarted_for_bundled = "1.1.5"; // the app updated since
        f.memory.restarted_for_reported = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
        f.memory.restarted_for_bundled = "1.2.0";
        f.memory.restarted_for_reported = "1.0.9"; // the plugin moved since
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
    }
    SECTION("a re-pointed link restarts even for a pair already tried") {
        f.link = PluginLinkCheck::Relinked;
        f.reported_version = "1.1.0";
        f.memory.restarted_for_bundled = "1.2.0";
        f.memory.restarted_for_reported = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
    }
    SECTION("a link re-pointed at the same file again after its restart is not restarted for") {
        f.link = PluginLinkCheck::Relinked;
        f.plugin_file = "/opt/helixscreen/moonraker-plugin/helix_print.py";
        f.memory.relinked_for = f.plugin_file;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::GiveUp);
        f.job_holds = true;
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::GiveUp);
        // A link re-pointed at another file is a new problem.
        f.job_holds = false;
        f.memory.relinked_for = "/home/pi/helixscreen/moonraker-plugin/helix_print.py";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::RestartNow);
    }
    SECTION("a remote Moonraker's mismatch is only reported, whatever the link") {
        f.local = false;
        f.link = PluginLinkCheck::Relinked;
        f.reported_version = "1.1.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::ReportOnly);
        f.reported_version = "1.2.0";
        CHECK(decide_plugin_self_heal(f) == PluginSelfHealStep::Nothing);
    }
}

TEST_CASE("remember_plugin_self_heal: what the startup check keeps for the next start",
          "[plugin_installer]") {
    PluginSelfHealFacts f;
    f.local = true;
    f.bundled_version = "1.2.0";
    f.reported_version = "1.1.0";
    f.plugin_file = "/opt/helixscreen/moonraker-plugin/helix_print.py";

    SECTION("a finished restart for a mismatch keeps the pair it was for") {
        f.link = PluginLinkCheck::Current;
        const auto m = remember_plugin_self_heal(f, true);
        CHECK(m.restarted_for_bundled == "1.2.0");
        CHECK(m.restarted_for_reported == "1.1.0");
        CHECK(m.relinked_for.empty());
    }
    SECTION("a finished restart for a re-pointed link keeps the file") {
        f.link = PluginLinkCheck::Relinked;
        f.reported_version = "";
        const auto m = remember_plugin_self_heal(f, true);
        CHECK(m.relinked_for == f.plugin_file);
        CHECK(m.restarted_for_bundled.empty());
    }
    SECTION("a restart that has not finished keeps nothing it was for") {
        f.link = PluginLinkCheck::Relinked;
        CHECK(remember_plugin_self_heal(f, false) == PluginSelfHealMemory{});
    }
    SECTION("a link found current forgets the file; agreeing versions forget the pair") {
        f.link = PluginLinkCheck::Current;
        f.reported_version = "1.2.0";
        f.memory = {"1.2.0", "1.1.0", f.plugin_file};
        const auto m = remember_plugin_self_heal(f, false);
        CHECK(m == PluginSelfHealMemory{});
    }
    SECTION("giving up keeps what the restart was for") {
        f.link = PluginLinkCheck::Relinked;
        f.memory.relinked_for = f.plugin_file;
        const auto m = remember_plugin_self_heal(f, false);
        CHECK(m.relinked_for == f.plugin_file);
    }
    SECTION("a failed check forgets nothing about the link") {
        f.link = PluginLinkCheck::Failed;
        f.memory.relinked_for = f.plugin_file;
        CHECK(remember_plugin_self_heal(f, false).relinked_for == f.plugin_file);
    }
}

TEST_CASE("the remote install command fetches a release's wrapper from the release CDN",
          "[plugin_installer]") {
    const std::string cmd = plugin_remote_install_command("v1.1.0-beta.10");

    CHECK(cmd == "curl -fsSL https://releases.helixscreen.org/plugin/remote-install.sh"
                 " | sh -s -- v1.1.0-beta.10");
    CHECK(cmd.find("bash") == std::string::npos);
    CHECK(cmd.find("githubusercontent") == std::string::npos);
}

TEST_CASE("the remote install command for main fetches main's wrapper from GitHub",
          "[plugin_installer]") {
    CHECK(plugin_remote_install_command("main") ==
          "curl -fsSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/"
          "moonraker-plugin/remote-install.sh | sh -s -- main");
}

// ============================================================================
// Test: Preference Management
// ============================================================================

// Note: Preference tests are integration tests that require a Config singleton.
// Since Config is typically initialized once at startup, these tests verify
// the logic but may need to be run in isolation or with a test config setup.

TEST_CASE("HelixPluginInstaller preferences logic", "[plugin_installer][preferences]") {
    SECTION("should_prompt_install requires a completed wizard") {
        helix::HelixPluginInstaller installer;
        helix::Config* config = helix::Config::get_instance();
        if (config->is_wizard_required()) {
            CHECK_FALSE(installer.should_prompt_install());
        }
    }
}

TEST_CASE("decide_plugin_prompt_snooze survives a clock it cannot trust", "[plugin_installer]") {
    constexpr int64_t kDay = 24 * 60 * 60;
    constexpr int64_t kReal = 1800000000;
    constexpr int64_t kEpochBoot = 30;
    constexpr int64_t kTrustFloor = 1577836800;

    SECTION("never snoozed opens on any clock") {
        CHECK(decide_plugin_prompt_snooze({}, kEpochBoot, true).open);
        CHECK(decide_plugin_prompt_snooze({}, kReal, true).open);
    }
    SECTION("seven measured days, to the second") {
        const PluginPromptSnooze snooze = plugin_prompt_snooze_at(kReal);
        CHECK(snooze.snoozed_at_s == kReal);
        CHECK_FALSE(decide_plugin_prompt_snooze(snooze, kReal + 7 * kDay - 1, true).open);
        CHECK(decide_plugin_prompt_snooze(snooze, kReal + 7 * kDay, true).open);
    }
    SECTION("the trust floor is the first trusted second") {
        CHECK(plugin_prompt_snooze_at(kTrustFloor - 1).snoozed_at_s == kPluginPromptStampUnknown);
        CHECK(plugin_prompt_snooze_at(kTrustFloor).snoozed_at_s == kTrustFloor);
    }
    SECTION("dismissed at the epoch: the first trusted reading starts the seven days") {
        const PluginPromptSnooze snooze = plugin_prompt_snooze_at(kEpochBoot);
        const PluginPromptSnoozeTurn turn = decide_plugin_prompt_snooze(snooze, kReal, true);
        CHECK_FALSE(turn.open);
        CHECK(turn.snooze.snoozed_at_s == kReal);
        CHECK_FALSE(decide_plugin_prompt_snooze(turn.snooze, kReal + 6 * kDay, true).open);
        CHECK(decide_plugin_prompt_snooze(turn.snooze, kReal + 7 * kDay, true).open);
    }
    SECTION("no clock ever: closed for six starts, open on the seventh and until answered") {
        PluginPromptSnooze snooze = plugin_prompt_snooze_at(kEpochBoot);
        for (int start = 1; start < kPluginPromptMaxUnmeasuredStarts; ++start) {
            const PluginPromptSnoozeTurn turn =
                decide_plugin_prompt_snooze(snooze, kEpochBoot, true);
            CHECK_FALSE(turn.open);
            snooze = turn.snooze;
        }
        const PluginPromptSnoozeTurn last = decide_plugin_prompt_snooze(snooze, kEpochBoot, true);
        CHECK(last.open);
        CHECK(decide_plugin_prompt_snooze(last.snooze, kEpochBoot, true).open);
    }
    SECTION("a trusted stamp survives an epoch boot, and NTP then measures it") {
        const PluginPromptSnooze snooze = plugin_prompt_snooze_at(kReal);
        const PluginPromptSnoozeTurn boot = decide_plugin_prompt_snooze(snooze, kEpochBoot, true);
        CHECK_FALSE(boot.open);
        CHECK(boot.snooze.snoozed_at_s == kReal);
        CHECK_FALSE(decide_plugin_prompt_snooze(boot.snooze, kReal + kDay, false).open);
        CHECK(decide_plugin_prompt_snooze(boot.snooze, kReal + 7 * kDay, false).open);
    }
    SECTION("a stamp from a clock far ahead cannot lock the prompt out") {
        PluginPromptSnooze snooze = plugin_prompt_snooze_at(kReal + 3650 * kDay);
        PluginPromptSnoozeTurn turn{};
        for (int start = 0; start < kPluginPromptMaxUnmeasuredStarts; ++start) {
            CHECK_FALSE(turn.open);
            turn = decide_plugin_prompt_snooze(snooze, kReal, true);
            snooze = turn.snooze;
        }
        CHECK(turn.open);
    }
    SECTION("a re-check within one run is not another start") {
        const PluginPromptSnoozeTurn turn =
            decide_plugin_prompt_snooze(plugin_prompt_snooze_at(kEpochBoot), kEpochBoot, false);
        CHECK(turn.snooze.unmeasured_starts == 0);
    }
}

// ============================================================================
// Test: Install Script Path Resolution
// ============================================================================

TEST_CASE("HelixPluginInstaller script path", "[plugin_installer]") {
    SandboxedInstaller sandbox;

    SECTION("an empty search root resolves to nothing") {
        CHECK(sandbox.installer.get_install_script_path().empty());
        // Path resolution is a pure query - it must not move the state machine.
        CHECK(sandbox.installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("a script under the search root is the one resolved") {
        sandbox.write_script("exit 0");
        CHECK(sandbox.installer.get_install_script_path() ==
              std::filesystem::canonical(sandbox.script_path()).string());
    }

    SECTION("a symlink named install.sh that points at another file is refused") {
        const std::string other = sandbox.root + "/payload.sh";
        std::ofstream(other) << "#!/bin/sh\nexit 0\n";
        std::filesystem::create_symlink(other, sandbox.script_path());
        CHECK(sandbox.installer.get_install_script_path().empty());
    }
}

// ============================================================================
// Test: Install State Machine
// ============================================================================

TEST_CASE("HelixPluginInstaller state", "[plugin_installer]") {
    SECTION("initial state is IDLE") {
        helix::HelixPluginInstaller installer;
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("is_installing returns false when IDLE") {
        helix::HelixPluginInstaller installer;
        REQUIRE(installer.is_installing() == false);
    }

    SECTION("state accessors are consistent") {
        helix::HelixPluginInstaller installer;

        // IDLE state
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
        REQUIRE(installer.is_installing() == false);
    }
}

// ============================================================================
// Test: install_local_sync() Error Paths
// ============================================================================

TEST_CASE("HelixPluginInstaller install_local_sync error handling", "[plugin_installer]") {
    SECTION("install_local_sync fails when not connected to local Moonraker") {
        helix::HelixPluginInstaller installer;

        // Set to remote URL
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        const auto result = installer.install_local_sync();

        REQUIRE(result.success == false);
        REQUIRE(result.message.find("local Moonraker") != std::string::npos);
        // State should remain IDLE since install never started
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("install_local_sync fails when no URL is set") {
        helix::HelixPluginInstaller installer;
        // No URL set - should fail with "local Moonraker" message

        const auto result = installer.install_local_sync();

        REQUIRE(result.success == false);
    }
}

TEST_CASE("HelixPluginInstaller install_local_sync runs the script it finds",
          "[plugin_installer]") {
    SandboxedInstaller sandbox;
    auto& installer = sandbox.installer;

    SECTION("no script: fails without starting") {
        const auto result = installer.install_local_sync();
        CHECK_FALSE(result.success);
        CHECK(result.message.find("not found") != std::string::npos);
        CHECK(installer.get_state() == helix::PluginInstallState::IDLE);
        CHECK(sandbox.script_args().empty());
    }

    SECTION("exit 0: succeeds, and the script was asked to install without restarting") {
        sandbox.write_script("exit 0");
        const auto result = installer.install_local_sync();
        CHECK(result.success);
        CHECK(installer.get_state() == helix::PluginInstallState::SUCCESS);
        CHECK(sandbox.script_args() == "--no-restart --auto ");
        // The script restarts nothing, so the result must not say Moonraker is restarting.
        CHECK(result.message.find("restarting") == std::string::npos);
    }

    SECTION("non-zero exit: fails") {
        sandbox.write_script("echo 'moonraker.conf not writable'\nexit 1");
        const auto result = installer.install_local_sync();
        CHECK_FALSE(result.success);
        CHECK(result.message.find("failed") != std::string::npos);
        CHECK(installer.get_state() == helix::PluginInstallState::FAILED);
    }

    SECTION("a script that outlives the timeout is killed and reported as timed out") {
        HelixPluginInstallerTestAccess::set_script_timeout_seconds(installer, 1);
        sandbox.write_script("exec sleep 30");
        const auto started = std::chrono::steady_clock::now();
        const auto result = installer.install_local_sync();
        const auto waited = std::chrono::steady_clock::now() - started;
        CHECK_FALSE(result.success);
        CHECK(result.message.find("timed out") != std::string::npos);
        CHECK(installer.get_state() == helix::PluginInstallState::FAILED);
        CHECK(waited < std::chrono::seconds(10));
    }
}

TEST_CASE("restart_moonraker_for_plugin reads install.sh --restart-moonraker's exit",
          "[plugin_installer]") {
    using Outcome = helix::PluginRestartOutcome;
    SandboxedInstaller sandbox;
    const std::string script = sandbox.script_path();

    SECTION("the script is asked to restart, with no --no-restart") {
        sandbox.write_script("exit 0");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::Restarted);
        CHECK(sandbox.script_args() == "--restart-moonraker ");
    }
    SECTION("Moonraker refused, or still runs the old plugin") {
        sandbox.write_script("exit 3");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::NeedsManual);
    }
    SECTION("a print holds it off") {
        sandbox.write_script("exit 4");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::HeldForPrint);
    }
    SECTION("the print state could not be read") {
        sandbox.write_script("exit 6");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::JobUnknown);
    }
    SECTION("Moonraker does not answer") {
        sandbox.write_script("exit 7");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::NotAnswering);
    }
    SECTION("any other exit is a failure") {
        sandbox.write_script("exit 1");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::Failed);
        sandbox.write_script("exit 5");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::Failed);
    }
    SECTION("no script") {
        CHECK(helix::restart_moonraker_for_plugin("", 10) == Outcome::Failed);
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::Failed);
    }
    SECTION("a script that outlives the timeout timed out") {
        sandbox.write_script("exec sleep 30");
        CHECK(helix::restart_moonraker_for_plugin(script, 1) == Outcome::TimedOut);
    }
    SECTION("a script that ignores SIGTERM is still killed so the call returns") {
        sandbox.write_script("trap '' TERM\nwhile :; do sleep 1; done");
        const auto started = std::chrono::steady_clock::now();
        CHECK(helix::restart_moonraker_for_plugin(script, 1) == Outcome::TimedOut);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    }
    SECTION("an inherited HELIX_JOB_STATE never reaches the script, which asks Moonraker") {
        helix::ScopedEnv inherited("HELIX_JOB_STATE", "idle");
        sandbox.write_script("[ -z \"${HELIX_JOB_STATE+set}\" ] && exit 0\nexit 9");
        CHECK(helix::restart_moonraker_for_plugin(script, 10) == Outcome::Restarted);
    }
}

TEST_CASE("plugin_restart_finished: a finished restart, or one needing a manual restart, is "
          "not repeated for the same versions",
          "[plugin_installer]") {
    using Outcome = helix::PluginRestartOutcome;
    CHECK(helix::plugin_restart_finished(Outcome::Restarted, true));
    CHECK(helix::plugin_restart_finished(Outcome::Restarted, false));
    CHECK(helix::plugin_restart_finished(Outcome::NeedsManual, true));
    // With no plugin loaded, a restart that did not load one is tried again.
    CHECK_FALSE(helix::plugin_restart_finished(Outcome::NeedsManual, false));
    for (const bool loaded : {true, false}) {
        CHECK_FALSE(helix::plugin_restart_finished(Outcome::HeldForPrint, loaded));
        CHECK_FALSE(helix::plugin_restart_finished(Outcome::JobUnknown, loaded));
        CHECK_FALSE(helix::plugin_restart_finished(Outcome::NotAnswering, loaded));
        CHECK_FALSE(helix::plugin_restart_finished(Outcome::TimedOut, loaded));
        CHECK_FALSE(helix::plugin_restart_finished(Outcome::Failed, loaded));
    }
}

// ============================================================================
// Test: uninstall_local() Error Paths
// ============================================================================

TEST_CASE("HelixPluginInstaller uninstall_local error handling", "[plugin_installer]") {
    using Outcome = helix::UninstallOutcome;

    SECTION("uninstall_local fails when not connected to local Moonraker") {
        helix::HelixPluginInstaller installer;

        // Set to remote URL
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        bool callback_called = false;
        Outcome callback_outcome = Outcome::SUCCESS;
        std::string callback_message;

        installer.uninstall_local([&](Outcome outcome, const std::string& msg) {
            callback_called = true;
            callback_outcome = outcome;
            callback_message = msg;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_outcome == Outcome::FAILED);
        REQUIRE(callback_message.find("local Moonraker") != std::string::npos);
        // State should remain IDLE since uninstall never started
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("uninstall_local fails when no URL is set") {
        helix::HelixPluginInstaller installer;
        // No URL set - should fail with "local Moonraker" message

        bool callback_called = false;
        Outcome callback_outcome = Outcome::SUCCESS;

        installer.uninstall_local([&](Outcome outcome, const std::string&) {
            callback_called = true;
            callback_outcome = outcome;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_outcome == Outcome::FAILED);
    }

    SECTION("uninstall_local handles nullptr callback safely") {
        helix::HelixPluginInstaller installer;
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        // Should not crash with nullptr callback
        installer.uninstall_local(nullptr);

        // State should remain IDLE (uninstall didn't start due to remote URL)
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }
}

TEST_CASE("HelixPluginInstaller uninstall_local runs the script it finds", "[plugin_installer]") {
    using Outcome = helix::UninstallOutcome;
    SandboxedInstaller sandbox;
    auto& installer = sandbox.installer;

    bool called = false;
    Outcome outcome = Outcome::SUCCESS;
    std::string message;
    auto record = [&](Outcome o, const std::string& m) {
        called = true;
        outcome = o;
        message = m;
    };

    SECTION("no script: fails without starting") {
        installer.uninstall_local(record);
        REQUIRE(called);
        CHECK(outcome == Outcome::FAILED);
        CHECK(message.find("not found") != std::string::npos);
        CHECK(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("exit 0: succeeds, and the script was asked to uninstall without restarting") {
        sandbox.write_script("exit 0");
        installer.uninstall_local(record);
        REQUIRE(called);
        CHECK(outcome == Outcome::SUCCESS);
        CHECK(sandbox.script_args() == "--no-restart --uninstall-auto ");
    }

    SECTION("exit 2: the plugin is gone but a config file needs attention") {
        sandbox.write_script("exit 2");
        installer.uninstall_local(record);
        REQUIRE(called);
        CHECK(outcome == Outcome::NEEDS_ATTENTION);
        CHECK(installer.get_state() == helix::PluginInstallState::NEEDS_ATTENTION);
    }

    SECTION("a script that outlives the timeout is reported as timed out") {
        HelixPluginInstallerTestAccess::set_script_timeout_seconds(installer, 1);
        sandbox.write_script("exec sleep 30");
        installer.uninstall_local(record);
        REQUIRE(called);
        CHECK(outcome == Outcome::FAILED);
        CHECK(message.find("timed out") != std::string::npos);
    }
}

TEST_CASE("script_output_lines strips install.sh color codes and blank lines",
          "[plugin_installer]") {
    const std::string out =
        "\033[0;32m[INFO]\033[0m HelixPrint Auto-Install Mode\n"
        "\n"
        "\033[0;31m[ERROR]\033[0m Could not auto-detect Moonraker installation.\n"
        "trailing without newline";
    const auto lines = helix::script_output_lines(out);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "[INFO] HelixPrint Auto-Install Mode");
    CHECK(lines[1] == "[ERROR] Could not auto-detect Moonraker installation.");
    CHECK(lines[2] == "trailing without newline");
}

TEST_CASE("decide_plugin_turn keeps a plugin action off a print and what follows it",
          "[plugin_installer]") {
    using helix::decide_plugin_turn;
    using Step = helix::PluginTurnStep;

    // wanted, job_holds, waited_for_job, {modal_up, print_status_open}
    CHECK(decide_plugin_turn(false, false, false, {false, false}) == Step::Stop);
    CHECK(decide_plugin_turn(false, true, true, {true, true}) == Step::Stop);
    CHECK(decide_plugin_turn(true, true, false, {false, false}) == Step::WaitForJob);
    CHECK(decide_plugin_turn(true, true, true, {true, true}) == Step::WaitForJob);
    // The first look after a job lets go waits for its completion flow.
    CHECK(decide_plugin_turn(true, false, true, {false, false}) == Step::WaitForScreen);
    CHECK(decide_plugin_turn(true, false, false, {true, false}) == Step::WaitForScreen);
    CHECK(decide_plugin_turn(true, false, false, {false, true}) == Step::WaitForScreen);
    CHECK(decide_plugin_turn(true, false, false, {false, false}) == Step::Go);
}
