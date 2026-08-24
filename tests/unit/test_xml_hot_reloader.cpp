// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_xml_hot_reloader.cpp
 * @brief Tests for XmlHotReloader — file scanning, mtime tracking, change detection
 *
 * Uses temp directories with real XML files to test the polling/detection logic
 * without needing LVGL initialized. The reload callback injection lets us verify
 * that changes are detected and the correct component names are derived.
 */

#include "layout_manager.h"
#include "xml_hot_reloader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

// ============================================================================
// Test access helper
// ============================================================================

class XmlHotReloaderTestAccess {
  public:
    static std::string component_name_from_path(const fs::path& path) {
        return helix::XmlHotReloader::component_name_from_path(path);
    }

    static const auto& file_mtimes(const helix::XmlHotReloader& hr) {
        return hr.file_mtimes_;
    }

    static const auto& file_to_lvgl_path(const helix::XmlHotReloader& hr) {
        return hr.file_to_lvgl_path_;
    }
};

// ============================================================================
// Fixture: temp directory with XML files
// ============================================================================

class HotReloadFixture {
  public:
    HotReloadFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_hot_reload_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);
        sub_dir_ = temp_dir_ / "components";
        fs::create_directories(sub_dir_);
    }

    ~HotReloadFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    /// Create a minimal XML file in the temp directory
    void create_xml(const std::string& filename, const std::string& content = "<component/>") {
        std::ofstream f(temp_dir_ / filename);
        f << content;
    }

    /// Create a minimal XML file in the components subdirectory
    void create_sub_xml(const std::string& filename, const std::string& content = "<component/>") {
        std::ofstream f(sub_dir_ / filename);
        f << content;
    }

    /// Touch a file to update its mtime (write same content)
    void touch_xml(const std::string& filename) {
        auto path = temp_dir_ / filename;
        if (!fs::exists(path)) {
            path = sub_dir_ / filename;
        }
        // Ensure mtime actually changes (some filesystems have 1s granularity)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ofstream f(path, std::ios::app);
        f << " ";
    }

    fs::path temp_dir_;
    fs::path sub_dir_;
};

// ============================================================================
// component_name_from_path [hot-reload]
// ============================================================================

TEST_CASE("component_name_from_path strips .xml extension", "[hot-reload]") {
    REQUIRE(XmlHotReloaderTestAccess::component_name_from_path("home_panel.xml") == "home_panel");
    REQUIRE(XmlHotReloaderTestAccess::component_name_from_path("settings_panel.xml") ==
            "settings_panel");
}

TEST_CASE("component_name_from_path handles paths with directories", "[hot-reload]") {
    REQUIRE(XmlHotReloaderTestAccess::component_name_from_path("ui_xml/home_panel.xml") ==
            "home_panel");
    REQUIRE(XmlHotReloaderTestAccess::component_name_from_path(
                "/abs/path/ui_xml/components/nozzle_icon.xml") == "nozzle_icon");
}

TEST_CASE("component_name_from_path handles file without extension", "[hot-reload]") {
    REQUIRE(XmlHotReloaderTestAccess::component_name_from_path("no_extension") == "no_extension");
}

// ============================================================================
// Initial scan [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "initial scan finds XML files in directory", "[hot-reload]") {
    create_xml("panel_a.xml");
    create_xml("panel_b.xml");
    create_xml("panel_c.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);

    REQUIRE(hr.tracked_file_count() == 3);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "initial scan ignores non-XML files", "[hot-reload]") {
    create_xml("panel.xml");
    // Create non-XML files
    {
        std::ofstream f(temp_dir_ / "readme.md");
        f << "# readme";
    }
    {
        std::ofstream f(temp_dir_ / "data.json");
        f << "{}";
    }
    {
        std::ofstream f(temp_dir_ / "image.png");
        f << "\x89PNG";
    }

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);

    REQUIRE(hr.tracked_file_count() == 1);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "initial scan handles multiple directories", "[hot-reload]") {
    create_xml("top_level.xml");
    create_sub_xml("component.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string(), sub_dir_.string()}, 50);

    REQUIRE(hr.tracked_file_count() == 2);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "initial scan handles non-existent directory gracefully",
                 "[hot-reload]") {
    create_xml("panel.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string(), "/tmp/nonexistent_dir_12345"}, 50);

    // Should still track the one file from the valid directory
    REQUIRE(hr.tracked_file_count() == 1);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "initial scan with empty directory tracks zero files",
                 "[hot-reload]") {
    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);

    REQUIRE(hr.tracked_file_count() == 0);

    hr.stop();
}

// ============================================================================
// LVGL path mapping [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "file_to_lvgl_path maps to A: prefixed relative path",
                 "[hot-reload]") {
    create_xml("test_widget.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);

    auto& paths = XmlHotReloaderTestAccess::file_to_lvgl_path(hr);
    REQUIRE(paths.size() == 1);

    // The LVGL path should start with "A:" and contain the filename
    auto& [abs_path, lvgl_path] = *paths.begin();
    REQUIRE(lvgl_path.substr(0, 2) == "A:");
    REQUIRE(lvgl_path.find("test_widget.xml") != std::string::npos);

    hr.stop();
}

// ============================================================================
// Start/stop lifecycle [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "stop without start is safe", "[hot-reload]") {
    helix::XmlHotReloader hr;
    hr.stop(); // Should not crash or hang
    REQUIRE(hr.is_running() == false);
}

