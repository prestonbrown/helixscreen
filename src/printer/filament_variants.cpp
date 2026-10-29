// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_variants.h"

#include "filament_catalog.h"
#include "filament_database.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <mutex>

#include "hv/json.hpp"

namespace filament {

namespace {

/// @p filled marks an affix that changes what it is like to PRINT the
/// filament, not just what it looks like: solid particles in the melt, or a
/// foaming agent whose density is temperature-driven. Those cut the usable
/// flow rate, alter cooling, and (for every particle fill) wear a brass nozzle.
/// The unfilled ones are the same polymer on the same hardware -- speed and
/// temperature ratings describe a profile, Silk is a co-polymer blend, Matte is
/// a surface agent. The distinction drives grades_match(); base extraction
/// strips both kinds alike.
///
/// "Cosmetic" is NOT the dividing line. Wood, Marble, Metal and Glow are all
/// particle-filled -- glow-in-the-dark is strontium aluminate, which outwears
/// carbon fiber on a brass nozzle -- so they sit with the fiber fills.
struct VariantAffix {
    const char* name;
    bool filled;
};

/// Known variant affixes, matched case-insensitively and ONLY when delimited by
/// a '-', '_' or ' ' separator. Derived from the type strings actually present
/// in assets/filaments.json (CF, GF, AERO), the variant rows in
/// the material-type table (Silk/Matte/Wood/Marble/Metal/Glow), and the
/// prefixed product names the catalog carries (HT-PLA-GF, PLA-HS, PETG+HS,
/// PLA-LW, Bambu PETG HF).
///
/// Deliberately NOT listed: "ABS" (so "PC-ABS" keeps its own identity and its
/// ABS_ASA compat group), "Soft"/"95A"/"85A" (TPU shore grades), "Change"
/// (Color-Change). Adding a polymer name here would merge two real materials.
constexpr VariantAffix kVariantAffixes[] = {
    // Fiber fills
    {"CF", true},
    {"GF", true},
    // Foaming / lightweight grades
    {"AERO", true},
    {"LW", true},
    // Speed / temperature grades
    {"HS", false},
    {"HF", false},
    {"HT", false},
    // Cosmetic finishes -- unfilled
    {"Silk", false},
    {"Matte", false},
    // Cosmetic finishes -- particle filled
    {"Wood", true},
    {"Marble", true},
    {"Metal", true},
    {"Glow", true},
};

/// Family for PAHT-branded products. "PAHT" is not a standardized polymer
/// designation — it is a marketing category, and the underlying resin varies by
/// vendor. Every PAHT product in assets/filaments.json is PA12/PA612-class
/// (Bambu and Creality both name PA12 as the base resin) and prints in the
/// ordinary PA envelope, which is why it maps to PA. Other vendors ship
/// PPA-based PAHT-CF under the same type string, at PPA temperatures and
/// needing a hardened nozzle and sealed enclosure: verify the base resin before
/// adding a PAHT product from a new vendor rather than assuming this mapping.
constexpr const char* kPahtFamily = "PA";

/// Explicit family mapping for names that affix-stripping alone cannot reduce,
/// because the modifier is fused to the polymer name with no separator.
///
/// Numbered nylon grades collapse into PA: the catalog does not carry enough of
/// each to justify separate headings (PA-CF 14 products, PA 8, PA-GF 3,
/// PA6-CF 3, PA12 a single Generic product), and it files PA6-CF products under
/// BOTH type=PA-CF and type=PA6-CF — one "PA" heading papers over that split.
///
/// Self-mapping rows are documented STOPS, not no-ops: they assert that a name
/// which superficially looks reducible must be left alone.
struct FamilyOverride {
    const char* name;
    const char* family;
};
constexpr FamilyOverride kFamilyOverrides[] = {
    {"PA6", "PA"},
    {"PA12", "PA"},
    {"PA66", "PA"},
    {"PA612", "PA"},
    {"PAHT", kPahtFamily},
    // STOP: PPA (polyphthalamide) must NEVER be normalized or aliased to PA.
    // The names are one letter apart, but it is a semi-aromatic polyamide in a
    // different processing regime — 280-310C nozzle, 100-120C bed, sealed
    // enclosure and hardened nozzle, against PA's 260-290C/90-110C. It keeps
    // its own heading, with PPA-CF and PPA-GF filed under it.
    {"PPA", "PPA"},
    // STOP: copolyester elastomer ends in "PE" but is unrelated to polyethylene.
    {"CoPE", "CoPE"},
    // STOP: polyethylene is a polyolefin, NOT polyethylene terephthalate.
    // "PE-CF" must reduce to PE and "PET-CF" to PET — never across.
    {"PE", "PE"},
    {"PET", "PET"},
};

bool is_separator(char c) {
    return c == '-' || c == '_' || c == ' ';
}

const VariantAffix* find_variant_affix(std::string_view token) {
    for (const auto& affix : kVariantAffixes) {
        if (helix::text_io::equals_ignore_case(token, affix.name)) {
            return &affix;
        }
    }
    return nullptr;
}

/// One pass of affix removal. Returns true if @p s was shortened. When
/// @p removed is non-null it receives the table row that was stripped, or
/// nullptr for the trailing-"+" rule (which has no row).
bool strip_one_affix(std::string_view& s, const VariantAffix** removed = nullptr) {
    if (removed) {
        *removed = nullptr;
    }
    // Leading affix: "HT-PLA-GF" -> "PLA-GF", "Silk PLA" -> "PLA"
    for (size_t i = 0; i < s.size(); ++i) {
        if (!is_separator(s[i]))
            continue;
        if (const VariantAffix* affix = find_variant_affix(s.substr(0, i))) {
            std::string_view rest = s.substr(i + 1);
            if (!rest.empty()) {
                s = rest;
                if (removed) {
                    *removed = affix;
                }
                return true;
            }
        }
        break; // only the first token can be a leading affix
    }
    // Trailing affix: "PLA-CF" -> "PLA", "PLA Silk" -> "PLA"
    for (size_t i = s.size(); i > 0; --i) {
        if (!is_separator(s[i - 1]))
            continue;
        if (const VariantAffix* affix = find_variant_affix(s.substr(i))) {
            std::string_view rest = s.substr(0, i - 1);
            if (!rest.empty()) {
                s = rest;
                if (removed) {
                    *removed = affix;
                }
                return true;
            }
        }
        break; // only the last token can be a trailing affix
    }
    // Trailing "+": a vendor grade marker ("PLA+", "ASA+"), not a separate
    // polymer. "+" is not in is_separator() because it never delimits an affix
    // TOKEN — it is always fused to the polymer name, so it needs its own rule.
    // Returning true re-enters the caller's loop, letting "PLA+-CF" reduce fully.
    if (s.size() > 1 && s.back() == '+') {
        s.remove_suffix(1);
        return true;
    }
    return false;
}

/// The filled affixes @p name carries, upper-cased so spelling never splits a
/// set. Mirrors extract_base_material()'s reduction loop exactly -- same alias
/// resolution, same override early-out, same guard -- so a name the reducer can
/// read is a name this can read. Anything it cannot parse simply carries no
/// filler, which makes an unknown string match every other unfilled name.
std::set<std::string> filled_affixes_of(std::string_view name);

/// Explicit family override lookup, or empty if the name has no entry.
std::string_view family_override(std::string_view name) {
    for (const auto& row : kFamilyOverrides) {
        if (helix::text_io::equals_ignore_case(name, row.name)) {
            return row.family;
        }
    }
    return {};
}

std::set<std::string> filled_affixes_of(std::string_view name) {
    std::set<std::string> filled;
    std::string_view work = helix::text_io::trim(name);
    if (work.empty()) {
        return filled;
    }
    work = resolve_alias(work);
    for (int guard = 0; guard < 8; ++guard) {
        // An override ENDS the reduction (extract_base_material returns here),
        // so stop looking for affixes at the same point it does. Anything the
        // rounds before this one stripped has already been recorded.
        if (!family_override(work).empty()) {
            break;
        }
        const VariantAffix* removed = nullptr;
        if (!strip_one_affix(work, &removed)) {
            break;
        }
        if (removed && removed->filled) {
            std::string key(removed->name);
            key = helix::text_io::to_upper(key);
            filled.insert(std::move(key));
        }
    }
    return filled;
}

/// Lazily-loaded Orca tables. Guarded because backends resolve from their own
/// threads; the tables are immutable after load.
std::mutex g_orca_mutex;
bool g_orca_loaded = false;
std::set<std::string> g_orca_library_types;
std::map<std::string, std::string> g_orca_overrides;

/**
 * @brief SAX reader for the two Orca tables, skipping the rest of the file
 *
 * The tables we want are 426 bytes of strings living beside a 72 KB `filaments`
 * array we do not read here (FilamentCatalog owns that, and loads it on demand).
 * A DOM parse would materialize all 360 product objects to reach two sibling
 * keys — measured at 872 kB of allocation versus 44 kB for this handler, and
 * twice the parse time. That matters at boot rather than later because freeing
 * a DOM does not hand the pages back (see PrinterDatabase::compact()), so the
 * transient parse permanently raises the arena high-water mark.
 *
 * Depth is tracked so that keys inside `filaments` entries (depth 3) can never
 * be mistaken for the top-level keys (depth 1) that select a capture target.
 */
class OrcaTableSax : public nlohmann::json_sax<nlohmann::json> {
  public:
    std::set<std::string> library_types;
    std::map<std::string, std::string> overrides;
    bool root_is_object = false;

