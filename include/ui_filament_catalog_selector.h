// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_product_edit_modal.h"

#include "filament_catalog.h"
#include "lvgl.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/// Embeddable branded-filament selector controller. Drives a
/// "filament_catalog_selector" XML fragment (vendor/type dropdowns + product
/// list) created by the host. Owns no chrome and no lifetime beyond the
/// attached fragment — hosts call attach()/populate() when the fragment
/// becomes relevant and detach() before its widget tree dies.
///
/// Multiple live instances are supported (standalone picker modal + the AMS
/// editor's filament-details view): static XML callbacks resolve the owning
/// instance by walking up from the event target to a registered fragment root.
class FilamentCatalogSelector {
  public:
    /// Fired whenever the highlighted product changes; nullptr = cleared.
    using SelectionChangedCallback =
        std::function<void(const helix::printer::EffectiveFilament* ef)>;

    FilamentCatalogSelector() = default;
    ~FilamentCatalogSelector();
    FilamentCatalogSelector(const FilamentCatalogSelector&) = delete;
    FilamentCatalogSelector& operator=(const FilamentCatalogSelector&) = delete;

    /// Register catalog_select_* XML callbacks (idempotent, process-wide).
    static void register_callbacks();

    /// Bind to a created "filament_catalog_selector" fragment root.
    void attach(lv_obj_t* fragment_root);
    /// Unbind (call before the fragment's widget tree is destroyed).
    void detach();

    /// seed_type: pre-select the Type dropdown; allowed_types: whitelist
    /// filter (case-insensitive), nullopt = all types. seed_vendor: pre-select
    /// the Vendor dropdown to this brand (case-insensitive match) when it exists
    /// in the catalog; nullopt/empty or a brand absent from the catalog falls
    /// back to "Generic" (index 0). Lets a host that opens on an already-branded
    /// slot round-trip the vendor instead of snapping it to Generic.
    void configure(std::optional<std::string> seed_type,
                   std::optional<std::vector<std::string>> allowed_types,
                   std::optional<std::string> seed_vendor = std::nullopt);

    /// Extra vendor names to merge into the Vendor dropdown beyond the catalog
    /// brands — e.g. a host that fetched a live Spoolman vendor list and wants
    /// server-only vendors selectable so a seeded brand round-trips instead of
    /// snapping to Generic. Case-insensitively deduped against "Generic" + the
    /// catalog brands; genuinely new names are appended after the catalog list
    /// (Generic stays pinned at index 0). Keeps the selector data-source-agnostic
    /// — it receives a plain string list and never fetches anything itself.
    ///
    /// Default empty, so callers that never set it are unaffected. If the
    /// selector is already populated, this rebuilds the Vendor dropdown (and the
    /// dependent Type/product views) and re-applies the seed vendor so a late
    /// async fetch selects it; the caller may follow with preselect_first() to
    /// re-check the matching product.
    void set_additional_vendors(std::vector<std::string> vendors);

    /// Load the catalog fresh and (re)fill dropdowns + product list.
    void populate();
    /// Free the ~356-product catalog between opens.
    void clear_catalog();

    /// Currently highlighted product, or nullptr. Pointer is valid until the
    /// next populate()/clear_catalog().
    [[nodiscard]] const helix::printer::EffectiveFilament* highlighted() const;

    /// Highlight the first product of the current vendor+type if nothing is
    /// highlighted yet (hosts that show an already-defined filament want its
    /// matching variant pre-checked rather than an unchecked list). Records the
    /// picked product as the preselect anchor so a later dropdown round-trip can
    /// restore it (see set_preselect_on_change()).
    void preselect_first();

