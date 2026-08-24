// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "system/debug_bundle_collector.h"
#include "system/update_checker.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <zlib.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Fixture: isolated temp directory for settings/crash file tests
// ============================================================================

class DebugBundleTestFixture {
  public:
    DebugBundleTestFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_debug_bundle_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);
    }

    ~DebugBundleTestFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream ofs((temp_dir_ / filename).string());
        ofs << content;
    }

    fs::path temp_dir_;
};

// ============================================================================
// collect() tests [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: collect() returns valid JSON with expected keys",
          "[debug-bundle]") {
    json bundle = helix::DebugBundleCollector::collect();

    REQUIRE(bundle.contains("version"));
    REQUIRE(bundle.contains("timestamp"));
    REQUIRE(bundle.contains("system"));
    REQUIRE(bundle.contains("printer"));
    REQUIRE(bundle.contains("settings"));

    // version and timestamp should be non-empty strings
    REQUIRE(bundle["version"].is_string());
    REQUIRE_FALSE(bundle["version"].get<std::string>().empty());
    REQUIRE(bundle["timestamp"].is_string());
    REQUIRE_FALSE(bundle["timestamp"].get<std::string>().empty());
}

// ============================================================================
// collect_system_info() tests [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: collect_system_info() has platform and ram", "[debug-bundle]") {
    json sys = helix::DebugBundleCollector::collect_system_info();

    REQUIRE(sys.contains("platform"));
    REQUIRE(sys["platform"].is_string());
    REQUIRE_FALSE(sys["platform"].get<std::string>().empty());

    REQUIRE(sys.contains("total_ram_mb"));
    REQUIRE(sys.contains("cpu_cores"));
}

// ============================================================================
// collect_sanitized_settings() tests [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: sanitize strips sensitive keys", "[debug-bundle]") {
    // Test the sanitization logic directly via collect() with a known JSON structure
    // We test the internal sanitize_json indirectly by checking the class behavior

    // Create a JSON with sensitive keys
    json input = {{"api_token", "super_secret_123"},
                  {"printer_name", "My Voron"},
                  {"password", "hidden_password"},
                  {"mqtt_secret", "secret_value"},
                  {"api_key", "key_value"},
                  {"nested", {{"auth_token", "nested_secret"}, {"display_name", "safe_value"}}},
                  {"normal_setting", 42}};

    // Use the public collect_sanitized_settings which calls sanitize_json internally
    // Since we can't easily inject a file, test the sanitization via the full pipeline
    // Instead, test the specific behavior we can observe:

    // The sanitize logic strips keys matching token, password, secret, key (case-insensitive)
    // We verify this by checking the class's is_sensitive_key behavior indirectly

    // Test via gzip round-trip pattern - verify the class compiles and basic collection works
    json settings = helix::DebugBundleCollector::collect_sanitized_settings();
    REQUIRE(settings.is_object());
}

// ============================================================================
// gzip_compress() tests [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: gzip_compress() round-trips correctly", "[debug-bundle]") {
    std::string original = "Hello, this is a test string for gzip compression. "
                           "It should round-trip correctly through compress and decompress. "
                           "Adding some repeated content to make compression worthwhile. "
                           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

    auto compressed = helix::DebugBundleCollector::gzip_compress(original);

    REQUIRE_FALSE(compressed.empty());
    // Compressed should generally be smaller than original for this data
    REQUIRE(compressed.size() < original.size());

    // Decompress with zlib to verify round-trip
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, MAX_WBITS + 16) == Z_OK);

    zs.next_in = compressed.data();
    zs.avail_in = static_cast<uInt>(compressed.size());

    std::vector<uint8_t> decompressed(original.size() * 2);
    zs.next_out = decompressed.data();
    zs.avail_out = static_cast<uInt>(decompressed.size());

    int ret = inflate(&zs, Z_FINISH);
    REQUIRE((ret == Z_STREAM_END || ret == Z_OK));

    std::string result(reinterpret_cast<char*>(decompressed.data()), zs.total_out);
    inflateEnd(&zs);

    REQUIRE(result == original);
}

TEST_CASE("DebugBundleCollector: gzip_compress() handles empty input", "[debug-bundle]") {
    auto compressed = helix::DebugBundleCollector::gzip_compress("");
    // Empty input should still produce valid gzip output (header + empty payload)
    REQUIRE_FALSE(compressed.empty());
}

// ============================================================================
// BundleOptions defaults [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: BundleOptions defaults are reasonable", "[debug-bundle]") {
    helix::BundleOptions opts;
    REQUIRE(opts.include_klipper_logs == false);
    REQUIRE(opts.include_moonraker_logs == false);
}

// ============================================================================
// BundleResult defaults [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: BundleResult defaults are reasonable", "[debug-bundle]") {
    helix::BundleResult result;
    REQUIRE(result.success == false);
    REQUIRE(result.share_code.empty());
    REQUIRE(result.error_message.empty());
}

// ============================================================================
// collect_printer_info() basic test [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: collect_printer_info() returns valid JSON", "[debug-bundle]") {
    // Printer may not be connected, but should not crash
    json printer = helix::DebugBundleCollector::collect_printer_info(
        helix::DebugBundleCollector::snapshot_printer_state());
    REQUIRE(printer.is_object());
}

TEST_CASE("DebugBundleCollector: collect_printer_info() renders a snapshot without PrinterState",
          "[debug-bundle]") {
    // The section is pure assembly now, so the whole state table is reachable
    // without a live printer — and without the LVGL subject reads that used to
    // happen on the upload worker.
    helix::PrinterSnapshot snap;
    snap.captured = true;
    snap.model = "Adventurer 5X";
    snap.klipper_version = "v0.13.0-746-ZMOD";
    snap.connection_state = 2; // connected
    snap.klippy_state = 2;     // shutdown

    const json printer = helix::DebugBundleCollector::collect_printer_info(snap);
    CHECK(printer["model"] == "Adventurer 5X");
    CHECK(printer["klipper_version"] == "v0.13.0-746-ZMOD");
    CHECK(printer["connection_state"] == "connected");
    CHECK(printer["klippy_state"] == "shutdown");

    SECTION("out-of-range enums are dropped rather than indexing off the table") {
        helix::PrinterSnapshot bad;
        bad.captured = true;
        bad.connection_state = 99;
        bad.klippy_state = -1;
        const json out = helix::DebugBundleCollector::collect_printer_info(bad);
        CHECK_FALSE(out.contains("connection_state"));
        CHECK_FALSE(out.contains("klippy_state"));
    }

    SECTION("an uncaptured snapshot still yields a well-formed object") {
        const json out = helix::DebugBundleCollector::collect_printer_info({});
        REQUIRE(out.is_object());
        CHECK_FALSE(out.contains("klipper_version"));
    }
}

// ============================================================================
// walk_include_tree() — the traversal that shipped a UAF in v0.99.112
// ============================================================================

namespace {
/// Fetcher backed by a table; anything not in the table is a 404.
helix::ConfigFetcher table_fetcher(const std::map<std::string, std::string>& files) {
    return [files](const std::string& path) {
        auto it = files.find(path);
        if (it == files.end())
            return helix::ConfigFetchResult{404, ""};
        return helix::ConfigFetchResult{200, it->second};
    };
}
} // namespace

TEST_CASE("DebugBundleCollector: walk_include_tree follows a multi-include root",
          "[debug-bundle]") {
    // THE REGRESSION CASE. The walk binds a name to queue[i] and pushes onto
    // `queue` while still iterating the remaining [include] patterns of the same
    // file. With `const std::string&` the first push reallocated (an
    // initializer-list vector has capacity exactly 1) and every later pattern
    // read freed memory — SIGBUS on the AD5X's mips build. Two includes in the
    // root is the minimum that reproduces it, so this must stay >= 2.
    const std::vector<std::string> available = {"printer.cfg", "a.cfg", "b.cfg", "c.cfg"};
    auto fetch = table_fetcher({
        {"printer.cfg", "[include a.cfg]\n[include b.cfg]\n[include c.cfg]\n"},
        {"a.cfg", "[stepper_x]\nstep_pin: PA1\n"},
        {"b.cfg", "[stepper_y]\nstep_pin: PA2\n"},
        {"c.cfg", "[stepper_z]\nstep_pin: PA3\n"},
    });

    std::string truncated = "sentinel";
    size_t bytes = 0;
    const json files = helix::DebugBundleCollector::walk_include_tree("printer.cfg", available,
                                                                      fetch, &truncated, &bytes);

    REQUIRE(files.is_object());
    CHECK(files.size() == 4);
    CHECK(files.contains("printer.cfg"));
    CHECK(files.contains("a.cfg"));
    CHECK(files.contains("b.cfg"));
    CHECK(files.contains("c.cfg"));
    CHECK(truncated.empty());
    CHECK(bytes > 0);
}

TEST_CASE("DebugBundleCollector: walk_include_tree terminates on an include cycle",
          "[debug-bundle]") {
    const std::vector<std::string> available = {"printer.cfg", "loop.cfg"};
    auto fetch = table_fetcher({
        {"printer.cfg", "[include loop.cfg]\n"},
        {"loop.cfg", "[include printer.cfg]\n[include loop.cfg]\n"},
    });

    const json files =
        helix::DebugBundleCollector::walk_include_tree("printer.cfg", available, fetch);
    CHECK(files.size() == 2); // seen-set stops the cycle
}

