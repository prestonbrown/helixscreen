// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_selector.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "filament_variants.h"

#include <sstream>

#include "../catch_amalgamated.hpp"

using helix::printer::EffectiveFilament;
using helix::ui::FilamentCatalogSelector;

namespace {
lv_obj_t* make_fragment() {
    FilamentCatalogSelector::register_callbacks();
    lv_xml_register_component_from_file("A:ui_xml/components/filament_catalog_selector.xml");
    return static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "filament_catalog_selector", nullptr));
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "selector populates and reports a highlighted product",
                 "[filament_picker][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PLA"), std::nullopt);
    sel.populate();

    REQUIRE(sel.current_vendor() == "Generic");
    REQUIRE(sel.current_type() == "PLA");
    REQUIRE(sel.highlighted() == nullptr);

    const EffectiveFilament* got = nullptr;
    sel.set_selection_changed([&](const EffectiveFilament* ef) { got = ef; });
    sel.select_first_product_for_test();
    REQUIRE(sel.highlighted() != nullptr);
    REQUIRE(got != nullptr);
    CHECK(got->type == "PLA");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "selector merges host additional vendors and seeds a Spoolman-only one",
                 "[filament_picker][catalog_selector][preselect][spoolman]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    // Seed the vendor to a brand absent from the bundled catalog. Without the
    // additional-vendor merge the seed can't resolve, so it snaps to Generic.
    sel.configure(std::nullopt, std::nullopt, std::string("PolyTerra"));
    sel.populate();
    REQUIRE(sel.current_vendor() == "Generic"); // not in the catalog yet

    // The host supplies the live (e.g. Spoolman) vendor list; the seed resolves.
    sel.set_additional_vendors({"PolyTerra"});
    CHECK(sel.current_vendor() == "PolyTerra");

    // Generic stays pinned at index 0 — the merge appends, never reorders.
    sel.change_vendor_for_test(0);
    CHECK(sel.current_vendor() == "Generic");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "selector additional-vendor merge dedups against the catalog case-insensitively",
                 "[filament_picker][catalog_selector][spoolman]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::nullopt, std::nullopt);
    sel.populate();

    // "Overture" is a catalog brand; a differently-cased duplicate must NOT be
    // appended, and "Generic" (already pinned) must not double up either.
    sel.set_additional_vendors({"overture", "Generic", "PolyTerra"});

    // Walk the dropdown and count occurrences via the index-aligned order.
    lv_obj_t* dd = lv_obj_find_by_name(root, "vendor_dropdown");
    REQUIRE(dd != nullptr);
    std::string opts = lv_dropdown_get_options(dd);
    auto count_token = [&](const std::string& name) {
        int n = 0;
        std::string line;
        std::stringstream ss(opts);
        while (std::getline(ss, line)) {
            if (line == name)
                ++n;
        }
        return n;
    };
    CHECK(count_token("Overture") == 1);  // catalog entry kept, dup dropped
    CHECK(count_token("overture") == 0);  // lowercased dup not appended
    CHECK(count_token("Generic") == 1);   // pinned once
    CHECK(count_token("PolyTerra") == 1); // genuinely new -> appended

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "selector allowed_types filter is case-insensitive",
                 "[filament_picker][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::nullopt, std::vector<std::string>{"pla"});
    sel.populate();

    CHECK(sel.type_options() == "PLA");

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "selector allowed_types appends whitelist entries missing from catalog",
                 "[filament_picker][catalog_selector][whitelist]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    // PEEK has no Generic-vendor catalog product at all, so a subtract-only filter
    // would silently drop it, locking users out of a firmware-supported material.
    // PLA and PETG both exist for Generic and are intersected as before.
    sel.configure(std::nullopt, std::vector<std::string>{"PLA", "PEEK", "PETG"});
    sel.populate();

    // Sorted family headings (PETG, PLA) first, then whitelist-only entries appended
    // in whitelist order, preserving whitelist spelling.
    CHECK(sel.type_options() == "PETG\nPLA\nPEEK");

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "selector folds a whitelisted variant type into its family heading",
                 "[filament_picker][catalog_selector][whitelist][family]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    // SILK's only Generic product is "Silk PLA" — it is stocked, so it is NOT an
    // absent-whitelist append. It folds under the PLA family heading instead of
    // standing as its own top-level entry, while still emitting type "SILK".
    sel.configure(std::nullopt, std::vector<std::string>{"PLA", "SILK", "PETG"});
    sel.populate();

    CHECK(sel.type_options() == "PETG\nPLA");

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture, "selector clears highlight when vendor changes",
                 "[filament_picker][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PLA"), std::nullopt);
    sel.populate();
    sel.select_first_product_for_test();
    REQUIRE(sel.highlighted() != nullptr);

    sel.change_vendor_for_test(1);
    CHECK(sel.highlighted() == nullptr);

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture, "preselect_on_change keeps a checked product across a type change",
                 "[filament_picker][catalog_selector][preselect]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.set_preselect_on_change(true);
    // Constrain the type dropdown to a known, sorted set so indices are stable:
    // catalog intersection is alphabetical -> "PETG\nPLA" (index 0 = PETG, 1 = PLA).
    sel.configure(std::string("PLA"), std::vector<std::string>{"PLA", "PETG"});
    sel.populate();
    sel.preselect_first();
    REQUIRE(sel.type_options() == "PETG\nPLA");
    REQUIRE(sel.current_type() == "PLA");
    const EffectiveFilament* pla = sel.highlighted();
    REQUIRE(pla != nullptr);
    CHECK(pla->type == "PLA");
    std::string pla_id = pla->id;

    // Change type to PETG: the rebuilt list must auto-highlight a PETG product
    // (invariant: never an all-unchecked list under preselect_on_change).
    sel.change_type_for_test(0);
    CHECK(sel.current_type() == "PETG");
    const EffectiveFilament* petg = sel.highlighted();
    REQUIRE(petg != nullptr);
    CHECK(petg->type == "PETG");

    // Navigate back to PLA: the original (anchor) product is restored, not just
    // whatever happens to be first.
    sel.change_type_for_test(1);
    CHECK(sel.current_type() == "PLA");
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->id == pla_id);

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "preselect_on_change leaves an empty product list unchecked",
                 "[filament_picker][catalog_selector][preselect]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.set_preselect_on_change(true);
    // PEEK is whitelisted but has no Generic catalog product -> appended heading,
    // empty product list. Nothing to check; the host decides Save semantics.
    sel.configure(std::nullopt, std::vector<std::string>{"PEEK"});
    sel.populate();
    CHECK(sel.current_type() == "PEEK");
    CHECK(sel.highlighted() == nullptr);

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "product list ranks the plain material first and Support materials last",
                 "[filament_picker][catalog_selector][ordering]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PLA"), std::nullopt);
    sel.populate();
    REQUIRE(sel.current_vendor() == "Generic");
    REQUIRE(sel.current_type() == "PLA");

    // Within the base-type (type == "PLA") run, display order must put the plain
    // "PLA" first, sink "Support for PLA" to the end, and sort the rest
    // alphabetically. Products of the family's VARIANT types (Glow PLA, PLA+,
    // PLA-CF, PLA-GF, SILK, Wood PLA...) follow as their own runs after the
    // whole base-type run, each run keyed by its type so the heading reads
    // base-then-variants rather than one interleaved alphabetical soup.
    //
    // The decorative PLAs each carry their own type (type == name) because they
    // are distinct filament_database.h rows; display_family() strips the
    // decorative affix so they land under the PLA heading as single-product runs.
    // "PLA+" is the catalog's trailing-plus grade product: display_family()
    // strips the '+' the same way it strips "-CF"/"-GF", so it groups here too
    // rather than getting its own heading. Its variant-run key is the raw type
    // string "PLA+", which sorts by ASCII before "PLA-CF"/"PLA-GF" because '+'
    // (0x2B) is less than '-' (0x2D) — same alphabetical-by-type rule as every
    // other variant run, not a special case.
    auto names = sel.product_names_for_test();
    REQUIRE(names == std::vector<std::string>{"PLA", "PLA High Speed", "PLA Matte", "PLA Silk",
                                              "Support for PLA", "Glow PLA", "Marble PLA",
                                              "Matte PLA", "Metal PLA", "PLA+", "PLA-CF", "PLA-GF",
                                              "Silk PLA", "Wood PLA"});

    sel.detach();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "type-change auto-preselect lands on the plain material, not Support",
                 "[filament_picker][catalog_selector][preselect][ordering]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.set_preselect_on_change(true);
    // Constrain to a known, sorted type set so indices are stable:
    // catalog intersection is alphabetical -> "ABS\nPLA" (index 0 = ABS, 1 = PLA).
    sel.configure(std::string("ABS"), std::vector<std::string>{"PLA", "ABS"});
    sel.populate();
    REQUIRE(sel.type_options() == "ABS\nPLA");
    REQUIRE(sel.current_type() == "ABS");

    // Switching to PLA (no anchor carried over from ABS) must auto-highlight
    // the plain "PLA" product per preselect_after_change()'s front() fallback
    // now walking the ranked list, not raw catalog/file order.
    sel.change_type_for_test(1);
    CHECK(sel.current_type() == "PLA");
    const EffectiveFilament* picked = sel.highlighted();
    REQUIRE(picked != nullptr);
    CHECK(picked->name == "PLA");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "preselect_first checks the first product but keeps a prior pick",
                 "[filament_picker][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("ABS"), std::nullopt);
    sel.populate();
    REQUIRE(sel.highlighted() == nullptr);

    sel.preselect_first();
    const EffectiveFilament* first = sel.highlighted();
    REQUIRE(first != nullptr);
    CHECK(first->type == "ABS");

    // Idempotent: a second call must not clobber the existing selection.
    std::string kept_id = first->id;
    sel.preselect_first();
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->id == kept_id);

    sel.detach();
}