TEST_CASE_METHOD(HotReloadFixture, "double stop is safe", "[hot-reload]") {
    create_xml("panel.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);
    REQUIRE(hr.is_running() == true);

    hr.stop();
    REQUIRE(hr.is_running() == false);

    hr.stop(); // Second stop should be no-op
    REQUIRE(hr.is_running() == false);
}

TEST_CASE_METHOD(HotReloadFixture, "double start is ignored", "[hot-reload]") {
    create_xml("panel.xml");

    helix::XmlHotReloader hr;
    hr.set_reload_callback([](const std::string&, const std::string&) {});
    hr.start({temp_dir_.string()}, 50);

    auto count = hr.tracked_file_count();
    REQUIRE(count == 1);

    // Create another file and try to start again — should be no-op
    create_xml("panel2.xml");
    hr.start({temp_dir_.string()}, 50);

    // Should still track only the original file (second start ignored)
    REQUIRE(hr.tracked_file_count() == count);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "destructor stops the polling thread", "[hot-reload]") {
    create_xml("panel.xml");

    {
        helix::XmlHotReloader hr;
        hr.set_reload_callback([](const std::string&, const std::string&) {});
        hr.start({temp_dir_.string()}, 50);
        REQUIRE(hr.is_running() == true);
        // Destructor should call stop() and join the thread
    }
    // If we get here without hanging, the destructor worked
    REQUIRE(true);
}

// ============================================================================
// Change detection [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload detects file modification", "[hot-reload]") {
    create_xml("my_panel.xml", "<view><lv_obj/></view>");

    std::mutex mtx;
    std::vector<std::string> reloaded_components;

    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string& name, const std::string& /*path*/) {
        std::lock_guard<std::mutex> lock(mtx);
        reloaded_components.push_back(name);
    });
    hr.start({temp_dir_.string()}, 50);

    // Modify the file
    touch_xml("my_panel.xml");

    // Wait for the polling thread to pick it up
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!reloaded_components.empty())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    hr.stop();

    std::lock_guard<std::mutex> lock(mtx);
    REQUIRE(reloaded_components.size() == 1);
    REQUIRE(reloaded_components[0] == "my_panel");
}

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload reports correct LVGL path", "[hot-reload]") {
    create_xml("widget.xml");

    std::mutex mtx;
    std::string reported_path;

    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string& /*name*/, const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx);
        reported_path = path;
    });
    hr.start({temp_dir_.string()}, 50);

    touch_xml("widget.xml");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!reported_path.empty())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    hr.stop();

    std::lock_guard<std::mutex> lock(mtx);
    REQUIRE(reported_path.substr(0, 2) == "A:");
    REQUIRE(reported_path.find("widget.xml") != std::string::npos);
}

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload does not fire for unmodified files",
                 "[hot-reload]") {
    create_xml("stable.xml");

    std::atomic<int> reload_count{0};

    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& /*name*/, const std::string& /*path*/) { reload_count++; });
    hr.start({temp_dir_.string()}, 50);

    // Wait a few poll cycles without touching anything
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    hr.stop();

    REQUIRE(reload_count.load() == 0);
}

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload handles deleted file gracefully",
                 "[hot-reload]") {
    create_xml("ephemeral.xml");

    std::atomic<int> reload_count{0};

    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& /*name*/, const std::string& /*path*/) { reload_count++; });
    hr.start({temp_dir_.string()}, 50);

    // Delete the file
    fs::remove(temp_dir_ / "ephemeral.xml");

    // Wait a few poll cycles — should not crash or fire reload
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    hr.stop();

    REQUIRE(reload_count.load() == 0);
}

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload detects changes in subdirectory",
                 "[hot-reload]") {
    create_sub_xml("nozzle_icon.xml");

    std::mutex mtx;
    std::vector<std::string> reloaded;

    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string& name, const std::string& /*path*/) {
        std::lock_guard<std::mutex> lock(mtx);
        reloaded.push_back(name);
    });
    hr.start({temp_dir_.string(), sub_dir_.string()}, 50);

    touch_xml("nozzle_icon.xml");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!reloaded.empty())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    hr.stop();

    std::lock_guard<std::mutex> lock(mtx);
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0] == "nozzle_icon");
}

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload detects multiple files changing",
                 "[hot-reload]") {
    create_xml("panel_a.xml");
    create_xml("panel_b.xml");
    create_xml("panel_c.xml"); // This one stays unchanged

    std::mutex mtx;
    std::vector<std::string> reloaded;

    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string& name, const std::string& /*path*/) {
        std::lock_guard<std::mutex> lock(mtx);
        reloaded.push_back(name);
    });
    hr.start({temp_dir_.string()}, 50);

    touch_xml("panel_a.xml");
    touch_xml("panel_b.xml");
    // panel_c.xml NOT touched

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (reloaded.size() >= 2)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    hr.stop();

    std::lock_guard<std::mutex> lock(mtx);
    REQUIRE(reloaded.size() == 2);

    // Both changed files should be reloaded (order may vary)
    std::sort(reloaded.begin(), reloaded.end());
    REQUIRE(reloaded[0] == "panel_a");
    REQUIRE(reloaded[1] == "panel_b");
}