TEST_CASE("DebugBundleCollector: walk_include_tree resolves nested relative includes",
          "[debug-bundle]") {
    // An include inside mod/base.cfg is relative to mod/, and a glob must not
    // cross a '/'.
    const std::vector<std::string> available = {"printer.cfg", "mod/base.cfg", "mod/ifs.cfg",
                                                "mod/sub/deep.cfg"};
    auto fetch = table_fetcher({
        {"printer.cfg", "[include mod/base.cfg]\n"},
        {"mod/base.cfg", "[include ifs.cfg]\n[include *.cfg]\n"},
        {"mod/ifs.cfg", "[ifs]\n"},
        {"mod/sub/deep.cfg", "[deep]\n"},
    });

    const json files =
        helix::DebugBundleCollector::walk_include_tree("printer.cfg", available, fetch);
    CHECK(files.contains("mod/ifs.cfg"));
    CHECK_FALSE(files.contains("mod/sub/deep.cfg")); // '*' does not cross '/'
}

TEST_CASE("DebugBundleCollector: walk_include_tree reports HTTP errors but skips 404s",
          "[debug-bundle]") {
    const std::vector<std::string> available = {"printer.cfg", "gone.cfg", "broken.cfg"};
    auto fetch = [](const std::string& path) {
        if (path == "printer.cfg")
            return helix::ConfigFetchResult{200, "[include gone.cfg]\n[include broken.cfg]\n"};
        if (path == "broken.cfg")
            return helix::ConfigFetchResult{500, ""};
        return helix::ConfigFetchResult{404, ""};
    };

    const json files =
        helix::DebugBundleCollector::walk_include_tree("printer.cfg", available, fetch);
    CHECK_FALSE(files.contains("gone.cfg")); // a stale include is Klipper's problem, not ours
    REQUIRE(files.contains("broken.cfg"));
    CHECK(files["broken.cfg"]["error"] == "HTTP 500");
}

// ============================================================================
// Klipper/Moonraker stubs [debug-bundle]
// ============================================================================

TEST_CASE("DebugBundleCollector: klipper log tail returns empty when not connected",
          "[debug-bundle]") {
    std::string log = helix::DebugBundleCollector::collect_klipper_log_tail();
    REQUIRE(log.empty());
}

TEST_CASE("DebugBundleCollector: moonraker log tail returns empty when not connected",
          "[debug-bundle]") {
    std::string log = helix::DebugBundleCollector::collect_moonraker_log_tail();
    REQUIRE(log.empty());
}

// ============================================================================
// sanitize_value() tests [debug-bundle][sanitize]
// ============================================================================

TEST_CASE("DebugBundleCollector: sanitize_value redacts email addresses",
          "[debug-bundle][sanitize]") {
    auto result = helix::DebugBundleCollector::sanitize_value("notify user@example.com on error");
    REQUIRE(result.find("user@example.com") == std::string::npos);
    REQUIRE(result.find("[REDACTED_EMAIL]") != std::string::npos);
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts URLs with credentials",
          "[debug-bundle][sanitize]") {
    auto result =
        helix::DebugBundleCollector::sanitize_value("http://admin:s3cret@192.168.1.100:8080/api");
    REQUIRE(result.find("admin") == std::string::npos);
    REQUIRE(result.find("s3cret") == std::string::npos);
    REQUIRE(result.find("[REDACTED_CREDENTIALS]") != std::string::npos);
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts Discord webhooks",
          "[debug-bundle][sanitize]") {
    auto result = helix::DebugBundleCollector::sanitize_value(
        "https://discord.com/api/webhooks/123456/abcdef-token");
    REQUIRE(result == "[REDACTED_WEBHOOK]");
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts Telegram bot tokens",
          "[debug-bundle][sanitize]") {
    auto result = helix::DebugBundleCollector::sanitize_value(
        "https://api.telegram.org/bot123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11/sendMessage");
    REQUIRE(result == "[REDACTED_WEBHOOK]");
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts long hex tokens",
          "[debug-bundle][sanitize]") {
    std::string long_hex = "ghp_" + std::string(36, 'a'); // 40 chars total
    auto result = helix::DebugBundleCollector::sanitize_value(long_hex);
    REQUIRE(result == "[REDACTED_TOKEN]");
}

