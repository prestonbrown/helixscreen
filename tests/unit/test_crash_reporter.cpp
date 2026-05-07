// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_crash_reporter.cpp
 * @brief TDD tests for CrashReporter singleton - crash report collection,
 *        formatting, GitHub URL generation, and file lifecycle.
 *
 * Written TDD-style before implementation. Tests WILL FAIL if CrashReporter
 * is removed or its interface changes.
 */

#include "system/crash_history.h"
#include "system/crash_reporter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Fixture: isolated temp directory with singleton reset
// ============================================================================

// Friend-class access pattern (L065): keep test-only knobs out of the
// production CrashReporter API.
class CrashReporterTestAccess {
  public:
    static void set_isolated_log_paths(CrashReporter& cr, bool v) {
        cr.isolated_log_paths_ = v;
    }
};

class CrashReporterTestFixture {
  public:
    CrashReporterTestFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_crash_reporter_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);

        // Reset singleton
        auto& cr = CrashReporter::instance();
        cr.shutdown();
        cr.init(temp_dir_.string());
        // Prevent dev-host system logs (journalctl, /var/log) from bleeding
        // into assertions that expect an empty log tail.
        CrashReporterTestAccess::set_isolated_log_paths(cr, true);
    }

    ~CrashReporterTestFixture() {
        CrashReporter::instance().shutdown();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    void write_crash_file(int signal = 11, const std::string& name = "SIGSEGV",
                          const std::string& version = "0.9.9",
                          const std::vector<std::string>& bt = {"0x400abc", "0x400def"}) {
        std::string crash_path = (temp_dir_ / "crash.txt").string();
        std::ofstream ofs(crash_path);
        ofs << "signal:" << signal << "\n";
        ofs << "name:" << name << "\n";
        ofs << "version:" << version << "\n";
        ofs << "timestamp:1707350400\n";
        ofs << "uptime:3600\n";
        for (const auto& addr : bt) {
            ofs << "bt:" << addr << "\n";
        }
    }

    void write_log_file(const std::string& content, const std::string& filename = "helix.log") {
        std::ofstream ofs((temp_dir_ / filename).string());
        ofs << content;
    }

    fs::path temp_dir_;
};

// ============================================================================
// Detection Tests [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: has_crash_report returns true when crash.txt exists",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    REQUIRE(cr.has_crash_report());
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: has_crash_report returns false when no crash.txt",
                 "[crash_reporter]") {
    auto& cr = CrashReporter::instance();
    REQUIRE_FALSE(cr.has_crash_report());
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: has_crash_report returns false after consume_crash_file",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    REQUIRE(cr.has_crash_report());

    cr.consume_crash_file();
    REQUIRE_FALSE(cr.has_crash_report());
}

// ============================================================================
// Report Collection [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report parses signal from crash.txt", "[crash_reporter]") {
    write_crash_file(6, "SIGABRT");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.signal == 6);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: collect_report parses signal name",
                 "[crash_reporter]") {
    write_crash_file(11, "SIGSEGV");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.signal_name == "SIGSEGV");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report parses backtrace addresses", "[crash_reporter]") {
    std::vector<std::string> bt = {"0x400abc", "0x400def", "0x401000"};
    write_crash_file(11, "SIGSEGV", "0.9.9", bt);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    REQUIRE(report.backtrace.size() == 3);
    REQUIRE(report.backtrace[0] == "0x400abc");
    REQUIRE(report.backtrace[1] == "0x400def");
    REQUIRE(report.backtrace[2] == "0x401000");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report handles missing optional fields gracefully",
                 "[crash_reporter]") {
    // Write a minimal crash file with only required fields
    std::ofstream ofs((temp_dir_ / "crash.txt").string());
    ofs << "signal:11\n";
    ofs << "name:SIGSEGV\n";
    ofs << "version:0.9.9\n";
    ofs << "timestamp:1707350400\n";
    ofs << "uptime:3600\n";
    ofs.close();

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    // Required fields parsed correctly
    REQUIRE(report.signal == 11);
    REQUIRE(report.signal_name == "SIGSEGV");
    REQUIRE(report.app_version == "0.9.9");

    // Optional fields have sensible defaults
    REQUIRE(report.backtrace.empty());
    REQUIRE(report.printer_model.empty());
    REQUIRE(report.klipper_version.empty());
}

