// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_thumbnail_integration.cpp
 * @brief Both active-print thumbnail consumers driven against ONE PrinterState.
 *
 * Every other test in this area drives a single producer or a single consumer in
 * isolation, which is why a wrong-thumbnail bug survived a green suite: the
 * defect only exists in the wiring. `ActivePrintMediaManager` publishes the
 * shared `print_thumbnail_path` subject, `PrintStatusPanel` subscribes to it,
 * and both also observe `print_filename` — so the ordering between them is part
 * of the behaviour, not an implementation detail.
 *
 * These cases run the configuration that actually ships (one manager + one panel
 * + one PrinterState) across print A -> idle -> print B, and pin:
 *   - both consumers show A while A is printing;
 *   - the display is deliberately PRESERVED when the filename goes empty
 *     (post-cancel UX — the user should still see what was printing);
 *   - A's image never survives into B, on either consumer;
 *   - a leftover path from a print this manager never processed is not adopted.
 *
 * Assertions read the panel's real widget src via PrintStatusPanelTestAccess,
 * not a screenshot, and not the panel's own bookkeeping alone.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/active_print_media_manager_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "active_print_media_manager.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"
#include "printer_state.h"
#include "thumbnail_processor.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// Real, distinct assets so lv_image_set_src resolves a decoder instead of
// logging a miss. Deliberately NOT benchy_thumbnail_white.png, which is the
// no-thumbnail placeholder — a test that used it could not tell "print A's
// image" apart from "nothing to show".
constexpr const char* THUMB_A = "A:assets/images/printer.png";
constexpr const char* THUMB_B = "A:assets/images/folder.png";

/// File API that answers metadata inline with a record carrying no
/// thumbnails — the customized-Moonraker condition (a fork whose metadata
/// path whitelist-drops them). A real router marshals this callback to the
/// main thread; firing inline keeps the test single-threaded, which is also
/// where the production callback applies its result.
class NoThumbnailMetadataFileAPI : public MoonrakerFileAPI {
  public:
    using MoonrakerFileAPI::MoonrakerFileAPI;

    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error, bool silent = false) override {
        (void)silent;
        last_filename_ = filename;
        if (fail_metadata_) {
            MoonrakerError err;
            err.message = "metadata endpoint unavailable";
            if (on_error) {
                on_error(err);
            }
            return;
        }
        if (on_success) {
            on_success(metadata_);
        }
    }

    /// Filename of the most recent metadata request.
    [[nodiscard]] const std::string& last_filename() const {
        return last_filename_;
    }

    /// Fail every metadata request — the broken-endpoint shape, where the
    /// retry ladder burns out and the give-up path is the only one left.
    void set_fail_metadata(bool fail) {
        fail_metadata_ = fail;
    }

  private:
    FileMetadata metadata_; ///< default record: no thumbnails, no layer count
    std::string last_filename_;
    bool fail_metadata_ = false;
};

/// Transfer mock that counts partial downloads (the lane header extraction
/// rides) and can serve canned gcode content instead of hitting disk, so a
/// test can hand the manager a file with nothing embedded.
class CountingTransferAPIMock : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error) override {
        ++partial_downloads_;
        if (fail_next_) {
            fail_next_ = false;
            MoonrakerError err;
            err.message = "simulated transient download failure";
            if (on_error) {
                on_error(err);
            }
            return;
        }
        if (!canned_content_.empty()) {
            if (on_success) {
                on_success(canned_content_);
            }
            return;
        }
        MoonrakerFileTransferAPIMock::download_file_partial(root, path, max_bytes, on_success,
                                                            on_error);
    }

    [[nodiscard]] int partial_downloads() const {
        return partial_downloads_;
    }

    /// Serve this exact content for every partial download (empty = serve
    /// real files from assets/test_gcodes/).
    void serve_canned_content(const std::string& content) {
        canned_content_ = content;
    }

    /// Fail exactly the next partial download (a WiFi blip shape), then
    /// resume serving normally.
    void fail_next_partial_download() {
        fail_next_ = true;
    }

  private:
    int partial_downloads_ = 0;
    bool fail_next_ = false;
    std::string canned_content_;
};

/// API for the self-serve cases: metadata from the no-thumbnail file API
/// above, transfers from CountingTransferAPIMock (real files under
/// assets/test_gcodes/, or canned content on request).
class SelfServeMockAPI : public MoonrakerAPI {
  public:
    SelfServeMockAPI(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        file_api_ = std::make_unique<NoThumbnailMetadataFileAPI>(client);
        file_transfer_api_ = std::make_unique<CountingTransferAPIMock>(client, get_http_base_url());
    }

    NoThumbnailMetadataFileAPI& meta_files() {
        return static_cast<NoThumbnailMetadataFileAPI&>(*file_api_);
    }