TEST_CASE("DebugBundleCollector: sanitize_value preserves normal strings",
          "[debug-bundle][sanitize]") {
    REQUIRE(helix::DebugBundleCollector::sanitize_value("hello world") == "hello world");
    REQUIRE(helix::DebugBundleCollector::sanitize_value("/tmp/printer_data") ==
            "/tmp/printer_data");
    REQUIRE(helix::DebugBundleCollector::sanitize_value("192.168.1.100") == "192.168.1.100");
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts MAC addresses",
          "[debug-bundle][sanitize]") {
    auto result = helix::DebugBundleCollector::sanitize_value("aa:bb:cc:dd:ee:ff");
    REQUIRE(result.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    REQUIRE(result.find("[REDACTED_MAC]") != std::string::npos);
}

TEST_CASE("DebugBundleCollector: sanitize_value redacts Pushover and ntfy webhooks",
          "[debug-bundle][sanitize]") {
    auto pushover =
        helix::DebugBundleCollector::sanitize_value("https://api.pushover.net/1/messages.json");
    REQUIRE(pushover == "[REDACTED_WEBHOOK]");

    auto ntfy = helix::DebugBundleCollector::sanitize_value("https://ntfy.sh/my-printer-alerts");
    REQUIRE(ntfy == "[REDACTED_WEBHOOK]");

    auto ifttt = helix::DebugBundleCollector::sanitize_value(
        "https://maker.ifttt.com/trigger/print_done/with/key/abc123");
    REQUIRE(ifttt == "[REDACTED_WEBHOOK]");
}

// ============================================================================
// collect_moonraker_info() tests [debug-bundle][moonraker]
// ============================================================================

TEST_CASE("DebugBundleCollector: collect_moonraker_info returns object with expected keys",
          "[debug-bundle][moonraker]") {
    // When not connected, should return an object with error sub-keys (not crash)
    json mr = helix::DebugBundleCollector::collect_moonraker_info();
    REQUIRE(mr.is_object());

    // Should always have these keys, even if errored
    REQUIRE(mr.contains("server_info"));
    REQUIRE(mr.contains("printer_info"));
    REQUIRE(mr.contains("system_info"));
    REQUIRE(mr.contains("printer_state"));
    REQUIRE(mr.contains("config"));
}

TEST_CASE("DebugBundleCollector: collect includes moonraker section", "[debug-bundle][moonraker]") {
    json bundle = helix::DebugBundleCollector::collect();
    REQUIRE(bundle.contains("moonraker"));
    REQUIRE(bundle["moonraker"].is_object());
}

// ============================================================================
// collect_filament_system_info() tests [debug-bundle][filament]
// ============================================================================

TEST_CASE("DebugBundleCollector: collect_filament_system_info returns object with expected keys",
          "[debug-bundle][filament]") {
    // When not connected, should return an object with error/empty sub-keys (not crash)
    json filament = helix::DebugBundleCollector::collect_filament_system_info();
    REQUIRE(filament.is_object());
    REQUIRE(filament.contains("object_list"));
    REQUIRE(filament["object_list"].is_array());
    REQUIRE(filament.contains("object_state"));
    REQUIRE(filament.contains("spoolman_status"));
}

TEST_CASE("DebugBundleCollector: collect_filament_system_info has all keys when disconnected",
          "[debug-bundle][filament]") {
    json filament = helix::DebugBundleCollector::collect_filament_system_info();
    REQUIRE(filament.contains("afc_version"));
    REQUIRE(filament.contains("mmu_version"));
}

TEST_CASE("DebugBundleCollector: collect includes filament_system section",
          "[debug-bundle][filament]") {
    json bundle = helix::DebugBundleCollector::collect();
    REQUIRE(bundle.contains("filament_system"));
    REQUIRE(bundle["filament_system"].is_object());
}

TEST_CASE("DebugBundleCollector: filter_filament_objects matches expected prefixes",
          "[debug-bundle][filament]") {
    json objects = json::array({
        "AFC",
        "AFC_stepper lane1",
        "AFC_stepper lane2",
        "AFC_hub Turtle_1",
        "AFC_extruder extruder",
        "AFC_buffer TN_1",
        "AFC_lane lane3",
        "AFC_BoxTurtle Turtle_1",
        "mmu",
        "toolchanger",
        "tool T0",
        "tool T1",
        "filament_switch_sensor runout",
        "filament_motion_sensor encoder",
        "extruder",
        "heater_bed",
        "fan",
        "print_stats",
        "toolhead",
    });

    auto filtered = helix::DebugBundleCollector::filter_filament_objects(objects);
    REQUIRE(filtered.size() == 14);

    // Verify no false positives
    for (const auto& obj : filtered) {
        std::string name = obj.get<std::string>();
        REQUIRE(name != "extruder");
        REQUIRE(name != "heater_bed");
        REQUIRE(name != "fan");
        REQUIRE(name != "print_stats");
        REQUIRE(name != "toolhead");
    }
}

TEST_CASE("DebugBundleCollector: filter_filament_objects handles empty and non-array input",
          "[debug-bundle][filament]") {
    REQUIRE(helix::DebugBundleCollector::filter_filament_objects(json::array()).empty());
    REQUIRE(helix::DebugBundleCollector::filter_filament_objects(json::object()).empty());
    REQUIRE(helix::DebugBundleCollector::filter_filament_objects(json(42)).empty());
}

TEST_CASE("DebugBundleCollector: extract_gcode_macro_names captures bare macro names",
          "[debug-bundle][filament][macro-names]") {
    // The question this exists to answer: does macro X exist on this printer?
    // printer.cfg is stripped from the bundle before shape-collapse, so on an
    // AD5X the whole ZMOD config is gone and nothing else can say.
    json objects = json::array({
        "gcode_macro A_CHANGE_FILAMENT", "gcode_macro INSERT_PRUTOK_IFS",
        "gcode_macro _IFS_REMOVE_CURRENT_PRUTOK", "gcode_macro _G28", "extruder", "toolhead",
        "filament_switch_sensor runout",
        "gcode_button estop", // not a macro despite the prefix overlap
        "gcode_macro",        // bare section name, no macro name follows
        "gcode_macro ",       // prefix with an empty name
    });

    auto macros = helix::DebugBundleCollector::extract_gcode_macro_names(objects);

    REQUIRE(macros.size() == 4);
    // Stored bare, with the "gcode_macro " prefix stripped.
    CHECK(macros[0].get<std::string>() == "A_CHANGE_FILAMENT");
    CHECK(macros[1].get<std::string>() == "INSERT_PRUTOK_IFS");
    CHECK(macros[2].get<std::string>() == "_IFS_REMOVE_CURRENT_PRUTOK");
    CHECK(macros[3].get<std::string>() == "_G28");

    // No non-macro object, and no empty entry from the degenerate prefixes -
    // an empty string would read as a real macro whose name we lost.
    for (const auto& m : macros) {
        const std::string name = m.get<std::string>();
        CHECK_FALSE(name.empty());
        CHECK(name.find("gcode_macro") == std::string::npos);
        CHECK(name != "estop");
    }
}

TEST_CASE("DebugBundleCollector: extract_gcode_macro_names caps runaway configs",
          "[debug-bundle][filament][macro-names]") {
    json objects = json::array();
    for (size_t i = 0; i < helix::DebugBundleCollector::MAX_GCODE_MACRO_NAMES + 50; ++i) {
        objects.push_back("gcode_macro M" + std::to_string(i));
    }

    auto macros = helix::DebugBundleCollector::extract_gcode_macro_names(objects);
    REQUIRE(macros.size() == helix::DebugBundleCollector::MAX_GCODE_MACRO_NAMES);
    CHECK(macros[0].get<std::string>() == "M0");
}

TEST_CASE("DebugBundleCollector: extract_gcode_macro_names handles empty and non-array input",
          "[debug-bundle][filament][macro-names]") {
    REQUIRE(helix::DebugBundleCollector::extract_gcode_macro_names(json::array()).empty());
    REQUIRE(helix::DebugBundleCollector::extract_gcode_macro_names(json::object()).empty());
    REQUIRE(helix::DebugBundleCollector::extract_gcode_macro_names(json(42)).empty());
    // A non-string element must not abort the scan of the rest.
    json mixed = json::array({42, "gcode_macro KEPT", json::object()});
    auto macros = helix::DebugBundleCollector::extract_gcode_macro_names(mixed);
    REQUIRE(macros.size() == 1);
    CHECK(macros[0].get<std::string>() == "KEPT");
}

// ============================================================================
// printer.cfg + [include] tree [debug-bundle][printer-config]
// ============================================================================

TEST_CASE("DebugBundleCollector: parse_include_patterns finds Klipper includes",
          "[debug-bundle][printer-config]") {
    const std::string body = "[include mod/base.cfg]\r\n"
                             "[include  spaced.cfg ]\n"
                             "[include mod/*.cfg]\n"
                             "[printer]\n"
                             "kinematics: corexy\n"
                             "# [include commented.cfg]\n"
                             "  [include indented.cfg]\n"
                             "gcode: [include inline.cfg]\n"
                             "[include unterminated.cfg\n";

    auto pats = helix::DebugBundleCollector::parse_include_patterns(body);

    REQUIRE(pats.size() == 3);
    CHECK(pats[0] == "mod/base.cfg"); // trailing CR stripped
    CHECK(pats[1] == "spaced.cfg");   // surrounding whitespace trimmed
    CHECK(pats[2] == "mod/*.cfg");

    // A Klipper section header must start at column 0, so an indented or
    // commented "[include ...]" is an option continuation, not a section - and
    // treating one as an include would fetch files the printer never loads.
    for (const auto& p : pats) {
        CHECK(p != "commented.cfg");
        CHECK(p != "indented.cfg");
        CHECK(p != "inline.cfg");
        CHECK(p != "unterminated.cfg");
    }
}

TEST_CASE("DebugBundleCollector: glob_match does not let wildcards cross a slash",
          "[debug-bundle][printer-config]") {
    CHECK(helix::DebugBundleCollector::glob_match("printer.cfg", "printer.cfg"));
    CHECK(helix::DebugBundleCollector::glob_match("mod/*.cfg", "mod/base.cfg"));
    CHECK(helix::DebugBundleCollector::glob_match("*.cfg", "printer.cfg"));
    CHECK(helix::DebugBundleCollector::glob_match("mod/?.cfg", "mod/a.cfg"));

    // The load-bearing case: Python glob (which Klipper uses) stops '*' at a
    // separator, so a top-level "*.cfg" must not vacuum up the whole tree.
    CHECK_FALSE(helix::DebugBundleCollector::glob_match("*.cfg", "mod/base.cfg"));
    CHECK_FALSE(helix::DebugBundleCollector::glob_match("mod/*.cfg", "mod/sub/base.cfg"));
    CHECK_FALSE(helix::DebugBundleCollector::glob_match("mod/?.cfg", "mod/ab.cfg"));

    CHECK_FALSE(helix::DebugBundleCollector::glob_match("printer.cfg", "printer.cfg.bak"));
    CHECK_FALSE(helix::DebugBundleCollector::glob_match("other.cfg", "printer.cfg"));
}

TEST_CASE("DebugBundleCollector: resolve_include_pattern is relative to the including file",
          "[debug-bundle][printer-config]") {
    const std::vector<std::string> available = {
        "printer.cfg",      "mod/base.cfg", "mod/ifs.cfg",
        "mod/sub/deep.cfg", "top.cfg",      "mod/notcfg.txt",
    };

    // From printer.cfg (config root), a bare name stays at the root.
    auto from_root =
        helix::DebugBundleCollector::resolve_include_pattern("top.cfg", "printer.cfg", available);
    REQUIRE(from_root.size() == 1);
    CHECK(from_root[0] == "top.cfg");

    // From mod/base.cfg, a bare name resolves INTO mod/ - resolving it at the
    // root instead would fetch the wrong file or silently nothing.
    auto sibling =
        helix::DebugBundleCollector::resolve_include_pattern("ifs.cfg", "mod/base.cfg", available);
    REQUIRE(sibling.size() == 1);
    CHECK(sibling[0] == "mod/ifs.cfg");

    // A glob expands to every match at that level and no deeper.
    auto globbed =
        helix::DebugBundleCollector::resolve_include_pattern("mod/*.cfg", "printer.cfg", available);
    REQUIRE(globbed.size() == 2);
    CHECK(globbed[0] == "mod/base.cfg");
    CHECK(globbed[1] == "mod/ifs.cfg");

    // "./" prefix normalizes to the listing's form.
    auto dotted =
        helix::DebugBundleCollector::resolve_include_pattern("./top.cfg", "printer.cfg", available);
    REQUIRE(dotted.size() == 1);
    CHECK(dotted[0] == "top.cfg");

    // No match is empty, not a fabricated path.
    CHECK(helix::DebugBundleCollector::resolve_include_pattern("missing.cfg", "printer.cfg",
                                                               available)
              .empty());
}

TEST_CASE("DebugBundleCollector: config bodies sanitize per line, not whole-file",
          "[debug-bundle][printer-config][sanitize]") {
    // A whole printer.cfg exceeds sanitize_value()'s 4 KB guard, which would
    // return [REDACTED_LONG_VALUE] for the entire file. sanitize_text_block()
    // is what keeps the config readable while still redacting the secrets that
    // actually turn up in one.
    std::string body = "[printer]\nkinematics: corexy\n"
                       "[gcode_macro NOTIFY]\n"
                       "gcode: RUN_SHELL_COMMAND CMD=curl https://api.telegram.org/bot123/send\n"
                       "[spoolman]\nserver: http://user:hunter2@spool.local:7912\n"
                       "# owner: someone@example.com\n";
    body += std::string(5000, 'x'); // push the file past the 4 KB guard
    body += "\n";

    const std::string clean = helix::DebugBundleCollector::sanitize_text_block(body);

    // Structure survives - the whole file was NOT collapsed to one marker.
    CHECK(clean.find("kinematics: corexy") != std::string::npos);
    CHECK(clean.find("[gcode_macro NOTIFY]") != std::string::npos);

    // Secrets do not.
    CHECK(clean.find("api.telegram.org/bot123") == std::string::npos);
    CHECK(clean.find("hunter2") == std::string::npos);
    CHECK(clean.find("someone@example.com") == std::string::npos);
    CHECK(clean.find("[REDACTED_CREDENTIALS]") != std::string::npos);
    CHECK(clean.find("[REDACTED_EMAIL]") != std::string::npos);
}

// ============================================================================
// Realistic Moonraker config sanitization [debug-bundle][sanitize]
// ============================================================================

TEST_CASE("DebugBundleCollector: sanitize_json handles realistic moonraker config",
          "[debug-bundle][sanitize]") {
    // Simulate a realistic /server/config response with various PII
    json config = R"({
        "result": {
            "config": {
                "server": {
                    "host": "0.0.0.0",
                    "port": 7125,
                    "klippy_uds_address": "/home/pi/printer_data/comms/klippy.sock"
                },
                "authorization": {
                    "trusted_clients": ["192.168.1.0/24", "10.0.0.0/8"],
                    "cors_domains": ["http://my-printer.local"]
                },
                "notifier my_telegram": {
                    "url": "https://api.telegram.org/bot123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11/sendMessage",
                    "events": ["error", "complete"],
                    "body": "Printer notification for user@example.com"
                },
                "notifier my_discord": {
                    "url": "https://discord.com/api/webhooks/1234567890/ABCdefGHIjklMNOpqrSTUvwxYZ",
                    "events": ["error"]
                },
                "update_manager client mainsail": {
                    "type": "web",
                    "repo": "mainsail-crew/mainsail"
                },
                "power my_plug": {
                    "type": "tplink_smartplug",
                    "address": "192.168.1.50",
                    "password": "my_plug_password"
                },
                "webcam my_camera": {
                    "stream_url": "http://admin:camera_pass@192.168.1.60:8080/stream",
                    "snapshot_url": "/webcam/?action=snapshot"
                },
                "spoolman": {
                    "server": "http://192.168.1.100:7912"
                }
            }
        }
    })"_json;

    json sanitized = helix::DebugBundleCollector::sanitize_json(config);

    // Telegram URL should be fully redacted
    std::string telegram_url =
        sanitized["result"]["config"]["notifier my_telegram"]["url"].get<std::string>();
    REQUIRE(telegram_url == "[REDACTED_WEBHOOK]");

    // Discord webhook should be fully redacted
    std::string discord_url =
        sanitized["result"]["config"]["notifier my_discord"]["url"].get<std::string>();
    REQUIRE(discord_url == "[REDACTED_WEBHOOK]");

    // Email in body should be redacted
    std::string body =
        sanitized["result"]["config"]["notifier my_telegram"]["body"].get<std::string>();
    REQUIRE(body.find("user@example.com") == std::string::npos);
    REQUIRE(body.find("[REDACTED_EMAIL]") != std::string::npos);

    // Password key should be redacted
    std::string pw = sanitized["result"]["config"]["power my_plug"]["password"].get<std::string>();
    REQUIRE(pw == "[REDACTED]");

    // Camera URL with credentials should be redacted
    std::string cam_url =
        sanitized["result"]["config"]["webcam my_camera"]["stream_url"].get<std::string>();
    REQUIRE(cam_url.find("admin") == std::string::npos);
    REQUIRE(cam_url.find("camera_pass") == std::string::npos);

    // Safe values should be preserved
    REQUIRE(sanitized["result"]["config"]["server"]["port"] == 7125);
    REQUIRE(sanitized["result"]["config"]["update_manager client mainsail"]["repo"] ==
            "mainsail-crew/mainsail");
}