// Regression: bundle CHUQCNAE (2026-05-05, AD5X v0.99.53) shipped a crash.txt
// containing only "signal:\n" — the signal handler was killed mid-write before
// the signum or any other field landed. Application::run still showed the
// crash dialog, the user clicked Send, and the resulting bundle had no
// actionable data. read_crash_file() returns null in this case (signal_name
// missing), so collect_report() yields a default-constructed CrashReport with
// signal_name == "". The Application::run gate keys on that to suppress the
// dialog; this test pins the contract.
TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report yields empty signal_name for truncated crash file",
                 "[crash_reporter]") {
    std::ofstream ofs((temp_dir_ / "crash.txt").string());
    ofs << "signal:";
    ofs.close();

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    REQUIRE(report.signal_name.empty());
    REQUIRE(report.signal == 0);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: collect_report includes platform key",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    // Platform should be detected at runtime (e.g. "linux-arm64", "darwin-x86_64")
    REQUIRE_FALSE(report.platform.empty());
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report includes RAM and CPU info", "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    // RAM and CPU should be non-negative (0 is acceptable if detection fails)
    REQUIRE(report.ram_total_mb >= 0);
    REQUIRE(report.cpu_cores >= 0);
}

// ============================================================================
// Log Tail [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: get_log_tail returns last 50 lines",
                 "[crash_reporter]") {
    // Write 100 lines
    std::string content;
    for (int i = 1; i <= 100; i++) {
        content += "line " + std::to_string(i) + "\n";
    }
    write_log_file(content);

    auto& cr = CrashReporter::instance();
    std::string tail = cr.get_log_tail(50);

    // Should contain line 51 through line 100
    REQUIRE(tail.find("line 51") != std::string::npos);
    REQUIRE(tail.find("line 100") != std::string::npos);
    // Should NOT contain line 1
    REQUIRE(tail.find("line 1\n") == std::string::npos);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: get_log_tail handles files shorter than 50 lines",
                 "[crash_reporter]") {
    std::string content;
    for (int i = 1; i <= 10; i++) {
        content += "short line " + std::to_string(i) + "\n";
    }
    write_log_file(content);

    auto& cr = CrashReporter::instance();
    std::string tail = cr.get_log_tail(50);

    // Should contain all 10 lines
    REQUIRE(tail.find("short line 1") != std::string::npos);
    REQUIRE(tail.find("short line 10") != std::string::npos);
}

// Missing-file / empty-file fallback is now handled by helix::logs::tail_best,
// which cascades file → syslog → journal. Hermetic coverage of the file-source
// contract lives in test_log_collector.cpp; this layer can't meaningfully
// assert empty when syslog/journal are real system sources.

// Crash report #827: the new session that reports a crash writes its own boot
// lines to syslog/journal before collecting the log tail. A small tail (50
// lines) ended up showing only post-crash startup noise, with the pre-crash
// context pushed off the top. The fix: collect a larger buffer and filter
// lines whose timestamp is after the crash time. These tests pin the
// filter's contract — pre-crash kept, post-crash dropped, unparseable kept.
TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report filters post-crash lines from log tail",
                 "[crash_reporter][log_tail]") {
    // Crash file uses unix epoch 1707350400 → ISO 2024-02-08T00:00:00Z
    write_crash_file();
    // File-log format: "[YYYY-MM-DD HH:MM:SS..." — parsed via mktime (local time)
    // but 2020 vs 2030 is unambiguous regardless of timezone.
    std::string content;
    content += "[2020-01-01 00:00:00.000] [info] pre_crash_early\n";
    content += "[2024-02-07 12:00:00.000] [info] pre_crash_recent\n";
    content += "a log line without any parseable timestamp\n";
    content += "[2030-12-31 00:00:00.000] [info] post_crash_noise\n";
    content += "[2030-12-31 00:00:01.000] [info] post_crash_more_noise\n";
    write_log_file(content);

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    REQUIRE(report.log_tail.find("pre_crash_early") != std::string::npos);
    REQUIRE(report.log_tail.find("pre_crash_recent") != std::string::npos);
    REQUIRE(report.log_tail.find("without any parseable timestamp") != std::string::npos);
    REQUIRE(report.log_tail.find("post_crash_noise") == std::string::npos);
    REQUIRE(report.log_tail.find("post_crash_more_noise") == std::string::npos);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report keeps full tail when filter would drop everything",
                 "[crash_reporter][log_tail]") {
    // Crash time is 2024-02-08, but all log lines are stamped AFTER. Rather
    // than return empty, the collector falls back to the unfiltered tail so
    // triage always sees something.
    write_crash_file();
    std::string content;
    content += "[2030-12-31 00:00:00.000] [info] only_post_crash_1\n";
    content += "[2030-12-31 00:00:01.000] [info] only_post_crash_2\n";
    write_log_file(content);

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    REQUIRE(report.log_tail.find("only_post_crash_1") != std::string::npos);
    REQUIRE(report.log_tail.find("only_post_crash_2") != std::string::npos);
}

