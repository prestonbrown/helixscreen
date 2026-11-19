// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_cost.cpp
 * @brief The running print's cost belongs to the running print: a new file
 *        drops the last one's estimate before its own metadata arrives.
 *
 * Run with: ./build/bin/helix-tests "[print_cost][active_print_media]"
 */

#include "../lvgl_test_fixture.h"
#include "active_print_media_manager.h"
#include "core/update_queue.h"
#include "moonraker_types.h"
#include "print_cost_service.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::print_cost::ActivePrintCost;

namespace {

class ActivePrintCostFixture : public LVGLTestFixture {
  public:
    ActivePrintCostFixture() {
        state_.init_subjects(false);
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
        ActivePrintCost::instance().init_subjects();
        helix::SettingsManager::instance().set_cost_price_per_kg("", 20.0);
    }
    ~ActivePrintCostFixture() override {
        helix::SettingsManager::instance().set_cost_price_per_kg("", 0.0);
        manager_.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

  protected:
    void start_print(const std::string& filename) {
        state_.update_from_status(nlohmann::json{{"print_stats", {{"filename", filename}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static int cost_visible() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "print_cost_visible"));
    }

    helix::PrinterState state_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
};

} // namespace

TEST_CASE_METHOD(ActivePrintCostFixture,
                 "ActivePrintCost: a new print drops the last print's estimate before its own "
                 "metadata arrives",
                 "[print_cost][active_print_media]") {
    auto& cost = ActivePrintCost::instance();
    start_print("a.gcode");
    FileMetadata a;
    a.filament_weight_total = 100.0;
    a.modified = 1.0;
    cost.set_file("a.gcode", a);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    cost.update(0.0, 0.5, 0.0);
    REQUIRE(cost_visible() == 1); // print A is priced

    // Print B starts; its metadata never arrives (a failed fetch, a plate-less 3MF).
    start_print("b.gcode");
    CHECK(cost_visible() == 0);

    std::string actual = "unset";
    cost.actual(60.0, 0.0, 1.0, [&actual](const std::string& text) { actual = text; });
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(actual.empty());
}