    /// Navigate the dropdowns to @p id's vendor + material family and highlight
    /// that exact product, recording it as the preselect anchor. Returns false
    /// (leaving the selector untouched) when @p id is empty or no longer
    /// resolves — a custom overlay product the user deleted, or a bundled id
    /// retired by an app update. Hosts restoring a saved pick call this FIRST
    /// and fall back to preselect_first() on false: preselect_first() can only
    /// ever land on ordered_products_for().front(), which for a vendor whose
    /// products all share one material collapses to lowercased-name
    /// alphabetical order and silently substitutes a different variant.
    bool preselect_product_id(const std::string& id);

    /// Opt-in: when enabled, a vendor/type dropdown change auto-highlights a
    /// product in the rebuilt list instead of leaving it unchecked — the
    /// anchor product if it survived into the new list, else the first row.
    /// Guarantees the list always shows a checked row so a host's Save can't
    /// silently drop an identity change. Empty rebuilt lists stay unchecked
    /// (host handles that case). Default off preserves the standalone picker's
    /// "nothing checked until the user taps a row" behavior.
    void set_preselect_on_change(bool enable);

    void set_selection_changed(SelectionChangedCallback cb);

    /// Show the per-row edit pencil (standalone catalog picker opts in; the AMS
    /// slot-assignment selector leaves it off, where catalog editing is out of
    /// place). Set before populate(). The "+ Add custom filament" row shows in
    /// both contexts regardless.
    void set_show_edit_affordances(bool v) {
        show_edit_affordances_ = v;
    }

    // === Introspection (tests + hosts) ===
    /// Whether @p vendor is the Favorites pseudo-vendor entry (the flat
    /// starred-products view that bypasses the Type dropdown). The sentinel
    /// string never matches a catalog or Spoolman brand.
    [[nodiscard]] static bool is_favorites_vendor(const std::string& vendor);
    [[nodiscard]] std::string current_vendor() const;
    /// Selected Type-dropdown text. Since grouping, this is a material FAMILY
    /// heading ("PLA"), not necessarily a catalog type — a family collapses
    /// PLA / PLA-CF / PLA-GF / PLA-AERO / SILK under one entry. Hosts that need
    /// the material a user actually picked must read highlighted()->type; this
    /// is only a last-resort fallback for a heading with no products behind it
    /// (a firmware-whitelisted type the catalog does not stock), where the
    /// heading text IS the whitelist type spelling. Empty in the favorites
    /// view: the Type dropdown is bypassed there, so its selection is stale
    /// and must never be read back as a material.
    [[nodiscard]] std::string current_type() const;
    [[nodiscard]] std::string type_options() const;

    // === Test drivers (mirror the XML event paths) ===
    void select_first_product_for_test();
    /// Highlight a specific product by id, mirroring a row tap.
    void select_product_for_test(const std::string& product_id);
    /// Flip a row's star, mirroring a star-icon tap.
    void toggle_star_for_test(const std::string& product_id);
    void change_vendor_for_test(uint32_t index);
    void change_type_for_test(uint32_t index);
    /// Product names in current display order (post display-ranking) for the
    /// active vendor+family — lets tests assert row order without walking the
    /// rendered widget tree.
    [[nodiscard]] std::vector<std::string> product_names_for_test() const;
    /// Ordered products for the active vendor+family. Lets tests assert which
    /// variant TYPES are grouped under a heading and that their temperatures
    /// stay distinct. Pointers valid until the next populate()/clear_catalog().
    [[nodiscard]] std::vector<const helix::printer::EffectiveFilament*> products_for_test() const;
    /// Row label the list renders for @p p under the active family heading —
    /// the variant-disambiguation string tests assert against.
    [[nodiscard]] std::string row_label_for_test(const helix::printer::EffectiveFilament* p) const;

