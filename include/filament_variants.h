// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

/**
 * @file filament_variants.h
 * @brief Base-material (family) derivation from a decorated material string.
 *
 * A "variant" is a modifier applied to a base polymer: fiber fills (CF/GF),
 * foaming grades (AERO), speed grades (HS/HF), cosmetic finishes (Silk/Matte/
 * Wood/...). Variants may appear as a suffix ("PLA-CF", "PLA Silk") or as a
 * prefix ("HT-PLA-GF", "Silk PLA").
 *
 * Two callers share this one implementation:
 *  - FilamentMapper::materials_match() — reduces a firmware-reported slot
 *    material to something the filament database can look up a compat group for.
 *  - FilamentCatalogSelector — groups catalog TYPE strings under one heading so
 *    the picker shows "ASA" once instead of ASA / ASA-CF / ASA-GF / ASA-AERO.
 *
 * DISPLAY-ONLY for the picker: nothing here changes the `type` string a
 * selection emits. Grouping affects which heading a product is filed under,
 * never its identity, its temperatures, or what gets persisted/sent.
 */

namespace filament {

/**
 * @brief Reduce a decorated material name to its base polymer.
 *
 * Strips known variant affixes (see kVariantAffixes in the .cpp) from either
 * end at '-', '_' or ' ' boundaries, applies the explicit family override table
 * for polymer grades that string-stripping cannot reach ("PA6" -> "PA"), then
 * falls back to a shortest-known-prefix walk for unrecognised compound names
 * ("PLA SnapSpeed" -> "PLA").
 *
 * Affixes are only stripped when delimited by a separator, so a name that
 * merely *contains* an affix substring is never mangled ("PPA" stays "PPA",
 * "CoPE" stays "CoPE", "PET-CF" reduces to "PET" and never to "PE").
 *
 * @param name Material name, possibly an alias, possibly decorated.
 * @return Base material name, or @p name unchanged if nothing is recognised.
 */
std::string extract_base_material(std::string_view name);

/**
 * @brief Are these two names the same polymer, for compatibility purposes?
 *
 * Exact match, else reduce both through extract_base_material() and compare
 * their filament-database compat groups. The reduction is the point: comparing
 * groups on RAW names sends anything that is not literally a material-type row
 * ("PLA SnapSpeed", "HT-PLA-GF", most spool-database names) into
 * are_materials_compatible()'s unknown-material fallback, which answers
 * "compatible with everything".
 *
 * A name that survives reduction unrecognised still gets that permissive
 * answer, deliberately: a firmware-only or hand-typed material should not block
 * the user. A name the reduction CAN read is compared by its compat group.
 *
 * @see grades_match() for the finer question this one does not ask.
 */
bool materials_compatible(std::string_view a, std::string_view b);

/**
 * @brief Do two names carry the same set of grade-significant fillers?
 *
 * One step finer than extract_base_material(): both "PLA+" and "PLA-CF" reduce
 * to PLA, but only the second is a different thing to print. The axis is
 * whether the filament carries solid particles, not what the marketing calls
 * the grade -- fiber (CF/GF), foaming (AERO/LW) and particle fills (Wood/
 * Marble/Metal/Glow) change flow, cooling and nozzle wear; "+", Silk, Matte and
 * the HS/HF/HT speed and temperature ratings do not.
 *
 * Presupposes the base materials already match: this answers "same polymer,
 * different grade?", never "same polymer?". Callers gate on
 * FilamentMapper::materials_match() first.
 *
 * Unparseable names carry no filler, so an unrecognised string matches any
 * other unfilled name. Staying silent on a name we cannot read is the safe
 * direction for a warning.
 *
 * @param a First material name, possibly an alias, possibly decorated.
 * @param b Second material name.
 * @return true when both sides carry the same fillers (usually none).
 */
bool grades_match(std::string_view a, std::string_view b);

/**
 * @brief Does this name carry any grade-significant filler?
 *
 * The directional half of grades_match(): tells a caller which of two
 * mismatched names is the abrasive, low-flow one, so a warning can say which
 * way the risk runs.
 */
bool is_filled_grade(std::string_view name);

/**
 * @brief Display family heading for a catalog type string.
 *
 * Thin wrapper over extract_base_material() that guarantees a non-empty result:
 * a type we cannot reduce becomes its own family (a heading with one entry),
 * which keeps unknown//user-overlay types visible instead of silently dropped.
 * A recycled grade files under its polymer ("rPLA" is PLA, variant Recycled),
 * although extract_base_material() keeps it a material of its own.
 */
std::string display_family(std::string_view type);

/**
 * @brief The variant a @p type adds over its @p family, as a chip label.
 *
 * The inverse half of display_family(): the remainder the reducer strips,
 * spelled as one short label. "PLA" in family PLA is "Basic"; "PLA-CF" is
 * "CF"; "Silk PLA" is "Silk"; "PLA+" is "+"; the r-prefix recycled spellings
 * ("rPLA") read as "Recycled". A @p type that is not @p family decorated at
 * all (an alias like "SILK", an override grade like "PA6") labels as its own
 * remainder, and a @p type equal to its family (or empty) is "Basic".
 *
 * @param type Catalog or firmware type string, possibly decorated.
 * @param family The family display_family() reduces @p type to. Empty means
 *               derive it here.
 * @return One variant label, never empty.
 */
// NAMESPACE_OK: module convention - extract_base_material lives in namespace filament
std::string variant_label(std::string_view type, std::string_view family = {});

/**
 * @brief The type string a family and a variant label compose to.
 *
 * The spelling follows the catalog's own conventions: a bare family for
 * "Basic", the r-prefix for "Recycled", a leading word for the wordy finishes
 * ("Silk", "Matte", "Wood", "Marble", "Metal", "Glow"), and a hyphenated
 * suffix for everything else ("CF", "GF", "HS", "+"). Callers that hold a
 * catalog prefer the catalog's own sample type for the same label — this is
 * the synthesized fallback for a pair the catalog does not carry.
 */
// NAMESPACE_OK: module convention - extract_base_material lives in namespace filament
std::string compose_family_type(const std::string& family, const std::string& variant);

/**
 * @brief Reduce a display type to a string OrcaSlicer can actually match.
 *
 * OrcaSlicer matches a lane to a filament preset by exact, case-sensitive
 * equality on `filament_type` (Preset.cpp:3327). On no match it does NOT leave
 * the lane unset — it falls back to the first library preset whose name
 * contains "PLA" (Preset.cpp:3300), and that id then resolves successfully in
 * sync_ams_list, bypassing the smarter similarity search. So an unmatchable
 * string yields PLA temperatures on whatever is actually loaded.
 *
 * Emitting a LESS precise string is therefore safer than a precise-but-
 * unmatchable one. Resolution order:
 *   1. explicit override from orca_type_overrides
 *   2. the type itself, if present in orca_library_types
 *   3. extract_base_material(type) plus the type's one filler ("PA12-CF" is
 *      "PA-CF"), if that is in orca_library_types
 *   4. extract_base_material(type), if that is in orca_library_types
 *   5. "" — caller must omit the field entirely
 *
 * Both tables ship in assets/filaments.json, generated from OrcaSlicer's
 * filament library by scripts/import_orca_filaments.py. Only library presets
 * are used because they alone carry `compatible_printers: []`, making them
 * matchable on every printer regardless of installed vendor profiles.
 *
 * @param display_type HelixScreen's precise type ("ASA-GF"). May be firmware-
 *        reported, free text, or a dropdown spelling — not necessarily in the
 *        catalog.
 * @return Orca-matchable type, or "" when nothing is safely matchable.
 */
std::string orca_match_type(std::string_view display_type);

/**
 * @brief The material string a firmware or tracker field records for a type.
 *
 * For fields that must carry SOME material and that the slicer reads back
 * verbatim: a Spoolman filament's `material`, AFC's `SET_MATERIAL`, which AFC
 * copies into lane_data. orca_match_type(), else the base polymer, else the
 * trimmed type itself, so "ABS+" records as "ABS" and "PLA-CF" stays
 * "PLA-CF". Empty only for a blank type.
 */
// NAMESPACE_OK: namespace filament, beside orca_match_type
std::string slicer_material(std::string_view display_type);

/// True once the Orca tables have been loaded and are non-empty. A missing or
/// pre-change assets/filaments.json yields empty tables, which makes
/// orca_match_type() return "" for every input — safe for emit (we omit the
/// field) but destructive for the heal, which would strip material from every
/// record. Callers that WRITE based on orca_match_type must check this first.
bool orca_tables_available();

/// Test-only access to the process-global Orca resolution tables. These tables
/// are otherwise private to filament_variants.cpp — production reads them only
/// through orca_match_type() / orca_tables_available(). Kept behind a named
/// access class so the mutator can't be mistaken for public API; the method is
/// defined in filament_variants.cpp, where the tables live.
class FilamentVariantsTestAccess {
  public:
    /// Inject the Orca tables directly, bypassing the lazy asset load. Empty
    /// containers for both arguments restore lazy loading from
    /// assets/filaments.json on the next orca_match_type() call.
    static void set_orca_tables(std::set<std::string> library_types,
                                std::map<std::string, std::string> overrides);

