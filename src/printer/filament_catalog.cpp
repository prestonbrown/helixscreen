// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"

#include "core/data_root_resolver.h"
#include "filament_database.h"
#include "filament_variants.h"
#include "helix_fs.h"
#include "json_utils.h"
#include "system/helix_paths.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "hv/json.hpp"

namespace helix::printer {

namespace hfs = helix::fs;
namespace tio = helix::text_io;

namespace detail {
std::string& user_overlay_dir_ref() {
    static std::string dir;
    return dir;
}
} // namespace detail

namespace {

/// The user overlay sits in the user config dir with the app's other writable
/// files, so the installer can link it out to printer_data. Resolved per call:
/// HELIX_CONFIG_DIR is read at the time of use, like every writable_path().
std::vector<std::string> user_paths() {
    const std::string& dir = detail::user_overlay_dir_ref();
    if (!dir.empty())
        return {dir + "/user_filaments.json"};
    return {helix::writable_path("user_filaments.json"), "../config/user_filaments.json"};
}

int get_int(const nlohmann::json& j, const char* key, int def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : def;
}

/// Resolve one product JSON into an EffectiveFilament, inheriting from its type.
///
/// The string fields go through safe_string for the same reason get_int above
/// uses find + is_number: config/user_filaments.json is hand-editable, and
/// nlohmann's .value() throws type_error.302 on a key that is PRESENT with a
/// null value (a missing key is fine). This function is the single resolution
/// point for every catalog load path, so one `"brand": null` used to unwind out
/// of FilamentCatalog::load_full() and take the whole catalog with it — a
/// per-field default keeps the cost to that field.
EffectiveFilament to_effective(const nlohmann::json& p) {
    EffectiveFilament e;
    e.id = helix::json_util::safe_string(p, "id");
    e.brand = helix::json_util::safe_string(p, "brand");
    e.name = helix::json_util::safe_string(p, "name");
    e.type = helix::json_util::safe_string(p, "type");

    auto base = filament::find_material(e.type); // std::optional<MaterialInfo>
    const int type_min = base ? base->nozzle_min : 0;
    const int type_max = base ? base->nozzle_max : 0;

    e.nozzle_min = get_int(p, "nozzle_min", type_min);
    e.nozzle_max = get_int(p, "nozzle_max", type_max);
    e.nozzle_recommended = get_int(p, "nozzle", base ? base->nozzle_recommended() : 0);
    e.bed_temp = get_int(p, "bed", base ? base->bed_temp : 0);
    e.chamber_temp_c = base ? base->chamber_temp_c : 0;
    e.dry_temp_c = base ? base->dry_temp_c : 0;
    e.dry_time_min = base ? base->dry_time_min : 0;
    e.density_g_cm3 = p.contains("density") && p["density"].is_number()
                          ? p["density"].get<float>()
                          : (base ? base->density_g_cm3 : 0.0f);
    e.compat_group = base ? base->compat_group : "";

    if (auto it = p.find("codes"); it != p.end() && it->is_object()) {
        for (auto& [scheme, code] : it->items()) {
            if (code.is_string())
                e.codes[scheme] = code.get<std::string>();
        }
    }
    return e;
}

/// Copy a product array, dropping entries that are not JSON objects.
///
/// The one gate every product passes through. A hand-edited overlay can easily
/// contain a stray scalar, and letting one through would produce a product with
/// an empty id — which then collides in by_id_ with every other malformed entry
/// and can shadow a real product. Dropping it costs exactly that entry.
std::vector<nlohmann::json> object_entries(const nlohmann::json& arr, const char* path) {
    std::vector<nlohmann::json> out;
    out.reserve(arr.size());
    for (const auto& item : arr) {
        if (item.is_object()) {
            out.push_back(item);
        } else {
            spdlog::warn("[filament] skipping non-object product entry in {}", path);
        }
    }
    return out;
}

enum class FileRead { Absent, Parsed, Corrupt };

/// The one way a filament JSON file is read. A missing file and one holding
/// nothing but whitespace are both Absent: an empty file is an overlay nobody
/// has written yet (a truncated seed, or `touch`), not a damaged one.
FileRead read_json_file(const std::string& path, nlohmann::json& doc) {
    if (path.empty())
        return FileRead::Absent;
    auto text = tio::read_file(path);
    if (!text)
        return FileRead::Absent;
    if (text->find_first_not_of(" \t\r\n") == std::string::npos)
        return FileRead::Absent;
    doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        spdlog::warn("[filament] {} does not parse as JSON", path);
        return FileRead::Corrupt;
    }
    return FileRead::Parsed;
}

std::vector<nlohmann::json> read_products(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        nlohmann::json doc;
        if (read_json_file(path, doc) != FileRead::Parsed)
            continue;
        if (doc.is_object() && doc.contains("filaments") && doc["filaments"].is_array())
            return object_entries(doc["filaments"], path.c_str());
        if (doc.is_array()) // user overlay is a bare array
            return object_entries(doc, path.c_str());
    }
    return {};
}

} // namespace