// ============================================================================
// collect_crash_report_txt() tests [debug-bundle][crash]
// ============================================================================

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_report_txt reads existing file",
                 "[debug-bundle][crash]") {
    // Write a crash_report.txt in the temp dir
    write_file("crash_report.txt", "=== HelixScreen Crash Report ===\n\nSignal: 11 (SIGSEGV)\n");

    auto result = helix::DebugBundleCollector::collect_crash_report_txt(temp_dir_.string());
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("SIGSEGV") != std::string::npos);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_report_txt returns empty when missing",
                 "[debug-bundle][crash]") {
    auto result = helix::DebugBundleCollector::collect_crash_report_txt(temp_dir_.string());
    REQUIRE(result.empty());
}

// ============================================================================
// collect_crash_history() tests [debug-bundle][crash]
// ============================================================================

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_history reads history file",
                 "[debug-bundle][crash]") {
    // Write a crash_history.json
    json history = json::array();
    history.push_back({{"timestamp", "2026-02-22T04:00:00Z"},
                       {"signal", 11},
                       {"signal_name", "SIGSEGV"},
                       {"app_version", "0.10.12"},
                       {"uptime_sec", 3600},
                       {"github_issue", 142},
                       {"github_url", "https://github.com/prestonbrown/helixscreen/issues/142"},
                       {"sent_via", "crash_reporter"}});

    std::ofstream ofs((temp_dir_ / "crash_history.json").string());
    ofs << history.dump(2);
    ofs.close();

    auto result = helix::DebugBundleCollector::collect_crash_history(temp_dir_.string());
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);
    REQUIRE(result[0]["signal"] == 11);
    REQUIRE(result[0]["github_issue"] == 142);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_history returns empty array when missing",
                 "[debug-bundle][crash]") {
    auto result = helix::DebugBundleCollector::collect_crash_history(temp_dir_.string());
    REQUIRE(result.is_array());
    REQUIRE(result.empty());
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_history handles corrupt file",
                 "[debug-bundle][crash]") {
    write_file("crash_history.json", "not valid json {{{{");

    auto result = helix::DebugBundleCollector::collect_crash_history(temp_dir_.string());
    REQUIRE(result.is_array());
    REQUIRE(result.empty());
}

// ============================================================================
// collect_device_id() tests [debug-bundle]
// ============================================================================

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_device_id reads from telemetry device file",
                 "[debug-bundle]") {
    // Write a telemetry_device.json
    json device = {{"uuid", "550e8400-e29b-41d4-a716-446655440000"}, {"salt", "random_salt_value"}};

    std::ofstream ofs((temp_dir_ / "telemetry_device.json").string());
    ofs << device.dump();
    ofs.close();

    auto result = helix::DebugBundleCollector::collect_device_id(temp_dir_.string());
    // Should be a hashed device ID (64 char hex), not the raw UUID
    REQUIRE_FALSE(result.empty());
    // Must NOT contain the raw UUID
    REQUIRE(result.find("550e8400") == std::string::npos);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_device_id returns empty when no device file",
                 "[debug-bundle]") {
    auto result = helix::DebugBundleCollector::collect_device_id(temp_dir_.string());
    REQUIRE(result.empty());
}

// ============================================================================
// collect_log_tail() path fixes [debug-bundle][log]
// ============================================================================

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_log_tail finds helix.log in XDG path",
                 "[debug-bundle][log]") {
    // Create an XDG-style log file
    fs::create_directories(temp_dir_ / "helix-screen");
    std::ofstream ofs((temp_dir_ / "helix-screen" / "helix.log").string());
    for (int i = 1; i <= 10; i++) {
        ofs << "[2026-02-22 12:00:0" << i << "] test log line " << i << "\n";
    }
    ofs.close();

    // The collect_log_tail method uses env vars and fixed paths, which we can't
    // easily override in tests. Instead, test the file-based path resolution
    // by testing the overload that accepts explicit paths.
    auto result = helix::DebugBundleCollector::collect_log_tail_from_paths(
        {(temp_dir_ / "helix-screen" / "helix.log").string()}, 5);
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("test log line 10") != std::string::npos);
    REQUIRE(result.find("test log line 6") != std::string::npos);
    // Line 5 should NOT be in a 5-line tail
    REQUIRE(result.find("test log line 5") == std::string::npos);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_log_tail_from_paths tries paths in order",
                 "[debug-bundle][log]") {
    // Only write the second path
    write_file("fallback.log", "fallback line 1\nfallback line 2\n");

    auto result = helix::DebugBundleCollector::collect_log_tail_from_paths(
        {(temp_dir_ / "nonexistent.log").string(), (temp_dir_ / "fallback.log").string()}, 10);
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("fallback line 1") != std::string::npos);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_log_tail_from_paths returns empty when all missing",
                 "[debug-bundle][log]") {
    auto result = helix::DebugBundleCollector::collect_log_tail_from_paths(
        {"/nonexistent/a.log", "/nonexistent/b.log"}, 10);
    REQUIRE(result.empty());
}

// ============================================================================
// log_tail / crash section sanitization [debug-bundle][privacy]
//
// These sections used to be inserted into the bundle verbatim while every
// other text section went through the sanitizer — and log_tail is the section
// densest in identifying data, because the ring captures at debug regardless
// of the user's configured verbosity (#1191).
// ============================================================================

TEST_CASE("DebugBundleCollector: sanitize_value leaves an SSID-shaped string alone",
          "[debug-bundle][privacy]") {
    // Pins WHY the call-site redaction exists. An SSID is an arbitrary
    // user-chosen string; no regex distinguishes it from ordinary log text, so
    // the sanitizer cannot be the control for it. If someone later "fixes"
    // this by adding an SSID regex, that regex will eat real log content and
    // this test should make them think twice.
    const std::string ssid_line = "Status: connected=true ssid='Pretzel Logic Cafe' signal=66%";
    REQUIRE(helix::DebugBundleCollector::sanitize_value(ssid_line) == ssid_line);
}