    /**
     * @brief Run the Orca table reader over an in-memory document
     *
     * The production reader walks assets/filaments.json from a fixed search
     * path, which a test cannot redirect. This exposes the same parse over a
     * string so the extraction rules — which keys are captured, which nesting
     * levels are ignored, what a malformed document does — can be pinned
     * directly.
     *
     * @param json_text Document to read
     * @param[out] library_types Captured `orca_library_types` entries
     * @param[out] overrides Captured `orca_type_overrides` pairs
     * @param[out] error Parser message when the document is malformed
     * @return true if the document parsed AND its root was an object
     */
    static bool parse_orca_tables(const std::string& json_text,
                                  std::set<std::string>& library_types,
                                  std::map<std::string, std::string>& overrides,
                                  std::string& error);
};

/// Force the Orca tables to load now, on the calling thread, instead of
/// lazily on the first orca_match_type() call. Call once at startup, on the
/// MAIN thread, before any AMS/filament backend can reach orca_match_type()
/// from a WebSocket background thread — otherwise the first lane_data heal
/// pays for the assets/filaments.json parse while holding the Orca mutex on
/// that background thread. A no-op if the tables are already loaded.
void warm_orca_tables();

/// Merge user-contributed overrides from config/user_filaments.json's
/// `orca_type_map` into the live Orca override table. Each entry supersedes any
/// shipped entry with a case-insensitively matching key (resolution step 1 wins
/// outright, so user entries always beat shipped ones — even when the case
/// differs from a shipped key). An empty-string value means "emit nothing for
/// this type" — the documented suppress case. Call once from main-thread
/// startup AFTER warm_orca_tables(); merges under g_orca_mutex so it is safe
/// against concurrent orca_match_type() callers. Idempotent: a duplicate key
/// simply overwrites. Empty input is a no-op.
void merge_user_orca_overrides(const std::map<std::string, std::string>& overrides);

} // namespace filament

namespace helix {

/// A filament's surface finish: how its colour should be painted.
enum class FilamentFinish : unsigned char { None, Silk, Matte, Marble, Sparkle };

/**
 * @brief The cosmetic finish a filament name announces, if any.
 *
 * Reads whole words of @p name ("PLA Silk", "Silk-PLA", "Galaxy Black",
 * "PLA Matte"), case-insensitively, at the same '-', '_' and ' ' boundaries as
 * the variant affixes, plus '+', '/', '(' and ')'. Silk also answers to Satin
 * and Silky; Matte to Matt; Marble to Granite and Stone; Sparkle to Glitter,
 * Galaxy, Shimmer and Starlight. The first finish word wins. A name that only
 * contains a finish as a substring ("Silkworm") has none.
 */
FilamentFinish filament_finish_of(std::string_view name);

/// The word for @p finish ("Silk", "Matte", "Marble", "Sparkle"; "" for none):
/// what a finish subject carries, read back by filament_finish_of().
const char* filament_finish_name(FilamentFinish finish);

} // namespace helix
