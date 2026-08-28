// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_selector.h"

#include "ui_icon_codepoints.h"
#include "ui_utils.h"

#include "filament_variants.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>

namespace helix::ui {

bool FilamentCatalogSelector::callbacks_registered_ = false;

std::map<lv_obj_t*, FilamentCatalogSelector*>& FilamentCatalogSelector::registry() {
    static std::map<lv_obj_t*, FilamentCatalogSelector*> instances;
    return instances;
}

FilamentCatalogSelector* FilamentCatalogSelector::from_event(lv_event_t* e) {
    // Walk up from the event target until we hit a registered fragment root.
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    while (obj) {
        auto it = registry().find(obj);
        if (it != registry().end()) {
            return it->second;
        }
        obj = lv_obj_get_parent(obj);
    }
    spdlog::warn("[FilamentCatalogSelector] Event with no attached selector");
    return nullptr;
}

void FilamentCatalogSelector::register_callbacks() {
    if (callbacks_registered_)
        return;
    lv_xml_register_event_cb(nullptr, "catalog_select_vendor_changed_cb", [](lv_event_t* e) {
        if (auto* self = from_event(e))
            self->handle_vendor_changed();
    });
    lv_xml_register_event_cb(nullptr, "catalog_select_type_changed_cb", [](lv_event_t* e) {
        if (auto* self = from_event(e))
            self->handle_type_changed();
    });
    // Row callbacks fired by the catalog_row / catalog_add_row XML components.
    lv_xml_register_event_cb(nullptr, "catalog_row_clicked_cb", on_row_clicked_cb);
    lv_xml_register_event_cb(nullptr, "catalog_row_edit_cb", on_row_edit_cb);
    lv_xml_register_event_cb(nullptr, "catalog_add_custom_cb", on_add_custom_cb);
    callbacks_registered_ = true;
}

void FilamentCatalogSelector::on_row_clicked_cb(lv_event_t* e) {
    // The row carries the click event_cb, so current_target IS the row whose
    // name is the product id (L069). Edit-icon taps hit-test to the icon (its
    // own clickable target) and never reach here.
    auto* row = lv_event_get_current_target_obj(e);
    FilamentCatalogSelector* self = from_event(e);
    const char* id = row ? lv_obj_get_name(row) : nullptr;
    if (self && id)
        self->handle_row_selected(id);
}

void FilamentCatalogSelector::on_row_edit_cb(lv_event_t* e) {
    // Fired by the edit_icon; its parent is the row whose name is the product id.
    auto* icon = lv_event_get_current_target_obj(e);
    lv_obj_t* row = icon ? lv_obj_get_parent(icon) : nullptr;
    const char* id = row ? lv_obj_get_name(row) : nullptr;
    FilamentCatalogSelector* self = from_event(e);
    if (self && id)
        self->handle_edit_product(id);
}

void FilamentCatalogSelector::on_add_custom_cb(lv_event_t* e) {
    if (auto* self = from_event(e))
        self->handle_add_custom();
}

FilamentCatalogSelector::~FilamentCatalogSelector() {
    detach();
}

void FilamentCatalogSelector::attach(lv_obj_t* fragment_root) {
    detach();
    root_ = fragment_root;
    if (root_) {
        registry()[root_] = this;
    }
}

void FilamentCatalogSelector::detach() {
    if (root_) {
        registry().erase(root_);
        root_ = nullptr;
    }
}

void FilamentCatalogSelector::configure(std::optional<std::string> seed_type,
                                        std::optional<std::vector<std::string>> allowed_types,
                                        std::optional<std::string> seed_vendor) {
    seed_type_ = std::move(seed_type);
    seed_vendor_ = std::move(seed_vendor);
    allowed_types_ = std::move(allowed_types);
    // Each open starts from the catalog-only vendor list; a host that has extra
    // vendors (e.g. a live Spoolman list) re-supplies them via
    // set_additional_vendors() after populate(). Prevents a prior open's list
    // from leaking into an unrelated caller.
    additional_vendors_.clear();
    highlighted_id_.clear();
    preselect_anchor_id_.clear();
}

void FilamentCatalogSelector::set_additional_vendors(std::vector<std::string> vendors) {
    additional_vendors_ = std::move(vendors);
    // Not yet populated (no attached fragment / dropdown not built): populate()
    // will read additional_vendors_ when it next runs. Nothing to rebuild.
    if (!root_ || vendor_order_.empty())
        return;
    // Already populated -> rebuild the Vendor dropdown with the merged list and
    // re-apply the seed vendor. Also rebuild the dependent Type/product views and
    // drop any stale highlight so a product left checked under the pre-merge
    // vendor can't survive into a host Save. The caller may follow with
    // preselect_first() to re-check the matching product for the resolved vendor.
    highlighted_id_.clear();
    preselect_anchor_id_.clear();
    populate_vendor_dropdown();
    populate_type_dropdown();
    rebuild_product_list();
}

void FilamentCatalogSelector::populate() {
    catalog_ = helix::printer::FilamentCatalog::load_full(); // fresh load per open
    highlighted_id_.clear();
    preselect_anchor_id_.clear();
    populate_vendor_dropdown();
    populate_type_dropdown();
    rebuild_product_list();
}

void FilamentCatalogSelector::clear_catalog() {
    catalog_ = helix::printer::FilamentCatalog{};
    highlighted_id_.clear();
}

const helix::printer::EffectiveFilament* FilamentCatalogSelector::highlighted() const {
    if (highlighted_id_.empty())
        return nullptr;
    return catalog_.resolve_id(highlighted_id_);
}

void FilamentCatalogSelector::set_selection_changed(SelectionChangedCallback cb) {
    on_selection_changed_ = std::move(cb);
}

lv_obj_t* FilamentCatalogSelector::find_child(const char* name) const {
    return root_ ? lv_obj_find_by_name(root_, name) : nullptr;
}

std::string FilamentCatalogSelector::current_vendor() const {
    lv_obj_t* dd = find_child("vendor_dropdown");
    if (!dd)
        return {};
    uint32_t sel = lv_dropdown_get_selected(dd);
    return sel < vendor_order_.size() ? vendor_order_[sel] : std::string{};
}

std::string FilamentCatalogSelector::current_type() const {
    lv_obj_t* dd = find_child("type_dropdown");
    if (!dd)
        return {};
    char buf[64] = {};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    return buf;
}

std::string FilamentCatalogSelector::type_options() const {
    lv_obj_t* dd = find_child("type_dropdown");
    return dd ? std::string(lv_dropdown_get_options(dd)) : std::string();
}

void FilamentCatalogSelector::preselect_first() {
    if (!highlighted_id_.empty())
        return; // keep an existing selection
    auto products = ordered_products_for(current_vendor(), current_type());
    if (products.empty())
        return;
    handle_row_selected(products.front()->id);
    // Remember the entry pick so a dropdown round-trip back to this vendor+type
    // re-checks the same product rather than the first row.
    preselect_anchor_id_ = highlighted_id_;
}

void FilamentCatalogSelector::set_preselect_on_change(bool enable) {
    preselect_on_change_ = enable;
}

void FilamentCatalogSelector::preselect_after_change() {
    auto products = ordered_products_for(current_vendor(), current_type());
    if (products.empty()) {
        // Genuinely no product for this vendor+type (e.g. a firmware-whitelisted
        // material we haven't seeded). Leave unchecked; the host decides what a
        // Save with no highlight means.
        if (on_selection_changed_)
            on_selection_changed_(nullptr);
        return;
    }
    // Prefer the anchor (the identity the host entered with) if it survived into
    // the rebuilt list — user navigated back to the original type.
    if (!preselect_anchor_id_.empty()) {
        for (const auto* p : products) {
            if (p->id == preselect_anchor_id_) {
                handle_row_selected(p->id);
                return;
            }
        }
    }
    handle_row_selected(products.front()->id);
}

void FilamentCatalogSelector::select_first_product_for_test() {
    auto products = ordered_products_for(current_vendor(), current_type());
    if (products.empty())
        return;
    handle_row_selected(products.front()->id);
}

std::vector<std::string> FilamentCatalogSelector::product_names_for_test() const {
    std::vector<std::string> names;
    for (const auto* p : ordered_products_for(current_vendor(), current_type()))
        names.push_back(p->name);
    return names;
}

std::vector<const helix::printer::EffectiveFilament*>
FilamentCatalogSelector::products_for_test() const {
    return ordered_products_for(current_vendor(), current_type());
}

void FilamentCatalogSelector::select_product_for_test(const std::string& product_id) {
    handle_row_selected(product_id);
}

void FilamentCatalogSelector::change_vendor_for_test(uint32_t index) {
    lv_obj_t* dd = find_child("vendor_dropdown");
    if (!dd)
        return;
    lv_dropdown_set_selected(dd, index);
    handle_vendor_changed();
}

void FilamentCatalogSelector::change_type_for_test(uint32_t index) {
    lv_obj_t* dd = find_child("type_dropdown");
    if (!dd)
        return;
    lv_dropdown_set_selected(dd, index);
    handle_type_changed();
}

void FilamentCatalogSelector::populate_vendor_dropdown() {
    lv_obj_t* dd = find_child("vendor_dropdown");
    if (!dd)
        return;
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size())
            return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
    };
    // "Generic" pinned first, then the catalog brands (all_brands() is already
    // sorted+deduped), then any host-supplied extra vendors not already present.
    // The extra list (e.g. a live Spoolman vendor list) is merged case-
    // insensitively so a differently-cased server spelling never doubles a
    // catalog brand, and it is only appended — Generic stays index 0 and the
    // catalog order is untouched.
    vendor_order_.clear();
    vendor_order_.push_back("Generic");
    for (const auto& b : catalog_.all_brands()) {
        if (b != "Generic")
            vendor_order_.push_back(b);
    }
    for (const auto& extra : additional_vendors_) {
        if (extra.empty())
            continue;
        bool present = std::any_of(vendor_order_.begin(), vendor_order_.end(),
                                   [&](const std::string& v) { return ieq(v, extra); });
        if (!present)
            vendor_order_.push_back(extra);
    }
    std::string options;
    for (size_t i = 0; i < vendor_order_.size(); ++i) {
        if (i)
            options += "\n";
        options += vendor_order_[i];
    }
    lv_dropdown_set_options(dd, options.c_str());

    // Seed the vendor to the host-provided brand (case-insensitive) when it
    // exists in the merged list; otherwise pin "Generic" (index 0). This lets a
    // host opening on an already-branded slot round-trip the vendor instead of
    // the selector silently snapping it to Generic (which a subsequent Save would
    // then bake in, dropping the user's saved vendor). A Spoolman-only vendor
    // resolves here once its name arrives via set_additional_vendors().
    uint32_t seed_idx = 0; // Generic
    if (seed_vendor_ && !seed_vendor_->empty()) {
        for (size_t i = 0; i < vendor_order_.size(); ++i) {
            if (ieq(vendor_order_[i], *seed_vendor_)) {
                seed_idx = static_cast<uint32_t>(i);
                break;
            }
        }
    }
    lv_dropdown_set_selected(dd, seed_idx);
}

namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::string FilamentCatalogSelector::family_of(const std::string& type) {
    return filament::display_family(type);
}

bool FilamentCatalogSelector::type_allowed(const std::string& type) const {
    if (!allowed_types_)
        return true;
    // Case-insensitive match: a backend whitelist may spell a type differently
    // than the catalog ("pla" vs "PLA").
    const std::string type_lc = to_lower_copy(type);
    return std::any_of(allowed_types_->begin(), allowed_types_->end(),
                       [&](const std::string& a) { return to_lower_copy(a) == type_lc; });
}

void FilamentCatalogSelector::populate_type_dropdown() {
    lv_obj_t* dd = find_child("type_dropdown");
    if (!dd)
        return;

    // Headings are material FAMILIES, not raw types: PLA / PLA-CF / PLA-GF /
    // PLA-AERO / SILK collapse into one "PLA" entry. Variants stay separately
    // selectable as rows underneath — this is display grouping only, and the
    // `type` a selection emits is untouched.
    //
    // The whitelist is applied to each TYPE before its family is derived, so a
    // family heading only appears if at least one variant behind it is allowed,
    // and it never smuggles in a rejected sibling (AD5X allows PLA and PLA-CF
    // but not PLA-GF — the PLA heading must not surface PLA-GF).
    std::vector<std::string> families;
    std::set<std::string> seen_family;
    std::set<std::string> covered_types_lc; // whitelist types with a real product
    for (const auto& type : catalog_.types_for_brand(current_vendor())) {
        if (!type_allowed(type))
            continue;
        covered_types_lc.insert(to_lower_copy(type));
        std::string family = family_of(type);
        if (seen_family.insert(family).second)
            families.push_back(family);
    }
    std::sort(families.begin(), families.end());

    if (allowed_types_) {
        // Whitelist entries the catalog has no product for (e.g. AD5X "SILK"
        // under a vendor that stocks none) are still appended so users aren't
        // locked out of a firmware-supported material. These become their own
        // heading and keep the WHITELIST spelling, because current_type() is
        // read back as the material string on exactly this no-product path.
        for (const auto& allowed : *allowed_types_) {
            if (covered_types_lc.count(to_lower_copy(allowed)))
                continue;
            bool dup = std::any_of(families.begin(), families.end(), [&](const std::string& f) {
                return to_lower_copy(f) == to_lower_copy(allowed);
            });
            if (!dup)
                families.push_back(allowed);
        }
    }

    if (families.empty()) {
        // The vendor dropdown offers every live Spoolman vendor (6f3e5639f),
        // but the bundled catalog carries only 21 brands. Picking one it has
        // never heard of — "Ambrosia", "Likesilk", most of a real Spoolman
        // library — matches no product, and this dropdown then rendered with
        // ZERO rows, so the material could not be set at all (K2 Plus,
        // 2026-08-24). A firmware whitelist already backfills the same hole for
        // backends that publish one (8ff1c582d); CFS publishes none, so nothing
        // caught it. Offer every family the catalog knows from any brand — the
        // type is a property of the filament, not of who sold it.
        for (const auto& type : catalog_.all_types()) {
            if (!type_allowed(type))
                continue;
            std::string family = family_of(type);
            if (seen_family.insert(family).second)
                families.push_back(family);
        }
        std::sort(families.begin(), families.end());
    }

    if (!allowed_types_ && seed_type_ && !seed_type_->empty()) {
        // The slot's OWN material must stay selectable even when nothing above
        // covers it, or reopening the editor silently reassigns it. Spoolman
        // free-text materials land here: "ASA-GF" is not an Orca library type
        // and no catalog product carries it. Skipped when a firmware whitelist
        // is present — there the whitelist is authoritative and must not be
        // widened by whatever a slot happens to claim.
        const std::string seed_family_now = family_of(*seed_type_);
        const bool dup = std::any_of(families.begin(), families.end(), [&](const std::string& f) {
            return to_lower_copy(f) == to_lower_copy(seed_family_now);
        });
        if (!dup)
            families.push_back(seed_family_now);
    }

    // Seed by the FAMILY of the requested type: a host seeding "PLA-CF" wants
    // the PLA heading open, then preselect lands on the PLA-CF product.
    const std::string seed_family = seed_type_ ? family_of(*seed_type_) : std::string{};
    std::string options;
    int seed_idx = 0;
    for (size_t i = 0; i < families.size(); ++i) {
        if (i)
            options += "\n";
        options += families[i];
        if (seed_type_ && (families[i] == seed_family || families[i] == *seed_type_))
            seed_idx = static_cast<int>(i);
    }
    spdlog::debug("[FilamentCatalogSelector] vendor='{}' -> {} family headings", current_vendor(),
                  families.size());
    lv_dropdown_set_options(dd, options.empty() ? "" : options.c_str());
    lv_dropdown_set_selected(dd, seed_idx);
}