// ============================================================================
// Manual scan_and_reload (no polling thread) [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "scan_and_reload can be called manually without start",
                 "[hot-reload]") {
    create_xml("manual.xml");

    // Use start() just to do the initial scan, then immediately stop
    std::vector<std::string> reloaded;

    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& name, const std::string& /*path*/) { reloaded.push_back(name); });
    hr.start({temp_dir_.string()}, 50);
    hr.stop();

    // No changes yet
    hr.scan_and_reload();
    REQUIRE(reloaded.empty());

    // Touch the file and scan manually
    touch_xml("manual.xml");
    hr.scan_and_reload();

    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0] == "manual");

    // Second scan without changes should not reload again
    hr.scan_and_reload();
    REQUIRE(reloaded.size() == 1);
}

// ============================================================================
// Recursive scan [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "recursive scan finds files in nested subdirs", "[hot-reload]") {
    auto ultrawide_dir = temp_dir_ / "ultrawide";
    fs::create_directories(ultrawide_dir);
    std::ofstream(ultrawide_dir / "home_panel.xml") << "<component/>";
    create_xml("settings_panel.xml");

    helix::XmlHotReloader hr;
    hr.start({temp_dir_.string()}, /*poll_ms=*/10000);
    hr.stop();

    const auto& tracked = XmlHotReloaderTestAccess::file_mtimes(hr);
    REQUIRE(tracked.size() == 2);
    bool found_ultrawide = false;
    for (const auto& [path, _] : tracked) {
        if (path.find("ultrawide/home_panel.xml") != std::string::npos) {
            found_ultrawide = true;
        }
    }
    REQUIRE(found_ultrawide);
}

TEST_CASE_METHOD(HotReloadFixture, "recursive scan skips translations and claude-recall dirs",
                 "[hot-reload]") {
    fs::create_directories(temp_dir_ / "translations");
    std::ofstream(temp_dir_ / "translations" / "en.xml") << "<component/>";
    fs::create_directories(temp_dir_ / ".claude-recall");
    std::ofstream(temp_dir_ / ".claude-recall" / "note.xml") << "<component/>";
    create_xml("real_panel.xml");

    helix::XmlHotReloader hr;
    hr.start({temp_dir_.string()}, 10000);
    hr.stop();

    REQUIRE(XmlHotReloaderTestAccess::file_mtimes(hr).size() == 1);
}

