// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_detail_gcode_download_integrity.cpp
 * @brief Cache-poisoning guards for the detail view's shared gcode download
 *
 * Pins the final-review findings on the tools-used cache plumbing:
 *
 * 1. A FAILED shared download must NOT persist an authoritative-empty
 *    tools-used cache entry (finish_scan(authoritative=false)) — on 2D-only
 *    platforms no viewer parse ever repairs it. A SUCCESSFUL scan that finds
 *    zero tools (single-extruder file) MUST still persist the empty set.
 * 2. The canonical disk probe must not trust a stale/partial copy when the
 *    expected file size is known — re-download instead of scanning stale
 *    bytes under a new (size, mtime) key. And the oversize viewer reject
 *    must remove the canonical file (bounded by the headless scan still
 *    needing it) so rejected downloads can't pile up on disk.
 * 3. The oversize-reject removal is gated on scan SETTLEMENT, not on
 *    readiness: the preflight safety timeout flips headless_scan_done_
 *    while a download/scan is still in flight (tap Print on a slow oversize
 *    download → timeout → late completion), and that stale done must not
 *    authorize deleting the file under the running scanner — it cannot tell
 *    a deleted file from "no tools used" and would store an
 *    authoritative-empty set (same poison family as 1).
 *
 * Determinism notes:
 * - MoonrakerAPIMock resolves downloads synchronously on the calling thread
 *   (asset copy, or immediate on_error for an unknown filename) — no network.
 *   Test 3 additionally holds the transfer open via DelayedFileTransfers
 *   (opt-in) so the timeout can fire mid-download, then releases it.
 * - HELIX_FORCE_GCODE_MEMORY_FAIL=1 forces is_gcode_2d_streaming_safe() to
 *   fail, keeping the gcode viewer's load out of these tests: they target
 *   the download/scan plumbing, not the render path. This is also the
 *   deterministic stand-in for the oversize reject gate.
 * - The tools scan itself runs on the real HttpExecutor slow lane; readiness
 *    is joined with wait_until() (which drains the UpdateQueue per pass).
 */

#include "ui_callback_helpers.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "tools_used_cache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// No-op stand-ins for the print_file_detail.xml event callbacks (normally
/// registered by PrintSelectPanel's init_subjects — the fixture doesn't init
/// that panel, but the XML references them at create() time).
void detail_noop_cb(lv_event_t* /*e*/) {}