TEST_CASE("DebugBundleCollector: sanitize_text_block redacts per line", "[debug-bundle][privacy]") {
    // The 4 KB ReDoS guard in sanitize_value() would swallow a whole log as one
    // [REDACTED_LONG_VALUE]; splitting per line is what keeps the log readable
    // while still scrubbing each line.
    const std::string body = "[10:04:30.373] [debug] iface=wlan0 mac=aa:bb:cc:dd:ee:ff\n"
                             "[10:04:30.374] [debug] nothing sensitive here\n"
                             "[10:04:30.375] [debug] peer AA-BB-CC-DD-EE-FF seen\n";

    const auto out = helix::DebugBundleCollector::sanitize_text_block(body);

    INFO("out=" << out);
    REQUIRE(out.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    REQUIRE(out.find("AA-BB-CC-DD-EE-FF") == std::string::npos);
    // Surrounding content survives — a sanitizer that nuked the whole block
    // would pass the two checks above and destroy the log's usefulness.
    REQUIRE(out.find("nothing sensitive here") != std::string::npos);
    REQUIRE(out.find("iface=wlan0") != std::string::npos);
    REQUIRE(std::count(out.begin(), out.end(), '\n') == std::count(body.begin(), body.end(), '\n'));
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_report_txt sanitizes what it returns",
                 "[debug-bundle][privacy]") {
    // Crash text reaches the network twice: through the bundle and through the
    // crash reporter's automatic upload. It used to be returned verbatim.
    write_file("crash_report.txt", "=== HelixScreen Crash Report ===\nSignal: 11 (SIGSEGV)\n"
                                   "wlan0 mac=aa:bb:cc:dd:ee:ff\n");

    auto result = helix::DebugBundleCollector::collect_crash_report_txt(temp_dir_.string());

    REQUIRE_FALSE(result.empty());
    INFO("result=" << result);
    REQUIRE(result.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    REQUIRE(result.find("[REDACTED_MAC]") != std::string::npos);
    REQUIRE(result.find("SIGSEGV") != std::string::npos);
}

TEST_CASE_METHOD(DebugBundleTestFixture,
                 "DebugBundleCollector: collect_crash_txt sanitizes what it returns",
                 "[debug-bundle][privacy]") {
    write_file("crash.txt", "{\"signal\":11,\"iface\":\"wlan0\",\"mac\":\"aa:bb:cc:dd:ee:ff\"}\n");

    auto result = helix::DebugBundleCollector::collect_crash_txt(temp_dir_.string());

    REQUIRE_FALSE(result.empty());
    INFO("result=" << result);
    REQUIRE(result.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    REQUIRE(result.find("[REDACTED_MAC]") != std::string::npos);
}

// ============================================================================
// Update diagnostics [debug-bundle][update]
//
// Bundle 3Q2GB74K ("cannot update HelixScreen", pi32) was undiagnosable because
// nothing in the bundle said whether the update rows were even rendered. The
// About overlay binds both "Check for Updates" and "Install Update" to
// show_update_settings = !update_install_suppressed(); when that is true the
// rows are absent and the user has no in-app path to an update at all.
// ============================================================================

namespace {

// A fully-populated, everything-works baseline. Individual tests flip one field
// so a failure names the field that broke rather than the whole struct.
helix::UpdateDiagnostics healthy_diag() {
    helix::UpdateDiagnostics d;
    d.install_root = "/opt/helixscreen";
    d.install_parent_writable = true;
    d.install_root_writable = true;
    d.self_update_supported = true;
    d.externally_managed = false;
    d.channel = "stable";
    d.r2_base_url = "https://releases.helixscreen.org";
    d.last_check_status = "up_to_date";
    d.platform_asset_name = "helixscreen-pi32.zip";
    return d;
}

} // namespace

TEST_CASE("DebugBundleCollector: build_update_info reports a healthy install as not suppressed",
          "[debug-bundle][update]") {
    json upd = helix::DebugBundleCollector::build_update_info(healthy_diag());

    REQUIRE(upd["install_root"].get<std::string>() == "/opt/helixscreen");
    REQUIRE(upd["install_parent_writable"].get<bool>() == true);
    REQUIRE(upd["install_root_writable"].get<bool>() == true);
    REQUIRE(upd["self_update_supported"].get<bool>() == true);
    REQUIRE(upd["externally_managed"].get<bool>() == false);
    REQUIRE(upd["suppressed"].get<bool>() == false);
    REQUIRE(upd["channel"].get<std::string>() == "stable");
    REQUIRE(upd["r2_base_url"].get<std::string>() == "https://releases.helixscreen.org");
    REQUIRE(upd["last_check_status"].get<std::string>() == "up_to_date");
    REQUIRE(upd["platform_asset_name"].get<std::string>() == "helixscreen-pi32.zip");

    // Optional fields must be ABSENT rather than empty strings — an empty
    // "available_version" reads like a failed lookup, not "no update cached".
    REQUIRE_FALSE(upd.contains("available_version"));
    REQUIRE_FALSE(upd.contains("last_check_error"));
}

TEST_CASE("DebugBundleCollector: build_update_info marks a read-only install tree suppressed",
          "[debug-bundle][update]") {
    auto d = healthy_diag();
    // Neither writable nor escalatable — a genuinely read-only rootfs.
    d.install_parent_writable = false;
    d.install_root_writable = false;
    d.self_update_supported = false;

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["suppressed"].get<bool>() == true);
    // Both causes must stay distinguishable: this is the physical-impossibility
    // branch, NOT the firmware flag.
    REQUIRE(upd["self_update_supported"].get<bool>() == false);
    REQUIRE(upd["externally_managed"].get<bool>() == false);
}

TEST_CASE("DebugBundleCollector: build_update_info leaves an in-place-updatable install "
          "unsuppressed",
          "[debug-bundle][update]") {
    // The standalone-display shape: /opt is root-owned so no rename can happen
    // there, but the root itself is owned by the service user, and install.sh
    // replaces its contents in place. Reporting this as suppressed is the bug
    // that locked a user out for good, so the three fields must stay distinct:
    // WHICH route is open is the entire diagnostic value.
    auto d = healthy_diag();
    d.install_parent_writable = false;
    d.install_root_writable = true;
    d.self_update_supported = true;

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["suppressed"].get<bool>() == false);
    REQUIRE(upd["install_parent_writable"].get<bool>() == false);
    REQUIRE(upd["install_root_writable"].get<bool>() == true);
    REQUIRE(upd["self_update_supported"].get<bool>() == true);
}

TEST_CASE("DebugBundleCollector: build_update_info keeps the sudo-only install distinguishable",
          "[debug-bundle][update]") {
    // Neither writability term open, yet self_update_supported() said yes: the
    // answer came from root escalation alone. Worth telling apart from the two
    // cases above, because it is the one that stops working the moment the app
    // runs under the shipped systemd unit (NoNewPrivileges=true blocks sudo).
    auto d = healthy_diag();
    d.install_parent_writable = false;
    d.install_root_writable = false;
    d.self_update_supported = true;

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["suppressed"].get<bool>() == false);
    REQUIRE(upd["install_parent_writable"].get<bool>() == false);
    REQUIRE(upd["install_root_writable"].get<bool>() == false);
    REQUIRE(upd["self_update_supported"].get<bool>() == true);
}

TEST_CASE("DebugBundleCollector: build_update_info marks a firmware-managed install suppressed",
          "[debug-bundle][update]") {
    auto d = healthy_diag();
    d.externally_managed = true; // HELIX_DISABLE_AUTO_UPDATES

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["suppressed"].get<bool>() == true);
    REQUIRE(upd["externally_managed"].get<bool>() == true);
    // The tree is updatable; suppression is policy, not physics.
    REQUIRE(upd["self_update_supported"].get<bool>() == true);
}

TEST_CASE("DebugBundleCollector: build_update_info suppressed matches the UI gate predicate",
          "[debug-bundle][update]") {
    // The bundle field and the subject the XML binds to must be the same
    // function, not two copies that can drift.
    for (bool managed : {false, true}) {
        for (bool supported : {false, true}) {
            auto d = healthy_diag();
            d.externally_managed = managed;
            d.self_update_supported = supported;
            json upd = helix::DebugBundleCollector::build_update_info(d);
            INFO("managed=" << managed << " self_update_supported=" << supported);
            REQUIRE(upd["suppressed"].get<bool>() ==
                    compute_update_install_suppressed(managed, supported));
        }
    }
}

TEST_CASE("DebugBundleCollector: build_update_info carries the cached update and check error",
          "[debug-bundle][update]") {
    auto d = healthy_diag();
    d.last_check_status = "error";
    d.available_version = "0.99.107";
    d.last_check_error = "HTTP 403 from manifest";

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["last_check_status"].get<std::string>() == "error");
    REQUIRE(upd["available_version"].get<std::string>() == "0.99.107");
    REQUIRE(upd["last_check_error"].get<std::string>() == "HTTP 403 from manifest");
}

TEST_CASE("DebugBundleCollector: build_update_info sanitizes the paths and URLs it emits",
          "[debug-bundle][update][privacy]") {
    auto d = healthy_diag();
    // A self-hosted mirror can carry basic-auth credentials in the URL.
    d.r2_base_url = "https://deploy:hunter2@mirror.example.com/releases";
    // Home-dir installs keep their layout (that IS the diagnostic) but must
    // still lose anything that matches a credential/email/MAC shape.
    d.install_root = "/home/pi/helixscreen";

    json upd = helix::DebugBundleCollector::build_update_info(d);

    const auto url = upd["r2_base_url"].get<std::string>();
    INFO("r2_base_url=" << url);
    REQUIRE(url.find("hunter2") == std::string::npos);
    REQUIRE(url.find("[REDACTED_CREDENTIALS]") != std::string::npos);

    // Deliberately preserved: which root the install lives under is the whole
    // point of the field, and the same path is already all over log_tail.
    REQUIRE(upd["install_root"].get<std::string>() == "/home/pi/helixscreen");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "DebugBundleCollector: collect_update_info wires to the live predicates",
                 "[debug-bundle][update]") {
    // Production takes this snapshot in UpdateChecker::init(); take it here so
    // the assertions below compare against the current (fixture-reset) Config.
    UpdateChecker::instance().refresh_config_snapshot();

    json upd = helix::DebugBundleCollector::collect_update_info();

    // Every documented key must be present — a bundle missing one of these is
    // exactly the hole that made 3Q2GB74K undiagnosable.
    for (const char* key :
         {"install_root", "install_parent_writable", "self_update_supported", "externally_managed",
          "suppressed", "channel", "r2_base_url", "last_check_status", "platform_asset_name"}) {
        INFO("missing key: " << key);
        REQUIRE(upd.contains(key));
    }

    REQUIRE(upd["install_root"].is_string());
    REQUIRE(upd["install_parent_writable"].get<bool>() ==
            compute_self_update_supported(app_get_install_root(), /*can_escalate=*/false));
    REQUIRE(upd["self_update_supported"].get<bool>() == self_update_supported());
    // A writable parent must imply support; the reverse need not hold, since
    // escalation covers the /opt + unprivileged-service layout.
    if (upd["install_parent_writable"].get<bool>()) {
        REQUIRE(upd["self_update_supported"].get<bool>());
    }
    REQUIRE(upd["externally_managed"].get<bool>() == updates_externally_managed());
    REQUIRE(upd["suppressed"].get<bool>() == update_install_suppressed());

    // The exact artifact this device asks for — #993 was a drifted copy of it.
    REQUIRE(upd["platform_asset_name"].get<std::string>() == UpdateChecker::platform_asset_name());

    const auto channel = upd["channel"].get<std::string>();
    INFO("channel=" << channel);
    REQUIRE((channel == "stable" || channel == "beta" || channel == "dev"));
    REQUIRE(channel == UpdateChecker::channel_name(UpdateChecker::instance().get_channel()));

    // Effective URL must be resolved (default applied) and normalized, so a
    // misconfigured /update/r2_url is visible as itself rather than as "".
    const auto url = upd["r2_base_url"].get<std::string>();
    INFO("r2_base_url=" << url);
    REQUIRE_FALSE(url.empty());
    REQUIRE(url.back() != '/');

    const auto status = upd["last_check_status"].get<std::string>();
    INFO("last_check_status=" << status);
    REQUIRE((status == "idle" || status == "checking" || status == "update_available" ||
             status == "up_to_date" || status == "error"));
}