// ============================================================================
// Report Formatting [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json includes all required fields", "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("signal"));
    REQUIRE(j.contains("signal_name"));
    REQUIRE(j.contains("app_version"));
    REQUIRE(j.contains("timestamp"));
    REQUIRE(j.contains("uptime_seconds"));
    REQUIRE(j.contains("backtrace"));
    REQUIRE(j.contains("platform"));
    REQUIRE(j.contains("display_backend"));
    REQUIRE(j.contains("ram_mb"));
    REQUIRE(j.contains("cpu_cores"));
    REQUIRE(j.contains("printer_model"));
    REQUIRE(j.contains("klipper_version"));
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json log_tail is array of lines", "[crash_reporter]") {
    write_crash_file();
    write_log_file("line one\nline two\nline three\n");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("log_tail"));
    REQUIRE(j["log_tail"].is_array());
    REQUIRE(j["log_tail"].size() == 3);
    REQUIRE(j["log_tail"][0] == "line one");
    REQUIRE(j["log_tail"][1] == "line two");
    REQUIRE(j["log_tail"][2] == "line three");
}

// Empty-log_tail omission is now dependent on helix::logs::tail_best — when
// syslog/journal are real system sources they may supply content. File-source
// contract is covered by test_log_collector.cpp.

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json round-trips debug_bundle_share_code",
                 "[crash_reporter][bundle]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    report.debug_bundle_share_code = "ZYZCAT4L";
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("debug_bundle_share_code"));
    REQUIRE(j["debug_bundle_share_code"] == "ZYZCAT4L");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json omits debug_bundle_share_code when empty",
                 "[crash_reporter][bundle]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    // Explicitly leave share_code empty — the worker should then render no
    // bundle link (and the client must not send a falsy field either).
    REQUIRE(report.debug_bundle_share_code.empty());
    json j = cr.report_to_json(report);
    REQUIRE_FALSE(j.contains("debug_bundle_share_code"));
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json backtrace is array of strings",
                 "[crash_reporter]") {
    write_crash_file(11, "SIGSEGV", "0.9.9", {"0xaaa", "0xbbb"});
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j["backtrace"].is_array());
    REQUIRE(j["backtrace"].size() == 2);
    REQUIRE(j["backtrace"][0].is_string());
    REQUIRE(j["backtrace"][1].is_string());
    REQUIRE(j["backtrace"][0] == "0xaaa");
    REQUIRE(j["backtrace"][1] == "0xbbb");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_text is human-readable with signal info",
                 "[crash_reporter]") {
    write_crash_file(11, "SIGSEGV", "1.0.0");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string text = cr.report_to_text(report);

    REQUIRE(text.find("SIGSEGV") != std::string::npos);
    REQUIRE(text.find("11") != std::string::npos);
    REQUIRE(text.find("1.0.0") != std::string::npos);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: report_to_text includes section headers",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string text = cr.report_to_text(report);

    // Should have identifiable sections for readability
    // Check for common section markers (e.g. headers, separators)
    REQUIRE(text.find("Signal") != std::string::npos);
    REQUIRE(text.find("Backtrace") != std::string::npos);
    REQUIRE(text.find("Version") != std::string::npos);
}