// preselect_first() can only ever pick ordered_products_for().front(). Every
// SUNLU PLA product shares variant_key "" and rank 1, so the tiebreak is
// lowercased-name alphabetical and "PLA Marble" always wins — which is exactly
// how a saved "PLA+ 2.0" came back as "PLA Marble" (bundle TDQCCQB3). Seeding
// by the stored catalog id is the only thing that can land on the right row.
TEST_CASE_METHOD(XMLTestFixture, "preselect_product_id lands on the exact product, not the first",
                 "[filament_picker][catalog_selector][preselect]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::nullopt, std::nullopt);
    sel.populate();

    // Baseline: navigating to SUNLU/PLA by hand and taking the first row gives
    // the WRONG product. This is the failure mode the id seed has to beat.
    sel.change_vendor_for_test(0); // reset to Generic so the seed does real work
    REQUIRE(sel.preselect_product_id("sunlu-pla-marble"));
    REQUIRE(sel.highlighted() != nullptr);
    REQUIRE(sel.current_vendor() == "SUNLU");
    REQUIRE(sel.current_type() == "PLA");
    CHECK(sel.product_names_for_test().front() == "PLA Marble");

    // Now the real assertion: the id seed must reach a NON-first product.
    CHECK(sel.preselect_product_id("sunlu-pla-plus-2-0"));
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->id == "sunlu-pla-plus-2-0");
    CHECK(sel.highlighted()->name == "PLA+ 2.0");
    // The dropdowns were navigated to the product's own vendor + family, not
    // left wherever they happened to be.
    CHECK(sel.current_vendor() == "SUNLU");
    CHECK(sel.current_type() == "PLA");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "preselect_product_id reports failure for an unresolvable id",
                 "[filament_picker][catalog_selector][preselect]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PLA"), std::nullopt);
    sel.populate();
    sel.select_first_product_for_test();
    REQUIRE(sel.highlighted() != nullptr);
    const std::string kept = sel.highlighted()->id;
    const std::string kept_vendor = sel.current_vendor();

    // A product the user deleted from their overlay, or an id retired by an app
    // update. The host needs a false so it can fall back to preselect_first();
    // silently doing nothing and returning true would strand it.
    CHECK_FALSE(sel.preselect_product_id("no-such-product-id"));
    CHECK_FALSE(sel.preselect_product_id("")); // empty id is never a resolve

    // A failed seed must not disturb what is already selected.
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->id == kept);
    CHECK(sel.current_vendor() == kept_vendor);

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