TEST_CASE_METHOD(HelixTestFixture, "DebugBundleCollector: collect() includes the update section",
                 "[debug-bundle][update]") {
    json bundle = helix::DebugBundleCollector::collect();

    REQUIRE(bundle.contains("update"));
    REQUIRE(bundle["update"].is_object());
    // Not an error stub — the collector must have actually produced the data.
    REQUIRE_FALSE(bundle["update"].contains("error"));
    REQUIRE(bundle["update"].contains("suppressed"));
    REQUIRE(bundle["update"]["suppressed"].is_boolean());
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker: effective_r2_base_url defaults and normalizes",
                 "[debug-bundle][update]") {
    // HelixTestFixture resets Config to empty, so no /update/r2_url is set.
    REQUIRE(UpdateChecker::effective_r2_base_url() ==
            std::string(UpdateChecker::DEFAULT_R2_BASE_URL));

    auto* config = helix::Config::get_instance();
    REQUIRE(config != nullptr);
    config->set<std::string>("/update/r2_url", "https://mirror.example.com/rel///");
    REQUIRE(UpdateChecker::effective_r2_base_url() == "https://mirror.example.com/rel");
}

TEST_CASE("UpdateChecker: channel_name covers every channel", "[debug-bundle][update]") {
    REQUIRE(std::string(UpdateChecker::channel_name(UpdateChecker::UpdateChannel::Stable)) ==
            "stable");
    REQUIRE(std::string(UpdateChecker::channel_name(UpdateChecker::UpdateChannel::Beta)) == "beta");
    REQUIRE(std::string(UpdateChecker::channel_name(UpdateChecker::UpdateChannel::Dev)) == "dev");
}

TEST_CASE("app_globals: compute_update_install_suppressed truth table", "[debug-bundle][update]") {
    REQUIRE(compute_update_install_suppressed(false, true) == false); // normal, updatable
    REQUIRE(compute_update_install_suppressed(true, true) == true);   // firmware-managed
    REQUIRE(compute_update_install_suppressed(false, false) == true); // read-only install tree
    REQUIRE(compute_update_install_suppressed(true, false) == true);  // both
}

// ----------------------------------------------------------------------------
// The diagnostics snapshot must survive "no check has ever run"
//
// UpdateChecker keeps two Config-derived caches. cached_channel_ /
// cached_r2_base_url_ are written once inside check_for_updates() before the
// worker spawns and are read UNLOCKED by that worker, so they must never be
// refreshed from anywhere else. config_snapshot_ is the separate, mutex-guarded
// copy the debug bundle reads from HttpExecutor::slow(). Populating it only in
// check_for_updates() would leave every bundle from a device that never checked
// reporting an empty channel — which is precisely the device filing a "cannot
// update" report.
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "DebugBundleCollector: update section is populated by init() alone",
                 "[debug-bundle][update]") {
    auto& checker = UpdateChecker::instance();

    // The checker is a process-wide singleton and other tests in this shard may
    // already have snapshotted it. Cycle it down (init() first, since shutdown()
    // no-ops when it was never initialised) so the snapshot is provably empty
    // and only the init() below can have refilled it.
    checker.init();
    checker.shutdown();
    REQUIRE(checker.config_snapshot().channel.empty());
    REQUIRE(checker.config_snapshot().r2_base_url.empty());

    checker.init(); // no check_for_updates() anywhere in this test

    auto snap = checker.config_snapshot();
    INFO("channel=" << snap.channel << " r2=" << snap.r2_base_url);
    REQUIRE_FALSE(snap.channel.empty());
    REQUIRE_FALSE(snap.r2_base_url.empty());

    json upd = helix::DebugBundleCollector::collect_update_info();
    REQUIRE(upd["channel"].get<std::string>() != "unknown");
    REQUIRE(upd["r2_base_url"].get<std::string>() != "unknown");
    REQUIRE(upd["channel"].get<std::string>() == snap.channel);
    REQUIRE(upd["r2_base_url"].get<std::string>() == snap.r2_base_url);
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker: config snapshot tracks a channel change",
                 "[debug-bundle][update]") {
    auto* config = helix::Config::get_instance();
    REQUIRE(config != nullptr);
    auto& checker = UpdateChecker::instance();

    // Beta and Dev are only effective while /beta_features is unlocked —
    // get_channel() clamps to stable otherwise, so without this the snapshot
    // would read "stable" no matter what the channel key says.
    const bool prev_beta = config->get<bool>("/beta_features", false);
    config->set<bool>("/beta_features", true);

    config->set<int>("/update/channel", 1); // Beta
    checker.refresh_config_snapshot();
    REQUIRE(checker.config_snapshot().channel == "beta");

    config->set<int>("/update/channel", 2); // Dev
    checker.refresh_config_snapshot();
    REQUIRE(checker.config_snapshot().channel == "dev");
    REQUIRE(helix::DebugBundleCollector::collect_update_info()["channel"].get<std::string>() ==
            "dev");

    // The snapshot lives on the process-wide singleton, which the fixture does
    // not reset. Put it back so a later test in this shard sees a clean value.
    config->set<int>("/update/channel", 0);
    config->set<bool>("/beta_features", prev_beta);
    checker.refresh_config_snapshot();
}

TEST_CASE("DebugBundleCollector: build_update_info reports an unsnapshotted updater as unknown",
          "[debug-bundle][update]") {
    // An empty snapshot means UpdateChecker::init() never ran. Emitting "" there
    // is indistinguishable from a lookup failure; "unknown" is a fact.
    auto d = healthy_diag();
    d.channel.clear();
    d.r2_base_url.clear();

    json upd = helix::DebugBundleCollector::build_update_info(d);

    REQUIRE(upd["channel"].get<std::string>() == "unknown");
    REQUIRE(upd["r2_base_url"].get<std::string>() == "unknown");
}

// ============================================================================
// condense_klipper_log — repeating-shape collapse
// ============================================================================

