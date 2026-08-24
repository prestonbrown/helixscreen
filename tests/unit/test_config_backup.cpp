// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_constants.h"
#include "config_backup.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix::config_backup;

namespace {

/// RAII helper to create a temp directory and clean it up on destruction.
struct TmpDir {
    fs::path path;
    explicit TmpDir(const std::string& suffix) {
        path =
            fs::temp_directory_path() / ("helix_test_" + suffix + "_" + std::to_string(getpid()));
        fs::create_directories(path);
    }
    ~TmpDir() {
        fs::remove_all(path);
    }

    std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

// ─── write_backup_file ────────────────────────────────────────────────────────

TEST_CASE("write_backup_file copies source to backup atomically", "[config_backup]") {
    TmpDir dir("backup");
    std::string src = dir.file("source.json");
    std::string dst = dir.file("backup.json");
    write_file(src, R"({"key":"value"})");

    REQUIRE(write_backup_file(src, dst));
    REQUIRE(read_file(dst) == R"({"key":"value"})");

    // Tmp file should not linger
    REQUIRE_FALSE(fs::exists(dst + ".tmp"));
}

TEST_CASE("write_backup_file returns false when source does not exist", "[config_backup]") {
    TmpDir dir("nosrc");
    REQUIRE_FALSE(write_backup_file(dir.file("nonexistent"), dir.file("backup")));
}

TEST_CASE("write_backup_file creates parent directories for backup path", "[config_backup]") {
    TmpDir dir("mkdirs");
    std::string src = dir.file("source.txt");
    std::string dst = (dir.path / "sub" / "deep" / "backup.txt").string();
    write_file(src, "hello");

    REQUIRE(write_backup_file(src, dst));
    REQUIRE(read_file(dst) == "hello");
}

TEST_CASE("write_backup_file overwrites existing backup", "[config_backup]") {
    TmpDir dir("overwrite");
    std::string src = dir.file("source.txt");
    std::string dst = dir.file("backup.txt");

    write_file(src, "v1");
    REQUIRE(write_backup_file(src, dst));
    REQUIRE(read_file(dst) == "v1");

    write_file(src, "v2");
    REQUIRE(write_backup_file(src, dst));
    REQUIRE(read_file(dst) == "v2");
}

TEST_CASE("write_backup_file cleans up tmp on rename failure", "[config_backup]") {
    // root bypasses the permission bits this case depends on, so the rename cannot fail
    // and the assertion below would be asserting the opposite of the point.
    if (::geteuid() == 0) {
        SKIP("Test requires non-root euid - root bypasses permission bits");
    }
    TmpDir dir("renamefail");
    std::string src = dir.file("source.txt");
    // Backup path in a read-only directory (create dir, then chmod)
    fs::path readonly_dir = dir.path / "readonly";
    fs::create_directories(readonly_dir);
    write_file(src, "data");

    std::string dst = (readonly_dir / "backup.txt").string();
    // Write the tmp file first so rename target dir exists, then make dir read-only
    write_file(dst + ".tmp", "stale");
    fs::permissions(readonly_dir, fs::perms::owner_read | fs::perms::owner_exec);

    bool result = write_backup_file(src, dst);
    // Restore permissions for cleanup
    fs::permissions(readonly_dir, fs::perms::owner_all);

    REQUIRE_FALSE(result);
    // Tmp file should be cleaned up (or at least the backup shouldn't exist)
    REQUIRE_FALSE(fs::exists(dst));
}

// ─── write_rolling_backup ─────────────────────────────────────────────────────

TEST_CASE("write_rolling_backup uses primary when available", "[config_backup]") {
    TmpDir dir("rolling_primary");
    std::string src = dir.file("config.json");
    std::string primary = dir.file("primary.backup");
    std::string fallback = dir.file("fallback.backup");
    write_file(src, "config_data");

    write_rolling_backup(src, primary, fallback);

    REQUIRE(fs::exists(primary));
    REQUIRE(read_file(primary) == "config_data");
    REQUIRE_FALSE(fs::exists(fallback));
}

TEST_CASE("write_rolling_backup falls back when primary dir is unwritable", "[config_backup]") {
    // root bypasses the permission bits this case depends on, so the primary dir is still writable
    // and the assertion below would be asserting the opposite of the point.
    if (::geteuid() == 0) {
        SKIP("Test requires non-root euid - root bypasses permission bits");
    }
    TmpDir dir("rolling_fallback");
    std::string src = dir.file("config.json");
    write_file(src, "config_data");

    // Primary in a read-only directory
    fs::path ro_dir = dir.path / "readonly";
    fs::create_directories(ro_dir);
    fs::permissions(ro_dir, fs::perms::owner_read | fs::perms::owner_exec);
    std::string primary = (ro_dir / "primary.backup").string();
    std::string fallback = dir.file("fallback.backup");

    write_rolling_backup(src, primary, fallback);

    // Restore permissions for cleanup
    fs::permissions(ro_dir, fs::perms::owner_all);

    REQUIRE_FALSE(fs::exists(primary));
    REQUIRE(fs::exists(fallback));
    REQUIRE(read_file(fallback) == "config_data");
}

TEST_CASE("write_rolling_backup handles both paths failing gracefully", "[config_backup]") {
    TmpDir dir("rolling_bothfail");
    // Source doesn't exist — both backups should fail silently
    write_rolling_backup(dir.file("nonexistent"), dir.file("a"), dir.file("b"));

    REQUIRE_FALSE(fs::exists(dir.file("a")));
    REQUIRE_FALSE(fs::exists(dir.file("b")));
}

// ─── find_backup ──────────────────────────────────────────────────────────────

TEST_CASE("find_backup returns first existing path", "[config_backup]") {
    TmpDir dir("find");
    std::string a = dir.file("a.backup");
    std::string b = dir.file("b.backup");
    write_file(b, "data_b");

    REQUIRE(find_backup({a, b}) == b);
}

TEST_CASE("find_backup prefers earlier paths", "[config_backup]") {
    TmpDir dir("find_prio");
    std::string a = dir.file("a.backup");
    std::string b = dir.file("b.backup");
    write_file(a, "data_a");
    write_file(b, "data_b");

    REQUIRE(find_backup({a, b}) == a);
}

TEST_CASE("find_backup returns empty when none exist", "[config_backup]") {
    REQUIRE(find_backup({"/nonexistent/a", "/nonexistent/b"}).empty());
}

// ─── restore_from_backup ──────────────────────────────────────────────────────

TEST_CASE("restore_from_backup restores missing file from backup", "[config_backup]") {
    TmpDir dir("restore");
    std::string target = dir.file("config.json");
    std::string backup = dir.file("config.backup");
    write_file(backup, R"({"restored":true})");

    REQUIRE(restore_from_backup(target, "Config", {backup}));
    REQUIRE(read_file(target) == R"({"restored":true})");
}

TEST_CASE("restore_from_backup skips when target exists", "[config_backup]") {
    TmpDir dir("restore_exists");
    std::string target = dir.file("config.json");
    std::string backup = dir.file("config.backup");
    write_file(target, "original");
    write_file(backup, "backup_data");

    REQUIRE_FALSE(restore_from_backup(target, "Config", {backup}));
    REQUIRE(read_file(target) == "original");
}

TEST_CASE("restore_from_backup returns false when no backups exist", "[config_backup]") {
    TmpDir dir("restore_none");
    std::string target = dir.file("missing.json");

    REQUIRE_FALSE(restore_from_backup(target, "Config", {"/nonexistent/a", "/nonexistent/b"}));
    REQUIRE_FALSE(fs::exists(target));
}

TEST_CASE("restore_from_backup creates parent directory for target", "[config_backup]") {
    TmpDir dir("restore_mkdir");
    std::string backup = dir.file("config.backup");
    write_file(backup, "data");

    std::string target = (dir.path / "new_dir" / "config.json").string();
    REQUIRE(restore_from_backup(target, "Config", {backup}));
    REQUIRE(read_file(target) == "data");
}

TEST_CASE("restore_from_backup uses priority order across backups", "[config_backup]") {
    TmpDir dir("restore_prio");
    std::string primary = dir.file("primary.backup");
    std::string fallback = dir.file("fallback.backup");
    write_file(primary, "primary_data");
    write_file(fallback, "fallback_data");

    std::string target = dir.file("config.json");
    REQUIRE(restore_from_backup(target, "Config", {primary, fallback}));
    REQUIRE(read_file(target) == "primary_data");
}

TEST_CASE("restore_from_backup falls back to second backup", "[config_backup]") {
    TmpDir dir("restore_fallback");
    std::string fallback = dir.file("fallback.backup");
    write_file(fallback, "fallback_data");

    std::string target = dir.file("config.json");
    REQUIRE(restore_from_backup(target, "Config", {"/nonexistent/primary", fallback}));
    REQUIRE(read_file(target) == "fallback_data");
}

// ─── backup_fallback_dir (app_constants.h) ────────────────────────────────────

TEST_CASE("backup_fallback_dir uses HOME env var", "[config_backup]") {
    // Copy the value out. setenv()/unsetenv() free the buffer getenv() returned,
    // so restoring through the raw pointer at the end of the test reads freed
    // memory — ASAN flags it as heap-use-after-free. has_original distinguishes
    // "HOME was unset" from "HOME was empty".
    const char* original_raw = std::getenv("HOME");
    const bool has_original = original_raw != nullptr;
    const std::string original = has_original ? original_raw : "";
    // Restore the PREVIOUS ref, not a freshly computed $HOME/.helixscreen:
    // the test binary deliberately points this at its own sandbox, and
    // recomputing would silently aim every later test in the shard at the
    // developer's real backup directory.
    const std::string prev_ref = AppConstants::Update::detail::backup_fallback_dir_ref();
    setenv("HOME", "/tmp/fake_home", 1);
    AppConstants::Update::detail::backup_fallback_dir_ref() =
        AppConstants::Update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";

    std::string dir = AppConstants::Update::backup_fallback_dir();
    REQUIRE(dir == "/tmp/fake_home/.helixscreen");

    if (has_original)
        setenv("HOME", original.c_str(), 1);
    else
        unsetenv("HOME");
    AppConstants::Update::detail::backup_fallback_dir_ref() = prev_ref;
}

TEST_CASE("backup_fallback_dir falls back to /tmp when HOME unset", "[config_backup]") {
    // Same copy-don't-alias rule as the test above: unsetenv() frees the buffer.
    const char* original_raw = std::getenv("HOME");
    const bool has_original = original_raw != nullptr;
    const std::string original = has_original ? original_raw : "";
    const std::string prev_ref = AppConstants::Update::detail::backup_fallback_dir_ref();
    unsetenv("HOME");
    AppConstants::Update::detail::backup_fallback_dir_ref() =
        AppConstants::Update::sanitize_home(std::getenv("HOME")) + "/.helixscreen";

    std::string dir = AppConstants::Update::backup_fallback_dir();
    REQUIRE(dir == "/tmp/.helixscreen");

    if (has_original)
        setenv("HOME", original.c_str(), 1);
    AppConstants::Update::detail::backup_fallback_dir_ref() = prev_ref;
}

// ─── end-to-end: rolling backup + restore cycle ──────────────────────────────

TEST_CASE("full backup-restore cycle survives simulated Moonraker wipe", "[config_backup]") {
    TmpDir dir("e2e");

    // Simulate install dir with config
    fs::path install = dir.path / "install" / "config";
    fs::create_directories(install);
    std::string config_path = (install / "helixconfig.json").string();
    std::string env_path = (install / "helixscreen.env").string();
    write_file(config_path, R"({"printer":"voron"})");
    write_file(env_path, "MOONRAKER_HOST=192.168.1.100");

    // Backup to "primary" (simulating /var/lib/helixscreen/)
    std::string primary_config = dir.file("primary_config.backup");
    std::string primary_env = dir.file("primary_env.backup");
    REQUIRE(write_backup_file(config_path, primary_config));
    REQUIRE(write_backup_file(env_path, primary_env));

    // Simulate Moonraker wipe (rm -rf install dir)
    fs::remove_all(install);
    REQUIRE_FALSE(fs::exists(config_path));
    REQUIRE_FALSE(fs::exists(env_path));

    // Restore from backups (recreates parent dir)
    REQUIRE(restore_from_backup(config_path, "Config", {primary_config}));
    REQUIRE(restore_from_backup(env_path, "Env", {primary_env}));

    REQUIRE(read_file(config_path) == R"({"printer":"voron"})");
    REQUIRE(read_file(env_path) == "MOONRAKER_HOST=192.168.1.100");
}

// ─── upgrade safety: backup freshness ─────────────────────────────────────────

TEST_CASE("rolling backup written on startup ensures fresh recovery data", "[config_backup]") {
    // Simulates the Config::init() fix: rolling backup is written every startup,
    // not just on explicit save. Verifies that even without a save() call,
    // a fresh backup exists for upgrade recovery.
    TmpDir dir("startup_backup");

    std::string config = dir.file("settings.json");
    std::string primary = dir.file("primary.backup");
    std::string fallback = dir.file("fallback.backup");

    write_file(config, R"({"moonraker_host":"192.168.1.50","wizard_completed":true})");

    // Simulate what Config::init() now does: write rolling backup on load
    write_rolling_backup(config, primary, fallback);

    REQUIRE(fs::exists(primary));
    REQUIRE(read_file(primary) == R"({"moonraker_host":"192.168.1.50","wizard_completed":true})");

    // Simulate upgrade wiping the config (tarball default removed, Phase 6 failed)
    fs::remove(config);
    REQUIRE_FALSE(fs::exists(config));

    // restore_from_backup should recover from the fresh rolling backup
    REQUIRE(restore_from_backup(config, "Config", {primary, fallback}));
    REQUIRE(read_file(config) == R"({"moonraker_host":"192.168.1.50","wizard_completed":true})");
}

TEST_CASE("default config blocks backup recovery without tarball removal fix", "[config_backup]") {
    // Demonstrates the bug: if a default settings.json exists (from tarball),
    // restore_from_backup skips recovery because "target exists".
    TmpDir dir("default_blocks");

    std::string target = dir.file("settings.json");
    std::string backup = dir.file("settings.json.backup");

    // User's real config in backup
    write_file(backup, R"({"moonraker_host":"192.168.1.50"})");
    // Tarball default in target
    write_file(target, R"({"moonraker_host":"127.0.0.1"})");

    // restore_from_backup refuses because target exists — this is the bug scenario
    REQUIRE_FALSE(restore_from_backup(target, "Config", {backup}));
    // User's config is NOT recovered; default remains
    REQUIRE(read_file(target) == R"({"moonraker_host":"127.0.0.1"})");
}

TEST_CASE("missing config triggers backup recovery after tarball removal fix", "[config_backup]") {
    // With the fix: tarball default is removed before Phase 6, so if restore
    // fails, the file is missing and restore_from_backup kicks in.
    TmpDir dir("fix_recovery");

    std::string target = dir.file("settings.json");
    std::string backup = dir.file("settings.json.backup");

    write_file(backup, R"({"moonraker_host":"192.168.1.50"})");
    // No target file (tarball default was removed by the fix)
    REQUIRE_FALSE(fs::exists(target));

    REQUIRE(restore_from_backup(target, "Config", {backup}));
    REQUIRE(read_file(target) == R"({"moonraker_host":"192.168.1.50"})");
}

TEST_CASE("restore_from_backup falls back to legacy backup names", "[config_backup]") {
    TmpDir dir("legacy_backup");

    std::string legacy_backup = dir.file("helixconfig.json.backup");
    std::string target = dir.file("settings.json");
    write_file(
        legacy_backup,
        R"({"config_version": 8, "printers": [{"name": "test", "moonraker_address": "127.0.0.1"}]})");

    std::string new_backup = dir.file("settings.json.backup");
    bool restored = restore_from_backup(target, "Config", {new_backup, legacy_backup});

    REQUIRE(restored);
    REQUIRE(fs::exists(target));
}
