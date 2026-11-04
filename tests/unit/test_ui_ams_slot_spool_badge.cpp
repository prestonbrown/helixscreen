// SPDX-License-Identifier: GPL-3.0-or-later

// The ams_slot badge's number is the lane's SPOOL number, and a routing change
// re-numbers lanes far from the one that changed: a lane stops sharing a spool
// and every lane after it shifts, with no status change of its own. The badge
// must follow the spool table (the slots_version subject), not only this
// lane's own status.

#include "ui_ams_slot.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_multiace.h"
#include "ams_state.h"
#include "multiace_status.h"
#include "test_helpers/backend_harness.h"

#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

json ace_fixture() {
    // Tests run from the repo root (make t and the documented direct run
    // alike), and a ccache-served object can carry a __FILE__ spelled
    // relative or from another tree, so the fixture opens cwd-relative first
    // and falls back to the __FILE__ derivation.
    std::ifstream f("tests/fixtures/multiace/head_mode_4ace.json");
    if (!f.is_open()) {
        const std::string src = __FILE__;
        f.open(src.substr(0, src.rfind("/tests/unit/")) +
               "/tests/fixtures/multiace/head_mode_4ace.json");
    }
    REQUIRE(f.is_open());
    json j;
    f >> j;
    return json{{"ace", j}};
}

using MultiAceOnMock = helix::test::StartedBackend<AmsBackendMultiAce>;

lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index_str = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index_str.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

std::string badge_text(lv_obj_t* slot) {
    lv_obj_t* label = lv_obj_find_by_name(slot, "slot_badge_label");
    REQUIRE(label != nullptr);
    return std::string(lv_label_get_text(label));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot badges renumber when routing changes",
                 "[ui][ams_slot][multiace]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<MultiAceOnMock>();
    MultiAceOnMock* backend = owned.get();
    ams.set_backend(std::move(owned));
    ui_ams_slot_register();

    // Head 0 routed to ACE 1 bay 1 and head 2 to ACE 3 bay 4, as the fixture
    // wires them. Badges before: the routed heads wear their bays' numbers;
    // the spool-owning lanes number 1..18 in global order.
    backend->fire_status(ace_fixture());
    ams.sync_from_backend();
    process_lvgl(30);

    lv_obj_t* head0 = create_ams_slot(test_screen(), 0);
    lv_obj_t* head1 = create_ams_slot(test_screen(), 1);
    lv_obj_t* bay0 = create_ams_slot(test_screen(), multiace::bay_global(0, 0));
    REQUIRE(head0 != nullptr);
    REQUIRE(head1 != nullptr);
    REQUIRE(bay0 != nullptr);
    process_lvgl(30);

    CHECK(badge_text(head0) == "3"); // aliases bay (0,0): the third own-spooled lane
    CHECK(badge_text(head1) == "1");
    CHECK(badge_text(bay0) == "3");

    // Unroute head 0: its lane keeps a spool number of its own now, head 1's
    // badge changes, and every lane from bay (0,0) onward shifts up by one -
    // including bay (0,0), whose own status did not move.
    json unrouted = ace_fixture();
    unrouted["ace"]["head_source"]["0"] = nullptr;
    backend->fire_status(unrouted);
    ams.sync_from_backend();
    process_lvgl(30);

    CHECK(badge_text(head0) == "1");
    CHECK(badge_text(head1) == "2");
    CHECK(badge_text(bay0) == "4");

    lv_obj_delete(head0);
    lv_obj_delete(head1);
    lv_obj_delete(bay0);
    process_lvgl(30);
    ams.clear_backends();
}