/// Per-test temp cache dir for HELIX_CACHE_DIR — keeps ToolsUsedCache disk
/// state and the shared gcode download out of the real user cache.
/// Saves/restores the env var so later tests in this binary are unaffected.
struct CacheDirGuard {
    std::filesystem::path dir;
    std::string prev_env_;
    bool had_prev_ = false;
    CacheDirGuard()
        : dir(std::filesystem::temp_directory_path() /
              ("detail_gcode_integrity_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir);
        if (const char* old = ::getenv("HELIX_CACHE_DIR")) {
            prev_env_ = old;
            had_prev_ = true;
        }
        ::setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }
    ~CacheDirGuard() {
        if (had_prev_) {
            ::setenv("HELIX_CACHE_DIR", prev_env_.c_str(), 1);
        } else {
            ::unsetenv("HELIX_CACHE_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

/// Save/set/restore one environment variable (used for
/// HELIX_FORCE_GCODE_MEMORY_FAIL — read per-call by memory_utils).
struct EnvGuard {
    std::string name_;
    std::string prev_env_;
    bool had_prev_ = false;
    EnvGuard(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* old = ::getenv(name_.c_str())) {
            prev_env_ = old;
            had_prev_ = true;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    ~EnvGuard() {
        if (had_prev_) {
            ::setenv(name_.c_str(), prev_env_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
};

/// Resolve a test gcode asset the way MoonrakerAPIMock does (repo root,
/// build/, build/bin/ cwd fallbacks) so the test works from any of them.
std::string find_test_asset(const std::string& filename) {
    for (const auto& prefix : {"", "../", "../../"}) {
        std::string path = std::string(prefix) + "assets/test_gcodes/" + filename;
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

/// MoonrakerFileTransferAPIMock with an opt-in transfer HOLD. While
/// hold_transfers is set, download_file_to_path captures the request instead
/// of resolving it; the test later releases the queue to simulate a slow
/// network finishing. Lets the preflight timeout fire genuinely mid-download
/// — the exact compound race the settlement gate guards against.
class DelayedFileTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file_to_path(const std::string& root, const std::string& path,
                               const std::string& dest_path, StringCallback on_success,
                               ErrorCallback on_error, ProgressCallback on_progress) override {
        (void)on_progress;
        if (!hold_transfers) {
            MoonrakerFileTransferAPIMock::download_file_to_path(
                root, path, dest_path, std::move(on_success), std::move(on_error));
            return;
        }
        held_.push_back([this, root, path, dest_path, on_success = std::move(on_success),
                         on_error = std::move(on_error)]() {
            MoonrakerFileTransferAPIMock::download_file_to_path(root, path, dest_path, on_success,
                                                                on_error);
        });
    }

    /// When set, every footer read reports failure. The detail view then
    /// falls back to the whole-file download + Tn scan — the path these
    /// cache-poisoning tests were written against, and the one a slicer that
    /// writes no per-tool usage line still takes in production.
    void download_file_tail(const std::string& root, const std::string& path, size_t max_bytes,
                            StringCallback on_success, ErrorCallback on_error) override {
        if (fail_tail_reads) {
            if (on_error) {
                on_error(MoonrakerError::file_not_found("download_file_tail",
                                                        "footer read disabled by test"));
            }
            return;
        }
        MoonrakerFileTransferAPIMock::download_file_tail(
            root, path, max_bytes, std::move(on_success), std::move(on_error));
    }

    bool hold_transfers = false;
    bool fail_tail_reads = false;

    /// Resolve every held transfer (copies the real asset, fires callbacks
    /// synchronously — same as an unheld call).
    void release_held() {
        auto held = std::move(held_);
        held_.clear();
        for (auto& h : held) {
            h();
        }
    }

  private:
    std::vector<std::function<void()>> held_;
};

/// MoonrakerAPIMock whose transfers() serves the holdable DelayedFileTransfers
/// instance (everything else behaves identically to the plain mock).
class DelayedMoonrakerAPIMock : public MoonrakerAPIMock {
  public:
    DelayedMoonrakerAPIMock(helix::MoonrakerClient& client, helix::PrinterState& state,
                            DelayedFileTransfers& transfers)
        : MoonrakerAPIMock(client, state), transfers_(transfers) {}

    MoonrakerFileTransferAPI& transfers() override {
        return transfers_;
    }

  private:
    DelayedFileTransfers& transfers_;
};

/// LVGL UI fixture + the mock API stack the detail view talks to (mirrors
/// MoonrakerAPIMockTestFixture in test_moonraker_api_mock.cpp), with an
/// opt-in delayed-transfer seam (see DelayedFileTransfers).
class DetailDownloadFixture : public LVGLUITestFixture {
  public:
    DetailDownloadFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false); // no XML bindings in tests
        api_ = std::make_unique<DelayedMoonrakerAPIMock>(client_, state_, transfers_);

        register_xml_callbacks({
            {"on_print_select_detail_backdrop", detail_noop_cb},
            {"on_print_select_print_button", detail_noop_cb},
            {"on_print_select_delete_button", detail_noop_cb},
            {"on_print_detail_back_clicked", detail_noop_cb},
            {"on_toggle_sliced_colors", detail_noop_cb},
        });

        view_.init_subjects();
        REQUIRE(view_.create(test_screen()) != nullptr);
        view_.set_dependencies(api_.get(), &state_);

        ready_ = lv_xml_get_subject(nullptr, "detail_mapping_ready");
        REQUIRE(ready_ != nullptr);
    }

    ~DetailDownloadFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    // Destruction order is reverse-declaration: the view MUST be destroyed
    // before the printer state it observes (production reaches the same
    // ordering via the PrinterState singleton outliving the view).
    MoonrakerClientMock client_;
    PrinterState state_;
    DelayedFileTransfers transfers_{client_, ""};
    std::unique_ptr<MoonrakerAPIMock> api_;
    helix::ui::PrintSelectDetailView view_;
    lv_subject_t* ready_ = nullptr;

    /// The mock fires its transfer callbacks synchronously, but each hop
    /// through ensure_gcode_downloaded / finish_scan crosses the UpdateQueue
    /// via tok.defer — pump a few passes so the whole chain has run.
    void drain_queue_chain() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Production close flow: go_back is deferred, so drain runs
    /// on_deactivate + the destroy-on-close callback before teardown.
    void pop_and_drain() {
        view_.hide();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Canonical shared-download path for `key` — mirrors
    /// PrintSelectDetailView::canonical_gcode_path() (full-path hash).
    std::filesystem::path canonical_path_for(const std::string& key) const {
        return std::filesystem::path(::getenv("HELIX_CACHE_DIR")) / "gcode_temp" /
               ("detail_" + std::to_string(std::hash<std::string>{}(key)) + ".gcode");
    }

    bool ready() const {
        return lv_subject_get_int(ready_) == 1;
    }
};

} // namespace

// ============================================================================
// Fix 1: transient download failure must not poison the persistent cache
// ============================================================================

TEST_CASE_METHOD(DetailDownloadFixture,
                 "failed shared download does not persist a tools-used cache entry",
                 "[print_select][detail_view][gcode_cache]") {
    CacheDirGuard guard;
    constexpr size_t kSize = 7777;
    constexpr time_t kMtime = 42;

    // Unknown filename: the mock fires on_error synchronously — the same
    // transient Moonraker failure the degrade path guards against.
    view_.show("no_such_mock_file.gcode", "", "PLA", {"#FF0000"}, {}, kSize, kMtime);
    drain_queue_chain();

    // The degrade path still resolves readiness (print gate must not hang)…
    REQUIRE(ready());

    // …but the empty set is NOT an answer about this file: nothing may be
    // persisted under its (path, size, mtime) key.
    helix::ToolsUsedCache fresh;
    REQUIRE(fresh.lookup("no_such_mock_file.gcode", kSize, kMtime) == std::nullopt);

    pop_and_drain();
}

TEST_CASE_METHOD(DetailDownloadFixture,
                 "successful scan of a zero-tool file persists the empty set",
                 "[print_select][detail_view][gcode_cache]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    const std::string asset = find_test_asset("3DBenchy.gcode");
    REQUIRE_FALSE(asset.empty()); // tests must run from repo/build cwd
    const auto benchy_size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 42;

    // Pins the whole-file scan path: 3DBenchy's footer WOULD answer {0}, so
    // without this the scan under test never runs.
    transfers_.fail_tail_reads = true;

    // 3DBenchy.gcode has no tool-change lines: a successful scan of it
    // returns an empty set — the legitimate single-extruder answer.
    view_.show("3DBenchy.gcode", "", "PLA", {"#FF0000"}, {}, benchy_size, kMtime);

    // Scan runs on the real slow lane; wait_until drains the queue each pass
    // so finish_scan's deferred body publishes readiness.
    REQUIRE(wait_until([this]() { return ready(); }, 15000));

    helix::ToolsUsedCache fresh;
    const auto got = fresh.lookup("3DBenchy.gcode", benchy_size, kMtime);
    // The empty set must be a HIT, not a miss — else every open of every
    // single-extruder file re-scans, and the fix over-corrected.
    REQUIRE(got.has_value());
    REQUIRE(got->empty());

    pop_and_drain();
}

// ============================================================================
// Fix 2: canonical disk probe + oversize-reject cleanup
// ============================================================================

TEST_CASE_METHOD(DetailDownloadFixture,
                 "stale on-disk copy with a mismatched size is re-downloaded",
                 "[print_select][detail_view][gcode_cache]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    const std::string asset = find_test_asset("3DBenchy.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto benchy_size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 42;

    // Simulate the aftermath of an app kill mid-download: a truncated (or
    // re-slice-stale) file sits at the canonical path.
    const auto canonical = canonical_path_for("3DBenchy.gcode");
    std::filesystem::create_directories(canonical.parent_path());
    {
        std::ofstream junk(canonical, std::ios::binary | std::ios::trunc);
        junk << "T0 ; partial";
    }
    REQUIRE(std::filesystem::file_size(canonical) != benchy_size);

    // The stale-copy probe under test lives in the whole-file download path;
    // a footer read would answer the scan without ever reaching it (and the
    // then-unreferenced file would be reclaimed right after the re-download).
    transfers_.fail_tail_reads = true;

    view_.show("3DBenchy.gcode", "", "PLA", {"#FF0000"}, {}, benchy_size, kMtime);
    drain_queue_chain();

    // The scan finishes on the slow lane; the oversize reject must NOT have
    // removed the file underneath it (scan was still pending at reject time).
    REQUIRE(wait_until([this]() { return ready(); }, 15000));

    // Size mismatch => the stale bytes were dropped and the REAL file was
    // re-downloaded to the canonical path.
    REQUIRE(std::filesystem::exists(canonical));
    REQUIRE(std::filesystem::file_size(canonical) == benchy_size);

    pop_and_drain();
}

TEST_CASE_METHOD(DetailDownloadFixture,
                 "oversize reject removes the canonical file once the scan no longer needs it",
                 "[print_select][detail_view][gcode_cache]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    const std::string asset = find_test_asset("3DBenchy.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto benchy_size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 42;

    // Warm the tools-used cache: show() seeds the scan answer, so the
    // download serves the viewer alone — the reprint-of-an-oversize-file
    // leak scenario (every open re-downloads, viewer rejects, file lingers).
    {
        helix::ToolsUsedCache warmer;
        warmer.store("3DBenchy.gcode", benchy_size, kMtime, {0});
    }

    view_.show("3DBenchy.gcode", "", "PLA", {"#FF0000"}, {}, benchy_size, kMtime);
    REQUIRE(ready()); // cache hit seeded readiness before activation

    drain_queue_chain(); // activation → download → oversize reject → cleanup

    const auto canonical = canonical_path_for("3DBenchy.gcode");
    REQUIRE_FALSE(std::filesystem::exists(canonical));

    pop_and_drain();
}

// ============================================================================
// Fix 3: timeout-fueled readiness must not authorize removal (settlement gate)
// ============================================================================

TEST_CASE_METHOD(DetailDownloadFixture,
                 "preflight timeout readiness does not authorize removing the file under a late "
                 "scan",
                 "[print_select][detail_view][gcode_cache]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    // u1_4color_ring.gcode carries standalone T0–T3 lines: a correct scan
    // yields {0,1,2,3}; a scan whose file was deleted mid-run yields {} —
    // distinguishable, unlike 3DBenchy's legitimate empty set.
    const std::string asset = find_test_asset("u1_4color_ring.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto file_size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 42;
    const std::string key = "u1_4color_ring.gcode";

    // The hole, reproduced deterministically with a held transfer:
    //   1. hold the shared download open (slow oversize file over network)
    //   2. user taps Print → preflight timeout armed
    //   3. timeout expires (virtual clock) → done=true, scan NOT settled
    //   4. download finally completes → scan submits + viewer rejects oversize
    //   5. the reject must keep the file: the scanner is still reading it
    transfers_.hold_transfers = true;
    // The held whole-file transfer IS the mechanism here; a footer read would
    // settle the scan instantly and there would be no race left to test.
    transfers_.fail_tail_reads = true;

    // 1–2. Cold cache: activation queues BOTH waiters on the held transfer.
    view_.show(key, "", "PLA", {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"}, {}, file_size, kMtime);
    drain_queue_chain();
    REQUIRE_FALSE(view_.is_preflight_ready()); // transfer held → nothing settled

    view_.run_when_preflight_ready([]() {}); // the Print tap → arms 8s timer

    // 3. Fire the safety timeout on the virtual clock.
    // (PREFLIGHT_READY_TIMEOUT_MS is private — 8000ms, mirrored here.)
    process_lvgl(8500);
    // The timeout flips readiness for the gate (member, not the subject —
    // the timeout deliberately doesn't republish detail_mapping_ready).
    REQUIRE(view_.is_preflight_ready());
    // …while the scan is UNSETTLED — this combination is the hole.

    // 4. The slow network finishes (held transfer resolves on this thread;
    // its completion crosses the UpdateQueue, and the fan-out submits the
    // slow-lane scan BEFORE the viewer waiter evaluates the oversize reject).
    transfers_.release_held();
    drain_queue_chain();

    // 5. The reject must NOT have removed the file (gate reads settlement,
    // not readiness). Under the old gate (headless_scan_done_) the file is
    // gone here and the in-flight scan stores {} as authoritative.
    const auto canonical = canonical_path_for(key);
    REQUIRE(std::filesystem::exists(canonical));

    // The late scan runs against the INTACT file and persists the real set.
    REQUIRE(wait_until(
        [&]() {
            helix::ToolsUsedCache fresh;
            return fresh.lookup(key, file_size, kMtime).has_value();
        },
        15000));
    helix::ToolsUsedCache fresh;
    const auto got = fresh.lookup(key, file_size, kMtime);
    REQUIRE(got.has_value());
    REQUIRE(*got == std::set<int>{0, 1, 2, 3});

    pop_and_drain();
}

// ============================================================================
// Footer read: the chips' answer without the whole-file download + parse
// ============================================================================

TEST_CASE_METHOD(DetailDownloadFixture,
                 "footer read answers tools_used without downloading the whole file",
                 "[print_select][detail_view][gcode_footer]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    // ssr_heat_sink_orca's footer says `filament used [g] = 0.00, 0.00, 0.00,
    // 0.00, 10.16` — only T4 prints, out of a 5-slot palette.
    const std::string asset = find_test_asset("ssr_heat_sink_orca.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 4242;
    const std::string key = "ssr_heat_sink_orca.gcode";

    // Hold every whole-file transfer: if the footer read cannot answer on its
    // own, nothing below ever resolves. That is the point — this pins that
    // the chips no longer wait on the 4.5 MB download.
    transfers_.hold_transfers = true;

    view_.show(key, "", "PLA", {"#FFFFFF", "#000000", "#00FFFF", "#DFDFDF", "#363636"}, {}, size,
               kMtime);
    drain_queue_chain();

    REQUIRE(ready());
    REQUIRE(view_.get_tools_used() == std::set<int>{4});
    REQUIRE_FALSE(std::filesystem::exists(canonical_path_for(key)));

    // Authoritative: the next open of this file renders final chips instantly.
    helix::ToolsUsedCache fresh;
    const auto cached = fresh.lookup(key, size, kMtime);
    REQUIRE(cached.has_value());
    REQUIRE(*cached == std::set<int>{4});

    transfers_.release_held();
    pop_and_drain();
}

TEST_CASE_METHOD(DetailDownloadFixture, "footer read sizes its window from gcode_end_byte",
                 "[print_select][detail_view][gcode_footer]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    // u1_4color_ring's usage line sits 26 KB from EOF — past a 20 KB guess,
    // inside both the exact gcode_end_byte window and the 64 KB default.
    const std::string asset = find_test_asset("u1_4color_ring.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 4242;
    const std::string key = "u1_4color_ring.gcode";

    transfers_.hold_transfers = true;

    // gcode_end_byte just ahead of the footer, as Moonraker reports it.
    view_.show(key, "", "PLA", {"#E2DEDB", "#080A0D", "#F4C032", "#E72F1D"}, {}, size, kMtime,
               /*gcode_end_byte=*/size - 30'000);
    drain_queue_chain();

    REQUIRE(ready());
    REQUIRE(view_.get_tools_used() == std::set<int>{0, 1, 2, 3});

    transfers_.release_held();
    pop_and_drain();
}

TEST_CASE_METHOD(DetailDownloadFixture,
                 "footer read backfills the palette when metadata carried none",
                 "[print_select][detail_view][gcode_footer]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    // calicat_calico: `filament used [g] = 6.34, 0.00, 0.00, 0.00` and
    // `filament_colour = #E7BD00;#00C502;#F4E2C1;#ED1C24`.
    const std::string asset = find_test_asset("calicat_calico.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 4242;

    transfers_.hold_transfers = true;

    // Empty colors: the Moonraker forks that don't return filament_colors
    // (Snapmaker and friends) — today the palette only arrives with the full
    // viewer parse, which 2D-only platforms never run.
    view_.show("calicat_calico.gcode", "", "PLA", {}, {}, size, kMtime);
    drain_queue_chain();

    REQUIRE(ready());
    REQUIRE(view_.get_tools_used() == std::set<int>{0});

    const auto tools = view_.get_used_tool_info();
    REQUIRE(tools.size() == 1);
    REQUIRE(tools[0].tool_index == 0);
    REQUIRE(tools[0].color_rgb == 0xE7BD00u); // straight out of the footer

    transfers_.release_held();
    pop_and_drain();
}

TEST_CASE_METHOD(DetailDownloadFixture,
                 "a footer read that cannot answer falls back to the full scan",
                 "[print_select][detail_view][gcode_footer]") {
    CacheDirGuard guard;
    EnvGuard mem_fail("HELIX_FORCE_GCODE_MEMORY_FAIL", "1");

    const std::string asset = find_test_asset("u1_4color_ring.gcode");
    REQUIRE_FALSE(asset.empty());
    const auto size = static_cast<size_t>(std::filesystem::file_size(asset));
    constexpr time_t kMtime = 4243;
    const std::string key = "u1_4color_ring.gcode";

    // Transport failure — same shape as a Moonraker that rejects the range
    // request, or a slicer whose footer says nothing about filament use.
    transfers_.fail_tail_reads = true;

    view_.show(key, "", "PLA", {"#E2DEDB", "#080A0D", "#F4C032", "#E72F1D"}, {}, size, kMtime);
    drain_queue_chain();

    // The whole-file scan still produces the answer — never worse than before.
    REQUIRE(wait_until([this]() { return ready(); }, 15000));
    REQUIRE(view_.get_tools_used() == std::set<int>{0, 1, 2, 3});
    REQUIRE(std::filesystem::exists(canonical_path_for(key)));

    pop_and_drain();
}
