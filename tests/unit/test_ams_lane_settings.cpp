// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The rules behind a lane's Endless Spool backup dropdown: when the row shows,
// what it offers, and what a pick is refused for (helix::ui::lane_settings).

#include "ui_ams_lane_settings.h"

#include "../test_helpers/backend_user_edit.h"
#include "ams_backend_mock.h"
#include "ams_types.h"
#include "filament_database.h"
#include "filament_variants.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using namespace helix::ui;

namespace {

// Spells the cases' inputs: a bay list from a slot count, and the noun.
struct Access {
    using BackupEligibleFn = lane_settings::BackupEligibleFn;

    static bool decide_show_backup_row(const EndlessSpoolCapabilities& caps,
                                       const EndlessSpoolConfig& cfg, int slot) {
        return lane_settings::show_backup_row(caps, cfg, slot);
    }
    static std::string build_backup_options_for(int total_slots, int item_index,
                                                const BackupEligibleFn& eligible,
                                                LaneNoun noun = LaneNoun::Slot) {
        std::vector<int> bays;
        for (int i = 0; i < total_slots; ++i) {
            bays.push_back(i);
        }
        return lane_settings::backup_options(
            noun, lane_settings::backup_candidates(bays, item_index), item_index, eligible);
    }
    static std::string build_backup_options_from(const std::vector<int>& candidates,
                                                 int item_index) {
        return lane_settings::backup_options(LaneNoun::Slot, candidates, item_index, nullptr);
    }
    static std::vector<int> backup_candidates_for(const std::vector<int>& bays, int item_index) {
        return lane_settings::backup_candidates(bays, item_index);
    }
    static bool decide_backup_refused(int item_index, int backup_slot,
                                      const BackupEligibleFn& eligible) {
        return lane_settings::backup_refused(item_index, backup_slot, eligible);
    }
};

} // namespace

// A read-only backend's row is only worth drawing on a slot that has a backup:
// anywhere else it reads a permanent "None" that looks like "nothing configured".
TEST_CASE("lane_settings::show_backup_row needs a relation, not just availability",
          "[ams][lane_settings][endless_spool]") {
    using namespace helix::printer;

    // Slot 0 backs onto slot 1; nothing else has an edge.
    const EndlessSpoolConfig none;
    const EndlessSpoolConfig edge_0_to_1 = endless_spool_config_from_edges({1, -1, -1, -1});

    const EndlessSpoolCapabilities unsupported;

    EndlessSpoolCapabilities afc{.availability = EndlessSpoolAvailability::Available,
                                 .enabled = EndlessSpoolEnabled::On,
                                 .editability = EndlessSpoolEditability::PerSlot};

    EndlessSpoolCapabilities hh_multi_unit{.availability = EndlessSpoolAvailability::Available,
                                           .enabled = EndlessSpoolEnabled::On,
                                           .editability = EndlessSpoolEditability::ReadOnly,
                                           .restriction = EndlessSpoolRestriction::MultiUnit};

    EndlessSpoolCapabilities cfs{.availability = EndlessSpoolAvailability::Available,
                                 .enabled = EndlessSpoolEnabled::On,
                                 .editability = EndlessSpoolEditability::ReadOnly,
                                 .restriction = EndlessSpoolRestriction::FirmwareManaged};

    EndlessSpoolCapabilities ad5x_stock{.availability = EndlessSpoolAvailability::Available,
                                        .enabled = EndlessSpoolEnabled::On,
                                        .editability = EndlessSpoolEditability::ReadOnly,
                                        .restriction = EndlessSpoolRestriction::FirmwareManaged,
                                        .provider = "zmod"};

    // No real backend currently reports RequiresPlugin — AD5X stock zMod moved
    // to Available/FirmwareManaged once source read of ANALOG_PRUTOK landed.
    // Kept as a synthetic so the rendering path stays covered for any future
    // backend whose package genuinely can be missing.
    EndlessSpoolCapabilities synthetic_plugin_missing{
        .availability = EndlessSpoolAvailability::RequiresPlugin,
        .enabled = EndlessSpoolEnabled::Off,
        .editability = EndlessSpoolEditability::ReadOnly,
        .restriction = EndlessSpoolRestriction::PluginMissing};

    SECTION("no such feature: never") {
        CHECK_FALSE(Access::decide_show_backup_row(unsupported, none, 0));
        CHECK_FALSE(Access::decide_show_backup_row(unsupported, edge_0_to_1, 0));
    }

    SECTION("plugin not installed: never - there is nothing to configure yet") {
        CHECK_FALSE(Access::decide_show_backup_row(synthetic_plugin_missing, none, 0));
        CHECK_FALSE(Access::decide_show_backup_row(synthetic_plugin_missing, edge_0_to_1, 0));
    }

    SECTION("AD5X stock zMod: same shape as CFS, hides when no relation") {
        // FirmwareManaged + ReadOnly + no per-slot relation -> the CFS rule.
        CHECK_FALSE(Access::decide_show_backup_row(ad5x_stock, none, 0));
    }

    SECTION("editable: always, even before anything is configured") {
        CHECK(Access::decide_show_backup_row(afc, none, 0));
        CHECK(Access::decide_show_backup_row(afc, edge_0_to_1, 0));
        CHECK(Access::decide_show_backup_row(afc, edge_0_to_1, 2));
    }

    SECTION("read-only WITH a relation: shown, so the user can see it") {
        CHECK(Access::decide_show_backup_row(hh_multi_unit, edge_0_to_1, 0));
    }

    SECTION("read-only with NO relation: hidden - this is the CFS fix") {
        // Showing it produced a dropdown stuck on "None" that could never be
        // told apart from "no backup configured".
        CHECK_FALSE(Access::decide_show_backup_row(cfs, none, 0));
    }

    SECTION("read-only: only the slot that HAS an edge shows the row (#1464)") {
        // The CFS fork publishes one edge, for the loaded slot. Every other
        // slot would render a disabled "None" the firmware never stated.
        CHECK(Access::decide_show_backup_row(cfs, edge_0_to_1, 0));
        CHECK_FALSE(Access::decide_show_backup_row(cfs, edge_0_to_1, 1));
        CHECK_FALSE(Access::decide_show_backup_row(cfs, edge_0_to_1, 2));
        CHECK_FALSE(Access::decide_show_backup_row(hh_multi_unit, edge_0_to_1, 3));
    }
}