void FilamentCatalog::index() {
    by_id_.clear();
    by_code_.clear();
    for (size_t i = 0; i < products_.size(); ++i) {
        const auto& e = products_[i];
        by_id_[e.id] = i;
        for (const auto& [scheme, code] : e.codes)
            by_code_[scheme][code] = i;
    }
}

FilamentCatalog FilamentCatalog::load_from_file(const std::string& path, bool codes_only,
                                                const std::string& scheme) {
    FilamentCatalog cat;
    for (const auto& jp : read_products({path})) {
        auto e = to_effective(jp);
        if (codes_only && e.codes.find(scheme) == e.codes.end())
            continue;
        cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

FilamentCatalog FilamentCatalog::load_with_overlay(const std::string& builtin_path,
                                                   const std::string& overlay_path) {
    // Raw product JSON keyed by id, so overlay can override before resolution.
    std::unordered_map<std::string, nlohmann::json> merged;
    std::vector<std::string> order;
    for (const auto& jp : read_products({builtin_path})) {
        std::string id = helix::json_util::safe_string(jp, "id");
        if (merged.find(id) == merged.end())
            order.push_back(id);
        merged[id] = jp;
    }
    for (const auto& jp : read_products({overlay_path})) {
        std::string id = helix::json_util::safe_string(jp, "id");
        if (merged.find(id) == merged.end()) {
            order.push_back(id);
            merged[id] = jp;
        } else {
            merged[id].merge_patch(jp); // field-level override
        }
    }
    FilamentCatalog cat;
    for (const auto& id : order)
        cat.products_.push_back(to_effective(merged[id]));
    cat.index();
    return cat;
}

namespace {

/// Bumped on every successful user-overlay write. The code-slice cache stores
/// the value it was built at and rebuilds when it no longer matches, so a
/// product the user just added shows up without a restart.
std::atomic<uint64_t> g_user_overlay_generation{0};

std::mutex g_codes_cache_mutex;
/// scheme -> (generation it was built at, snapshot)
std::map<std::string, std::pair<uint64_t, std::shared_ptr<const FilamentCatalog>>> g_codes_cache;

} // namespace

uint64_t FilamentCatalog::user_overlay_generation() {
    return g_user_overlay_generation.load(std::memory_order_acquire);
}

std::shared_ptr<const FilamentCatalog>
FilamentCatalog::load_codes_cached(const std::string& scheme) {
    const uint64_t gen = g_user_overlay_generation.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_codes_cache_mutex);
        auto it = g_codes_cache.find(scheme);
        if (it != g_codes_cache.end() && it->second.first == gen) {
            return it->second.second;
        }
    }

    // Built outside the lock: load_codes() parses ~100 KB and we do not want a
    // save on the main thread blocking behind a rebuild on the WebSocket thread.
    // Two threads racing here both build and the later one wins — wasteful once,
    // never wrong, and cheaper than serializing every reader.
    auto built = std::make_shared<const FilamentCatalog>(load_codes(scheme));
    {
        std::lock_guard<std::mutex> lock(g_codes_cache_mutex);
        g_codes_cache[scheme] = {gen, built};
    }
    spdlog::debug("[filament] built '{}' code-slice cache (generation {})", scheme, gen);
    return built;
}

