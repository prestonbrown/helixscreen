# Update Telemetry Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Track in-app update success/failure via anonymous opt-in telemetry so we can measure update success rates and diagnose failures.

**Architecture:** Two new event types (`update_failed`, `update_success`) added to TelemetryManager. Failure events fire immediately from UpdateChecker error paths. Success uses persist-and-detect pattern (like crash reporting): write flag file before restart, read it on next boot. No backend changes needed — endpoint accepts arbitrary event types.

**Tech Stack:** C++ (TelemetryManager singleton, nlohmann::json), Catch2 tests, Python analysis script

**Design doc:** `docs/plans/2026-02-26-update-telemetry-design.md`

---

### Task 1: TelemetryManager — record_update_failure() with tests

**Files:**
- Modify: `include/system/telemetry_manager.h:167` (after record_print_outcome declaration)
- Modify: `src/system/telemetry_manager.cpp:422` (after record_print_outcome impl)
- Modify: `tests/unit/test_telemetry_manager.cpp` (add new test section)

**Step 1: Write the failing tests**

Add to `tests/unit/test_telemetry_manager.cpp` after the existing session event tests (~line 390):

```cpp
// ============================================================================
// Update Failed Event [telemetry][update]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: has required envelope fields",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("download_failed", "0.14.0", "ad5m");

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["schema_version"] == 2);
    REQUIRE(event["event"] == "update_failed");
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));
    REQUIRE(event["reason"] == "download_failed");
    REQUIRE(event["version"] == "0.14.0");
    REQUIRE(event["platform"] == "ad5m");
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: includes optional fields when provided",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("corrupt_download", "0.14.0", "pi", 200, 1048576);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["reason"] == "corrupt_download");
    REQUIRE(event["http_code"] == 200);
    REQUIRE(event["file_size"] == 1048576);
    REQUIRE_FALSE(event.contains("exit_code"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: includes exit_code for install failures",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("install_failed", "0.14.0", "ad5m", -1, -1, 127);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["reason"] == "install_failed");
    REQUIRE(event["exit_code"] == 127);
    REQUIRE_FALSE(event.contains("http_code"));
    REQUIRE_FALSE(event.contains("file_size"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: not recorded when telemetry disabled",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    tm.record_update_failure("download_failed", "0.14.0", "pi");

    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: from_version included when available",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("download_failed", "0.14.0", "pi");

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    // from_version should be current HELIX_VERSION
    REQUIRE(event.contains("from_version"));
}
```

**Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[telemetry][update]" -v`
Expected: FAIL — `record_update_failure` does not exist

**Step 3: Declare in header**

In `include/system/telemetry_manager.h`, after `record_print_outcome()` declaration (line 169), add:

```cpp
    /**
     * @brief Record an update failure event
     *
     * Call when an in-app update fails at any stage (download, verify, install).
     * No-op if telemetry is disabled.
     *
     * Thread-safe: may be called from any thread.
     *
     * @param reason Short failure reason (e.g., "download_failed", "corrupt_download")
     * @param version Target version being installed
     * @param platform Platform key (e.g., "pi", "ad5m")
     * @param http_code HTTP status code (-1 to omit)
     * @param file_size Downloaded file size in bytes (-1 to omit)
     * @param exit_code install.sh exit code (-1 to omit)
     */
    void record_update_failure(const std::string& reason, const std::string& version,
                               const std::string& platform, int http_code = -1,
                               int64_t file_size = -1, int exit_code = -1);
```

In the private section, after `build_print_outcome_event()` (line 441), add:

```cpp
    nlohmann::json build_update_failed_event(const std::string& reason, const std::string& version,
                                             const std::string& platform, int http_code,
                                             int64_t file_size, int exit_code) const;
```

**Step 4: Implement**

In `src/system/telemetry_manager.cpp`, after `record_print_outcome()` (line 422), add:

```cpp
void TelemetryManager::record_update_failure(const std::string& reason, const std::string& version,
                                             const std::string& platform, int http_code,
                                             int64_t file_size, int exit_code) {
    if (!enabled_.load() || !initialized_.load()) {
        return;
    }

    spdlog::info("[TelemetryManager] Recording update failure: reason={} version={}", reason,
                 version);
    auto event =
        build_update_failed_event(reason, version, platform, http_code, file_size, exit_code);
    enqueue_event(std::move(event));
    save_queue();
}
```

After `build_print_outcome_event()` (~line 1027), add:

```cpp
nlohmann::json TelemetryManager::build_update_failed_event(const std::string& reason,
                                                           const std::string& version,
                                                           const std::string& platform,
                                                           int http_code, int64_t file_size,
                                                           int exit_code) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "update_failed";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["reason"] = reason;
    event["version"] = version;
    event["from_version"] = HELIX_VERSION;
    event["platform"] = platform;

    if (http_code >= 0) {
        event["http_code"] = http_code;
    }
    if (file_size >= 0) {
        event["file_size"] = file_size;
    }
    if (exit_code >= 0) {
        event["exit_code"] = exit_code;
    }

    return event;
}
```

**Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[telemetry][update]" -v`
Expected: ALL PASS

**Step 6: Commit**

```bash
git add include/system/telemetry_manager.h src/system/telemetry_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add record_update_failure() with tests"
```

---

### Task 2: TelemetryManager — update_success flag file + check_previous_update()

**Files:**
- Modify: `include/system/telemetry_manager.h` (add declarations)
- Modify: `src/system/telemetry_manager.cpp` (add implementations + call from init)
- Modify: `tests/unit/test_telemetry_manager.cpp` (add tests)

**Step 1: Write the failing tests**

Add after the update_failed tests:

```cpp
// ============================================================================
// Update Success Event [telemetry][update]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: check_previous_update enqueues event from flag file",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Write a flag file simulating a successful update
    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["schema_version"] == 2);
    REQUIRE(event["event"] == "update_success");
    REQUIRE(event["version"] == "0.14.0");
    REQUIRE(event["from_version"] == "0.13.4");
    REQUIRE(event["platform"] == "pi");
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: flag file deleted after reading",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: no-op when no flag file exists",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: discarded when telemetry disabled",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
    // Flag file should still be removed even if telemetry is disabled
    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: malformed flag file handled gracefully",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    write_file("update_success.json", "not valid json {{{{");

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
    // Malformed file should still be cleaned up
    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Write update success flag: creates valid JSON file",
                 "[telemetry][update]") {
    TelemetryManager::write_update_success_flag(temp_dir().string(), "0.14.0", "0.13.4", "pi");

    REQUIRE(fs::exists(temp_dir() / "update_success.json"));
    auto content = read_file("update_success.json");
    auto flag = json::parse(content);

    REQUIRE(flag["version"] == "0.14.0");
    REQUIRE(flag["from_version"] == "0.13.4");
    REQUIRE(flag["platform"] == "pi");
    REQUIRE(flag.contains("timestamp"));
}
```

**Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[telemetry][update]" -v`
Expected: FAIL — `check_previous_update` and `write_update_success_flag` don't exist

**Step 3: Declare in header**

In `include/system/telemetry_manager.h`, in the public EVENT RECORDING section (after `record_update_failure`), add:

```cpp
    /**
     * @brief Check for a successful update from a previous session
     *
     * Looks for update_success.json flag file. If found, enqueues an
     * update_success event and deletes the file. Called from init().
     */
    void check_previous_update();

    /**
     * @brief Write update success flag file before restart
     *
     * Static method callable from UpdateChecker before _exit(0).
     * The flag is read by check_previous_update() on next boot.
     *
     * @param config_dir Config directory path
     * @param version Version that was installed
     * @param from_version Version before the update
     * @param platform Platform key
     */
    static void write_update_success_flag(const std::string& config_dir,
                                          const std::string& version,
                                          const std::string& from_version,
                                          const std::string& platform);
```

In the private section, after `build_update_failed_event`, add:

```cpp
    nlohmann::json build_update_success_event(const std::string& version,
                                              const std::string& from_version,
                                              const std::string& platform,
                                              const std::string& timestamp) const;
```

**Step 4: Implement**

In `src/system/telemetry_manager.cpp`:

Add `write_update_success_flag` (static, after `record_update_failure`):

```cpp
void TelemetryManager::write_update_success_flag(const std::string& config_dir,
                                                 const std::string& version,
                                                 const std::string& from_version,
                                                 const std::string& platform) {
    json flag;
    flag["version"] = version;
    flag["from_version"] = from_version;
    flag["platform"] = platform;

    // Use ISO 8601 timestamp
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc {};
    gmtime_r(&tt, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    flag["timestamp"] = buf;

    std::string path = config_dir + "/update_success.json";
    std::ofstream ofs(path);
    if (ofs) {
        ofs << flag.dump();
        spdlog::info("[TelemetryManager] Wrote update success flag: {}", path);
    } else {
        spdlog::error("[TelemetryManager] Failed to write update success flag: {}", path);
    }
}
```

Add `check_previous_update`:

```cpp
void TelemetryManager::check_previous_update() {
    std::string flag_path = config_dir_ + "/update_success.json";

    if (!fs::exists(flag_path)) {
        return;
    }

    spdlog::info("[TelemetryManager] Found update success flag from previous session");

    // Read and parse the flag file
    json flag;
    {
        std::ifstream ifs(flag_path);
        if (!ifs) {
            spdlog::warn("[TelemetryManager] Could not open update success flag");
            std::remove(flag_path.c_str());
            return;
        }
        try {
            flag = json::parse(ifs);
        } catch (const json::parse_error& e) {
            spdlog::warn("[TelemetryManager] Malformed update success flag: {}", e.what());
            std::remove(flag_path.c_str());
            return;
        }
    }

    // Always clean up the flag file
    std::remove(flag_path.c_str());

    if (enabled_.load()) {
        auto event = build_update_success_event(flag.value("version", "unknown"),
                                                flag.value("from_version", "unknown"),
                                                flag.value("platform", "unknown"),
                                                flag.value("timestamp", get_timestamp()));
        enqueue_event(std::move(event));
        save_queue();
        spdlog::info("[TelemetryManager] Enqueued update_success event (version={})",
                     flag.value("version", "unknown"));
    } else {
        spdlog::debug("[TelemetryManager] Update success event discarded (telemetry disabled)");
    }
}
```

Add `build_update_success_event`:

```cpp
nlohmann::json TelemetryManager::build_update_success_event(const std::string& version,
                                                            const std::string& from_version,
                                                            const std::string& platform,
                                                            const std::string& timestamp) const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "update_success";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = timestamp;
    event["version"] = version;
    event["from_version"] = from_version;
    event["platform"] = platform;

    return event;
}
```

In `TelemetryManager::init()` (~line 259), after the call to `check_previous_crash()`, add:

```cpp
    check_previous_update();
```

Add `#include <filesystem>` to the top of `telemetry_manager.cpp` if not already present. Use `namespace fs = std::filesystem;`.

**Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[telemetry][update]" -v`
Expected: ALL PASS

**Step 6: Run the full telemetry test suite for regressions**

Run: `./build/bin/helix-tests "[telemetry]" -v`
Expected: ALL PASS

**Step 7: Commit**

```bash
git add include/system/telemetry_manager.h src/system/telemetry_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add update_success flag file and check_previous_update()"
```

---

### Task 3: Wire UpdateChecker failure paths to record_update_failure()

**Files:**
- Modify: `src/system/update_checker.cpp` (add telemetry calls at each error path)

**Ref:** `include/system/telemetry_manager.h` for the API, `include/helix_version.h` for HELIX_VERSION

**Step 1: Add include**

At top of `src/system/update_checker.cpp`, the include for `telemetry_manager.h` already exists (the explore agent found it). Verify line ~21:
```cpp
#include "system/telemetry_manager.h"
```

**Step 2: Add telemetry calls to start_download() error paths**

At line 825 (print in progress), after the `report_download_status` call, add:

```cpp
        TelemetryManager::instance().record_update_failure("print_in_progress", "", get_platform_key());
```

At line 836 (no cached update), after the `report_download_status` call, add:

```cpp
        TelemetryManager::instance().record_update_failure("no_cached_update", "", get_platform_key());
```

**Step 3: Add telemetry calls to do_download() error paths**

At line 878 (no disk space), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("no_disk_space", version, get_platform_key());
```

At line 916 (download failed, result==0), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("download_failed", version, get_platform_key());
```

At line 925 (file too small), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("file_too_small", version, get_platform_key(), -1, static_cast<int64_t>(result));
```

At line 932 (file too large), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("file_too_large", version, get_platform_key(), -1, static_cast<int64_t>(result));
```

At line 945 (corrupt download / gunzip failed), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("corrupt_download", version, get_platform_key(), -1, static_cast<int64_t>(result));
```

At line 956 (wrong architecture), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("wrong_architecture", version, get_platform_key());
```

**Step 4: Add telemetry calls to do_install() error paths**

At line 1143 (installer not found), after the `report_download_status` call:

```cpp
        TelemetryManager::instance().record_update_failure("installer_not_found", version, get_platform_key());
```

Note: `version` needs to be extracted from `cached_info_` at this point. Check if it's available. Look for where `version` is used in `do_install`. If it's only obtained at line ~1307, extract it earlier. Add near the top of `do_install`:

```cpp
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = cached_info_ ? cached_info_->version : "unknown";
    }
```

Move the existing version extraction (~line 1307) to this earlier location and use `version` throughout.

At line 1297 (install.sh failed), distinguish timeout vs failure. Add a `bool timed_out = false;` before the fork/wait loop. Set `timed_out = true;` in the timeout branch (line 1247). Then after the `report_download_status` call at line 1299:

