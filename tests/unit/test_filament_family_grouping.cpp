// SPDX-License-Identifier: GPL-3.0-or-later
//
// Display-only material family grouping for the filament catalog picker.
//
// Two invariants are load-bearing here:
//  1. Grouping is PRESENTATION. The `type` string a selection emits must be
//     byte-identical to what it was before grouping — OrcaSlicer matches
//     presets by that string verbatim, and MaterialSettingsManager keys user
//     temperature overrides by material name.
//  2. Grouping is not merging. ASA-CF and ASA-GF share a heading but remain
//     separately selectable entries with their own temperatures, and the user
//     must be able to tell which one they picked.

#include "ui_filament_catalog_selector.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "ams_backend_ad5x_ifs.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_variants.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::printer::EffectiveFilament;
using helix::printer::FilamentCatalog;
using helix::ui::FilamentCatalogSelector;

namespace {

lv_obj_t* make_fragment() {
    FilamentCatalogSelector::register_callbacks();
    lv_xml_register_component_from_file("A:ui_xml/components/filament_catalog_selector.xml");
    return static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "filament_catalog_selector", nullptr));
}

/// Ordered product list for a vendor + family heading, driven through the real
/// dropdown path so tests exercise what the UI actually renders.
std::vector<const EffectiveFilament*> products_under(FilamentCatalogSelector& sel,
                                                     const std::string& family) {
    // type_options() is "\n"-joined; find the family's index and select it.
    std::string opts = sel.type_options();
    uint32_t idx = 0;
    size_t start = 0;
    bool found = false;
    while (start <= opts.size()) {
        size_t end = opts.find('\n', start);
        std::string entry =
            opts.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (entry == family) {
            found = true;
            break;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
        ++idx;
    }
    REQUIRE(found);
    sel.change_type_for_test(idx);
    return sel.products_for_test();
}

std::vector<std::string> types_of(const std::vector<const EffectiveFilament*>& ps) {
    std::vector<std::string> out;
    for (const auto* p : ps)
        out.push_back(p->type);
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

// ===========================================================================
// Family derivation — the affix table and the explicit override table
// ===========================================================================

TEST_CASE("family derivation reduces suffix variants to their base polymer",
          "[filament][family][derivation]") {
    CHECK(filament::display_family("PLA-CF") == "PLA");
    CHECK(filament::display_family("PLA-GF") == "PLA");
    CHECK(filament::display_family("PLA-AERO") == "PLA");
    CHECK(filament::display_family("ASA-CF") == "ASA");
    CHECK(filament::display_family("ASA-GF") == "ASA");
    CHECK(filament::display_family("ASA-AERO") == "ASA");
    CHECK(filament::display_family("PETG-CF") == "PETG");
    CHECK(filament::display_family("PETG-GF") == "PETG");
    CHECK(filament::display_family("PPS-CF") == "PPS");
    CHECK(filament::display_family("PP-GF") == "PP");
    CHECK(filament::display_family("PP-CF") == "PP");
    CHECK(filament::display_family("ABS-CF") == "ABS");
    CHECK(filament::display_family("PC-GF") == "PC");
}

TEST_CASE("family derivation strips known variant PREFIXES", "[filament][family][derivation]") {
    // Left-prefix walking alone cannot reach these: the base polymer is not a
    // prefix of the string.
    CHECK(filament::display_family("HT-PLA-GF") == "PLA");
    CHECK(filament::display_family("HT-PLA") == "PLA");
    CHECK(filament::display_family("HS-PLA") == "PLA");
}

TEST_CASE("family derivation handles space-separated variant words",
          "[filament][family][derivation]") {
    // Variant word on either side of the base, separated by a space rather
    // than a hyphen.
    CHECK(filament::display_family("Silk PLA") == "PLA");
    CHECK(filament::display_family("PLA Silk") == "PLA");
    CHECK(filament::display_family("Matte PLA") == "PLA");
    CHECK(filament::display_family("Wood PLA") == "PLA");
    // "SILK" is an alias for "Silk PLA"; alias resolution must happen before
    // affix stripping or the alias hides the base.
    CHECK(filament::display_family("SILK") == "PLA");
}

TEST_CASE("materials with no variants are their own family", "[filament][family][derivation]") {
    // A heading with a single entry is the consistent outcome — no special case.
    CHECK(filament::display_family("TPU") == "TPU");
    CHECK(filament::display_family("PC") == "PC");
    CHECK(filament::display_family("PLA") == "PLA");
    CHECK(filament::display_family("PETG") == "PETG");
    CHECK(filament::display_family("SBS") == "SBS");
    CHECK(filament::display_family("PVA") == "PVA");
    CHECK(filament::display_family("HIPS") == "HIPS");
}

TEST_CASE("numbered nylon grades collapse into the PA family",
          "[filament][family][derivation][polyamide]") {
    // Settled product decision: the catalog does not carry enough of each
    // numbered grade to justify separate headings.
    CHECK(filament::display_family("PA") == "PA");
    CHECK(filament::display_family("PA-CF") == "PA");
    CHECK(filament::display_family("PA-GF") == "PA");
    CHECK(filament::display_family("PA6") == "PA");
    CHECK(filament::display_family("PA6-CF") == "PA");
    CHECK(filament::display_family("PA6-GF") == "PA");
    CHECK(filament::display_family("PA12") == "PA");
    CHECK(filament::display_family("PA12-CF") == "PA");
    CHECK(filament::display_family("PA612-CF") == "PA");
    CHECK(filament::display_family("PA66") == "PA");
}

TEST_CASE("PAHT files under PA", "[filament][family][derivation][polyamide]") {
    // Every PAHT product currently in the catalog is PA12/PA612-class and
    // prints in the ordinary PA envelope. Documented as a deliberate mapping,
    // not an accident of string parsing — see PAHT_FAMILY.
    CHECK(filament::display_family("PAHT") == "PA");
    CHECK(filament::display_family("PAHT-CF") == "PA");
}

TEST_CASE("PPA is NEVER normalized to PA", "[filament][family][derivation][polyamide]") {
    // PPA (polyphthalamide) is one letter from PA and contains it as a
    // substring — exactly the false-positive class affix stripping must not
    // hit. It is a different processing regime and keeps its own heading.
    CHECK(filament::display_family("PPA") == "PPA");
    CHECK(filament::display_family("PPA-CF") == "PPA");
    CHECK(filament::display_family("PPA-GF") == "PPA");
    CHECK(filament::display_family("PPA-CF") != "PA");
}

TEST_CASE("family derivation does not mangle names that merely contain an affix",
          "[filament][family][derivation][falsepositive]") {
    // Affixes are only stripped when delimited by a separator.
    CHECK(filament::display_family("CoPE") == "CoPE"); // ends in "PE", not a PE variant
    CHECK(filament::display_family("SBS") == "SBS");
    CHECK(filament::display_family("PCTG") == "PCTG");
    CHECK(filament::display_family("HIPS") == "HIPS"); // contains "HS" unseparated
    // "ABS" is deliberately absent from the affix table so a real blend keeps
    // its own identity (and its ABS_ASA compat group).
    CHECK(filament::display_family("PC-ABS") == "PC-ABS");
}

TEST_CASE("polyethylene and PET are never grouped together",
          "[filament][family][derivation][falsepositive]") {
    // PE is a polyolefin; PET is polyethylene terephthalate. Unrelated.
    // Stripping "-CF" from "PET-CF" must stop at PET and never continue to PE.
    CHECK(filament::display_family("PE") == "PE");
    CHECK(filament::display_family("PE-CF") == "PE");
    CHECK(filament::display_family("PET") == "PET");
    CHECK(filament::display_family("PET-CF") == "PET");
    CHECK(filament::display_family("PET-GF") == "PET");
    CHECK(filament::display_family("PET-CF") != "PE");
    CHECK(filament::display_family("PE-CF") != "PET");
}

TEST_CASE("family derivation keeps the legacy compound-name fallback",
          "[filament][family][derivation]") {
    // Brand-decorated names that are not affix-shaped still resolve via the
    // shortest-known-prefix walk.
    CHECK(filament::display_family("PLA SnapSpeed") == "PLA");
    CHECK(filament::display_family("PETG Pro") == "PETG");
    // Genuinely unknown input is returned unchanged rather than guessed at.
    CHECK(filament::display_family("Unobtainium") == "Unobtainium");
    CHECK(filament::display_family("") == "");
}

TEST_CASE("extract_base_material strips a trailing plus", "[filament][family][derivation]") {
    CHECK(filament::extract_base_material("PLA+") == "PLA");
    CHECK(filament::extract_base_material("ASA+") == "ASA");
    CHECK(filament::extract_base_material("ABS+") == "ABS");
    // Composes with affix stripping in either order.
    CHECK(filament::extract_base_material("PLA+-CF") == "PLA");
    // A bare "+" has no polymer left; return the input rather than "".
    CHECK(filament::extract_base_material("+") == "+");
}

// ===========================================================================
// Grade awareness: filled vs unfilled, one step below family
// ===========================================================================

TEST_CASE("grades_match forgives marketing grades", "[filament][family][grade]") {
    // "+" is a toughener brand suffix, not a filler. Same polymer, same
    // hardware, same flow envelope.
    CHECK(filament::grades_match("PLA", "PLA+"));
    CHECK(filament::grades_match("ABS+", "ABS"));
    // Speed and temperature ratings describe the print profile, not the
    // filament's abrasiveness.
    CHECK(filament::grades_match("PLA", "PLA-HS"));
    CHECK(filament::grades_match("PETG-HF", "PETG"));
    CHECK(filament::grades_match("HT-PLA", "PLA"));
    // Unfilled cosmetic grades: a co-polymer blend and a surface agent.
    CHECK(filament::grades_match("Silk PLA", "PLA"));
    CHECK(filament::grades_match("Matte PLA", "PLA"));
    // Identity, and case insensitivity.
    CHECK(filament::grades_match("ASA-GF", "asa-gf"));
    CHECK(filament::grades_match("", ""));
}

TEST_CASE("grades_match flags filled grades", "[filament][family][grade]") {
    // Fiber fills: abrasive, and printed well below the base polymer's flow.
    CHECK_FALSE(filament::grades_match("ASA", "ASA-GF"));
    CHECK_FALSE(filament::grades_match("PLA-CF", "PLA"));
    // Two different fillers are not each other either.
    CHECK_FALSE(filament::grades_match("ASA-CF", "ASA-GF"));
    // Foaming grades: density is a function of temperature, so a profile
    // sliced for one is meaningless on the other.
    CHECK_FALSE(filament::grades_match("PLA-AERO", "PLA"));
    CHECK_FALSE(filament::grades_match("PLA-LW", "PLA"));
    // Particle fills the marketing calls cosmetic. Glow is strontium
    // aluminate and outwears carbon fiber on a brass nozzle.
    CHECK_FALSE(filament::grades_match("Glow PLA", "PLA"));
    CHECK_FALSE(filament::grades_match("Wood PLA", "PLA"));
    CHECK_FALSE(filament::grades_match("Metal PLA", "PLA"));
    CHECK_FALSE(filament::grades_match("Marble PLA", "PLA"));
}

TEST_CASE("grades_match composes with the affix stripper", "[filament][family][grade]") {
    // Benign affixes are transparent: both sides reduce to the same filler set.
    CHECK(filament::grades_match("HT-PLA-GF", "PLA-GF"));
    CHECK(filament::grades_match("PLA+-CF", "PLA-CF"));
    // A benign affix does not launder a filler.
    CHECK_FALSE(filament::grades_match("HT-PLA-GF", "HT-PLA"));
    // Fused polymer grades reduce through the override table with the filler
    // still recorded ("PA6-CF" -> CF + PA6 -> CF + PA).
    CHECK_FALSE(filament::grades_match("PA6-CF", "PA6"));
    CHECK(filament::grades_match("PA6-CF", "PA-CF"));
    // Names the affix table cannot parse carry no filler and match anything
    // else that carries none. Silence beats a guess.
    CHECK(filament::grades_match("PLA SnapSpeed", "PLA"));
    CHECK(filament::grades_match("Unobtainium", "PLA"));
}

TEST_CASE("is_filled_grade identifies the abrasive side", "[filament][family][grade]") {
    // Drives which of the two directional warnings the dialog shows.
    CHECK(filament::is_filled_grade("ASA-GF"));
    CHECK(filament::is_filled_grade("PLA-CF"));
    CHECK(filament::is_filled_grade("Glow PLA"));
    CHECK(filament::is_filled_grade("PLA-AERO"));
    CHECK_FALSE(filament::is_filled_grade("ASA"));
    CHECK_FALSE(filament::is_filled_grade("PLA+"));
    CHECK_FALSE(filament::is_filled_grade("Silk PLA"));
    CHECK_FALSE(filament::is_filled_grade(""));
}

TEST_CASE("every filled database material reads as filled", "[filament][family][grade]") {
    // The affix table and MATERIALS[] are edited independently. Any row whose
    // NAME advertises a filler must be visible to is_filled_grade(), or the
    // warning silently skips that material.
    for (const auto& mat : filament::MATERIALS) {
        std::string name = mat.name;
        const bool advertises_filler =
            name.find("-CF") != std::string::npos || name.find("-GF") != std::string::npos ||
            name.find("-AERO") != std::string::npos || name.find("Wood") != std::string::npos ||
            name.find("Metal") != std::string::npos || name.find("Marble") != std::string::npos ||
            name.find("Glow") != std::string::npos;
        if (!advertises_filler) {
            continue;
        }
        INFO("material=" << name);
        CHECK(filament::is_filled_grade(name));
    }
}

// ===========================================================================
// materials_compatible: one answer to "same polymer?"
// ===========================================================================

TEST_CASE("materials_compatible reduces before comparing groups", "[filament][family][compat]") {
    CHECK(filament::materials_compatible("PLA", "PLA-CF"));
    CHECK(filament::materials_compatible("PLA", "PLA+"));
    CHECK(filament::materials_compatible("ABS", "ASA")); // one compat group
    CHECK_FALSE(filament::materials_compatible("PLA", "PETG"));
    CHECK_FALSE(filament::materials_compatible("PLA", "ABS"));
}

TEST_CASE("materials_compatible sees through a decorated product name",
          "[filament][family][compat]") {
    // The bug this function exists to close: are_materials_compatible() looks a
    // name up in MATERIALS[] and reads a miss as "unknown, compatible with
    // everything". A name the family reducer CAN read must never reach that
    // fallback, or a lane the user labelled from a spool database silently
    // pairs with any other lane.
    CHECK_FALSE(filament::materials_compatible("PLA SnapSpeed", "ABS"));
    CHECK_FALSE(filament::materials_compatible("HT-PLA-GF", "PETG"));
    CHECK(filament::materials_compatible("PLA SnapSpeed", "PLA"));
    CHECK(filament::materials_compatible("HT-PLA-GF", "PLA-CF"));

    // Proof the old rule really was permissive here, so this test cannot quietly
    // stop testing anything if the two implementations are ever re-merged.
    CHECK(filament::are_materials_compatible("PLA SnapSpeed", "ABS"));
}

TEST_CASE("materials_compatible stays permissive for genuinely unknown names",
          "[filament][family][compat]") {
    // Nothing to reduce and nothing in the database: an unrecognised name is
    // compatible with anything, which keeps a firmware-only or hand-typed
    // material from blocking the user. Deliberate, and the reason the previous
    // case has to name something the reducer CAN read.
    CHECK(filament::materials_compatible("Unobtainium", "PLA"));
    CHECK(filament::materials_compatible("Unobtainium", "Unobtainium"));
}

// ===========================================================================
// Blast-radius guard: materials_match() must be unchanged
// ===========================================================================

TEST_CASE("base extraction preserves compat group for every database material",
          "[filament][family][derivation][regression]") {
    // extract_base_material() feeds FilamentMapper::materials_match(), which
    // only ever consults the compat group of the result. If reducing a material
    // to its base kept the same compat group, every materials_match() outcome
    // is provably unchanged by the affix table. This guards the whole
    // endless-spool / preflight / slot-picker surface at once.
    for (const auto& mat : filament::MATERIALS) {
        std::string base = filament::extract_base_material(mat.name);
        const char* g_orig = filament::get_compatibility_group(mat.name);
        const char* g_base = filament::get_compatibility_group(base);
        INFO("material=" << mat.name << " base=" << base);
        REQUIRE(g_orig != nullptr);
        REQUIRE(g_base != nullptr);
        CHECK(std::string(g_orig) == std::string(g_base));
    }
}

TEST_CASE("materials_match behavior is unchanged by family grouping",
          "[filament][family][regression][filament_mapper]") {
    using helix::FilamentMapper;
    // Same compat group -> still matches.
    CHECK(FilamentMapper::materials_match("PLA", "PLA-CF"));
    CHECK(FilamentMapper::materials_match("PLA", "PLA+"));
    CHECK(FilamentMapper::materials_match("ABS", "ASA"));
    CHECK(FilamentMapper::materials_match("PLA SnapSpeed", "PLA"));
    CHECK(FilamentMapper::materials_match("ASA-CF", "ASA-GF"));
    // Cross-material -> still rejected.
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "PETG"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "ABS"));
    CHECK_FALSE(FilamentMapper::materials_match("PETG", "TPU"));
    CHECK_FALSE(FilamentMapper::materials_match("PLA", "PA"));
    // PC-ABS keeps its ABS_ASA group rather than collapsing to PC.
    CHECK(FilamentMapper::materials_match("PC-ABS", "ABS"));
    CHECK_FALSE(FilamentMapper::materials_match("PC-ABS", "PC"));
}