// Backup eligibility is the BACKEND's rule, reached through
// AmsBackend::endless_spool_backup_eligibility(), so a backend that tightens it
// (AD5X IFS) tightens the "(incompatible)" label too.

TEST_CASE("Backup options are tagged by the backend's eligibility rule",
          "[ams][lane_settings][endless_spool][1250]") {
    // The base-class default, replicated here so the "unchanged for AFC/HH"
    // claim is checked against the rule itself and not against a mock of it.
    // AmsBackend::endless_spool_backup_eligibility() is this function over
    // get_slot_info(slot).material.
    const std::vector<std::string> materials = {"PLA", "PLA", "ABS", ""};
    const auto default_rule = [&materials](int slot, int candidate) {
        if (slot < 0 || candidate < 0 || slot == candidate) {
            return BackupEligibility::Incompatible;
        }
        const std::string& a = materials[static_cast<size_t>(slot)];
        const std::string& b = materials[static_cast<size_t>(candidate)];
        if (a.empty() || b.empty()) {
            return BackupEligibility::Eligible;
        }
        if (!filament::materials_compatible(a, b)) {
            return BackupEligibility::Incompatible;
        }
        return filament::grades_match(a, b) ? BackupEligibility::Eligible
                                            : BackupEligibility::GradeDiffers;
    };

    SECTION("a bay of a box that is not there is not offered") {
        // Bays 0-3 and 8-11 exist; 4-7 belong to an absent box.
        const std::vector<int> bays = {0, 1, 2, 3, 8, 9, 10, 11};
        const auto candidates = Access::backup_candidates_for(bays, 9);
        CHECK(candidates == std::vector<int>{0, 1, 2, 3, 8, 10, 11});
        const auto opts = Access::build_backup_options_from(candidates, 9);
        CHECK(opts.find("Slot 5") == std::string::npos);
        CHECK(opts.find("Slot 9") != std::string::npos);
        CHECK(opts.find("Slot 12") != std::string::npos);
    }

    SECTION("the current slot is skipped and None leads") {
        const auto opts = Access::build_backup_options_for(4, 1, default_rule);
        CHECK(opts.rfind("None", 0) == 0);
        // Slots 1, 3, 4 in 1-based labels — slot index 1 ("Slot 2") is absent.
        CHECK(opts.find("Slot 2") == std::string::npos);
        CHECK(opts.find("Slot 1") != std::string::npos);
        CHECK(opts.find("Slot 3") != std::string::npos);
        CHECK(opts.find("Slot 4") != std::string::npos);
    }

    SECTION("default rule: incompatible material is tagged, unknown material is not") {
        // Slot 0 is PLA. Slot 1 (PLA) compatible; slot 2 (ABS) not; slot 3
        // (unlabelled) counts as eligible rather than blocking a lane the user
        // simply has not filled in yet.
        const auto opts = Access::build_backup_options_for(4, 0, default_rule);
        const auto slot2 = opts.find("Slot 2");
        const auto slot3 = opts.find("Slot 3");
        const auto slot4 = opts.find("Slot 4");
        REQUIRE(slot2 != std::string::npos);
        REQUIRE(slot3 != std::string::npos);
        REQUIRE(slot4 != std::string::npos);

        CHECK(opts.substr(slot2, slot3 - slot2).find("(incompatible)") == std::string::npos);
        CHECK(opts.substr(slot3, slot4 - slot3).find("(incompatible)") != std::string::npos);
        CHECK(opts.substr(slot4).find("(incompatible)") == std::string::npos);
    }

    SECTION("a stricter backend rule reaches the label - the AD5X shape") {
        // Exact material AND exact colour AND port present. All four lanes hold
        // PLA, so the default material rule allows every pairing and tags
        // nothing; AD5X still refuses two of them, and that refusal has to
        // reach the label.
        const std::vector<std::string> ifs_materials = {"PLA", "PLA", "PLA", "PLA"};
        const std::vector<std::string> colors = {"FF0000", "00FF00", "FF0000", "FF0000"};
        const std::vector<bool> present = {true, true, true, false};
        const auto strict = [&](int slot, int candidate) {
            if (slot < 0 || candidate < 0 || slot == candidate) {
                return BackupEligibility::Incompatible;
            }
            const auto s = static_cast<size_t>(slot);
            const auto c = static_cast<size_t>(candidate);
            if (ifs_materials[s].empty() || colors[s].empty()) {
                return BackupEligibility::Incompatible;
            }
            const bool ok =
                present[c] && ifs_materials[c] == ifs_materials[s] && colors[c] == colors[s];
            // A firmware rule has no soft verdict: it either performs the swap
            // or it does not. GradeDiffers is the base rule's to hand out.
            return ok ? BackupEligibility::Eligible : BackupEligibility::Incompatible;
        };
        const auto lenient = [&ifs_materials](int slot, int candidate) {
            if (slot < 0 || candidate < 0 || slot == candidate) {
                return BackupEligibility::Incompatible;
            }
            return filament::materials_compatible(ifs_materials[static_cast<size_t>(slot)],
                                                  ifs_materials[static_cast<size_t>(candidate)])
                       ? BackupEligibility::Eligible
                       : BackupEligibility::Incompatible;
        };

        const auto strict_opts = Access::build_backup_options_for(4, 0, strict);
        const auto lenient_opts = Access::build_backup_options_for(4, 0, lenient);
        CHECK(strict_opts != lenient_opts);
        // All PLA: the old rule had nothing to say about any of these lanes.
        CHECK(lenient_opts.find("(incompatible)") == std::string::npos);

        const auto s2 = strict_opts.find("Slot 2");
        const auto s3 = strict_opts.find("Slot 3");
        const auto s4 = strict_opts.find("Slot 4");
        REQUIRE(s2 != std::string::npos);
        REQUIRE(s3 != std::string::npos);
        REQUIRE(s4 != std::string::npos);
        // Slot index 1: same material, different colour -> refused.
        CHECK(strict_opts.substr(s2, s3 - s2).find("(incompatible)") != std::string::npos);
        // Slot index 2: same material, same colour, port present -> allowed.
        CHECK(strict_opts.substr(s3, s4 - s3).find("(incompatible)") == std::string::npos);
        // Slot index 3: material and colour match, but the port reads empty.
        CHECK(strict_opts.substr(s4).find("(incompatible)") != std::string::npos);
    }

    SECTION("a grade difference gets its own tag, not the incompatible one") {
        // Slot 0 is PLA. Slot 1 is PLA-CF: same polymer, filled, so the swap
        // will work but is worth naming. Slot 2 is ABS and is refused outright.
        const std::vector<std::string> graded = {"PLA", "PLA-CF", "ABS", "PLA"};
        const auto rule = [&graded](int slot, int candidate) {
            if (slot < 0 || candidate < 0 || slot == candidate) {
                return BackupEligibility::Incompatible;
            }
            const std::string& a = graded[static_cast<size_t>(slot)];
            const std::string& b = graded[static_cast<size_t>(candidate)];
            if (!filament::materials_compatible(a, b)) {
                return BackupEligibility::Incompatible;
            }
            return filament::grades_match(a, b) ? BackupEligibility::Eligible
                                                : BackupEligibility::GradeDiffers;
        };

        const auto opts = Access::build_backup_options_for(4, 0, rule);
        const auto s2 = opts.find("Slot 2");
        const auto s3 = opts.find("Slot 3");
        const auto s4 = opts.find("Slot 4");
        REQUIRE(s2 != std::string::npos);
        REQUIRE(s3 != std::string::npos);
        REQUIRE(s4 != std::string::npos);

        const std::string entry2 = opts.substr(s2, s3 - s2);
        const std::string entry3 = opts.substr(s3, s4 - s3);
        const std::string entry4 = opts.substr(s4);

        // PLA-CF: tagged as a grade difference and NOT as incompatible. Both
        // halves matter - the whole point is that this lane stays choosable.
        CHECK(entry2.find("(different grade)") != std::string::npos);
        CHECK(entry2.find("(incompatible)") == std::string::npos);
        // ABS: the hard tag, and not the soft one.
        CHECK(entry3.find("(incompatible)") != std::string::npos);
        CHECK(entry3.find("(different grade)") == std::string::npos);
        // PLA: untagged.
        CHECK(entry4.find("(") == std::string::npos);
    }

    SECTION("no backend: nothing is tagged") {
        // eligibility_of(nullptr) answers Eligible for every pairing.
        const auto always = [](int, int) { return BackupEligibility::Eligible; };
        const auto opts = Access::build_backup_options_for(4, 0, always);
        CHECK(opts.find("(incompatible)") == std::string::npos);
    }

    SECTION("an out-of-menu item index tags nothing rather than indexing garbage") {
        bool called = false;
        const auto spy = [&called](int, int) {
            called = true;
            return BackupEligibility::Incompatible;
        };
        const auto opts = Access::build_backup_options_for(4, -1, spy);
        CHECK_FALSE(called);
        CHECK(opts.find("(incompatible)") == std::string::npos);
    }

    SECTION("every option is spelled in the backend's own word") {
        const auto always = [](int, int) { return BackupEligibility::Eligible; };
        const auto opts = Access::build_backup_options_for(4, 0, always, LaneNoun::Lane);
        // An AFC user picks a backup Lane, not a backup Slot, and the numbers
        // stay 1-based whichever word spells them.
        CHECK(opts.find("Lane 2") != std::string::npos);
        CHECK(opts.find("Lane 3") != std::string::npos);
        CHECK(opts.find("Lane 4") != std::string::npos);
        CHECK(opts.find("Slot") == std::string::npos);
        CHECK(opts.find("Lane 1") == std::string::npos); // the open slot is skipped
    }
}

