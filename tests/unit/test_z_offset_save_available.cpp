// SPDX-License-Identifier: GPL-3.0-or-later
//
// The z_offset_save_available subject: the single published answer to "is there
// an offset worth saving". Every save surface binds it, and the Controls panel's
// click handler asks the same question through save_available(), so a surface
// cannot offer a save the save path calls unnecessary.
//
// What these pin is that the subject tracks all three inputs. A publisher wired
// to only the machine-wide offset passes a global-dirty test and still leaves a
// tool-only adjustment with no save affordance anywhere.

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "tool_state.h"
#include "z_offset_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using helix::ToolState;
using nlohmann::json;

namespace {

class SaveAvailableFixture : public LVGLTestFixture {
  public:
    SaveAvailableFixture() {
        // Order mirrors SubjectInitializer: PrinterState, then ToolState, then
        // the derived subject that observes both.
        get_printer_state().deinit_subjects();
        get_printer_state().init_subjects(true);

        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);

        // One tool, so set_tool_offset_local() has somewhere to land.
        helix::PrinterDiscovery hw;
        json objects = json::array({"gcode_move"});
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);

        helix::zoffset::deinit_save_available_subject();
        helix::zoffset::init_save_available_subject(true);
    }

    ~SaveAvailableFixture() override {
        helix::zoffset::deinit_save_available_subject();
        ToolState::instance().deinit_subjects();
        get_printer_state().deinit_subjects();
    }

    /// The published subject's value. Fails loudly rather than defaulting, so a
    /// publisher that never registered cannot read as "not available".
    int published() {
        lv_subject_t* subj = helix::zoffset::get_save_available_subject();
        REQUIRE(subj != nullptr);
        return lv_subject_get_int(subj);
    }

    /// The name XML binds. Separate from published() because a subject that
    /// exists but was never registered leaves every binding inert.
    int published_via_xml_name() {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, "z_offset_save_available");
        REQUIRE(subj != nullptr);
        return lv_subject_get_int(subj);
    }

    void set_global_offset_mm(double mm) {
        get_printer_state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
    }

    void set_tool_dirty(bool dirty) {
        if (dirty) {
            ToolState::instance().set_tool_offset_local(0, helix::Axis::Z, 60);
        } else {
            ToolState::instance().mark_tool_offsets_saved(0);
        }
    }

    void set_firmware_auto_saves(bool auto_saves) {
        lv_subject_set_int(get_printer_state().get_z_offset_can_save_subject(), auto_saves ? 0 : 1);
    }
};

} // namespace

TEST_CASE_METHOD(SaveAvailableFixture, "z_offset_save_available: clean printer offers no save",
                 "[zoffset][save-rule][subject]") {
    CHECK(published() == 0);
    CHECK(published_via_xml_name() == 0);
}

TEST_CASE_METHOD(SaveAvailableFixture,
                 "z_offset_save_available: the machine-wide offset raises and clears it",
                 "[zoffset][save-rule][subject]") {
    set_global_offset_mm(-0.15);
    CHECK(published() == 1);

    set_global_offset_mm(0.0);
    CHECK(published() == 0);
}

TEST_CASE_METHOD(SaveAvailableFixture,
                 "z_offset_save_available: a dirty tool raises it with the global at zero",
                 "[zoffset][save-rule][subject]") {
    // SET_TOOL_PARAMETER is runtime-only. With no affordance here the tool
    // offset dies at the next Klipper restart with nothing on screen to say so.
    set_tool_dirty(true);
    CHECK(lv_subject_get_int(
              ToolState::instance().get_any_tool_axis_dirty_subject(helix::Axis::Z)) == 1);
    CHECK(published() == 1);
    CHECK(published_via_xml_name() == 1);

    set_tool_dirty(false);
    CHECK(published() == 0);
}

TEST_CASE_METHOD(SaveAvailableFixture,
                 "z_offset_save_available: both dirty stays up until both are clean",
                 "[zoffset][save-rule][subject]") {
    set_global_offset_mm(-0.15);
    set_tool_dirty(true);
    CHECK(published() == 1);

    set_global_offset_mm(0.0);
    CHECK(published() == 1); // the tool is still unsaved

    set_tool_dirty(false);
    CHECK(published() == 0);
}

TEST_CASE_METHOD(SaveAvailableFixture,
                 "z_offset_save_available: firmware that auto-saves suppresses every case",
                 "[zoffset][save-rule][subject]") {
    set_global_offset_mm(-0.15);
    set_tool_dirty(true);
    REQUIRE(published() == 1);

    set_firmware_auto_saves(true);
    CHECK(published() == 0);

    set_firmware_auto_saves(false);
    CHECK(published() == 1);
}

TEST_CASE_METHOD(SaveAvailableFixture,
                 "z_offset_save_available: published for state that was already dirty at init",
                 "[zoffset][save-rule][subject]") {
    // The publisher owns no seeding of its own: it relies on LVGL notifying an
    // observer once at attach. A reconnect lands the offset before the UI
    // exists, so a publisher that waited for the next write would leave the
    // button hidden over an unsaved offset. This pins that contract.
    set_global_offset_mm(-0.15);
    helix::zoffset::deinit_save_available_subject();
    helix::zoffset::init_save_available_subject(true);

    CHECK(published() == 1);
}

TEST_CASE_METHOD(SaveAvailableFixture, "save_available(): the click handler and the subject agree",
                 "[zoffset][save-rule][subject]") {
    CHECK_FALSE(helix::zoffset::save_available());

    set_tool_dirty(true);
    CHECK(helix::zoffset::save_available());
    CHECK(published() == 1);

    set_firmware_auto_saves(true);
    CHECK_FALSE(helix::zoffset::save_available());
    CHECK(published() == 0);
}