// ============================================================================
// GitHub URL Generation [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: generate_github_url produces valid URL",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string url = cr.generate_github_url(report);

    REQUIRE(url.find("https://github.com") == 0);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: generate_github_url stays under 2000 chars", "[crash_reporter]") {
    // Create a report with a large backtrace
    std::vector<std::string> large_bt;
    for (int i = 0; i < 100; i++) {
        large_bt.push_back("0x" + std::to_string(0x400000 + i * 0x100));
    }
    write_crash_file(11, "SIGSEGV", "0.9.9", large_bt);

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string url = cr.generate_github_url(report);

    // GitHub URLs over ~2000 chars get rejected by browsers
    REQUIRE(url.size() <= 2000);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: generate_github_url includes signal and version in title",
                 "[crash_reporter]") {
    write_crash_file(6, "SIGABRT", "1.2.3");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string url = cr.generate_github_url(report);

    // URL-encoded title should contain signal name and version
    REQUIRE(url.find("SIGABRT") != std::string::npos);
    REQUIRE(url.find("1.2.3") != std::string::npos);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: generate_github_url truncates long backtraces",
                 "[crash_reporter]") {
    // Create a report with a very large backtrace
    std::vector<std::string> huge_bt;
    for (int i = 0; i < 200; i++) {
        huge_bt.push_back("0x" + std::to_string(0x400000 + i));
    }
    write_crash_file(11, "SIGSEGV", "0.9.9", huge_bt);

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string url = cr.generate_github_url(report);

    // URL must be valid length even with huge backtrace
    REQUIRE(url.size() <= 2000);
    // Should still start with the GitHub URL
    REQUIRE(url.find("https://github.com") == 0);
}

// ============================================================================
// File Save [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: save_to_file creates crash_report.txt in config dir",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    cr.save_to_file(report);

    REQUIRE(fs::exists(temp_dir_ / "crash_report.txt"));
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: save_to_file content matches report_to_text", "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    std::string expected_text = cr.report_to_text(report);
    cr.save_to_file(report);

    // Read the saved file
    std::ifstream ifs((temp_dir_ / "crash_report.txt").string());
    std::string saved_content((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());

    REQUIRE(saved_content == expected_text);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: save_to_file returns true on success",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    REQUIRE(cr.save_to_file(report) == true);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: save_to_file returns false on bad path",
                 "[crash_reporter]") {
    // Re-init with a non-existent directory that cannot be created
    auto& cr = CrashReporter::instance();
    cr.shutdown();
    cr.init("/nonexistent/path/that/should/not/exist");

    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "0.9.9";

    REQUIRE(cr.save_to_file(report) == false);

    // Re-init back to temp dir for fixture cleanup
    cr.shutdown();
    cr.init(temp_dir_.string());
}

// ============================================================================
// Singleton Lifecycle [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: init with config_dir sets crash file path", "[crash_reporter]") {
    // Write crash file and verify it is detected via the configured path
    write_crash_file();
    auto& cr = CrashReporter::instance();
    REQUIRE(cr.has_crash_report());
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: re-init resets state cleanly",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    REQUIRE(cr.has_crash_report());

    // Create a second temp directory
    fs::path temp_dir_2 =
        fs::temp_directory_path() /
        ("helix_crash_reporter_test2_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp_dir_2);

    // Re-init with new dir (no crash file there)
    cr.shutdown();
    cr.init(temp_dir_2.string());
    REQUIRE_FALSE(cr.has_crash_report());

    // Cleanup second temp dir
    cr.shutdown();
    std::error_code ec;
    fs::remove_all(temp_dir_2, ec);

    // Restore original init for fixture teardown
    cr.init(temp_dir_.string());
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: shutdown clears state",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    REQUIRE(cr.has_crash_report());

    cr.shutdown();

    // After shutdown, has_crash_report should return false (no config dir set)
    REQUIRE_FALSE(cr.has_crash_report());

    // Re-init for fixture teardown
    cr.init(temp_dir_.string());
}