FilamentCatalog FilamentCatalog::load_codes(const std::string& scheme) {
    FilamentCatalog cat;
    for (const auto& jp : read_products(builtin_candidate_paths())) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    // User overlay may add coded products too.
    for (const auto& jp : read_products(user_paths())) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

namespace {

/// First path in the list that exists on disk, or "" if none do.
std::string first_existing(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (tio::open_file(path, "rb"))
            return path;
    }
    return "";
}

} // namespace

std::vector<std::string> FilamentCatalog::builtin_candidate_paths() {
    return {helix::asset_path("assets/filaments.json"), "../assets/filaments.json",
            "/opt/helixscreen/assets/filaments.json"};
}

FilamentCatalog FilamentCatalog::load_full() {
    return load_with_overlay(first_existing(builtin_candidate_paths()),
                             first_existing(user_paths()));
}

std::unordered_map<std::string, std::string>
FilamentCatalog::orca_id_map_from(const std::string& builtin_path,
                                  const std::string& overlay_path) {
    // catalog id -> the Orca id that product carries, built-ins first, then the
    // overlay. An overlay entry speaks for the whole product: it may have
    // changed what the product is, so one with no orca_id of its own carries
    // none, rather than inheriting the built-in's.
    std::unordered_map<std::string, std::string> by_product;
    for (const auto& jp : read_products({builtin_path}))
        by_product[helix::json_util::safe_string(jp, "id")] =
            helix::json_util::safe_string(jp, "orca_id");
    for (const auto& jp : read_products({overlay_path}))
        by_product[helix::json_util::safe_string(jp, "id")] =
            helix::json_util::safe_string(jp, "orca_id");

    // An Orca id carried by more than one product names none of them exactly:
    // child Orca profiles inherit their parent's id, so a shared id can belong
    // to another product or another material altogether.
    std::unordered_map<std::string, int> carriers;
    for (const auto& [id, orca] : by_product)
        if (!orca.empty())
            ++carriers[orca];

    std::unordered_map<std::string, std::string> out;
    for (auto& [id, orca] : by_product)
        if (!id.empty() && !orca.empty() && carriers[orca] == 1)
            out.emplace(id, std::move(orca));
    return out;
}

namespace {
std::mutex g_orca_ids_mutex;
uint64_t g_orca_ids_generation = 0;
std::shared_ptr<const std::unordered_map<std::string, std::string>> g_orca_ids;
} // namespace

std::string FilamentCatalog::orca_id_for(const std::string& catalog_id) {
    if (catalog_id.empty())
        return {};
    const uint64_t gen = g_user_overlay_generation.load(std::memory_order_acquire);
    std::shared_ptr<const std::unordered_map<std::string, std::string>> ids;
    {
        std::lock_guard<std::mutex> lock(g_orca_ids_mutex);
        if (g_orca_ids && g_orca_ids_generation == gen)
            ids = g_orca_ids;
    }
    if (!ids) {
        // Built outside the lock, as in load_codes_cached(): the parse is the
        // whole bundled catalog, and a racing build is wasted once, never wrong.
        ids = std::make_shared<const std::unordered_map<std::string, std::string>>(orca_id_map_from(
            first_existing(builtin_candidate_paths()), first_existing(user_paths())));
        std::lock_guard<std::mutex> lock(g_orca_ids_mutex);
        g_orca_ids = ids;
        g_orca_ids_generation = gen;
    }
    auto it = ids->find(catalog_id);
    return it == ids->end() ? std::string{} : it->second;
}

std::map<std::string, std::string> FilamentCatalog::load_user_orca_type_map() {
    return load_user_orca_type_map_from(first_existing(user_paths()));
}

std::map<std::string, std::string>
FilamentCatalog::load_user_orca_type_map_from(const std::string& path) {
    std::map<std::string, std::string> out;
    nlohmann::json doc;
    if (read_json_file(path, doc) != FileRead::Parsed)
        return out;
    // Only the object form carries orca_type_map. A bare array is a
    // product-only overlay (the historical minimum) and contributes nothing
    // here — by design, since users add Orca hints via the UI, which writes
    // the object form.
    if (!doc.is_object())
        return out;
    auto it = doc.find("orca_type_map");
    if (it == doc.end() || !it->is_object())
        return out;
    for (const auto& [k, v] : it->items()) {
        if (v.is_string())
            out[k] = v.get<std::string>();
    }
    return out;
}