std::vector<const helix::printer::EffectiveFilament*>
FilamentCatalogSelector::ordered_products_for(const std::string& vendor,
                                              const std::string& family) const {
    // Collect every product of this vendor whose type belongs to `family`.
    // Whitelist gating is per TYPE (see type_allowed): a heading is only as
    // permissive as the individual variants behind it.
    std::vector<const helix::printer::EffectiveFilament*> products;
    for (const auto* p : catalog_.products_for_brand(vendor)) {
        if (!type_allowed(p->type))
            continue;
        if (family_of(p->type) == family)
            products.push_back(p);
    }

    const std::string family_lc = to_lower_copy(family);
    // Variant grouping key: base-type products ("" sorts first) cluster ahead of
    // variants, and each variant type ("ASA-CF", "ASA-GF") forms its own
    // contiguous run so a heading reads as base-then-variants rather than one
    // interleaved alphabetical soup.
    auto variant_key = [&](const helix::printer::EffectiveFilament* p) -> std::string {
        std::string type_lc = to_lower_copy(p->type);
        return type_lc == family_lc ? std::string{} : type_lc;
    };
    // Within a variant run: 0 = the plain material whose name is just the type,
    // 1 = everything else (alphabetical), 2 = "Support..." (sunk to the bottom,
    // file order preserved).
    auto rank_of = [&](const helix::printer::EffectiveFilament* p) -> int {
        std::string name_lc = to_lower_copy(p->name);
        if (name_lc == to_lower_copy(p->type))
            return 0;
        if (name_lc.rfind("support", 0) == 0)
            return 2;
        return 1;
    };
    std::stable_sort(products.begin(), products.end(),
                     [&](const helix::printer::EffectiveFilament* a,
                         const helix::printer::EffectiveFilament* b) {
                         std::string ka = variant_key(a);
                         std::string kb = variant_key(b);
                         if (ka != kb)
                             return ka < kb;
                         int ra = rank_of(a);
                         int rb = rank_of(b);
                         if (ra != rb)
                             return ra < rb;
                         if (ra != 1)
                             return false; // stable within ranks 0 and 2
                         return to_lower_copy(a->name) < to_lower_copy(b->name);
                     });
    return products;
}