    bool key(string_t& val) override {
        if (depth_ == 1) {
            in_types_ = (val == "orca_library_types");
            in_overrides_ = (val == "orca_type_overrides");
        } else if (in_overrides_ && depth_ == 2) {
            pending_key_ = val;
        }
        return true;
    }

    bool string(string_t& val) override {
        if (in_types_ && depth_ == 2) {
            library_types.insert(val);
        } else if (in_overrides_ && depth_ == 2 && !pending_key_.empty()) {
            overrides[pending_key_] = val;
            pending_key_.clear();
        }
        return true;
    }

    bool start_object(std::size_t) override {
        if (depth_ == 0) {
            root_is_object = true;
        }
        ++depth_;
        return true;
    }
    bool end_object() override {
        --depth_;
        if (depth_ <= 1) {
            in_overrides_ = false;
        }
        return true;
    }
    bool start_array(std::size_t) override {
        ++depth_;
        return true;
    }
    bool end_array() override {
        --depth_;
        if (depth_ <= 1) {
            in_types_ = false;
        }
        return true;
    }

    // Everything else in the file is skipped rather than stored.
    bool null() override {
        return true;
    }
    bool boolean(bool) override {
        return true;
    }
    bool number_integer(number_integer_t) override {
        return true;
    }
    bool number_unsigned(number_unsigned_t) override {
        return true;
    }
    bool number_float(number_float_t, const string_t&) override {
        return true;
    }
    bool binary(binary_t&) override {
        return true;
    }
    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::detail::exception& e) override {
        error = e.what();
        return false;
    }