// preselect_first() records its pick as the preselect anchor so a dropdown
// round-trip restores it. An id seed is a stronger statement of the same
// intent, so it must set the anchor too — otherwise a user who switches type
// away and back loses the variant they had saved.
TEST_CASE_METHOD(XMLTestFixture, "preselect_product_id survives a dropdown round-trip",
                 "[filament_picker][catalog_selector][preselect]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.set_preselect_on_change(true);
    sel.configure(std::nullopt, std::vector<std::string>{"PLA", "PETG"});
    sel.populate();

    REQUIRE(sel.preselect_product_id("sunlu-pla-plus-2-0"));
    REQUIRE(sel.type_options() == "PETG\nPLA");
    REQUIRE(sel.current_type() == "PLA");

    // Away to PETG and back: the anchor restores the saved variant, not the
    // alphabetically-first "PLA Marble".
    sel.change_type_for_test(0);
    CHECK(sel.current_type() == "PETG");
    sel.change_type_for_test(1);
    CHECK(sel.current_type() == "PLA");
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->id == "sunlu-pla-plus-2-0");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "a Spoolman-only vendor still offers material types",
                 "[filament_picker][catalog_selector]") {
    // K2 Plus, 2026-08-24: lanes 2 and 3 held Ambrosia ASA-GF. The vendor
    // dropdown offers every live Spoolman vendor, but the bundled catalog
    // carries 21 brands and "Ambrosia" is not one, so types_for_brand() came
    // back empty and the Type dropdown rendered with ZERO rows — the material
    // could not be set at all.
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    // Mirrors AmsEditOverlay: configure() deliberately clears the extra vendor
    // list, and the host re-supplies it AFTER populate() once the async Spoolman
    // vendor fetch lands. set_additional_vendors() rebuilds both dropdowns.
    sel.configure(std::nullopt, std::nullopt, std::string("Ambrosia"));
    sel.populate();
    sel.set_additional_vendors({"Ambrosia"});

    REQUIRE(sel.current_vendor() == "Ambrosia");
    CHECK_FALSE(sel.current_type().empty());

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "an off-catalog vendor still seeds the slot's material family",
                 "[filament_picker][catalog_selector]") {
    // The slot claims ASA-GF. Headings are FAMILIES by design (PLA-CF shows
    // under "PLA"), so the dropdown lands on ASA and the variant stays selectable
    // in the product list underneath — what matters is that it is no longer
    // stranded on an empty dropdown with no family at all.
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("ASA-GF"), std::nullopt, std::string("Ambrosia"));
    sel.populate();
    sel.set_additional_vendors({"Ambrosia"});

    CHECK(sel.current_type() == filament::display_family("ASA-GF"));
    CHECK_FALSE(sel.current_type().empty());

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "a material no catalog product carries is still offered",
                 "[filament_picker][catalog_selector]") {
    // A family the fallback list cannot supply -- free-text Spoolman materials
    // reduce to nothing -- must still be appended, or reopening the editor
    // silently reassigns the slot's material to whatever sorts first.
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("Ambrosia House Blend"), std::nullopt, std::string("Ambrosia"));
    sel.populate();
    sel.set_additional_vendors({"Ambrosia"});

    CHECK(sel.current_type() == "Ambrosia House Blend");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}