// ============================================================================
// Fingerprint & Deduplication [crash_reporter]
// ============================================================================

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: fingerprint matches server-side formula",
                 "[crash_reporter]") {
    write_crash_file(11, "SIGSEGV", "0.9.9", {"0x400abc", "0x400def"});
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string fp = CrashReporter::fingerprint(report);
    REQUIRE(fp == "SIGSEGV/0.9.9/0x400abc");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: fingerprint uses no-bt when backtrace is empty",
                 "[crash_reporter]") {
    // Write crash file with no backtrace
    std::ofstream ofs((temp_dir_ / "crash.txt").string());
    ofs << "signal:11\nname:SIGSEGV\nversion:1.0.0\ntimestamp:1707350400\nuptime:3600\n";
    ofs.close();

    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string fp = CrashReporter::fingerprint(report);
    REQUIRE(fp == "SIGSEGV/1.0.0/no-bt");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: is_duplicate returns false with empty history",
                 "[crash_reporter]") {
    write_crash_file();
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    // Initialize CrashHistory to a fresh temp dir
    helix::CrashHistory::instance().shutdown();
    helix::CrashHistory::instance().init(temp_dir_.string());

    REQUIRE_FALSE(cr.is_duplicate(report));

    helix::CrashHistory::instance().shutdown();
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: is_duplicate returns true when fingerprint matches history",
                 "[crash_reporter]") {
    write_crash_file(11, "SIGSEGV", "0.9.9", {"0x400abc", "0x400def"});
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();

    helix::CrashHistory::instance().shutdown();
    helix::CrashHistory::instance().init(temp_dir_.string());

    // Add matching entry to history
    helix::CrashHistoryEntry entry;
    entry.fingerprint = "SIGSEGV/0.9.9/0x400abc";
    entry.signal_name = "SIGSEGV";
    entry.app_version = "0.9.9";
    entry.sent_via = "crash_reporter";
    helix::CrashHistory::instance().add_entry(entry);

    REQUIRE(cr.is_duplicate(report));

    helix::CrashHistory::instance().shutdown();
}

// ============================================================================
// Phase 2: Fault Info & Register State in CrashReport [crash_reporter]
// ============================================================================

/// Helper to write a V2 crash file with fault/register fields
void write_crash_file_v2(const fs::path& dir, int signal = 11, const std::string& name = "SIGSEGV",
                         const std::string& version = "0.9.18",
                         const std::string& fault_addr = "0x00000000", int fault_code = 1,
                         const std::string& fault_code_name = "SEGV_MAPERR",
                         const std::vector<std::string>& bt = {"0x920bac", "0xf7101290"}) {
    std::string crash_path = (dir / "crash.txt").string();
    std::ofstream ofs(crash_path);
    ofs << "signal:" << signal << "\n";
    ofs << "name:" << name << "\n";
    ofs << "version:" << version << "\n";
    ofs << "timestamp:1707350400\n";
    ofs << "uptime:3174\n";
    if (!fault_addr.empty())
        ofs << "fault_addr:" << fault_addr << "\n";
    if (fault_code >= 0)
        ofs << "fault_code:" << fault_code << "\n";
    if (!fault_code_name.empty())
        ofs << "fault_code_name:" << fault_code_name << "\n";
    ofs << "reg_pc:0x00920bac\n";
    ofs << "reg_sp:0xbe8ff420\n";
    ofs << "reg_lr:0x0091a3c0\n";
    for (const auto& addr : bt) {
        ofs << "bt:" << addr << "\n";
    }
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: collect_report includes fault address",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.fault_addr == "0x00000000");
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: collect_report includes fault code info",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_, 11, "SIGSEGV", "0.9.18", "0xdeadbeef", 2, "SEGV_ACCERR");
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.fault_code == 2);
    REQUIRE(report.fault_code_name == "SEGV_ACCERR");
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: collect_report includes register state",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.reg_pc == "0x00920bac");
    REQUIRE(report.reg_sp == "0xbe8ff420");
    REQUIRE(report.reg_lr == "0x0091a3c0");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report handles old format without fault fields",
                 "[crash_reporter]") {
    write_crash_file(); // Old format without fault fields
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.signal == 11);
    REQUIRE(report.fault_addr.empty());
    REQUIRE(report.fault_code == 0);
    REQUIRE(report.fault_code_name.empty());
    REQUIRE(report.reg_pc.empty());
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json includes fault and register fields",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("fault_addr"));
    REQUIRE(j["fault_addr"] == "0x00000000");
    REQUIRE(j.contains("fault_code"));
    REQUIRE(j["fault_code"] == 1);
    REQUIRE(j.contains("fault_code_name"));
    REQUIRE(j["fault_code_name"] == "SEGV_MAPERR");
    REQUIRE(j.contains("registers"));
    REQUIRE(j["registers"].contains("pc"));
    REQUIRE(j["registers"]["pc"] == "0x00920bac");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json omits fault fields when absent",
                 "[crash_reporter]") {
    write_crash_file(); // Old format
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE_FALSE(j.contains("fault_addr"));
    REQUIRE_FALSE(j.contains("fault_code"));
    REQUIRE_FALSE(j.contains("registers"));
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_text includes fault and register info",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string text = cr.report_to_text(report);

    REQUIRE(text.find("SEGV_MAPERR") != std::string::npos);
    REQUIRE(text.find("0x00000000") != std::string::npos);
    REQUIRE(text.find("0x00920bac") != std::string::npos);
}