std::string FilamentCatalog::choose_overlay_write_path(const std::vector<std::string>& paths) {
    std::string path = first_existing(paths);
    // Fresh install: no overlay exists yet, so first_existing() is empty. Fall
    // back to the primary path so the file can be created — without this the
    // very first save (from the edit modal) has nowhere to write and fails.
    if (path.empty() && !paths.empty())
        path = paths.front();
    return path;
}

bool FilamentCatalog::save_user_products(const std::vector<nlohmann::json>& products) {
    return save_user_products_to(products, choose_overlay_write_path(user_paths()));
}

std::vector<nlohmann::json> FilamentCatalog::load_user_products() {
    return read_products(user_paths());
}

std::vector<nlohmann::json> FilamentCatalog::load_user_products_from(const std::string& path) {
    return read_products({path});
}

std::vector<nlohmann::json> FilamentCatalog::load_user_types() {
    return load_user_types_from(first_existing(user_paths()));
}

std::vector<nlohmann::json> FilamentCatalog::load_user_types_from(const std::string& path) {
    nlohmann::json doc;
    if (read_json_file(path, doc) == FileRead::Parsed && doc.is_object() && doc.contains("types") &&
        doc["types"].is_array())
        return object_entries(doc["types"], path.c_str());
    return {};
}

bool FilamentCatalog::overlay_file_is_corrupt(const std::string& path) {
    nlohmann::json doc;
    return read_json_file(path, doc) == FileRead::Corrupt;
}

std::string FilamentCatalog::builtin_asset_path() {
    return first_existing(builtin_candidate_paths());
}

std::string FilamentCatalog::user_overlay_path() {
    return first_existing(user_paths());
}

bool FilamentCatalog::upsert_product(std::vector<nlohmann::json>& products,
                                     const nlohmann::json& product) {
    // safe_string handles both hazards .value() has here: a non-object receiver
    // (type_error.306) and a key present with a null value (type_error.302).
    // The is_object() checks stay for their non-throwing role — see the note in
    // remove_product below.
    const std::string id = helix::json_util::safe_string(product, "id");
    if (!id.empty()) {
        for (auto& p : products) {
            if (p.is_object() && helix::json_util::safe_string(p, "id") == id) {
                p = product; // replace in place, preserving list order
                return true;
            }
        }
    }
    products.push_back(product);
    return false;
}

bool FilamentCatalog::remove_product(std::vector<nlohmann::json>& products, const std::string& id) {
    const size_t before = products.size();
    products.erase(std::remove_if(products.begin(), products.end(),
                                  [&](const nlohmann::json& p) {
                                      // is_object() stays: it is not redundant
                                      // with safe_string when `id` is empty,
                                      // where dropping it would start matching
                                      // (and erasing) non-object entries.
                                      return p.is_object() &&
                                             helix::json_util::safe_string(p, "id") == id;
                                  }),
                   products.end());
    return products.size() != before;
}

bool FilamentCatalog::save_user_products_to(const std::vector<nlohmann::json>& products,
                                            const std::string& path) {
    return save_user_section_to("filaments", products, path);
}

bool FilamentCatalog::save_user_types(const std::vector<nlohmann::json>& types) {
    return save_user_types_to(types, choose_overlay_write_path(user_paths()));
}

bool FilamentCatalog::save_user_types_to(const std::vector<nlohmann::json>& types,
                                         const std::string& path) {
    return save_user_section_to("types", types, path);
}