    CountingTransferAPIMock& transfers() {
        return static_cast<CountingTransferAPIMock&>(*file_transfer_api_);
    }
};

/// One PrinterState, one ActivePrintMediaManager, one PrintStatusPanel.
///
/// The state is OWNED here rather than shared with the process-wide instance:
/// the panel's observers are registered against whatever reference it is handed,
/// and a global whose subjects were never init_subjects()-ed reads "" for every
/// string, which makes every assertion below pass vacuously.
///
/// Consumers are brought up by start_consumers() rather than in the constructor
/// so a case can seed the subject BEFORE the manager exists — that is the
/// reconnect-mid-print shape, and it is a different bug from A -> B.
struct ActivePrintThumbnailFixture : public LVGLTestFixture {
    ActivePrintThumbnailFixture() {
        // register_xml=false keeps these subjects out of the process-wide XML
        // registry, which outlives this stack frame.
        state_.init_subjects(false);
    }

    ~ActivePrintThumbnailFixture() override {
        panel_.reset();
        media_.reset();
        api_.reset();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void start_consumers() {
        media_ = std::make_unique<ActivePrintMediaManager>(state_);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        // Stands in for the XML build. The panel's observer only touches the
        // image when this pointer is non-null, so leaving it null would skip
        // the very code path under test.
        PrintStatusPanelTestAccess::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
        drain();
    }

    /// Wire a mock API (metadata with no thumbnails, gcode transfers from
    /// assets/test_gcodes/) into the manager, then bring up the consumers.
    SelfServeMockAPI& start_consumers_with_api() {
        api_ = std::make_unique<SelfServeMockAPI>(client_, state_);
        start_consumers();
        media().set_api(api_.get());
        drain();
        return *api_;
    }

    /// Deliver a print_stats.filename update the way Moonraker does.
    void set_print_filename(const std::string& filename) {
        nlohmann::json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    /// Async work (the panel's deferred filename observer, queued subject
    /// writes) only lands on a queue tick [L048].
    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Settle every hop a self-serve thumbnail load takes: the prescale pool
    /// task ThumbnailCache queues, and the queued publishes a drained
    /// callback commits. Join the pool first, then drain — repeated, since a
    /// drained callback can commit further pool work. (Model:
    /// ActivePrintMediaAsyncFixture::drain in test_active_print_media_manager.cpp.)
    void settle() {
        auto& processor = helix::ThumbnailProcessor::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        for (int pass = 0; pass < 4; ++pass) {
            processor.wait_for_completion();
            if (processor.pending_tasks() == 0 &&
                helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
                break;
            }
            helix::ui::UpdateQueueTestAccess::drain_all(queue);
        }
    }

    /// Simulate a Moonraker notification arriving (fires the persistent
    /// method callbacks the manager registered on the client).
    void fire_notification(const std::string& method, const json& msg) {
        MoonrakerClientTestAccess::fire_method_callbacks(client_, method, msg);
    }

    std::string subject_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

    std::string panel_src() const {
        return PrintStatusPanelTestAccess::displayed_src(*panel_);
    }

    PrinterState& state() {
        return state_;
    }
    ActivePrintMediaManager& media() {
        return *media_;
    }
    PrintStatusPanel& panel() {
        return *panel_;
    }

    PrinterState state_;
    MoonrakerClientMock client_;
    std::unique_ptr<SelfServeMockAPI> api_;
    std::unique_ptr<ActivePrintMediaManager> media_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: print B never displays print A's image",
                 "[print_status][thumbnail][integration]") {
    start_consumers();

    // --- Print A starts and its thumbnail resolves -------------------------
    set_print_filename("model_a.gcode");
    // Stands in for the resolved thumbnail (PrintStartController's USB pre-set
    // takes this exact route; a Moonraker fetch lands on the same subject).
    media().set_thumbnail_path("model_a.gcode", THUMB_A);
    drain();

    REQUIRE(state().get_print_thumbnail_file() == "model_a.gcode");
    REQUIRE(subject_path() == THUMB_A);
    REQUIRE(panel_src() == THUMB_A);
    REQUIRE(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_a.gcode");

    // --- Print A ends: the display is deliberately preserved ---------------
    // Klipper reports an empty filename on cancel/complete. Both consumers must
    // keep showing what was printing; clearing here is what made a cancelled
    // print flash to a blank card before the user could read it.
    set_print_filename("");

    CHECK(subject_path() == THUMB_A);
    CHECK(panel_src() == THUMB_A);

    // --- Print B starts, thumbnail not resolved yet ------------------------
    set_print_filename("model_b.gcode");

    // The subject must no longer attribute anything to A: identity moves to B,
    // and A's path is gone. Whatever stands in for "nothing yet" (empty string
    // or an explicit placeholder), it is not A's image.
    CHECK(state().get_print_thumbnail_file() == "model_b.gcode");
    CHECK(subject_path() != THUMB_A);
    // "Nothing yet" is the placeholder, published explicitly, so the panel
    // actually repaints instead of leaving A's pixels on B's card.
    CHECK(subject_path() == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(panel_src() == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(PrintStatusPanelTestAccess::cached_thumbnail_path(panel()) != THUMB_A);

    // --- Print B's thumbnail resolves --------------------------------------
    media().set_thumbnail_path("model_b.gcode", THUMB_B);
    drain();

    CHECK(subject_path() == THUMB_B);
    CHECK(panel_src() == THUMB_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_b.gcode");

    // --- A stale in-flight result for A lands late -------------------------
    // A fetch started for A can still complete after B took over. The panel
    // compares the identity the subject carries against the file it is showing,
    // so this must not repaint B's card with A's image.
    state().set_print_thumbnail("model_a.gcode", THUMB_A);
    drain();

    CHECK(panel_src() == THUMB_B);
    CHECK(PrintStatusPanelTestAccess::displayed_file(panel()) == "model_b.gcode");
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a leftover from a print the manager never saw is dropped",
                 "[print_status][thumbnail][integration]") {
    // Reconnect / restart mid-print: the subject already holds the previous
    // print's path and identity, but this manager has no history of its own.
    // The clear must key off the path's identity, not off whether THIS manager
    // has loaded anything before.
    state().set_print_thumbnail("model_a.gcode", THUMB_A);

    start_consumers();
    REQUIRE(subject_path() == THUMB_A);

    set_print_filename("model_b.gcode");

    CHECK(state().get_print_thumbnail_file() == "model_b.gcode");
    CHECK(subject_path() != THUMB_A);
    CHECK(panel_src() != THUMB_A);
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a file with no thumbnail shows the placeholder on "
                 "every consumer",
                 "[print_status][thumbnail][integration]") {
    start_consumers();

    set_print_filename("model_a.gcode");
    media().set_thumbnail_path("model_a.gcode", THUMB_A);
    drain();
    REQUIRE(panel_src() == THUMB_A);

    // Print B has no thumbnail: nothing pre-set, and no API to fetch one.
    set_print_filename("no_thumb.gcode");

    const std::string shown = subject_path();
    CHECK_FALSE(shown.empty());
    CHECK(shown == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(panel_src() == shown);
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: the shared subject is never the empty string",
                 "[print_status][thumbnail][integration]") {
    // This is the invariant that lets all three consumers drop their
    // empty-string branches. It is not cosmetic: lv_image_set_src("") has a
    // first byte of 0x00, which lv_image_src_get_type classifies as
    // LV_IMAGE_SRC_VARIABLE, so LVGL dereferences the one-byte literal as an
    // lv_image_dsc_t. A consumer without a guard is only safe if the subject
    // genuinely never carries "".
    CHECK(subject_path() == ActivePrintMediaManager::no_thumbnail_placeholder());

    start_consumers();
    CHECK_FALSE(subject_path().empty());

    // Print starts, nothing resolved yet.
    set_print_filename("model_a.gcode");
    CHECK_FALSE(subject_path().empty());

    // Print ends.
    set_print_filename("");
    CHECK_FALSE(subject_path().empty());
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: metadata without thumbnails self-serves from the "
                 "gcode header",
                 "[print_status][thumbnail][integration]") {
    // The customized-Moonraker shape: the metadata record exists but carries
    // no thumbnails, while the gcode file itself has embedded ones. The
    // manager must extract them client-side rather than park on the
    // placeholder for the whole print.
    SelfServeMockAPI& api = start_consumers_with_api();

    set_print_filename("3DBenchy.gcode");
    settle();

    // The branch precondition actually held: metadata was asked for this file
    // and answered without thumbnails.
    REQUIRE(api.meta_files().last_filename() == "3DBenchy.gcode");

    CHECK(subject_path() != ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK_FALSE(subject_path().empty());
    CHECK(state().get_print_thumbnail_file() == "3DBenchy.gcode");
    CHECK(panel_src() == subject_path());
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a metadata error self-serves from the gcode header "
                 "without waiting out the ladder",
                 "[print_status][thumbnail][integration]") {
    // The broken-endpoint shape: metadata errors on every attempt (a
    // customized fork can 404 the endpoint outright). Waiting for the retry
    // ladder to give up costs minutes of placeholder per print; the first
    // error must already self-serve from the gcode header.
    SelfServeMockAPI& api = start_consumers_with_api();
    api.meta_files().set_fail_metadata(true);

    set_print_filename("3DBenchy.gcode");
    settle();

    REQUIRE(api.meta_files().last_filename() == "3DBenchy.gcode");
    REQUIRE(api.transfers().partial_downloads() >= 1);

    REQUIRE(ActivePrintMediaManagerTestAccess::thumbnail_loaded(media()));
    CHECK(subject_path() != ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(state().get_print_thumbnail_file() == "3DBenchy.gcode");
    CHECK(panel_src() == subject_path());
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a failed header extraction is not retried down the "
                 "ladder",
                 "[print_status][thumbnail][integration]") {
    // A file with nothing embedded fails extraction permanently. The failure
    // must be remembered: one header download for the whole retry ladder
    // (error retries, the give-up path), not one per attempt.
    SelfServeMockAPI& api = start_consumers_with_api();
    api.meta_files().set_fail_metadata(true);
    api.transfers().serve_canned_content("; no thumbnails here\nG28\n");

    set_print_filename("no_thumb.gcode");
    settle();

    // The early self-serve ran once...
    REQUIRE(api.transfers().partial_downloads() == 1);

    // ...and walking the whole ladder (9 backoff retries, then give-up) must
    // not download the header again.
    ActivePrintMediaManager& mgr = media();
    while (ActivePrintMediaManagerTestAccess::has_pending_retry(mgr)) {
        REQUIRE(ActivePrintMediaManagerTestAccess::fire_pending_retry(mgr));
        settle();
    }
    settle();

    CHECK(api.transfers().partial_downloads() == 1);
    CHECK_FALSE(ActivePrintMediaManagerTestAccess::thumbnail_loaded(mgr));
    CHECK(subject_path() == ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(state().get_print_thumbnail_file() == "no_thumb.gcode");
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a transient extraction failure does not poison "
                 "self-serve",
                 "[print_status][thumbnail][integration]") {
    // A failed header DOWNLOAD is transient (WiFi blip, busy executor). Only
    // the permanent verdict — the header was read and has nothing embedded —
    // may stop self-serve from retrying: on a broken-metadata fork the
    // extracted header is the only thumbnail source there is.
    SelfServeMockAPI& api = start_consumers_with_api();
    api.meta_files().set_fail_metadata(true);
    api.transfers().fail_next_partial_download();

    set_print_filename("3DBenchy.gcode");
    settle();

    // First self-serve attempted and failed transiently.
    REQUIRE(api.transfers().partial_downloads() == 1);
    REQUIRE_FALSE(ActivePrintMediaManagerTestAccess::thumbnail_loaded(media()));

    // The ladder's first retry re-attempts extraction (not poisoned) and the
    // file has thumbnails: it must publish.
    REQUIRE(ActivePrintMediaManagerTestAccess::has_pending_retry(media()));
    ActivePrintMediaManagerTestAccess::fire_pending_retry(media());
    settle();

    CHECK(api.transfers().partial_downloads() == 2);
    REQUIRE(ActivePrintMediaManagerTestAccess::thumbnail_loaded(media()));
    CHECK(subject_path() != ActivePrintMediaManager::no_thumbnail_placeholder());
    CHECK(state().get_print_thumbnail_file() == "3DBenchy.gcode");
}

TEST_CASE_METHOD(ActivePrintThumbnailFixture,
                 "Active print thumbnail: a completed self-serve is not re-downloaded by "
                 "later reload triggers",
                 "[print_status][thumbnail][integration]") {
    // Once the extracted thumbnail is displayed (Fetched), the reload
    // triggers that keep firing while metadata stays broken must not
    // re-download and re-prescale the same header. The notification routes
    // carry their own Fetched guards upstream; the live route for the gate
    // under test is rearm at print-start confirmation with layers still
    // missing (the broken-metadata fork shape), which re-enters the load,
    // hits the metadata error, and reaches self-serve with origin Fetched.
    SelfServeMockAPI& api = start_consumers_with_api();
    api.meta_files().set_fail_metadata(true);

    set_print_filename("3DBenchy.gcode");
    settle();

    REQUIRE(ActivePrintMediaManagerTestAccess::thumbnail_loaded(media()));
    REQUIRE(api.transfers().partial_downloads() == 1);

    // Upstream-gated routes (pin those guards too).
    fire_notification("notify_filelist_changed",
                      {{"params", json::array({{{"action", "modify_file"},
                                                {"item", {{"path", "3DBenchy.gcode"}}}}})}});
    settle();
    fire_notification("notify_klippy_ready", {{"params", json::array({})}});
    settle();
    CHECK(api.transfers().partial_downloads() == 1);

    // The ungated-for-self-serve route: rearm with layers missing reloads,
    // the metadata error body reaches self_serve_from_gcode, and the Fetched
    // gate must stop it there.
    ActivePrintMediaManagerTestAccess::rearm_media(media());
    settle();

    CHECK(api.transfers().partial_downloads() == 1);
    CHECK(ActivePrintMediaManagerTestAccess::thumbnail_loaded(media()));
    CHECK(subject_path() != ActivePrintMediaManager::no_thumbnail_placeholder());
}