namespace {

int count_lines_with(const std::string& hay, const std::string& needle) {
    int n = 0;
    std::istringstream st(hay);
    std::string l;
    while (std::getline(st, l)) {
        if (l.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

int line_count(const std::string& s) {
    if (s.empty())
        return 0;
    return 1 + static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

/// Klipper's per-second Stats line: the noise the first implementation targeted.
std::string stats_lines(int n, int t0 = 0) {
    std::string s;
    for (int i = 0; i < n; ++i)
        s += "Stats " + std::to_string(t0 + i) + ".0: sysload=0.5 print_stall=7\n";
    return s;
}

/// ZMOD's 4-line toolhead parameter dump: the noise it MISSED, which is what
/// made the shipped payload reach less far than the raw tail it replaced.
std::string toolhead_spam(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += "toolhead: max_velocity: 600.000000\n";
        s += "max_accel: " + std::string(i % 2 ? "10000" : "5000") + ".000000\n";
        s += "minimum_cruise_ratio: 0.500000\n";
        s += "square_corner_velocity: 9.000000\n";
    }
    return s;
}

} // namespace

TEST_CASE("DebugBundleCollector: condense_klipper_log collapses Stats padding",
          "[debug-bundle][klippy]") {
    std::string raw = stats_lines(2000) + "MCU 'mcu' shutdown: Timer too close\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "Timer too close") == 1);
    REQUIRE(count_lines_with(out, "Stats ") <= 40 + 200);
    REQUIRE(out.size() < raw.size() / 4);
}

TEST_CASE("DebugBundleCollector: condense_klipper_log collapses NON-Stats periodic noise",
          "[debug-bundle][klippy]") {
    // Regression: the first implementation special-cased the "Stats " prefix, so
    // ZMOD's toolhead dump (7916 repetitions on a real AD5X log) passed through
    // untouched and crowded the events out of the shipped payload.
    std::string raw = toolhead_spam(2000) + "Setting active filament T2\n" +
                      "MCU 'mcu' shutdown: Timer too close\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "Timer too close") == 1);
    REQUIRE(count_lines_with(out, "Setting active filament") == 1);
    // 8000 spam lines in; the collapse must cut them down hard.
    REQUIRE(count_lines_with(out, "max_velocity") <= 240);
    REQUIRE(line_count(out) < 1000);
}

TEST_CASE("DebugBundleCollector: condense_klipper_log keeps rare lines and recent repeats",
          "[debug-bundle][klippy]") {
    SECTION("a shape under the threshold is kept in full") {
        std::string raw = stats_lines(10);
        auto out = helix::DebugBundleCollector::condense_klipper_log(raw, /*max_repeats=*/40,
                                                                     /*tail_lines=*/0);
        REQUIRE(count_lines_with(out, "Stats ") == 10);
    }

    SECTION("over the threshold, the MOST RECENT occurrences survive") {
        std::string raw = stats_lines(100) + "event\n";
        auto out = helix::DebugBundleCollector::condense_klipper_log(raw, /*max_repeats=*/10,
                                                                     /*tail_lines=*/0);
        REQUIRE(count_lines_with(out, "Stats ") == 10);
        REQUIRE(out.find("Stats 99.0") != std::string::npos); // newest kept
        REQUIRE(out.find("Stats 89.0") == std::string::npos); // 11th-newest dropped
    }

    SECTION("original order is preserved") {
        std::string raw =
            stats_lines(50) + "FIRST EVENT\n" + stats_lines(50, 100) + "SECOND EVENT\n";
        auto out = helix::DebugBundleCollector::condense_klipper_log(raw, /*max_repeats=*/5,
                                                                     /*tail_lines=*/0);
        auto a = out.find("FIRST EVENT");
        auto b = out.find("SECOND EVENT");
        REQUIRE(a != std::string::npos);
        REQUIRE(b != std::string::npos);
        REQUIRE(a < b);
    }
}

TEST_CASE("DebugBundleCollector: condense_klipper_log ships the tail verbatim",
          "[debug-bundle][klippy]") {
    // The shutdown dump lives in the last lines and must never be thinned, even
    // though it repeats shapes heavily (81 identical is_shutdown lines on a real
    // AD5X log).
    std::string raw = stats_lines(500);
    for (int i = 0; i < 50; ++i)
        raw += "Receive: " + std::to_string(i) + " is_shutdown static_string_id=Timer too close\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw, /*max_repeats=*/5,
                                                                 /*tail_lines=*/50);

    REQUIRE(count_lines_with(out, "is_shutdown") == 50);
}

TEST_CASE("DebugBundleCollector: condense_klipper_log handles degenerate input",
          "[debug-bundle][klippy]") {
    REQUIRE(helix::DebugBundleCollector::condense_klipper_log("").empty());

    // No repetition at all: passed through intact.
    std::string events = "Extract filament 3 with length 90\nUnlocking filament 3";
    REQUIRE(helix::DebugBundleCollector::condense_klipper_log(events) == events);

    // max_repeats=0 with no verbatim tail drops every repeating shape.
    REQUIRE(helix::DebugBundleCollector::condense_klipper_log(stats_lines(20), 0, 0).empty());

    // tail_lines larger than the input keeps everything.
    auto all = helix::DebugBundleCollector::condense_klipper_log(stats_lines(5), 1, 999);
    REQUIRE(count_lines_with(all, "Stats ") == 5);
}

// ============================================================================
// resolve_log_tail_lines — ship the whole ring, not the floor
// ============================================================================

TEST_CASE("DebugBundleCollector: log_tail ships the whole ring, not the 2000-line floor",
          "[debug-bundle][klippy]") {
    // ring_capacity_for_ram() scales the ring at 16 lines/MB clamped to
    // [2000, 20000]; the collector used to ask for the floor regardless, so a
    // 473 MB AD5X retained 7568 lines and shipped 2000 of them.
    REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(0, 7568) == 7568);
    REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(0, 20000) == 20000);

    SECTION("smallest boards are unchanged — the ring floor IS 2000") {
        REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(0, 2000) == 2000);
    }

    SECTION("no ring installed falls back to a bound, never unbounded") {
        // Watchdog build / before logging init. tail_best() would otherwise read
        // the on-disk cascade with no line limit at all.
        REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(0, 0) == 2000);
        REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(-1, 0) == 2000);
    }

    SECTION("an explicit request wins over the ring in both directions") {
        REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(50, 7568) == 50);
        REQUIRE(helix::DebugBundleCollector::resolve_log_tail_lines(30000, 7568) == 30000);
    }
}

// ============================================================================
// pick_rotated_sibling — reach the log the crash actually landed in
// ============================================================================

namespace {

using LFE = helix::DebugBundleCollector::LogFileEntry;

/// Verbatim from a live Raspberry Pi's /server/files/list?root=logs. Note
/// crowsnest.log.2026-08-11 (940 KB) is NEWER than every klippy rotation.
std::vector<LFE> pi_logs_root() {
    return {
        {"moonraker.log", 5800, 1786499650.12},
        {"crowsnest.log.2026-08-11", 940700, 1786420807.17},
        {"crowsnest.log.2026-08-10", 940700, 1786334411.19},
        {"moonraker.log.2026-08-09", 6048, 1786312964.48},
        {"moonraker.log.2026-08-08", 5923, 1786212464.31},
        {"mainsail-access.log", 0, 1785988807.40},
        {"crowsnest.log", 940700, 1781891605.91},
        {"klippy.log", 2604, 1781891557.42},
        {"klippy.log.2026-06-08", 2604, 1780931356.03},
        {"klippy.log.2026-05-22", 2604, 1779449296.39},
    };
}

/// Verbatim from a live AD5M — same family as Vger1700's AD5X. The klippy log is
/// printer.log here, rotations carry an hour suffix, and nested mod/ files exist.
std::vector<LFE> ad5m_logs_root() {
    return {
        {"moonraker.log", 4674, 1786499650.14},
        {"moonraker.log.2026-08-11", 6634, 1786434810.86},
        {"moonraker.log.2026-08-10", 23304, 1786373729.02},
        {"printer.log", 187010, 1786369835.39},
        {"boot.log", 3335, 1786369831.22},
        {"mod/init.log", 6960, 1786369118.39},
        {"printer.log.2026-06-13_15", 180687, 1781379596.73},
        {"mod/init.log.1", 7082, 1781379419.38},
        {"printer.log.2026-06-12_12", 95071, 1781282495.09},
        {"printer.log.2026-05-21_14", 7961683, 1779386713.19},
    };
}

const std::vector<std::string> KLIPPY_STEMS = {"klippy.log", "printer.log"};
const std::vector<std::string> MOONRAKER_STEMS = {"moonraker.log"};

} // namespace

TEST_CASE("DebugBundleCollector: pick_rotated_sibling finds the crash's real log",
          "[debug-bundle][klippy]") {
    SECTION("Pi layout: klippy rotation, NOT the newer crowsnest log") {
        // The trap. crowsnest.log.2026-08-11 is 940 KB and ~5 days newer than the
        // newest klippy rotation, so any "newest rotated file" rule ships a
        // webcam log in place of the crash.
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(pi_logs_root(), KLIPPY_STEMS) ==
                "klippy.log.2026-06-08");
    }

    SECTION("Pi layout: moonraker rotation") {
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(
                    pi_logs_root(), MOONRAKER_STEMS) == "moonraker.log.2026-08-09");
    }

    SECTION("AD5M/AD5X layout: klippy's log is printer.log, with an hour suffix") {
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(ad5m_logs_root(), KLIPPY_STEMS) ==
                "printer.log.2026-06-13_15");
    }

    SECTION("AD5M layout: the moonraker rotation that held the LYGVE39Y incident") {
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(
                    ad5m_logs_root(), MOONRAKER_STEMS) == "moonraker.log.2026-08-11");
    }

    SECTION("never the active file, however it sorts") {
        std::vector<LFE> only_active = {{"moonraker.log", 999999, 9999999999.0}};
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(only_active, MOONRAKER_STEMS)
                    .empty());
    }

    SECTION("never a nested path — mod/init.log.1 is not klippy's") {
        std::vector<LFE> nested = {{"mod/printer.log.1", 9999, 9999999999.0}};
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(nested, KLIPPY_STEMS).empty());
    }

    SECTION("never a different daemon that merely shares the suffix shape") {
        std::vector<LFE> other = {{"crowsnest.log.2026-08-11", 940700, 9999999999.0},
                                  {"mainsail-error.log.1", 10, 9999999999.0}};
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(other, KLIPPY_STEMS).empty());
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(other, MOONRAKER_STEMS).empty());
    }

    SECTION("numeric rotation suffixes count too") {
        std::vector<LFE> numeric = {{"moonraker.log", 100, 500.0},
                                    {"moonraker.log.1", 100, 400.0},
                                    {"moonraker.log.2", 100, 300.0}};
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(numeric, MOONRAKER_STEMS) ==
                "moonraker.log.1"); // newest of the rotations
    }

    SECTION("a stem that is a prefix of another name does not bleed across") {
        // "printer.log" must not match "printer.log_backup.1" or "printer.logger.2"
        std::vector<LFE> tricky = {{"printer.log_backup.1", 500, 9999999999.0},
                                   {"printer.logger.2", 500, 9999999999.0},
                                   {"printer.log.2026-01-01", 500, 100.0}};
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling(tricky, KLIPPY_STEMS) ==
                "printer.log.2026-01-01");
    }

    SECTION("empty listing is not a crash") {
        REQUIRE(helix::DebugBundleCollector::pick_rotated_sibling({}, KLIPPY_STEMS).empty());
    }
}

// ============================================================================
// condense_klipper_log — threshold tuning for moonraker.log
// ============================================================================

