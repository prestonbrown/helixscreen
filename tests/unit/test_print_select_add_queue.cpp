// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_add_queue.cpp
 * @brief Add to Queue's in-flight guard and failure surfacing.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][add_queue]"
 *
 * add_to_queue() puts one add_job request on the wire and disables the
 * button for the flight: a second tap before the callback lands must not
 * queue the file twice, and a refused add must re-enable the button and say
 * so. The mock dispatches inline while the panel marshals its callbacks
 * through UpdateQueue, so the flight window is observable without sleeps:
 * call add_to_queue() twice before draining.
 */

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "printer_state.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

/// Collects the user-facing error toasts: toasts are compiled out of the
/// test build, so the hook is the only observable.
struct ErrorLog {
    std::vector<std::string> messages;

    ErrorLog() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& message) { messages.push_back(message); });
    }
    ~ErrorLog() {
        helix::ui::set_test_notification_error_hook(nullptr);
    }
    ErrorLog(const ErrorLog&) = delete;
    ErrorLog& operator=(const ErrorLog&) = delete;

    bool contains(const std::string& needle) const {
        for (const auto& m : messages) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

/// Holds the machine busy with the job_queue component present — the Add to
/// Queue state — over the real panel and a live JobQueueState, which
/// add_to_queue() requires on both its success and error paths.
class AddQueueFixture : private helix::PrintSelectGlobalStateReset,
                        public helix::PrintSelectPanelFixture {
  public:
    AddQueueFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered) {
        auto& ps = get_printer_state();

        previous_jqs_ = get_job_queue_state();
        jqs_ = std::make_unique<JobQueueState>(api_.get(), &mock_client_);
        set_job_queue_state(jqs_.get());
        drain();

        // Open a real file's detail view, then put the machine in the
        // queueing state the way a running print does.
        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();

        set_wire_state(ps, PrintJobState::PRINTING);
        ps.capabilities_state().set_job_queue_available(true);
        drain();
    }

    ~AddQueueFixture() override {
        auto& ps = get_printer_state();
        set_wire_state(ps, PrintJobState::STANDBY);
        ps.capabilities_state().set_job_queue_available(false);
        drain();
        set_job_queue_state(previous_jqs_);
        jqs_.reset();
        drain();
    }

    /// How many queued jobs carry this filename (the mock answers
    /// synchronously, so a capturing lambda is safe here).
    int queued_count_for(const std::string& filename) {
        int count = 0;
        api_->queue().get_queue_status(
            [&count, &filename](const JobQueueStatus& status) {
                for (const auto& job : status.queued_jobs) {
                    if (job.filename == filename) {
                        ++count;
                    }
                }
            },
            [](const MoonrakerError& err) { FAIL(err.message); });
        return count;
    }

    /// Makes the mock refuse every add_job, answering error_cb where the
    /// stock handler succeeds.
    void refuse_post_job() {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "server.job_queue.post_job",
            [](MoonrakerClientMock*, const json&,
               helix::SharedCallback<void(const json&)> /*success_cb*/,
               helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
                if (error_cb) {
                    error_cb(MoonrakerError::unknown("refused", "server.job_queue.post_job"));
                }
                return true;
            });
    }

    /// Makes the mock hold every add_job answer until the test releases it,
    /// keeping the flight open across drains the way a real wire does.
    struct HeldPostJob {
        helix::SharedCallback<void(const json&)> success_cb;
    };
    void hold_post_job(HeldPostJob& held) {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "server.job_queue.post_job",
            [&held](MoonrakerClientMock*, const json&,
                    helix::SharedCallback<void(const json&)> success_cb,
                    helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                held.success_cb = std::move(success_cb);
                return true;
            });
    }

    int can_print() const {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_can_print"));
    }

    std::string blocked_reason() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "print_select_blocked_reason");
        return s ? lv_subject_get_string(s) : std::string("<missing>");
    }

    helix::PlantedGcode file_{"add_queue_tap.gcode"};

  private:
    JobQueueState* previous_jqs_ = nullptr;
    std::unique_ptr<JobQueueState> jqs_;
};

} // namespace

TEST_CASE_METHOD(AddQueueFixture, "a second Add to Queue tap during the flight is a no-op",
                 "[job_queue][add_queue]") {
    REQUIRE(queued_count_for(file_.name()) == 0);

    ::PrintSelectPanelTestAccess::add_to_queue(*panel_);
    REQUIRE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));

    // The hardware double-tap: still in flight, so nothing new is asked of
    // Moonraker and the button reads disabled with the in-flight caption.
    ::PrintSelectPanelTestAccess::add_to_queue(*panel_);
    CHECK(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 0);
    CHECK(std::string(blocked_reason()) == "Adding to queue...");

    drain();

    CHECK_FALSE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(queued_count_for(file_.name()) == 1);
    CHECK(can_print() == 1);
}

TEST_CASE_METHOD(AddQueueFixture, "a refused add re-enables the button and surfaces the failure",
                 "[job_queue][add_queue]") {
    ErrorLog errors;
    refuse_post_job();

    ::PrintSelectPanelTestAccess::add_to_queue(*panel_);
    REQUIRE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 0);

    drain();

    CHECK_FALSE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 1);
    CHECK(errors.contains("Could not add to queue"));
    CHECK(queued_count_for(file_.name()) == 0);
}

TEST_CASE_METHOD(AddQueueFixture, "the print ending mid-flight leaves the button disabled",
                 "[job_queue][add_queue][button_view]") {
    AddQueueFixture::HeldPostJob held;
    hold_post_job(held);

    ::PrintSelectPanelTestAccess::add_to_queue(*panel_);
    REQUIRE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 0);

    // The print ends while the add is still unanswered: the button must not
    // become a usable Print for the file the add is about to queue.
    set_wire_state(get_printer_state(), PrintJobState::STANDBY);
    drain();

    CHECK(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 0);
    CHECK(blocked_reason() == "Adding to queue...");

    // The add answering re-enables the button on the now-idle machine.
    REQUIRE(held.success_cb);
    json result;
    result["queue_state"] = "ready";
    json jobs = json::array();
    jobs.push_back({{"job_id", "0004"},
                    {"filename", file_.name()},
                    {"time_added", 0.0},
                    {"time_in_queue", 0.0}});
    result["queued_jobs"] = jobs;
    held.success_cb(json{{"result", result}});
    drain();

    CHECK_FALSE(::PrintSelectPanelTestAccess::queue_add_in_flight(*panel_));
    CHECK(can_print() == 1);
}