TEST_CASE_METHOD(CrashReporterTestFixture, "CrashReporter: generate_github_url includes fault info",
                 "[crash_reporter]") {
    write_crash_file_v2(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    std::string url = cr.generate_github_url(report);

    // URL should mention the fault type
    REQUIRE(url.find("SEGV_MAPERR") != std::string::npos);
    // Still under 2000 chars
    REQUIRE(url.size() <= 2000);
}

// ============================================================================
// Phase 3: Stack Dump, Extra Registers, Full Memory Map [crash_reporter]
// ============================================================================

/// Helper to write a V3 crash file with stack dump, extra registers, and memory map
void write_crash_file_v3(const fs::path& dir) {
    std::string crash_path = (dir / "crash.txt").string();
    std::ofstream ofs(crash_path);
    ofs << "signal:11\n";
    ofs << "name:SIGSEGV\n";
    ofs << "version:0.98.6\n";
    ofs << "timestamp:1710451217\n";
    ofs << "uptime:65\n";
    ofs << "fault_addr:0x62\n";
    ofs << "fault_code:1\n";
    ofs << "fault_code_name:SEGV_MAPERR\n";
    ofs << "reg_pc:0xb1e9b2ec\n";
    ofs << "reg_sp:0xb1c6b070\n";
    ofs << "reg_lr:0xb1ddd180\n";
    ofs << "reg_r0:0x00000000\n";
    ofs << "reg_r1:0x00000062\n";
    ofs << "reg_r4:0x009a1234\n";
    ofs << "reg_fp:0xb1c6b0a0\n";
    ofs << "load_base:0x0\n";
    ofs << "queue_callback:discovery_complete\n";
    ofs << "bt:0xb1e9b2ec\n";
    ofs << "bt:0xb1ddd180\n";
    ofs << "bt:0x14508a\n";
    ofs << "bt:0x52d990\n";
    ofs << "stack_base:0xb1c6b070\n";
    ofs << "stk:0x009a1234\n";
    ofs << "stk:0x00144bd0\n";
    ofs << "stk:0xb1ddd180\n";
    ofs << "stk:0x000be16c\n";
    ofs << "map:00010000-0057c000 r-xp 00000000 b3:02 1234 /opt/helixscreen/helix-screen\n";
    ofs << "map:0057c000-009e9000 rw-p 0056c000 b3:02 1234 /opt/helixscreen/helix-screen\n";
    ofs << "map:b1b6a000-b1f6a000 rw-p 00000000 00:00 0 [stack:1234]\n";
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report extracts stack_dump and stack_base",
                 "[crash_reporter]") {
    write_crash_file_v3(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE(report.stack_base == "0xb1c6b070");
    REQUIRE(report.stack_dump.size() == 4);
    REQUIRE(report.stack_dump[0] == "0x009a1234");
    REQUIRE(report.stack_dump[1] == "0x00144bd0");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: collect_report extracts extra ARM32 registers",
                 "[crash_reporter]") {
    write_crash_file_v3(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    REQUIRE_FALSE(report.extra_registers.empty());
    bool found_r0 = false;
    for (const auto& [name, value] : report.extra_registers) {
        if (name == "r0") {
            found_r0 = true;
            REQUIRE(value == "0x00000000");
        }
    }
    REQUIRE(found_r0);
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json includes stack_dump and extra_registers",
                 "[crash_reporter]") {
    write_crash_file_v3(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("stack_base"));
    REQUIRE(j["stack_base"] == "0xb1c6b070");
    REQUIRE(j.contains("stack_dump"));
    REQUIRE(j["stack_dump"].is_array());
    REQUIRE(j["stack_dump"].size() == 4);
    REQUIRE(j.contains("extra_registers"));
    REQUIRE(j["extra_registers"].contains("r0"));
    REQUIRE(j["extra_registers"]["r0"] == "0x00000000");
}

TEST_CASE_METHOD(CrashReporterTestFixture,
                 "CrashReporter: report_to_json includes all memory map lines not just r-xp",
                 "[crash_reporter]") {
    write_crash_file_v3(temp_dir_);
    auto& cr = CrashReporter::instance();
    auto report = cr.collect_report();
    json j = cr.report_to_json(report);

    REQUIRE(j.contains("memory_map"));
    REQUIRE(j["memory_map"].size() == 3);
}