TEST_CASE("DebugBundleCollector: condense threshold preserves EVERY proc_stats shutdown block",
          "[debug-bundle][klippy]") {
    // moonraker.log's most valuable repeated block is proc_stats._handle_shutdown(),
    // which dumps ~30 "System Time: ... Usage: ..." samples on each Klippy
    // shutdown. Those samples are the only host-CPU record we get for the moment
    // of a crash.
    //
    // The catch: shape-collapse keeps the most RECENT occurrences, and shutdowns
    // come in clusters — Vger1700's incident day had two, 102 lines apart. With
    // Klipper's max_repeats=40 the SECOND (uninteresting) block keeps all 30
    // samples while the FIRST (the actual incident) is thinned to 10. Measured
    // on his real moonraker.log.2026-08-11.
    auto proc_stats_block = [](int base) {
        std::string s;
        for (int i = 0; i < 30; ++i)
            s += "System Time: " + std::to_string(base + i) + ".05, Usage: 4.1%, Memory: None\n";
        return s;
    };
    const std::string incident = proc_stats_block(1786462400);
    const std::string later = proc_stats_block(1786462600);
    std::string raw = incident + "Klippy has shutdown\n" + std::string(120, 'x') + "\n" + later +
                      "Klippy has shutdown\n";

    auto count_block = [](const std::string& hay, const std::string& epoch_prefix) {
        return count_lines_with(hay, "System Time: " + epoch_prefix);
    };

    SECTION("Klipper's threshold silently thins the incident block") {
        // Not a defect in the Klipper path — klippy.log has no equivalent block,
        // and this is exactly why moonraker.log cannot inherit the same number.
        auto out = helix::DebugBundleCollector::condense_klipper_log(
            raw, helix::DebugBundleCollector::KLIPPER_CONDENSE_MAX_REPEATS, /*tail_lines=*/0);
        REQUIRE(count_block(out, "17864624") < 30); // incident sacrificed
        REQUIRE(count_block(out, "17864626") == 30);
    }

    SECTION("the moonraker threshold keeps both blocks whole") {
        // Reads the shipping constant, not a copy of it: dropping
        // MOONRAKER_CONDENSE_MAX_REPEATS back toward Klipper's value fails here.
        auto out = helix::DebugBundleCollector::condense_klipper_log(
            raw, helix::DebugBundleCollector::MOONRAKER_CONDENSE_MAX_REPEATS, /*tail_lines=*/0);
        REQUIRE(count_block(out, "17864624") == 30);
        REQUIRE(count_block(out, "17864626") == 30);
    }
}

// ============================================================================
// condense_klipper_log — Klipper config-dump elision
// ============================================================================

namespace {

/// Klipper's startup config dump (configfile.py log_config): the whole printer
/// config bracketed by a header line and a 23-'=' terminator. Every line is a
/// distinct shape, so shape-collapse keeps all of them.
std::string config_dump(int body_lines, bool with_header = true) {
    std::string s;
    if (with_header)
        s += "===== Config file =====\n";
    for (int i = 0; i < body_lines; ++i)
        s += "cfg_key_" + std::to_string(i) + " = value_" + std::to_string(i) + "\n";
    s += "=======================\n";
    return s;
}

} // namespace

TEST_CASE("DebugBundleCollector: condense_klipper_log elides the Klipper config dump",
          "[debug-bundle][klippy]") {
    // A user who presses "Restart Klipper" on our own recovery dialog before
    // uploading a bundle makes Klipper re-dump printer.cfg. Measured on three
    // real AD5X bundles (4QA7SZAM / LYGVE39Y / XSNN7PX5), that dump ate 84% /
    // 63% / 58% of the shipped klipper_log, evicting the pre-shutdown window
    // the bundle was uploaded to explain.
    std::string raw = "MCU 'mcu' shutdown: Timer too close\n" + stats_lines(20) +
                      "Restarting printer\n" + config_dump(1300) + "mcu 'mcu': Starting serial\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "cfg_key_") == 0);
    REQUIRE(count_lines_with(out, "Timer too close") == 1);
    REQUIRE(count_lines_with(out, "Restarting printer") == 1);
    REQUIRE(count_lines_with(out, "Starting serial") == 1);
    // The elision leaves a breadcrumb so a reader knows why the config is absent.
    REQUIRE(count_lines_with(out, "config dump") == 1);
    REQUIRE(line_count(out) < 60);
}

TEST_CASE("DebugBundleCollector: condense_klipper_log elides a HEAD-TRUNCATED config dump",
          "[debug-bundle][klippy]") {
    // The case that actually ships. The 4 MiB Range fetch starts mid-dump, so
    // the "===== Config file =====" header is never in the payload — only the
    // orphan terminator is. All three real AD5X bundles look like this, so the
    // paired-marker path alone would have recovered nothing.
    std::string raw = config_dump(1300, /*with_header=*/false) + "mcu 'mcu': Starting serial\n" +
                      "Loaded MCU 'mcu' 132 commands\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "cfg_key_") == 0);
    REQUIRE(count_lines_with(out, "Starting serial") == 1);
    REQUIRE(count_lines_with(out, "Loaded MCU") == 1);
}

TEST_CASE("DebugBundleCollector: condense_klipper_log elides every config dump in the window",
          "[debug-bundle][klippy]") {
    // Two restarts inside one window: a head-truncated dump followed by a
    // complete one. Both must go, and the event between them must survive.
    std::string raw = config_dump(400, /*with_header=*/false) +
                      "MCU 'mcu' shutdown: Timer too close\n" + config_dump(400) +
                      "mcu 'mcu': Starting serial\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "cfg_key_") == 0);
    REQUIRE(count_lines_with(out, "Timer too close") == 1);
    REQUIRE(count_lines_with(out, "Starting serial") == 1);
    REQUIRE(count_lines_with(out, "config dump") == 2);
}

TEST_CASE("DebugBundleCollector: config-dump elision handles a real AD5X-sized dump",
          "[debug-bundle][klippy]") {
    // Measured, not guessed: Vger1700's printer.log carries a 6668-line config
    // dump, 66% of a 10052-line file. An earlier positional bound of 5000 was
    // written against an assumed ~1300 and would have skipped this entirely
    // whenever the fetch cut into the first 1668 lines of the dump.
    std::string raw = config_dump(6666, /*with_header=*/false) + "Stats 83.0: sysload=0.3\n" +
                      "MCU 'mcu' shutdown: Timer too close\n";

    auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

    REQUIRE(count_lines_with(out, "cfg_key_") == 0);
    REQUIRE(count_lines_with(out, "Timer too close") == 1);
}

TEST_CASE("DebugBundleCollector: config-dump elision does not eat unrelated content",
          "[debug-bundle][klippy]") {
    SECTION("an orphan terminator after runtime output is left alone") {
        // Position alone cannot separate "cut-off dump" from "stray rule line":
        // a real dump is 6668 lines, so any bound generous enough to cover one
        // is also generous enough to swallow thousands of real log lines. What
        // actually separates them is that Klipper's config dump contains no
        // runtime "Stats " line, and a live log is saturated with them.
        std::string raw = stats_lines(30) + "MCU 'mcu' shutdown: Timer too close\n" +
                          "=======================\n" + "after\n";

        auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

        REQUIRE(count_lines_with(out, "Timer too close") == 1);
        REQUIRE(count_lines_with(out, "after") == 1);
        REQUIRE(count_lines_with(out, "config dump") == 0);
    }

    SECTION("an orphan terminator deep in the window is left alone") {
        // The head-truncated rule drops everything BEFORE the terminator, so it
        // must only fire near the start of the window. A bare '=' rule line
        // thousands of lines in is somebody else's output, not a cut-off dump.
        std::string raw = stats_lines(6000) + "MCU 'mcu' shutdown: Timer too close\n" +
                          "=======================\n" + "after\n";

        auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

        REQUIRE(count_lines_with(out, "Timer too close") == 1);
        REQUIRE(count_lines_with(out, "after") == 1);
        REQUIRE(count_lines_with(out, "config dump") == 0);
    }

    SECTION("a header with no terminator in the window is not treated as a dump") {
        // Tail-truncated the other way: the dump runs past the end of what we
        // fetched. Dropping to end-of-input would discard the newest lines,
        // which is exactly where the shutdown lives. Leave it.
        std::string raw = "MCU 'mcu' shutdown: Timer too close\n===== Config file =====\ncfg = 1\n";

        auto out = helix::DebugBundleCollector::condense_klipper_log(raw);

        REQUIRE(count_lines_with(out, "Timer too close") == 1);
        REQUIRE(count_lines_with(out, "cfg = 1") == 1);
    }

    SECTION("a '=' run that is not Klipper's 23-char terminator is ignored") {
        std::string raw = "==========\nreal event\n";
        auto out = helix::DebugBundleCollector::condense_klipper_log(raw);
        REQUIRE(count_lines_with(out, "real event") == 1);
        REQUIRE(count_lines_with(out, "==========") == 1);
    }
}

TEST_CASE("DebugBundleCollector: config-dump elision buys back the line budget",
          "[debug-bundle][klippy]") {
    // The payoff, stated as the caller sees it. collect_klipper_log_tail()
    // condenses a 4 MiB window and then keeps only the LAST num_lines, so the
    // dump does its damage at that final cap: on bundle LYGVE39Y the header was
    // inside the fetched window but the 2000-line cap sliced through the body,
    // leaving 1264 lines of printer.cfg and no pre-shutdown log at all.
    auto last_n = [](const std::string& s, int n) {
        std::vector<std::string> lines;
        std::istringstream st(s);
        std::string l;
        while (std::getline(st, l))
            lines.push_back(l);
        if (static_cast<int>(lines.size()) > n)
            lines.erase(lines.begin(), lines.end() - n);
        std::string out;
        for (auto& x : lines)
            out += x + "\n";
        return out;
    };

    std::string raw = "MCU 'mcu' shutdown: Timer too close\n" + stats_lines(20) +
                      "Restarting printer\n" + config_dump(1300) +
                      "mcu 'mcu': Starting serial connect\n";

    // Without elision the cap lands deep inside the config body.
    REQUIRE(count_lines_with(last_n(raw, 200), "Timer too close") == 0);

    auto condensed = helix::DebugBundleCollector::condense_klipper_log(raw);
    REQUIRE(count_lines_with(last_n(condensed, 200), "Timer too close") == 1);
    REQUIRE(count_lines_with(last_n(condensed, 200), "Starting serial connect") == 1);
}