    std::string error;

  private:
    int depth_ = 0;
    bool in_types_ = false;
    bool in_overrides_ = false;
    std::string pending_key_;
};

/**
 * @brief Read both Orca tables out of JSON text
 *
 * @param[out] error Set only when the document is malformed; a well-formed
 *                   document whose root is not an object returns false with
 *                   @p error empty, so the caller can tell "corrupt" (warn)
 *                   apart from "wrong shape" (skip quietly).
 * @return true when the tables were extracted from an object root
 */
bool read_orca_tables(const std::string& text, std::set<std::string>& types,
                      std::map<std::string, std::string>& overrides, std::string& error) {
    OrcaTableSax sax;
    if (!nlohmann::json::sax_parse(text, &sax)) {
        error = sax.error;
        return false;
    }
    if (!sax.root_is_object) {
        return false;
    }
    types = std::move(sax.library_types);
    overrides = std::move(sax.overrides);
    return true;
}

/// Load the tables from the first readable asset. Caller holds g_orca_mutex.
void load_orca_tables_locked() {
    if (g_orca_loaded)
        return;
    g_orca_loaded = true; // one attempt; a missing asset must not retry per call
    for (const auto& path : helix::printer::FilamentCatalog::builtin_candidate_paths()) {
        const auto text = helix::text_io::read_file(path);
        if (!text)
            continue;
        std::set<std::string> types;
        std::map<std::string, std::string> overrides;
        std::string error;
        if (!read_orca_tables(*text, types, overrides, error)) {
            if (!error.empty()) {
                spdlog::warn("[filament] Orca table parse failed {}: {}", path, error);
            }
            continue;
        }
        g_orca_library_types = std::move(types);
        g_orca_overrides = std::move(overrides);
        spdlog::debug("[filament] loaded {} Orca library types, {} overrides from {}",
                      g_orca_library_types.size(), g_orca_overrides.size(), path);
        return;
    }
    // No asset: every lookup misses, so orca_match_type returns "" and the
    // caller omits `material`. Orca then shows the lane empty — visibly wrong
    // rather than confidently wrong, which is the safe failure direction.
    spdlog::warn("[filament] no Orca library tables found; lane_data will omit material");
}

/// Case-insensitive lookup against the library set. Orca's own match is
/// case-sensitive, so we return the CANONICAL spelling from the table, never
/// the caller's casing.
const std::string* find_library_type(const std::string& candidate) {
    auto exact = g_orca_library_types.find(candidate);
    if (exact != g_orca_library_types.end())
        return &*exact;
    for (const auto& t : g_orca_library_types) {
        if (helix::text_io::equals_ignore_case(t, candidate))
            return &t;
    }
    return nullptr;
}

} // namespace