std::string
FilamentCatalogSelector::row_label_for_test(const helix::printer::EffectiveFilament* p) const {
    if (!p)
        return {};
    // Mirrors rebuild_product_list(): name, plus the variant chip when the row's
    // type differs from the family heading it sits under.
    const std::string family = current_type();
    if (!p->type.empty() && p->type != family)
        return p->name + " " + p->type;
    return p->name;
}

void FilamentCatalogSelector::rebuild_product_list() {
    lv_obj_t* list = find_child("product_list");
    if (!list)
        return;
    helix::ui::safe_clean_children(list);

    // Leading "+ Add custom filament" affordance — declarative component with its
    // own click callback (catalog_add_custom_cb -> handle_add_custom). First row
    // so it reads as a "create new" action and never gets buried under the list.
    lv_xml_create(list, "filament_catalog_add_row", nullptr);

    // Data-driven theme colors (accent/text/muted) are set in C++ — the row's
    // structure, fonts, padding, radii and pressed styling all live in
    // ui_xml/components/filament_catalog_row.xml.
    lv_color_t accent = theme_manager_get_color("primary");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t muted = theme_manager_get_color("text_muted");
    const char* check = ui_icon::lookup_codepoint("check");
    const char* pencil = ui_icon::lookup_codepoint("pencil");
    const std::string family = current_type();

    for (const auto* p : ordered_products_for(current_vendor(), family)) {
        const bool is_current = (highlighted_id_ == p->id);
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "filament_catalog_row", nullptr));
        if (!row)
            continue;
        lv_obj_set_name(row, p->id.c_str()); // identity for click handlers (L069)

        if (auto* ind = lv_obj_find_by_name(row, "check_indicator")) {
            lv_label_set_text(ind, (is_current && check) ? check : "");
            lv_obj_set_style_text_color(ind, accent, 0);
        }
        if (auto* nm = lv_obj_find_by_name(row, "name_label")) {
            lv_label_set_text(nm, p->name.c_str());
            lv_obj_set_style_text_color(nm, is_current ? accent : text_color, 0);
        }
        // Variant chip: under a collapsed family heading the row's own type is
        // the only thing separating ASA-CF from ASA-GF, and it is exactly the
        // string this selection emits. Base-type rows carry no chip — the
        // heading already says it. Untranslated: material names are identifiers
        // (L070).
        if (auto* chip = lv_obj_find_by_name(row, "variant_chip")) {
            if (!p->type.empty() && p->type != family) {
                lv_label_set_text(chip, p->type.c_str());
                lv_obj_set_style_text_color(chip, is_current ? accent : muted, 0);
                lv_obj_remove_flag(chip, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (auto* temp = lv_obj_find_by_name(row, "temp_label")) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d\xC2\xB0 / %d\xC2\xB0", p->nozzle_recommended,
                     p->bed_temp);
            lv_label_set_text(temp, buf);
        }
        // Edit pencil: shown only where the host opts in (standalone picker),
        // not the AMS slot-assignment selector. The icon intercepts its own tap
        // declaratively (clickable, no event_bubble in the XML), so editing
        // never triggers row-select.
        if (auto* edit = lv_obj_find_by_name(row, "edit_icon")) {
            if (show_edit_affordances_) {
                if (pencil)
                    lv_label_set_text(edit, pencil); // XML already sets #icon_pencil
                lv_obj_remove_flag(edit, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void FilamentCatalogSelector::handle_vendor_changed() {
    highlighted_id_.clear();  // stale row no longer visible under the new vendor
    populate_type_dropdown(); // vendor changed -> types change
    rebuild_product_list();
    if (preselect_on_change_) {
        preselect_after_change(); // keep a checked row (invariant)
    } else if (on_selection_changed_) {
        on_selection_changed_(nullptr);
    }
}

void FilamentCatalogSelector::handle_type_changed() {
    highlighted_id_.clear(); // stale row no longer visible under the new type
    rebuild_product_list();
    if (preselect_on_change_) {
        preselect_after_change(); // keep a checked row (invariant)
    } else if (on_selection_changed_) {
        on_selection_changed_(nullptr);
    }
}

void FilamentCatalogSelector::handle_row_selected(const std::string& product_id) {
    highlighted_id_ = product_id;
    rebuild_product_list(); // redraw to move the checkmark
    if (on_selection_changed_)
        on_selection_changed_(highlighted());
}

void FilamentCatalogSelector::handle_add_custom() {
    edit_modal_.set_on_saved([this](const std::string& saved_id) { refresh_after_edit(saved_id); });
    edit_modal_.show_for_add(lv_screen_active());
}

void FilamentCatalogSelector::handle_edit_product(const std::string& product_id) {
    edit_modal_.set_on_saved([this](const std::string& saved_id) { refresh_after_edit(saved_id); });
    edit_modal_.show_for_edit(lv_screen_active(), product_id);
}

void FilamentCatalogSelector::select_vendor(const std::string& brand) {
    lv_obj_t* dd = find_child("vendor_dropdown");
    if (!dd)
        return;
    for (size_t i = 0; i < vendor_order_.size(); ++i) {
        if (vendor_order_[i] == brand) {
            lv_dropdown_set_selected(dd, static_cast<uint32_t>(i));
            return;
        }
    }
}

void FilamentCatalogSelector::select_type_family(const std::string& family) {
    lv_obj_t* dd = find_child("type_dropdown");
    if (!dd)
        return;
    const std::string opts = lv_dropdown_get_options(dd);
    uint32_t idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= opts.size(); ++i) {
        if (i == opts.size() || opts[i] == '\n') {
            if (opts.compare(start, i - start, family) == 0) {
                lv_dropdown_set_selected(dd, idx);
                return;
            }
            ++idx;
            start = i + 1;
        }
    }
}

bool FilamentCatalogSelector::focus_product(const std::string& id) {
    if (id.empty())
        return false;
    const auto* ef = catalog_.resolve_id(id);
    if (!ef)
        return false;
    select_vendor(ef->brand.empty() ? std::string("Generic") : ef->brand);
    populate_type_dropdown(); // the vendor change re-derives the family headings
    select_type_family(family_of(ef->type));
    highlighted_id_ = id;
    // Anchor it too, so a dropdown round-trip (preselect_after_change) restores
    // THIS product rather than the family's first row. refresh_after_edit used
    // to skip this, which meant a just-edited product silently lost its place
    // the moment the user browsed to another type and back.
    preselect_anchor_id_ = id;
    rebuild_product_list();
    if (on_selection_changed_)
        on_selection_changed_(highlighted());
    return true;
}

bool FilamentCatalogSelector::preselect_product_id(const std::string& id) {
    return focus_product(id);
}

void FilamentCatalogSelector::refresh_after_edit(const std::string& focus_id) {
    // Re-read the overlay from disk so the just-saved/removed product is
    // reflected, then rebuild dropdowns (a new brand may have appeared).
    catalog_ = helix::printer::FilamentCatalog::load_full();
    populate_vendor_dropdown();

    if (focus_product(focus_id))
        return;

    // Delete/restore, or the product no longer resolves: keep the current
    // vendor/type view and just repaint.
    populate_type_dropdown();
    rebuild_product_list();
}

} // namespace helix::ui