// ===========================================================================
// THE Orca regression guard — emitted material strings must not change
// ===========================================================================

TEST_CASE("grouping never alters the type string a product emits",
          "[filament][family][regression][orca]") {
    // OrcaSlicer matches presets by the emitted `material` string verbatim, and
    // MaterialSettingsManager keys user temperature overrides by material name.
    // Family grouping changes which HEADING a product is filed under and
    // nothing else: every product must still carry its original type.
    //
    // This is the most important test in this file. If it fails, saved user
    // temperature overrides orphan and Orca preset matching breaks.
    // Ground truth straight from the shipped asset, independent of any code
    // path that grouping touches.
    std::ifstream f("assets/filaments.json");
    REQUIRE(f.is_open());
    nlohmann::json doc = nlohmann::json::parse(f);
    REQUIRE(doc.contains("filaments"));

    FilamentCatalog raw = FilamentCatalog::load_full();
    REQUIRE(raw.all_products().size() > 300); // catalog actually loaded

    size_t checked = 0;
    for (const auto& jp : doc["filaments"]) {
        std::string id = jp.value("id", "");
        std::string json_type = jp.value("type", "");
        if (id.empty())
            continue;
        const auto* p = raw.resolve_id(id);
        INFO("product id=" << id << " json type=" << json_type);
        REQUIRE(p != nullptr);
        // The emitted material string, byte-identical to the asset.
        CHECK(p->type == json_type);
        // Deriving a family must not be able to rewrite it.
        CHECK_FALSE(filament::display_family(p->type).empty());
        CHECK(p->type == json_type);
        ++checked;
    }
    CHECK(checked > 300);

    // Generic ASA variants keep their distinct type strings.
    bool saw_asa_cf = false, saw_asa_gf = false;
    for (const auto* p : raw.all_products()) {
        if (p->brand == "Generic" && p->type == "ASA-CF")
            saw_asa_cf = true;
        if (p->brand == "Generic" && p->type == "ASA-GF")
            saw_asa_gf = true;
    }
    CHECK(saw_asa_cf);
    CHECK(saw_asa_gf);
}