TEST_CASE("The change handler refuses exactly what the option list tagged",
          "[ams][lane_settings][endless_spool][1250]") {
    const auto refuse_all = [](int, int) { return BackupEligibility::Incompatible; };
    const auto allow_all = [](int, int) { return BackupEligibility::Eligible; };

    SECTION("clearing a backup is always allowed") {
        // "None" needs nothing to be compatible with, so the rule is not even
        // consulted - a backend that refuses everything must not block a clear.
        CHECK_FALSE(Access::decide_backup_refused(0, -1, refuse_all));
    }

    SECTION("an ineligible pairing is refused") {
        CHECK(Access::decide_backup_refused(0, 2, refuse_all));
    }

    SECTION("an eligible pairing goes through") {
        CHECK_FALSE(Access::decide_backup_refused(0, 2, allow_all));
    }

    SECTION("no open slot: nothing to refuse") {
        CHECK_FALSE(Access::decide_backup_refused(-1, 2, refuse_all));
    }

    SECTION("a grade difference is tagged but NOT refused") {
        // The one asymmetry between the label and the refusal, and the reason
        // the verdict is tri-state at all: the user is told, then allowed to go
        // ahead. A refusal here would leave a print dying at a runout with a
        // usable spool sitting in the next lane.
        const auto grade_differs = [](int, int) { return BackupEligibility::GradeDiffers; };
        CHECK_FALSE(Access::decide_backup_refused(0, 2, grade_differs));
    }

    SECTION("label and refusal agree for every pairing under one rule") {
        // The invariant that matters: a slot tagged "(incompatible)" must be the
        // slot the handler bounces, for the same rule, with no third opinion.
        const std::vector<std::string> materials = {"PLA", "ABS", "PETG", "PLA"};
        const auto rule = [&materials](int slot, int candidate) {
            if (slot < 0 || candidate < 0 || slot == candidate) {
                return BackupEligibility::Incompatible;
            }
            return filament::materials_compatible(materials[static_cast<size_t>(slot)],
                                                  materials[static_cast<size_t>(candidate)])
                       ? BackupEligibility::Eligible
                       : BackupEligibility::Incompatible;
        };

        for (int item = 0; item < 4; ++item) {
            const auto opts = Access::build_backup_options_for(4, item, rule);
            for (int cand = 0; cand < 4; ++cand) {
                if (cand == item) {
                    continue;
                }
                const std::string label = "Slot " + std::to_string(cand + 1);
                const auto pos = opts.find(label);
                REQUIRE(pos != std::string::npos);
                const auto end = opts.find('\n', pos);
                const std::string entry = opts.substr(pos, end - pos);
                const bool tagged = entry.find("(incompatible)") != std::string::npos;
                CHECK(tagged == Access::decide_backup_refused(item, cand, rule));
                // The soft tag never appears under a rule that only ever
                // answers Eligible or Incompatible.
                CHECK(entry.find("(different grade)") == std::string::npos);
            }
        }
    }
}

