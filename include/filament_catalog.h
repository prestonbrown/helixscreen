// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::printer {

namespace detail {
/// When non-empty, the user overlay is `<dir>/user_filaments.json` instead of
/// the user config dir. The test sandbox points it at its own directory.
std::string& user_overlay_dir_ref();
} // namespace detail

/// Fully-resolved filament: product deltas merged over the base material type.
struct EffectiveFilament {
    std::string id, brand, name, type;
    int nozzle_min = 0;
    int nozzle_max = 0;
    int nozzle_recommended = 0;
    int bed_temp = 0;
    int chamber_temp_c = 0;
    int dry_temp_c = 0;
    int dry_time_min = 0;
    float density_g_cm3 = 0.0f;
    std::string compat_group;
    std::map<std::string, std::string> codes; ///< scheme -> code
};

/// Transient, on-demand filament catalog. NO resident singleton: construct via a
/// static loader, query, then let it fall out of scope (frees all parsed data).
/// The one exception is load_codes_cached(), which keeps a shared snapshot of a
/// single code-scheme slice alive for callers on a repeating path — see there
/// for why the transient rule does not fit that case.
class FilamentCatalog {
  public:
    FilamentCatalog() = default;
    FilamentCatalog(const FilamentCatalog&) = delete;
    FilamentCatalog& operator=(const FilamentCatalog&) = delete;
    FilamentCatalog(FilamentCatalog&&) = default;
    FilamentCatalog& operator=(FilamentCatalog&&) = default;

    /// Whole catalog (built-in + user overlay). For the picker.
    static FilamentCatalog load_full();
    /// Only products carrying a code in `scheme`. For CFS decode (small slice).
    static FilamentCatalog load_codes(const std::string& scheme);

    /**
     * @brief Cached load_codes(), for callers on a repeating path
     *
     * load_codes() re-reads and re-parses the whole 100 KB bundled catalog —
     * ~872 kB of transient heap — to keep the products carrying `scheme`
     * (73 of 360 for "cfs", 14 KB serialized). That is fine once, and wrong per
     * CFS box update, which is why AmsBackendCfs::parse_box_status uses this.
     *
     * The result is shared and immutable: a rebuild swaps in a new snapshot and
     * existing holders keep the one they took, so a reader on the WebSocket
     * thread cannot have the catalog freed under it by a save on the main
     * thread. Invalidated by user-overlay writes (save_user_products_to), so a
     * user-added product still appears without a restart.
     */
    static std::shared_ptr<const FilamentCatalog> load_codes_cached(const std::string& scheme);

    /**
     * @brief OrcaSlicer's preset `filament_id` for catalog product @p catalog_id
     *
     * "" when the product carries no `orca_id`, or carries one that another
     * product in the effective catalog (built-ins plus user overlay) also
     * carries: a shared id does not name this product exactly. Keeps only a
     * flat catalog id -> Orca id map resident, rebuilt after a user-overlay
     * write. Thread-safe.
     */
    static std::string orca_id_for(const std::string& catalog_id);
    /// The map orca_id_for() serves, from explicit paths (tests). An overlay
    /// entry for a built-in id replaces its Orca id, with none when it carries none.
    static std::unordered_map<std::string, std::string>
    orca_id_map_from(const std::string& builtin_path, const std::string& overlay_path);

    /// Bumped by every successful user-overlay write; the cache keys off it.
    /// Exposed so tests can assert invalidation rather than infer it.
    static uint64_t user_overlay_generation();
    /// Explicit path (tests / non-default locations).
    static FilamentCatalog load_from_file(const std::string& path, bool codes_only,
                                          const std::string& scheme);
    /// Explicit built-in + overlay paths; overlay overrides existing ids and adds new ones.
    static FilamentCatalog load_with_overlay(const std::string& builtin_path,
                                             const std::string& overlay_path);

    /// Read the user overlay's `orca_type_map` (Helix display name -> Orca wire
    /// string). Returns an empty map if the user overlay is missing, is a bare
    /// array, or carries no `orca_type_map` key. A value of "" means "emit
    /// nothing for this type" (the suppress case). Single map by design: users
    /// contribute overrides, not library-type membership, which stays a
    /// shipped-asset concept. See FILAMENT_MANAGEMENT.md § "User overlay format".
    static std::map<std::string, std::string> load_user_orca_type_map();
    /// Explicit-path variant for tests / non-default locations.
    static std::map<std::string, std::string> load_user_orca_type_map_from(const std::string& path);

    /// Atomically replace the user overlay's `filaments` section with
    /// `products`, preserving its other sections (`types`, `orca_type_map`). Legacy bare-array
    /// overlays are migrated to object form on first save. Caller provides
    /// pre-built product objects (typically from a modal's form fields); each
    /// entry should carry at minimum an `id`. Written via temp-file + rename
    /// (POSIX rename is atomic within a filesystem), so a *process* crash
    /// mid-write never leaves a partial overlay. This is not a power-loss
    /// durability guarantee (no fsync), but the write is infrequent and the
    /// original is untouched until the rename succeeds. An unparseable existing
    /// file is preserved as `<path>.bak` before being replaced. Returns false
    /// on parse or I/O error (logged via spdlog at warn). Empty `products`
    /// writes a valid empty overlay (still preserves `orca_type_map`).
    static bool save_user_products(const std::vector<nlohmann::json>& products);
    /// Explicit-path variant for tests / non-default locations.
    static bool save_user_products_to(const std::vector<nlohmann::json>& products,
                                      const std::string& path);
    /// Resolve the overlay write target from a search list: the first path that
    /// already exists on disk, or the first list entry when none do (a fresh
    /// install must still be able to create the file). Returns "" only for an
    /// empty list. Exposed for tests; `save_user_products` uses it with the
    /// built-in user-path list.
    static std::string choose_overlay_write_path(const std::vector<std::string>& paths);