TEST_CASE_METHOD(XMLTestFixture, "selector emits the product's own type, not the family heading",
                 "[filament][family][regression][orca][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("ASA"), std::nullopt);
    sel.populate();
    REQUIRE(sel.current_vendor() == "Generic");
    REQUIRE(sel.current_type() == "ASA"); // family heading

    // Every row under the ASA heading emits its OWN type when tapped —
    // "ASA-GF" must still emit "ASA-GF", never the "ASA" heading text.
    std::vector<std::pair<std::string, std::string>> expected; // id -> type
    for (const auto* p : sel.products_for_test())
        expected.emplace_back(p->id, p->type);
    REQUIRE(expected.size() >= 3);

    std::string emitted_type;
    sel.set_selection_changed(
        [&](const EffectiveFilament* ef) { emitted_type = ef ? ef->type : std::string("<none>"); });

    bool saw_cf = false, saw_gf = false;
    for (const auto& [id, type] : expected) {
        emitted_type.clear();
        sel.select_product_for_test(id); // drives the real row-tap path
        INFO("product id=" << id << " under heading ASA");
        CHECK(emitted_type == type);
        CHECK(filament::display_family(type) == "ASA");
        if (type == "ASA-CF") {
            CHECK(emitted_type == "ASA-CF"); // Orca preset key, verbatim
            saw_cf = true;
        }
        if (type == "ASA-GF") {
            CHECK(emitted_type == "ASA-GF"); // Orca preset key, verbatim
            saw_gf = true;
        }
    }
    CHECK(saw_cf);
    CHECK(saw_gf);

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

// ===========================================================================
// Picker grouping behavior
// ===========================================================================

TEST_CASE_METHOD(XMLTestFixture,
                 "ASA-CF and ASA-GF sit under one ASA heading and stay distinguishable",
                 "[filament][family][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("ASA"), std::nullopt);
    sel.populate();

    // One heading, not three.
    std::string opts = sel.type_options();
    INFO("type options: " << opts);
    CHECK(opts.find("ASA-CF") == std::string::npos);
    CHECK(opts.find("ASA-GF") == std::string::npos);
    CHECK(opts.find("ASA") != std::string::npos);

    REQUIRE(sel.current_type() == "ASA");
    auto products = sel.products_for_test();
    auto types = types_of(products);

    // Both variants present as separate, selectable entries.
    CHECK(contains(types, "ASA"));
    CHECK(contains(types, "ASA-CF"));
    CHECK(contains(types, "ASA-GF"));

    // Base material first, variants after.
    REQUIRE(types.size() >= 3);
    CHECK(types.front() == "ASA");

    // Distinguishable at a glance: each variant row carries its own type as a
    // chip, and the two fiber fills never render an identical label.
    const EffectiveFilament* cf = nullptr;
    const EffectiveFilament* gf = nullptr;
    for (const auto* p : products) {
        if (p->type == "ASA-CF")
            cf = p;
        if (p->type == "ASA-GF")
            gf = p;
    }
    REQUIRE(cf != nullptr);
    REQUIRE(gf != nullptr);
    std::string cf_label = sel.row_label_for_test(cf);
    std::string gf_label = sel.row_label_for_test(gf);
    INFO("cf label: " << cf_label << " / gf label: " << gf_label);
    CHECK(cf_label != gf_label);
    CHECK(cf_label.find("ASA-CF") != std::string::npos);
    CHECK(gf_label.find("ASA-GF") != std::string::npos);

    // Not merged or aliased: two distinct products, each independently
    // selectable and each carrying its own emitted material string.
    CHECK(cf->id != gf->id);
    CHECK(cf->type == "ASA-CF");
    CHECK(gf->type == "ASA-GF");
    sel.select_product_for_test(cf->id);
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->type == "ASA-CF");
    sel.select_product_for_test(gf->id);
    REQUIRE(sel.highlighted() != nullptr);
    CHECK(sel.highlighted()->type == "ASA-GF");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "the base material stays selectable inside its own family heading",
                 "[filament][family][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("ASA"), std::nullopt);
    sel.populate();
    REQUIRE(sel.current_type() == "ASA");

    // Preselect must land on the plain base material, not a fiber-filled variant.
    sel.preselect_first();
    const EffectiveFilament* picked = sel.highlighted();
    REQUIRE(picked != nullptr);
    CHECK(picked->type == "ASA");
    CHECK(picked->name == "ASA");
    // The base row carries no variant chip — the heading already names it.
    CHECK(sel.row_label_for_test(picked) == "ASA");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "a variant-only row is chipped with the type it emits",
                 "[filament][family][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PETG"), std::nullopt);
    sel.populate();
    REQUIRE(sel.current_type() == "PETG");

    // A row whose type differs from the heading it sits under emits a material
    // the heading does not name, so it must carry a chip spelling out what it
    // actually emits. Generic's "PETG-CF" is typed PETG-CF and groups under the
    // PETG heading; the plain "PETG" row is typed PETG and needs no chip. Without
    // the chip the CF row would read as an unremarkable sibling of the base row
    // while emitting a different string.
    //
    // (This used to probe two Generic products BOTH named "PETG-CF" — one typed
    // PETG, one typed PETG-CF. That pair was a catalog duplication bug, since
    // fixed by merging them, so the collision no longer exists to test. The
    // property under test is unchanged: rows emitting different materials must
    // render distinguishably.)
    auto products = sel.products_for_test();
    const EffectiveFilament* base = nullptr;
    const EffectiveFilament* typed = nullptr;
    for (const auto* p : products) {
        if (p->name == "PETG" && p->type == "PETG")
            base = p;
        if (p->name == "PETG-CF" && p->type == "PETG-CF")
            typed = p;
    }
    REQUIRE(base != nullptr);
    REQUIRE(typed != nullptr);
    CHECK(sel.row_label_for_test(base) != sel.row_label_for_test(typed));
    // The variant row names the type it emits; the base row carries no chip.
    CHECK(sel.row_label_for_test(typed).find("PETG-CF") != std::string::npos);
    CHECK(sel.row_label_for_test(base) == "PETG");
    // Emitted strings differ, exactly as the catalog defines them.
    CHECK(base->type == "PETG");
    CHECK(typed->type == "PETG-CF");

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("a variant-NAMED product typed as the base still emits the base material",
          "[filament][family][catalog_selector]") {
    // The catalog deliberately types some variant-named products as the base
    // material — Elegoo "PETG-CF" is type=PETG, matching how the vendor's own
    // profile declares it. That row groups under the PETG heading and emits
    // "PETG", NOT "PETG-CF". The typing is correct and must not be "fixed":
    // OrcaSlicer matches the emitted string verbatim.
    //
    // This is the surviving half of the name/type-mismatch property that the
    // Generic PETG-CF de-duplication removed from the vendor-collision case above.
    FilamentCatalog raw = FilamentCatalog::load_full();
    int elegoo_petg_cf = 0;
    for (const auto* p : raw.all_products()) {
        if (p->brand == "Elegoo" && p->name == "PETG-CF") {
            ++elegoo_petg_cf;
            CHECK(p->type == "PETG");                           // unchanged
            CHECK(filament::display_family(p->type) == "PETG"); // under PETG heading
        }
    }
    CHECK(elegoo_petg_cf == 1); // present, and not duplicated
}