std::string extract_base_material(std::string_view name) {
    std::string_view work = helix::text_io::trim(name);
    if (work.empty()) {
        return std::string(name);
    }

    // Resolve aliases first so decoration on the CANONICAL spelling is visible:
    // "SILK" -> "Silk PLA" -> (leading affix) -> "PLA".
    work = resolve_alias(work);

    // Strip known affixes from both ends until nothing more is recognised. The
    // override table is consulted each round so a fused grade name exposed by
    // stripping is caught ("PA6-CF" -> "PA6" -> "PA").
    for (int guard = 0; guard < 8; ++guard) {
        if (auto fam = family_override(work); !fam.empty()) {
            return std::string(fam);
        }
        if (!strip_one_affix(work)) {
            break;
        }
    }

    if (find_material(work).has_value()) {
        return std::string(work);
    }

    // Unrecognised compound name ("PLA SnapSpeed"): walk progressively shorter
    // prefixes at separator boundaries against the database.
    for (size_t i = work.size(); i > 0; --i) {
        if (!is_separator(work[i - 1]))
            continue;
        auto prefix = work.substr(0, i - 1);
        if (!prefix.empty() && find_material(prefix).has_value()) {
            return std::string(prefix);
        }
    }

    return std::string(work);
}

bool materials_compatible(std::string_view a, std::string_view b) {
    // Empty vs non-empty is always a mismatch. (Callers that want "unlabelled
    // means do not block me" check for empty themselves, before asking.)
    if (a.empty() != b.empty()) {
        return false;
    }
    if (helix::text_io::equals_ignore_case(a, b)) {
        return true;
    }
    return are_materials_compatible(extract_base_material(a), extract_base_material(b));
}

bool grades_match(std::string_view a, std::string_view b) {
    return filled_affixes_of(a) == filled_affixes_of(b);
}

bool is_filled_grade(std::string_view name) {
    return !filled_affixes_of(name).empty();
}

std::string display_family(std::string_view type) {
    std::string base = extract_base_material(type);
    // A recycled grade ("rPLA") is a material of its own to the database, but
    // a variant of its polymer on the shelf: it files under that family.
    if (base.size() > 1 && base[0] == 'r' && find_material(std::string_view(base).substr(1))) {
        return base.substr(1);
    }
    // A type we cannot reduce is its own family — one heading, one entry — so
    // user-overlay and firmware-only types stay reachable instead of vanishing.
    return base.empty() ? std::string(type) : base;
}