TEST_CASE("The option list is built from the live backend virtual, not a local rule",
          "[ams][lane_settings][endless_spool][integration][1250]") {
    // A pure function is only as good as whether its inputs are the ones it gets
    // at runtime. Production binds `eligible` to
    // AmsBackend::endless_spool_backup_eligibility() on the live backend
    // (lane_settings::eligibility_of()); this binds the same thing, so the
    // assertions below are about real slot data going through the real virtual.
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    auto set_material = [&backend](int slot, const char* material) {
        SlotInfo info = backend.get_slot_info(slot);
        info.material = material;
        info.status = SlotStatus::AVAILABLE;
        REQUIRE(helix::test::apply_edit(backend, slot, info));
    };
    set_material(0, "PLA");
    set_material(1, "PLA");
    set_material(2, "ABS");
    set_material(3, "");

    const Access::BackupEligibleFn live = [&backend](int slot, int candidate) {
        return backend.endless_spool_backup_eligibility(slot, candidate);
    };

    SECTION("the virtual holds the values the call site actually passes") {
        // Guard against the pure function being right about inputs the backend
        // never produces: assert the backend's own view of the same slots.
        CHECK(backend.get_slot_info(0).material == "PLA");
        CHECK(backend.get_slot_info(2).material == "ABS");
        CHECK(backend.get_slot_info(3).material.empty());

        CHECK(live(0, 1) == BackupEligibility::Eligible);     // PLA / PLA
        CHECK(live(0, 2) == BackupEligibility::Incompatible); // PLA / ABS
        // PLA / unlabelled counts as eligible.
        CHECK(live(0, 3) == BackupEligibility::Eligible);
        // A slot is never its own backup, and there is no open slot at -1.
        CHECK(live(0, 0) == BackupEligibility::Incompatible);
        CHECK(live(-1, 1) == BackupEligibility::Incompatible);
    }

    SECTION("the default backend rule tags exactly the incompatible slot") {
        // On same-grade slot data only the polymer half of the base virtual
        // speaks. The grade half only speaks when a filler differs, which these
        // four lanes do not exercise - the grade case has its own section above.
        const auto opts = Access::build_backup_options_for(4, 0, live);
        const auto s2 = opts.find("Slot 2");
        const auto s3 = opts.find("Slot 3");
        const auto s4 = opts.find("Slot 4");
        REQUIRE(s2 != std::string::npos);
        REQUIRE(s3 != std::string::npos);
        REQUIRE(s4 != std::string::npos);

        CHECK(opts.substr(s2, s3 - s2).find("(incompatible)") == std::string::npos);
        CHECK(opts.substr(s3, s4 - s3).find("(incompatible)") != std::string::npos);
        CHECK(opts.substr(s4).find("(incompatible)") == std::string::npos);

        // And the handler refuses the same one.
        CHECK_FALSE(Access::decide_backup_refused(0, 1, live));
        CHECK(Access::decide_backup_refused(0, 2, live));
        CHECK_FALSE(Access::decide_backup_refused(0, 3, live));
    }

    backend.stop();
}