TEST_CASE("products whose NAME implies a variant but whose TYPE is the base are not hidden",
          "[filament][family][catalog_selector]") {
    // The catalog deliberately types some variant-named products as the base
    // material (Elegoo ASA-CF is type=ASA, Elegoo PETG-CF is type=PETG). Those
    // type assignments are correct and must not change; the products must
    // simply appear under the base heading, exactly once.
    FilamentCatalog raw = FilamentCatalog::load_full();
    int elegoo_asa_cf = 0;
    for (const auto* p : raw.all_products()) {
        if (p->brand == "Elegoo" && p->name == "ASA-CF") {
            ++elegoo_asa_cf;
            CHECK(p->type == "ASA");                           // unchanged
            CHECK(filament::display_family(p->type) == "ASA"); // under ASA heading
        }
    }
    CHECK(elegoo_asa_cf == 1); // present, and not duplicated
}

TEST_CASE("PA6-CF products land under one PA heading despite inconsistent catalog typing",
          "[filament][family][polyamide][catalog_selector]") {
    // The catalog files PA6-CF products under BOTH type=PA-CF (COEX, Creality,
    // Generic) and type=PA6-CF (Bambu, DREMC, Polymaker). Grouping papers over
    // that split: all of them must reach the same "PA" heading, while each
    // keeps the type string it emits.
    FilamentCatalog raw = FilamentCatalog::load_full();
    std::set<std::string> seen_types;
    int count = 0;
    for (const auto* p : raw.all_products()) {
        if (p->name.find("PA6-CF") == std::string::npos)
            continue;
        ++count;
        seen_types.insert(p->type);
        INFO("product " << p->brand << " " << p->name << " type=" << p->type);
        CHECK(filament::display_family(p->type) == "PA");
    }
    CHECK(count >= 6);
    // Both inconsistent typings really are present — otherwise this test would
    // pass vacuously if the catalog were cleaned up.
    CHECK(seen_types.count("PA-CF") == 1);
    CHECK(seen_types.count("PA6-CF") == 1);
}