    /// Read the user overlay's authored `filaments` entries as raw JSON objects
    /// (sparse — type inheritance is NOT applied), for read-modify-write by the
    /// product-edit UI. Accepts both the object form and the legacy bare array.
    /// Empty when no overlay exists or it can't be parsed. This is the read half
    /// of the save_user_products round-trip.
    static std::vector<nlohmann::json> load_user_products();
    /// Explicit-path variant for tests / non-default locations.
    static std::vector<nlohmann::json> load_user_products_from(const std::string& path);

    /// The overlay's `types` entries as raw objects: a sparse patch over the
    /// shipped type of the same name, or a new type. Empty for a bare-array
    /// overlay, a missing one, or one that cannot be parsed.
    static std::vector<nlohmann::json> load_user_types();
    /// Explicit-path variant for tests / non-default locations.
    static std::vector<nlohmann::json> load_user_types_from(const std::string& path);
    /// Replace the overlay's `types` section, with save_user_products' write
    /// guarantees and preserving the other sections.
    static bool save_user_types(const std::vector<nlohmann::json>& types);
    /// Explicit-path variant for tests / non-default locations.
    static bool save_user_types_to(const std::vector<nlohmann::json>& types,
                                   const std::string& path);

    /// True when @p path exists, holds more than whitespace, and does not parse.
    /// A missing or blank file is no overlay yet, not a corrupt one.
    static bool overlay_file_is_corrupt(const std::string& path);

    /// Where the shipped filaments.json may live, in search order. The first
    /// entry resolves under asset_root(), the only one the firmware can reach.
    static std::vector<std::string> builtin_candidate_paths();
    /// The first shipped asset on the search path that exists, or "".
    static std::string builtin_asset_path();
    /// The first user overlay on the search path that exists, or "".
    static std::string user_overlay_path();
    /// Insert `product` into `products`, replacing an existing entry with the
    /// same `"id"` (exact match) in place (preserving order), else appending.
    /// Returns true if an existing entry was replaced (an edit), false if
    /// appended (an add). A non-object `product`, or one with no/empty `"id"`,
    /// is appended.
    static bool upsert_product(std::vector<nlohmann::json>& products,
                               const nlohmann::json& product);
    /// Remove every entry whose `"id"` equals `id`. Returns true if one or more
    /// were removed. Used by the edit UI's Delete / Restore-Defaults action.
    static bool remove_product(std::vector<nlohmann::json>& products, const std::string& id);

    const EffectiveFilament* resolve_code(const std::string& scheme, const std::string& code) const;
    const EffectiveFilament* resolve_id(const std::string& id) const;
    std::vector<const EffectiveFilament*> products_for_type(const std::string& type) const;

    /// One chip of the spool editor's variant row: the label shown and a real
    /// catalog type that carries it (the exact spelling an identity emits when
    /// this variant is chosen — never re-composed by hand).
    struct FamilyVariant {
        std::string label;       ///< "Basic", "CF", "Silk", "Recycled", ...
        std::string sample_type; ///< "PLA", "PLA-CF", "Silk PLA", "rPLA", ...
    };

    /// Every material family in the catalog (the distinct display_family()
    /// headings), case-insensitively deduped, alphabetical.
    std::vector<std::string> all_families() const;

    /// The variant labels the catalog carries for @p family, deduped
    /// case-insensitively. "Basic" first when the family has a bare type, then
    /// alphabetical. A family the catalog does not know answers one entry:
    /// the family itself as "Basic".
    std::vector<FamilyVariant> variants_in_family(const std::string& family) const;

    /// Every brand with at least one product in @p family (case-insensitive
    /// family match on display_family()), deduped case-insensitively and
    /// alphabetical. "Generic" appears only when a Generic product exists.
    std::vector<std::string> brands_in_family(const std::string& family) const;

    /// The products @p brand sells in @p family — that brand's lines for that
    /// material, in sort_for_display() order. Case-insensitive on both arguments.
    std::vector<const EffectiveFilament*> products_in_family(const std::string& brand,
                                                             const std::string& family) const;
    std::vector<const EffectiveFilament*> products_for_brand(const std::string& brand) const;
    std::vector<std::string> all_brands() const;
    std::vector<const EffectiveFilament*> all_products() const;
    std::vector<std::string> types_for_brand(const std::string& brand) const;
    /// Every material type in the catalog, from any brand. The fallback for a
    /// vendor the catalog does not carry: a type is a property of the filament,
    /// not of who sold it.
    std::vector<std::string> all_types() const;
    std::vector<std::string> brands_for_type(const std::string& type) const;
    std::vector<const EffectiveFilament*> products_for(const std::string& brand,
                                                       const std::string& type) const;

  private:
    static bool save_user_section_to(const char* key, nlohmann::json value,
                                     const std::string& path);

    std::vector<EffectiveFilament> products_;
    std::unordered_map<std::string, size_t> by_id_;
    // scheme -> (code -> product index)
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> by_code_;
    void index();
};

/// Order @p products for display under the @p family heading (case-insensitive):
/// the product named exactly the family ("PLA"), then names that extend it with
/// no space ("PLA+", "PLA+ 2.0", "PLA-CF"), alphabetical; then everything else,
/// base-type products ahead of each variant type's own run, alphabetical within
/// a run with "Support..." last. Stable, so equal rows keep catalog order.
void sort_for_display(std::vector<const EffectiveFilament*>& products, const std::string& family);

} // namespace helix::printer