namespace {

/// Trim leading and trailing separator characters off a token remnant.
std::string_view trim_separators(std::string_view s) {
    while (!s.empty() && is_separator(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_separator(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// The wordy finishes spell "Silk PLA", never "PLA-Silk".
bool variant_leads_with_word(const std::string& label) {
    static const char* const k_leading[] = {"Silk", "Matte", "Wood", "Marble",  "Metal",
                                            "Glow", "AERO",  "LW",   "Recycled"};
    for (const char* word : k_leading) {
        if (helix::text_io::equals_ignore_case(label, word)) {
            return true;
        }
    }
    return false;
}

} // namespace

// NAMESPACE_OK: namespace filament, beside extract_base_material
std::string variant_label(std::string_view type, std::string_view family) {
    std::string_view work = helix::text_io::trim(type);
    if (work.empty()) {
        return "Basic";
    }
    // Resolve the alias first so decoration sits on the canonical spelling
    // ("SILK" -> "Silk PLA"), the same order extract_base_material uses.
    work = resolve_alias(work);
    const std::string fam = family.empty() ? display_family(work) : std::string(family);

    if (helix::text_io::equals_ignore_case(work, fam)) {
        return "Basic";
    }
    std::string_view rem;
    if (work.size() > fam.size() &&
        helix::text_io::equals_ignore_case(work.substr(0, fam.size()), fam)) {
        rem = trim_separators(work.substr(fam.size()));
    } else if (work.size() > fam.size() &&
               helix::text_io::equals_ignore_case(work.substr(work.size() - fam.size()), fam)) {
        rem = trim_separators(work.substr(0, work.size() - fam.size()));
    } else {
        // The reduction came from the override table or an alias; the whole
        // spelling is the variant as far as a chip can show it.
        rem = work;
    }
    if (rem.empty()) {
        return "Basic";
    }
    if (helix::text_io::equals_ignore_case(rem, "r")) {
        return "Recycled";
    }
    return std::string(rem);
}

// NAMESPACE_OK: namespace filament, beside extract_base_material
std::string compose_family_type(const std::string& family, const std::string& variant) {
    if (variant.empty() || variant == "Basic") {
        return family;
    }
    if (variant == "Recycled") {
        return "r" + family;
    }
    if (variant_leads_with_word(variant)) {
        return variant + " " + family;
    }
    // A "+" grade fuses to the polymer name ("PLA+", never "PLA-+").
    if (variant.front() == '+') {
        return family + variant;
    }
    return family + "-" + variant;
}

bool FilamentVariantsTestAccess::parse_orca_tables(const std::string& json_text,
                                                   std::set<std::string>& library_types,
                                                   std::map<std::string, std::string>& overrides,
                                                   std::string& error) {
    return read_orca_tables(json_text, library_types, overrides, error);
}

void FilamentVariantsTestAccess::set_orca_tables(std::set<std::string> library_types,
                                                 std::map<std::string, std::string> overrides) {
    std::lock_guard<std::mutex> lock(g_orca_mutex);
    g_orca_library_types = std::move(library_types);
    g_orca_overrides = std::move(overrides);
    // Empty tables mean "restore lazy load"; non-empty means "tests own these".
    g_orca_loaded = !(g_orca_library_types.empty() && g_orca_overrides.empty());
}

bool orca_tables_available() {
    std::lock_guard<std::mutex> lock(g_orca_mutex);
    load_orca_tables_locked();
    return !g_orca_library_types.empty();
}

void warm_orca_tables() {
    std::lock_guard<std::mutex> lock(g_orca_mutex);
    load_orca_tables_locked();
}

void merge_user_orca_overrides(const std::map<std::string, std::string>& overrides) {
    if (overrides.empty())
        return;
    std::lock_guard<std::mutex> lock(g_orca_mutex);
    // Ensure the shipped tables are present before merging on top — otherwise
    // warm_orca_tables() would race to populate them after the merge and (being
    // a no-op once g_orca_loaded latches) leave the user entries stranded in a
    // set that never got loaded. Belt-and-suspenders: callers pair this with a
    // warm_orca_tables() call, but the merge must be correct standalone too.
    load_orca_tables_locked();
    size_t added = 0, updated = 0;
    for (const auto& [k, v] : overrides) {
        // Case-insensitive replace: erase any existing entry whose key matches
        // case-insensitively before inserting the user's. Shipped override keys
        // are mixed-case (SILK, rPLA, Color-Change, ...); without this a
        // case-variant user key would coexist with the shipped one, and
        // orca_match_type()'s sorted iteration — not user precedence — would
        // pick the winner, silently ignoring the user's override.
        bool replaced = false;
        for (auto it = g_orca_overrides.begin(); it != g_orca_overrides.end();) {
            if (helix::text_io::equals_ignore_case(it->first, k)) {
                it = g_orca_overrides.erase(it);
                replaced = true;
            } else {
                ++it;
            }
        }
        g_orca_overrides[k] = v;
        replaced ? ++updated : ++added;
    }
    spdlog::debug("[filament] merged user orca_type_map: {} new, {} updated", added, updated);
}

std::string orca_match_type(std::string_view display_type) {
    std::string work(helix::text_io::trim(display_type));
    if (work.empty())
        return "";

    std::lock_guard<std::mutex> lock(g_orca_mutex);
    load_orca_tables_locked();

    // 1. Explicit override wins outright, including an intentional "" that
    //    means "this type must never be emitted".
    for (const auto& [k, v] : g_orca_overrides) {
        if (helix::text_io::equals_ignore_case(k, work))
            return v;
    }
    // 2. The type itself, if Orca's library carries it.
    if (const std::string* hit = find_library_type(work))
        return *hit;
    // 3. Base polymer, if the library carries that.
    std::string base = extract_base_material(work);
    if (const std::string* hit = find_library_type(base))
        return *hit;
    // 4. Nothing safe to say. Caller omits the field.
    return "";
}

} // namespace filament

namespace helix {

FilamentFinish filament_finish_of(std::string_view name) {
    struct FinishWord {
        const char* word;
        FilamentFinish finish;
    };
    static constexpr FinishWord kWords[] = {
        {"Silk", FilamentFinish::Silk},         {"Silky", FilamentFinish::Silk},
        {"Satin", FilamentFinish::Silk},        {"Matte", FilamentFinish::Matte},
        {"Matt", FilamentFinish::Matte},        {"Marble", FilamentFinish::Marble},
        {"Granite", FilamentFinish::Marble},    {"Stone", FilamentFinish::Marble},
        {"Sparkle", FilamentFinish::Sparkle},   {"Glitter", FilamentFinish::Sparkle},
        {"Galaxy", FilamentFinish::Sparkle},    {"Shimmer", FilamentFinish::Sparkle},
        {"Starlight", FilamentFinish::Sparkle},
    };
    auto is_boundary = [](char c) {
        return filament::is_separator(c) || c == '+' || c == '/' || c == '(' || c == ')' ||
               c == ',';
    };
    size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && is_boundary(name[i])) {
            ++i;
        }
        size_t end = i;
        while (end < name.size() && !is_boundary(name[end])) {
            ++end;
        }
        const std::string_view token = name.substr(i, end - i);
        for (const auto& w : kWords) {
            if (helix::text_io::equals_ignore_case(token, w.word)) {
                return w.finish;
            }
        }
        i = end;
    }
    return FilamentFinish::None;
}

const char* filament_finish_name(FilamentFinish finish) {
    switch (finish) {
    case FilamentFinish::Silk:
        return "Silk"; // i18n: do not translate
    case FilamentFinish::Matte:
        return "Matte"; // i18n: do not translate
    case FilamentFinish::Marble:
        return "Marble"; // i18n: do not translate
    case FilamentFinish::Sparkle:
        return "Sparkle"; // i18n: do not translate
    case FilamentFinish::None:
        break;
    }
    return "";
}

} // namespace helix