TEST_CASE("PPA keeps its own heading separate from PA",
          "[filament][family][polyamide][catalog_selector]") {
    FilamentCatalog raw = FilamentCatalog::load_full();
    bool saw_ppa = false;
    for (const auto* p : raw.all_products()) {
        if (p->type == "PPA-CF" || p->type == "PPA-GF") {
            saw_ppa = true;
            CHECK(filament::display_family(p->type) == "PPA");
            CHECK(filament::display_family(p->type) != "PA");
        }
    }
    CHECK(saw_ppa);
}

TEST_CASE_METHOD(XMLTestFixture, "family grouping collapses the type dropdown",
                 "[filament][family][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::nullopt, std::nullopt);
    sel.populate();

    std::string opts = sel.type_options();
    // No variant type may appear as a top-level heading for the Generic vendor.
    for (const char* variant :
         {"PLA-CF", "PLA-GF", "PLA-AERO", "ASA-CF", "ASA-GF", "PETG-CF", "PETG-GF", "PA-CF",
          "PA-GF", "ABS-CF", "PC-CF", "PP-CF", "PPS-CF", "SILK"}) {
        INFO("variant heading leaked: " << variant << " in " << opts);
        CHECK(opts.find(variant) == std::string::npos);
    }
    // Base families are present.
    for (const char* base : {"PLA", "PETG", "ASA", "ABS", "PA", "TPU", "PC"}) {
        INFO("missing base heading: " << base);
        CHECK(opts.find(base) != std::string::npos);
    }

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

// ===========================================================================
// AD5X firmware whitelist must still gate variants
// ===========================================================================

#if HELIX_HAS_IFS
TEST_CASE_METHOD(XMLTestFixture, "AD5X whitelist filters entries, not just headings",
                 "[filament][family][whitelist][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    // Stock AD5X firmware whitelist, derived from the backend rather than re-typed.
    sel.configure(std::nullopt, std::vector<std::string>(AmsBackendAd5xIfs::STOCK_WHITELIST.begin(),
                                                         AmsBackendAd5xIfs::STOCK_WHITELIST.end()));
    sel.populate();

    // Headings collapse to the four allowed families.
    CHECK(sel.type_options() == "ABS\nPETG\nPLA\nTPU");

    // The PLA heading is only as permissive as the variants behind it: PLA-CF
    // and SILK are whitelisted and appear, PLA-GF and PLA-AERO are NOT
    // whitelisted and must not be reachable just because PLA is allowed.
    auto pla_types = types_of(products_under(sel, "PLA"));
    INFO("PLA entries: " << pla_types.size());
    CHECK(contains(pla_types, "PLA"));
    CHECK(contains(pla_types, "PLA-CF"));
    CHECK(contains(pla_types, "SILK"));
    CHECK_FALSE(contains(pla_types, "PLA-GF"));
    CHECK_FALSE(contains(pla_types, "PLA-AERO"));

    // Same for PETG: PETG-CF allowed, PETG-GF not.
    auto petg_types = types_of(products_under(sel, "PETG"));
    CHECK(contains(petg_types, "PETG"));
    CHECK(contains(petg_types, "PETG-CF"));
    CHECK_FALSE(contains(petg_types, "PETG-GF"));

    // And ABS: the whitelist has no ABS-CF, so it must not leak in.
    auto abs_types = types_of(products_under(sel, "ABS"));
    CHECK(contains(abs_types, "ABS"));
    CHECK_FALSE(contains(abs_types, "ABS-CF"));
    CHECK_FALSE(contains(abs_types, "ABS-GF"));

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}
#endif // HELIX_HAS_IFS

TEST_CASE_METHOD(XMLTestFixture, "an unwhitelisted family produces no heading at all",
                 "[filament][family][whitelist][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::nullopt, std::vector<std::string>{"PLA"});
    sel.populate();

    // Only PLA — and the PLA heading holds nothing but type=PLA products,
    // because no sibling variant is whitelisted.
    CHECK(sel.type_options() == "PLA");
    auto types = types_of(sel.products_for_test());
    REQUIRE_FALSE(types.empty());
    for (const auto& t : types) {
        INFO("unexpected variant under a PLA-only whitelist: " << t);
        CHECK(t == "PLA");
    }

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(XMLTestFixture, "no whitelist means every variant is reachable under its family",
                 "[filament][family][whitelist][catalog_selector]") {
    lv_obj_t* root = make_fragment();
    REQUIRE(root != nullptr);

    FilamentCatalogSelector sel;
    sel.attach(root);
    sel.configure(std::string("PLA"), std::nullopt);
    sel.populate();
    REQUIRE(sel.current_type() == "PLA");

    auto types = types_of(sel.products_for_test());
    CHECK(contains(types, "PLA"));
    CHECK(contains(types, "PLA-CF"));
    CHECK(contains(types, "PLA-GF"));
    CHECK(contains(types, "SILK"));

    sel.detach();
    helix::ui::UpdateQueue::instance().drain();
}
