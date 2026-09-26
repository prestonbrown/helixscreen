// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cli_args.cpp
 * @brief Unit tests for CLI argument struct helpers
 *
 * Tests the CliArgs struct defaults and parse_cli_args() for flags that do
 * not require graphics or printer state.
 */

#include "../test_helpers/scoped_runtime_config.h"
#include "cli_args.h"
#include "runtime_config.h"
#include "test_helpers/scoped_env.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// CliArgs Tests
// ============================================================================

TEST_CASE("parse_cli_args: --detect-printer with host/port", "[cli_args][detect]") {
    const char* argv[] = {"helix-screen", "--detect-printer", "--host",
                          "127.0.0.1",    "--port",           "7125"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(6, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.detect_printer);
    REQUIRE(args.detect_host == "127.0.0.1");
    REQUIRE(args.detect_port == 7125);
}

TEST_CASE("parse_cli_args: --detect-printer defaults", "[cli_args][detect]") {
    const char* argv[] = {"helix-screen", "--detect-printer"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(2, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.detect_printer);
    REQUIRE(args.detect_host == "127.0.0.1");
    REQUIRE(args.detect_port == 7125);
}

TEST_CASE("CliArgs: default values", "[cli_args]") {
    CliArgs args;

    SECTION("screen settings default to auto") {
        REQUIRE_FALSE(args.size_was_explicit);
        REQUIRE(args.dpi == -1);
        REQUIRE(args.display_num == -1);
        REQUIRE(args.x_pos == -1);
        REQUIRE(args.y_pos == -1);
    }

    SECTION("wizard defaults off") {
        REQUIRE_FALSE(args.force_wizard);
        REQUIRE(args.wizard_step == -1);
    }

    SECTION("automation defaults off") {
        REQUIRE_FALSE(args.screenshot_enabled);
        REQUIRE(args.screenshot_delay_sec == 2);
        REQUIRE(args.timeout_sec == 0);
    }

    SECTION("theme defaults to not set") {
        REQUIRE(args.dark_mode_cli == -1);
    }

    SECTION("logging defaults to warning level") {
        REQUIRE(args.verbosity == 0);
    }

    SECTION("memory profiling defaults off") {
        REQUIRE_FALSE(args.memory_report);
        REQUIRE_FALSE(args.show_memory);
    }

    SECTION("moonraker URL default empty") {
        REQUIRE(args.moonraker_url.empty());
    }

    SECTION("layout default empty") {
        REQUIRE(args.layout.empty());
    }
}

// ============================================================================
// Screen size parsing
// ============================================================================

TEST_CASE("parse_screen_size_string: named presets", "[cli_args]") {
    int w = 0, h = 0;

    SECTION("each preset resolves to its resolution") {
        REQUIRE(parse_screen_size_string("micro", w, h));
        REQUIRE(w == 480);
        REQUIRE(h == 272);

        REQUIRE(parse_screen_size_string("medium", w, h));
        REQUIRE(w == 800);
        REQUIRE(h == 480);

        REQUIRE(parse_screen_size_string("large", w, h));
        REQUIRE(w == 1024);
        REQUIRE(h == 600);

        REQUIRE(parse_screen_size_string("xlarge", w, h));
        REQUIRE(w == 1280);
        REQUIRE(h == 720);
    }

    SECTION("names are case-insensitive") {
        REQUIRE(parse_screen_size_string("Large", w, h));
        REQUIRE(w == 1024);
        REQUIRE(h == 600);

        REQUIRE(parse_screen_size_string("XLARGE", w, h));
        REQUIRE(w == 1280);
        REQUIRE(h == 720);

        REQUIRE(parse_screen_size_string("MeDiUm", w, h));
        REQUIRE(w == 800);
        REQUIRE(h == 480);
    }

    SECTION("an unknown name is still rejected whatever its case") {
        REQUIRE_FALSE(parse_screen_size_string("HUGE", w, h));
    }
}

TEST_CASE("parse_screen_size_string: WxH", "[cli_args]") {
    int w = 0, h = 0;

    SECTION("landscape and portrait both pass through verbatim") {
        REQUIRE(parse_screen_size_string("1920x1080", w, h));
        REQUIRE(w == 1920);
        REQUIRE(h == 1080);

        REQUIRE(parse_screen_size_string("480x800", w, h));
        REQUIRE(w == 480);
        REQUIRE(h == 800);
    }

    SECTION("a capital separator parses too") {
        REQUIRE(parse_screen_size_string("1920X1080", w, h));
        REQUIRE(w == 1920);
        REQUIRE(h == 1080);
    }

    SECTION("a rejected string leaves the outputs untouched") {
        w = -7;
        h = -9;
        REQUIRE_FALSE(parse_screen_size_string("not-a-size", w, h));
        REQUIRE(w == -7);
        REQUIRE(h == -9);
    }

    SECTION("malformed and non-positive dimensions are rejected") {
        REQUIRE_FALSE(parse_screen_size_string("800x", w, h));
        REQUIRE_FALSE(parse_screen_size_string("x480", w, h));
        REQUIRE_FALSE(parse_screen_size_string("0x480", w, h));
        REQUIRE_FALSE(parse_screen_size_string("800x0", w, h));
        REQUIRE_FALSE(parse_screen_size_string("-800x480", w, h));
        REQUIRE_FALSE(parse_screen_size_string("", w, h));
    }
}

// ============================================================================
// Remote-control flags
// ============================================================================

TEST_CASE("parse_cli_args: remote-control flag defaults", "[cli_args][remote]") {
    CliArgs args;
    REQUIRE_FALSE(args.remote_control);
    REQUIRE(args.remote_transport == "socket");
    REQUIRE(args.remote_http_bind == "127.0.0.1");
    REQUIRE(args.remote_http_port == 7130);
    REQUIRE(args.remote_socket.empty());
    REQUIRE_FALSE(args.skip_wizard);
}

TEST_CASE("parse_cli_args: --remote enables the control server", "[cli_args][remote]") {
    const char* argv[] = {"helix-screen", "--remote"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(2, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.remote_control);
    REQUIRE(args.remote_transport == "socket"); // unchanged default
}

TEST_CASE("parse_cli_args: --skip-wizard sets the flag", "[cli_args][remote]") {
    const char* argv[] = {"helix-screen", "--skip-wizard"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(2, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.skip_wizard);
}

TEST_CASE("parse_cli_args: --remote-socket overrides path and implies --remote",
          "[cli_args][remote]") {
    const char* argv[] = {"helix-screen", "--remote-socket", "/tmp/custom.sock"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.remote_socket == "/tmp/custom.sock");
    REQUIRE(args.remote_control);
}

TEST_CASE("parse_cli_args: --remote-transport validates socket|http", "[cli_args][remote]") {
    SECTION("http is accepted") {
        const char* argv[] = {"helix-screen", "--remote-transport", "http"};
        CliArgs args;
        int w = 0, h = 0;
        REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
        REQUIRE(args.remote_transport == "http");
    }
    SECTION("an unknown transport is rejected") {
        const char* argv[] = {"helix-screen", "--remote-transport", "carrierpigeon"};
        CliArgs args;
        int w = 0, h = 0;
        REQUIRE_FALSE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    }
}

TEST_CASE("parse_cli_args: HTTP options imply http transport", "[cli_args][remote]") {
    SECTION("--remote-http-bind implies http") {
        const char* argv[] = {"helix-screen", "--remote-http-bind", "0.0.0.0"};
        CliArgs args;
        int w = 0, h = 0;
        REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
        REQUIRE(args.remote_http_bind == "0.0.0.0");
        REQUIRE(args.remote_transport == "http");
        REQUIRE(args.remote_control);
    }
    SECTION("--remote-http-port sets port and implies http") {
        const char* argv[] = {"helix-screen", "--remote-http-port", "8080"};
        CliArgs args;
        int w = 0, h = 0;
        REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
        REQUIRE(args.remote_http_port == 8080);
        REQUIRE(args.remote_transport == "http");
    }
}

TEST_CASE("parse_cli_args: --remote-http-port rejects out-of-range values", "[cli_args][remote]") {
    int w = 0, h = 0;
    SECTION("zero is rejected") {
        const char* argv[] = {"helix-screen", "--remote-http-port", "0"};
        CliArgs args;
        REQUIRE_FALSE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    }
    SECTION("above 65535 is rejected") {
        const char* argv[] = {"helix-screen", "--remote-http-port", "70000"};
        CliArgs args;
        REQUIRE_FALSE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    }
    SECTION("a valid port is accepted") {
        const char* argv[] = {"helix-screen", "--remote-http-port", "7130"};
        CliArgs args;
        REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
        REQUIRE(args.remote_http_port == 7130);
    }
}

// --- wizard suppression under --test ------------------------------------
//
// Mock mode sets the active printer to "mock-printer", and the wizard gate
// reads printers.<active>.wizard_completed — a key no real settings.json
// carries. So a --test boot always landed on the first-run wizard, which
// swallowed every ctl navigate/click while each command still reported
// success. --test now implies --skip-wizard; -w/--wizard is the escape the
// screenshot pipeline uses to capture the wizard itself.

namespace {

/// --test sets test_mode on the process-wide RuntimeConfig, which parse_cli_args
/// never clears. Restore it so a later test parsing an argv *without* --test
/// still sees a clean slate.
struct TestModeGuard {
    bool saved = get_runtime_config()->test_mode;
    ~TestModeGuard() {
        get_runtime_config()->test_mode = saved;
    }
};

bool parse(std::vector<const char*> argv, CliArgs& args) {
    int w = 0, h = 0;
    return parse_cli_args(static_cast<int>(argv.size()), const_cast<char**>(argv.data()), args, w,
                          h);
}

} // namespace

TEST_CASE("parse_cli_args: --test implies --skip-wizard", "[cli_args][wizard]") {
    TestModeGuard guard;
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test"}, args));
    REQUIRE(args.skip_wizard);
    REQUIRE_FALSE(args.force_wizard);
}

TEST_CASE("parse_cli_args: --wizard overrides the --test implication", "[cli_args][wizard]") {
    TestModeGuard guard;
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test", "--wizard"}, args));
    REQUIRE(args.force_wizard);
    REQUIRE_FALSE(args.skip_wizard); // else the wizard capture path breaks
}

TEST_CASE("parse_cli_args: --wizard wins regardless of flag order", "[cli_args][wizard]") {
    // The implication is applied after the whole parse, so -w before --test
    // must behave identically to -w after it.
    TestModeGuard guard;
    CliArgs args;
    REQUIRE(parse({"helix-screen", "-w", "--test"}, args));
    REQUIRE(args.force_wizard);
    REQUIRE_FALSE(args.skip_wizard);
}

TEST_CASE("parse_cli_args: --wizard-step still forces the wizard under --test",
          "[cli_args][wizard]") {
    TestModeGuard guard;
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test", "--wizard-step", "3"}, args));
    REQUIRE(args.force_wizard);
    REQUIRE(args.wizard_step == 3);
    REQUIRE_FALSE(args.skip_wizard);
}

TEST_CASE("parse_cli_args: an explicit --skip-wizard is still honoured under --test",
          "[cli_args][wizard]") {
    TestModeGuard guard;
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test", "--skip-wizard"}, args));
    REQUIRE(args.skip_wizard);
    REQUIRE_FALSE(args.force_wizard);
}

TEST_CASE("parse_cli_args: without --test the wizard gate is untouched", "[cli_args][wizard]") {
    TestModeGuard guard;
    get_runtime_config()->test_mode = false; // isolate from a prior --test parse
    CliArgs args;
    REQUIRE(parse({"helix-screen"}, args));
    REQUIRE_FALSE(args.skip_wizard);
    REQUIRE_FALSE(args.force_wizard);
}

// --- creator5_zmod implies the real AMS backend --------------------------
//
// The persona is mock HARDWARE, not a mock backend: MoonrakerClientMock
// publishes Z-Mod's objects and the production AmsBackendToolChanger is meant
// to drive them, which is what --real-ams selects. An explicit HELIX_MOCK_AMS
// names its own topology, so it wins over the persona's implication.

TEST_CASE("parse_cli_args: HELIX_MOCK_PRINTER=creator5_zmod implies --real-ams under --test",
          "[cli_args][mock]") {
    ScopedRuntimeConfig scoped_config;
    ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    ScopedEnv printer{"HELIX_MOCK_PRINTER", "creator5_zmod"};
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test"}, args));
    REQUIRE(get_runtime_config()->use_real_ams);
}

TEST_CASE("parse_cli_args: an explicit HELIX_MOCK_AMS wins over the persona", "[cli_args][mock]") {
    ScopedRuntimeConfig scoped_config;
    ScopedEnv ams{"HELIX_MOCK_AMS", "toolchanger"};
    ScopedEnv printer{"HELIX_MOCK_PRINTER", "creator5_zmod"};
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test"}, args));
    REQUIRE_FALSE(get_runtime_config()->use_real_ams);
}

TEST_CASE("parse_cli_args: other personas do not imply --real-ams", "[cli_args][mock]") {
    ScopedRuntimeConfig scoped_config;
    ScopedEnv ams{"HELIX_MOCK_AMS", nullptr};
    ScopedEnv printer{"HELIX_MOCK_PRINTER", "creator5"};
    CliArgs args;
    REQUIRE(parse({"helix-screen", "--test"}, args));
    REQUIRE_FALSE(get_runtime_config()->use_real_ams);
}

// ============================================================================
// --sim-speed
// ============================================================================

TEST_CASE("parse_cli_args: --sim-speed", "[cli_args][simclock]") {
    ScopedRuntimeConfig scoped_config;
    CliArgs args;
    int w = 0, h = 0;

    SECTION("accepted with --test") {
        const char* argv[] = {"helix-screen", "--test", "--sim-speed", "60"};
        REQUIRE(parse_cli_args(4, const_cast<char**>(argv), args, w, h));
        REQUIRE(get_runtime_config()->sim_speedup == Catch::Approx(60.0));
    }

    SECTION("rejected without --test") {
        // Outside a mock run the factor would fast-forward the pre-print clock
        // against a real printer.
        const char* argv[] = {"helix-screen", "--sim-speed", "60"};
        REQUIRE_FALSE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    }

    SECTION("1.0 without --test is not a speedup, so it passes") {
        const char* argv[] = {"helix-screen", "--sim-speed", "1.0"};
        REQUIRE(parse_cli_args(3, const_cast<char**>(argv), args, w, h));
    }

    SECTION("out of range is rejected, not clamped") {
        const char* below[] = {"helix-screen", "--test", "--sim-speed", "0.5"};
        REQUIRE_FALSE(parse_cli_args(4, const_cast<char**>(below), args, w, h));

        const char* above[] = {"helix-screen", "--test", "--sim-speed", "1001"};
        REQUIRE_FALSE(parse_cli_args(4, const_cast<char**>(above), args, w, h));
    }
}