TEST_CASE_METHOD(HotReloadFixture, "after_reload callback fires with component name",
                 "[hot-reload]") {
    create_xml("motion_panel.xml");
    helix::XmlHotReloader hr;

    std::vector<std::string> reload_log;
    std::vector<std::string> after_log;
    std::mutex m;
    hr.set_reload_callback([&](const std::string& name, const std::string&) {
        std::lock_guard<std::mutex> lock(m);
        reload_log.push_back(name);
    });
    hr.set_after_reload_callback([&](const std::string& name) {
        std::lock_guard<std::mutex> lock(m);
        after_log.push_back(name);
    });

    hr.start({temp_dir_.string()}, 10000);
    touch_xml("motion_panel.xml");
    hr.scan_and_reload();
    hr.stop();

    std::lock_guard<std::mutex> lock(m);
    REQUIRE(reload_log == std::vector<std::string>{"motion_panel"});
    REQUIRE(after_log == std::vector<std::string>{"motion_panel"});
}

// ============================================================================
// Parse-then-swap failure handling [hot-reload]
// ============================================================================

/// Overwrite a tracked XML file with new content + guarantee mtime changes
/// (some filesystems have 1s granularity, so we sleep to be safe).
static void overwrite_xml(const fs::path& path, const std::string& content) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ofstream f(path, std::ios::trunc);
    f << content;
}

TEST_CASE_METHOD(HotReloadFixture, "invalid XML is skipped without firing reload", "[hot-reload]") {
    create_xml("motion_panel.xml", "<component/>");

    std::atomic<int> reload_count{0};
    std::atomic<int> after_count{0};
    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string&, const std::string&) { reload_count++; });
    hr.set_after_reload_callback([&](const std::string&) { after_count++; });

    hr.start({temp_dir_.string()}, 10000);

    // Overwrite with malformed XML — should be deferred, not reloaded.
    overwrite_xml(temp_dir_ / "motion_panel.xml",
                  "<?xml version=\"1.0\"?>\n"
                  "<!-- comment before root is fine, but unterminated:\n"
                  "<component>");

    hr.scan_and_reload();
    hr.stop();

    REQUIRE(reload_count.load() == 0);
    REQUIRE(after_count.load() == 0);
}

TEST_CASE_METHOD(HotReloadFixture, "invalid XML does not update mtime cache (retry on next poll)",
                 "[hot-reload]") {
    create_xml("my_panel.xml", "<component/>");

    std::atomic<int> reload_count{0};
    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string&, const std::string&) { reload_count++; });

    hr.start({temp_dir_.string()}, 10000);

    // First overwrite: invalid XML — should be deferred.
    overwrite_xml(temp_dir_ / "my_panel.xml", "<component"); // unterminated
    hr.scan_and_reload();
    REQUIRE(reload_count.load() == 0);

    // Second overwrite: valid XML — must trigger reload (mtime cache wasn't
    // updated on the failed attempt, so this new change is visible).
    overwrite_xml(temp_dir_ / "my_panel.xml", "<component><view/></component>");
    hr.scan_and_reload();
    REQUIRE(reload_count.load() == 1);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "empty file is deferred (editor mid-write)", "[hot-reload]") {
    create_xml("home_panel.xml", "<component/>");

    std::atomic<int> reload_count{0};
    helix::XmlHotReloader hr;
    hr.set_reload_callback([&](const std::string&, const std::string&) { reload_count++; });

    hr.start({temp_dir_.string()}, 10000);

    // Simulate an editor's atomic-rename window: file briefly empty.
    overwrite_xml(temp_dir_ / "home_panel.xml", "");
    hr.scan_and_reload();
    REQUIRE(reload_count.load() == 0);

    // Editor finishes the write: valid content lands. Reload fires.
    overwrite_xml(temp_dir_ / "home_panel.xml", "<component><view/></component>");
    hr.scan_and_reload();
    REQUIRE(reload_count.load() == 1);

    hr.stop();
}

// ============================================================================
// Breakpoint-variant resolution [hot-reload]
// ============================================================================

// Note: LayoutManagerTestAccess is also defined in test_layout_manager.cpp and
// test_grid_layout.cpp, but Catch2 amalgamated builds compile each test file
// separately, so no ODR conflict.
class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