  private:
    void populate_vendor_dropdown();
    void populate_type_dropdown();
    void rebuild_product_list();
    /// Every product of `vendor` whose type belongs to material family
    /// `family`, whitelist-filtered by its OWN type, ranked for display:
    /// base-type products first (plain material name first within them),
    /// then variants grouped by type alphabetically, "Support…" last.
    /// Used everywhere the selector renders the product list or auto-picks
    /// "the first product" so preselect lands on the plain base material
    /// rather than a fiber-filled variant or raw file order.
    std::vector<const helix::printer::EffectiveFilament*>
    ordered_products_for(const std::string& vendor, const std::string& family) const;
    /// Whitelist gate for a single catalog TYPE. Filtering happens per entry,
    /// never per heading: a family heading is only as permissive as the
    /// variants behind it, so a whitelisted printer never sees a variant its
    /// firmware would reject just because a sibling variant is allowed.
    [[nodiscard]] bool type_allowed(const std::string& type) const;
    /// Display family heading for a catalog type (thin wrapper for logging).
    [[nodiscard]] static std::string family_of(const std::string& type);
    void handle_vendor_changed();
    void handle_type_changed();
    void handle_row_selected(const std::string& product_id);
    /// Star-icon tap: flip the id's favorite state (persisted immediately by
    /// filament::toggle_favorite) and refresh the active view — starred rows
    /// float to the top of a vendor+type view; an unstarred row vanishes from
    /// the Favorites view, taking the highlight with it when it was the
    /// selected row.
    void handle_star_toggled(const std::string& product_id);
    /// Hide/show the Type dropdown group to match favorites mode (the flat
    /// list spans every type, so the type filter is meaningless there).
    void sync_type_group_visibility();
    /// Open the product-edit modal in add mode (the "+ Add custom filament" row).
    void handle_add_custom();
    /// Open the product-edit modal in edit mode for @p product_id (row edit icon).
    void handle_edit_product(const std::string& product_id);
    /// Resolve @p id, navigate the dropdowns to its vendor + family, highlight
    /// it, set the preselect anchor and notify. False = unresolvable (empty id
    /// or absent from the catalog), selector left untouched. Shared by
    /// refresh_after_edit() and preselect_product_id().
    bool focus_product(const std::string& id);
    /// Reload the catalog after a save/delete and, when @p focus_id resolves,
    /// navigate the dropdowns to its vendor+family and highlight it.
    void refresh_after_edit(const std::string& focus_id);
    /// Select a vendor / type-family in the dropdowns by string (no-op if absent).
    void select_vendor(const std::string& brand);
    void select_type_family(const std::string& family);
    /// After a dropdown rebuild with preselect_on_change_ set: highlight the
    /// anchor product if present in the new list, else the first row; notify
    /// nullptr only when the list is genuinely empty.
    void preselect_after_change();

    lv_obj_t* find_child(const char* name) const;

    static FilamentCatalogSelector* from_event(lv_event_t* e);
    static std::map<lv_obj_t*, FilamentCatalogSelector*>& registry();
    static bool callbacks_registered_;

    // Declarative row event callbacks (registered in register_callbacks(),
    // wired from the catalog_row / catalog_add_row XML components). Each resolves
    // its owning instance via from_event() and the row identity via
    // lv_obj_get_name (L069).
    static void on_row_clicked_cb(lv_event_t* e);
    static void on_row_edit_cb(lv_event_t* e);
    static void on_row_star_cb(lv_event_t* e);
    static void on_add_custom_cb(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    helix::printer::FilamentCatalog catalog_;
    std::optional<std::string> seed_type_;
    std::optional<std::string> seed_vendor_; // pre-select the Vendor dropdown
    std::optional<std::vector<std::string>> allowed_types_;
    std::string highlighted_id_;
    std::string preselect_anchor_id_; // product to restore on a dropdown round-trip
    bool preselect_on_change_ = false;
    bool show_edit_affordances_ = false;          // per-row edit pencil (picker opts in)
    std::vector<std::string> vendor_order_;       // dropdown index -> brand
    std::vector<std::string> additional_vendors_; // host-supplied, merged into the vendor list
    SelectionChangedCallback on_selection_changed_;

    // Product add/edit modal, opened from the list affordances. Owned here so
    // its completion callback can refresh this selector in place.
    FilamentProductEditModal edit_modal_;
};

} // namespace helix::ui