bool FilamentCatalog::save_user_section_to(const char* key, nlohmann::json value,
                                           const std::string& path) {
    if (path.empty()) {
        spdlog::warn("[filament] save {}: no overlay path configured", key);
        return false;
    }

    // Read-modify-write: preserve the sections this call does not own. If the file is
    // missing, a bare array (legacy), or unparseable, start fresh with an empty
    // object — a corrupt existing file must not block the user's save.
    nlohmann::json doc = nlohmann::json::object();
    nlohmann::json parsed;
    switch (read_json_file(path, parsed)) {
    case FileRead::Absent:
        break;
    case FileRead::Parsed:
        // The legacy bare-array form carries only products: kept as the
        // `filaments` section, so a types save does not drop them, and a
        // products save overwrites them with the full list it was given.
        // Any other shape starts fresh.
        if (parsed.is_object())
            doc = std::move(parsed);
        else if (parsed.is_array())
            doc["filaments"] = std::move(parsed);
        break;
    case FileRead::Corrupt: {
        // We must not block the save, but the user may have hand-authored
        // sections in there. Preserve the original as a .bak (best-effort) so
        // it stays recoverable, then start fresh with an empty object.
        const std::string bak = path + ".bak";
        const bool backed_up = hfs::copy_file(path, bak, /*overwrite=*/true);
        spdlog::warn("[filament] starting {} fresh; {} the unparseable original to {}", path,
                     backed_up ? "backed up" : "could not back up", bak);
        break;
    }
    }

    // Atomic write: tmp file + rename. POSIX rename is atomic within a single
    // filesystem. The tmp file lives next to the target so the rename never
    // crosses a mount boundary.
    std::string target = helix::paths::write_target(path);

    // Ensure parent dir exists (the on-device runtime config dir is created
    // elsewhere, but tests / fresh installs may hit this path first).
    if (std::string parent{hfs::parent_path(target)}; !parent.empty()) {
        // False only when `parent` is not a directory afterwards.
        if (!hfs::create_directories(parent)) {
            spdlog::warn("[filament] save {}: cannot create parent dir {}: {}", key, parent,
                         std::strerror(errno));
            return false;
        }
    }

    doc[key] = std::move(value);

    if (!tio::write_file_atomic(target, helix::json_util::safe_dump(doc, 2))) {
        spdlog::warn("[filament] save {}: cannot write {}: {}", key, target, std::strerror(errno));
        return false;
    }

    // The overlay contributes to every code-slice snapshot, so a successful
    // write retires them. Bumped only after the rename lands: a failed save must
    // leave readers on the snapshot that still matches what is on disk.
    g_user_overlay_generation.fetch_add(1, std::memory_order_release);
    return true;
}

const EffectiveFilament* FilamentCatalog::resolve_code(const std::string& scheme,
                                                       const std::string& code) const {
    auto s = by_code_.find(scheme);
    if (s == by_code_.end())
        return nullptr;
    auto c = s->second.find(code);
    return c == s->second.end() ? nullptr : &products_[c->second];
}

const EffectiveFilament* FilamentCatalog::resolve_id(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &products_[it->second];
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_type(const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.type == type)
            out.push_back(&e);
    return out;
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_brand(const std::string& brand) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.brand == brand)
            out.push_back(&e);
    return out;
}

std::vector<std::string> FilamentCatalog::all_brands() const {
    std::set<std::string> seen;
    for (const auto& p : products_)
        seen.insert(p.brand);
    return {seen.begin(), seen.end()}; // sorted + deduped
}

std::vector<const EffectiveFilament*> FilamentCatalog::all_products() const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        out.push_back(&e);
    return out;
}

std::vector<std::string> FilamentCatalog::types_for_brand(const std::string& brand) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.brand == brand && seen.insert(p.type).second) {
            out.push_back(p.type);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> FilamentCatalog::all_types() const {
    std::set<std::string> seen;
    for (const auto& p : products_)
        seen.insert(p.type);
    return {seen.begin(), seen.end()}; // sorted + deduped
}

std::vector<std::string> FilamentCatalog::brands_for_type(const std::string& type) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.type == type && seen.insert(p.brand).second) {
            out.push_back(p.brand);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const EffectiveFilament*> FilamentCatalog::products_for(const std::string& brand,
                                                                    const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& p : products_) {
        if (p.brand == brand && p.type == type) {
            out.push_back(&p);
        }
    }
    return out;
}

namespace {

/// insert-if-absent over a case-insensitive key, preserving the first spelling.
template <typename T>
bool insert_ci(std::set<std::string>& keys, std::vector<T>& out, const std::string& key, T value) {
    const std::string lower = tio::to_lower(key);
    if (!keys.insert(lower).second) {
        return false;
    }
    out.push_back(std::move(value));
    return true;
}

} // namespace