/// Stands up a miniature ui_xml/ tree holding both a base component and a
/// micro/ override of it, and makes the temp root the process CWD.
/// LayoutManager resolves variant paths relative to the CWD, so the reloader
/// and the layout resolver must be looking at the same tree for these tests to
/// mean anything.
class HotReloadVariantFixture {
  public:
    HotReloadVariantFixture() {
        prev_cwd_ = fs::current_path();
        root_ = fs::temp_directory_path() /
                ("helix_hot_reload_variant_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root_ / "ui_xml" / "micro");
        overwrite_xml(base_path(), "<component><view name=\"controls_panel\"/></component>");
        overwrite_xml(micro_path(), "<component><view name=\"controls_panel\"/></component>");
        fs::current_path(root_);
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }

    ~HotReloadVariantFixture() {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
        std::error_code ec;
        fs::current_path(prev_cwd_, ec);
        fs::remove_all(root_, ec);
    }

    fs::path base_path() const {
        return root_ / "ui_xml" / "controls_panel.xml";
    }
    fs::path micro_path() const {
        return root_ / "ui_xml" / "micro" / "controls_panel.xml";
    }

    fs::path root_;
    fs::path prev_cwd_;
};

TEST_CASE_METHOD(HotReloadVariantFixture,
                 "micro layout reloads the micro copy and ignores the shadowed base",
                 "[hot-reload]") {
    helix::LayoutManager::instance().init(480, 272); // MICRO

    std::vector<std::string> reloaded;
    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& name, const std::string&) { reloaded.push_back(name); });
    hr.start({"ui_xml"}, 10000);

    // The base copy is shadowed by micro/ — reloading it would swap the live
    // component for a layout this display is not using.
    overwrite_xml(base_path(), "<component><view name=\"controls_panel\"/><!--base--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.empty());

    // The micro copy is the one in effect, so it reloads.
    overwrite_xml(micro_path(),
                  "<component><view name=\"controls_panel\"/><!--micro--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0] == "controls_panel");

    hr.stop();
}

TEST_CASE_METHOD(HotReloadVariantFixture,
                 "standard layout reloads the base copy and ignores the micro override",
                 "[hot-reload]") {
    helix::LayoutManager::instance().init(800, 480); // STANDARD

    std::vector<std::string> reloaded;
    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& name, const std::string&) { reloaded.push_back(name); });
    hr.start({"ui_xml"}, 10000);

    overwrite_xml(micro_path(),
                  "<component><view name=\"controls_panel\"/><!--micro--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.empty());

    overwrite_xml(base_path(), "<component><view name=\"controls_panel\"/><!--base--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0] == "controls_panel");

    hr.stop();
}

// ============================================================================
// Non-reloadable components [hot-reload]
// ============================================================================

TEST_CASE_METHOD(HotReloadFixture, "globals.xml is never reloaded", "[hot-reload]") {
    // globals owns every XML subject and the runtime theme constants;
    // unregistering it frees storage LVGL does not own.
    create_xml("globals.xml", "<component><view/></component>");
    create_xml("home_panel.xml", "<component><view/></component>");

    std::vector<std::string> reloaded;
    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& name, const std::string&) { reloaded.push_back(name); });
    hr.start({temp_dir_.string()}, 10000);

    overwrite_xml(temp_dir_ / "globals.xml", "<component><view/><!--edited--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.empty());

    // A normal component in the same scan still reloads.
    overwrite_xml(temp_dir_ / "home_panel.xml", "<component><view/><!--edited--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0] == "home_panel");

    // The skip is permanent, not a one-poll deferral.
    overwrite_xml(temp_dir_ / "globals.xml", "<component><view/><!--again--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.size() == 1);

    hr.stop();
}

TEST_CASE_METHOD(HotReloadFixture, "components with C++-injected constants are never reloaded",
                 "[hot-reload]") {
    create_xml("color_picker.xml", "<component><view/></component>");
    create_sub_xml("color_swatch_grid.xml", "<component><view/></component>");

    std::vector<std::string> reloaded;
    helix::XmlHotReloader hr;
    hr.set_reload_callback(
        [&](const std::string& name, const std::string&) { reloaded.push_back(name); });
    hr.start({temp_dir_.string()}, 10000);

    overwrite_xml(temp_dir_ / "color_picker.xml", "<component><view/><!--edited--></component>");
    overwrite_xml(sub_dir_ / "color_swatch_grid.xml",
                  "<component><view/><!--edited--></component>");
    hr.scan_and_reload();
    REQUIRE(reloaded.empty());

    hr.stop();
}