```cpp
        std::string reason = timed_out ? "install_timeout" : "install_failed";
        TelemetryManager::instance().record_update_failure(reason, version, get_platform_key(), -1, -1, ret);
```

**Step 5: Build to verify compilation**

Run: `make -j`
Expected: Clean build, no errors

**Step 6: Run existing tests for regressions**

Run: `make test && ./build/bin/helix-tests "[telemetry]" -v`
Expected: ALL PASS

**Step 7: Commit**

```bash
git add src/system/update_checker.cpp
git commit -m "feat(telemetry): wire update failure paths to record_update_failure()"
```

---

### Task 4: Wire UpdateChecker success path to write flag file

**Files:**
- Modify: `src/system/update_checker.cpp:1304` (success path in do_install)

**Step 1: Add flag file write before _exit(0)**

In `do_install()`, after the success log line (~line 1304 `"Update installed successfully!"`), before the `report_download_status(Complete, ...)` call, add:

```cpp
    // Write update success flag for telemetry (picked up on next boot)
    {
        auto config_dir = helix::Config::get_config_dir();
        TelemetryManager::write_update_success_flag(config_dir, version, HELIX_VERSION,
                                                    get_platform_key());
    }
```

Note: `HELIX_VERSION` is the *current* version (from_version). `version` is the version being installed (target). Check that `helix_version.h` is included (it likely is via transitive includes, but verify).

**Step 2: Build to verify compilation**

Run: `make -j`
Expected: Clean build

**Step 3: Verify the full test suite**

Run: `make test-run`
Expected: ALL PASS

**Step 4: Commit**

```bash
git add src/system/update_checker.cpp
git commit -m "feat(telemetry): write update_success flag before restart"
```

---

### Task 5: Update telemetry-analyze.py

**Files:**
- Modify: `scripts/telemetry-analyze.py:136-165` (event type parsing)
- Modify: `scripts/telemetry-analyze.py:186-188` (dataframe filtering)
- Modify: The report output section (search for where sessions/prints/crashes are reported)

**Step 1: Add update event parsing**

After the crash event branch (~line 164), add:

```python
        elif ev.get("event") == "update_failed":
            for k in (
                "reason",
                "version",
                "from_version",
                "platform",
                "http_code",
                "file_size",
                "exit_code",
            ):
                flat[k] = ev.get(k)

        elif ev.get("event") == "update_success":
            for k in (
                "version",
                "from_version",
                "platform",
            ):
                flat[k] = ev.get(k)
```

**Step 2: Add dataframe filtering**

After the crashes line (~line 188), add:

```python
        self.update_failures = df[df["event"] == "update_failed"].copy()
        self.update_successes = df[df["event"] == "update_success"].copy()
```

**Step 3: Add update report section**

Find where the crash report section outputs (search for `self.crashes` usage in the report method). Add a similar section for updates:

```python
        # --- Updates ---
        n_fail = len(self.update_failures)
        n_success = len(self.update_successes)
        n_total = n_fail + n_success
        if n_total > 0:
            rate = n_success / n_total * 100 if n_total > 0 else 0
            print(f"\n{'='*60}")
            print(f"UPDATES: {n_total} attempts, {n_success} succeeded, {n_fail} failed ({rate:.0f}% success rate)")
            print(f"{'='*60}")
            if n_fail > 0:
                reason_counts = self.update_failures["reason"].value_counts()
                print("\nFailure reasons:")
                for reason, count in reason_counts.items():
                    print(f"  {reason}: {count}")

                # Show failures by platform
                if "platform" in self.update_failures.columns:
                    platform_counts = self.update_failures["platform"].value_counts()
                    print("\nFailures by platform:")
                    for platform, count in platform_counts.items():
                        print(f"  {platform}: {count}")
```

**Step 4: Commit**

```bash
git add scripts/telemetry-analyze.py
git commit -m "feat(telemetry): add update event analysis to telemetry-analyze.py"
```

---

### Task 6: Final integration test + squash commit

**Step 1: Run full test suite**

Run: `make test-run`
Expected: ALL PASS

**Step 2: Run telemetry tests specifically**

Run: `./build/bin/helix-tests "[telemetry]" -v`
Expected: ALL PASS, including all new `[update]` tests

**Step 3: Verify no regressions in other tests**

Run: `./build/bin/helix-tests "~[.] ~[slow]" -v`
Expected: ALL PASS

**Step 4: Code review**

Use superpowers:requesting-code-review skill to review all changes.