std::vector<std::string> FilamentCatalog::all_families() const {
    std::set<std::string> keys;
    std::vector<std::string> out;
    for (const auto& p : products_) {
        insert_ci(keys, out, filament::display_family(p.type), filament::display_family(p.type));
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return tio::to_lower(a) < tio::to_lower(b);
    });
    return out;
}

std::vector<FilamentCatalog::FamilyVariant>
FilamentCatalog::variants_in_family(const std::string& family) const {
    std::set<std::string> keys;
    std::vector<FamilyVariant> out;
    for (const auto& p : products_) {
        const std::string fam = filament::display_family(p.type);
        if (!tio::equals_ignore_case(fam, family)) {
            continue;
        }
        FamilyVariant fv{filament::variant_label(p.type, fam), p.type};
        // Copy the key BEFORE the move: argument evaluation order is
        // unspecified, and a key bound to fv.label after std::move(fv) emptied
        // it reads back empty, collapsing every variant onto one entry.
        const std::string key = fv.label;
        insert_ci(keys, out, key, std::move(fv));
    }
    std::sort(out.begin(), out.end(), [](const FamilyVariant& a, const FamilyVariant& b) {
        if (a.label == "Basic" || b.label == "Basic") {
            return a.label == "Basic" && b.label != "Basic";
        }
        return tio::to_lower(a.label) < tio::to_lower(b.label);
    });
    if (out.empty()) {
        // A family the catalog does not know still gets one chip: the family
        // itself, unvarianted, so an unknown material round-trips.
        out.push_back({"Basic", family});
    }
    return out;
}

std::vector<std::string> FilamentCatalog::brands_in_family(const std::string& family) const {
    std::set<std::string> keys;
    std::vector<std::string> out;
    for (const auto& p : products_) {
        if (tio::equals_ignore_case(filament::display_family(p.type), family)) {
            insert_ci(keys, out, p.brand, p.brand);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return tio::to_lower(a) < tio::to_lower(b);
    });
    return out;
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_in_family(const std::string& brand, const std::string& family) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& p : products_) {
        if (tio::equals_ignore_case(p.brand, brand) &&
            tio::equals_ignore_case(filament::display_family(p.type), family)) {
            out.push_back(&p);
        }
    }
    sort_for_display(out, family);
    return out;
}

void sort_for_display(std::vector<const EffectiveFilament*>& products, const std::string& family) {
    const std::string family_lc = tio::to_lower(family);
    // 0 = named exactly the family, 1 = the family's name extended with no space
    // (a grade like "+" or a fill like "-CF"), 2 = everything else.
    auto tier_of = [&](const EffectiveFilament* p) {
        const std::string name_lc = tio::to_lower(p->name);
        if (name_lc == family_lc)
            return 0;
        if (name_lc.size() > family_lc.size() &&
            name_lc.compare(0, family_lc.size(), family_lc) == 0 &&
            name_lc[family_lc.size()] != ' ')
            return 1;
        return 2;
    };
    // Tier 2 groups base-type products ahead of each variant type's own run.
    auto variant_key = [&](const EffectiveFilament* p) -> std::string {
        std::string type_lc = tio::to_lower(p->type);
        return type_lc == family_lc ? std::string{} : type_lc;
    };
    // Within a run: the product named its own type, then the rest, then
    // "Support...".
    auto rank_of = [](const EffectiveFilament* p) {
        const std::string name_lc = tio::to_lower(p->name);
        if (name_lc == tio::to_lower(p->type))
            return 0;
        if (name_lc.rfind("support", 0) == 0)
            return 2;
        return 1;
    };
    std::stable_sort(products.begin(), products.end(),
                     [&](const EffectiveFilament* a, const EffectiveFilament* b) {
                         const int ta = tier_of(a);
                         const int tb = tier_of(b);
                         if (ta != tb)
                             return ta < tb;
                         if (ta == 0)
                             return false;
                         if (ta == 2) {
                             const std::string ka = variant_key(a);
                             const std::string kb = variant_key(b);
                             if (ka != kb)
                                 return ka < kb;
                             const int ra = rank_of(a);
                             const int rb = rank_of(b);
                             if (ra != rb)
                                 return ra < rb;
                             if (ra != 1)
                                 return false;
                         }
                         return tio::to_lower(a->name) < tio::to_lower(b->name);
                     });
}

} // namespace helix::printer
