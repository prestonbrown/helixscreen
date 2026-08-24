// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"

#include "filament_database.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>

#include "hv/json.hpp"

namespace helix::printer {

namespace {

// Search paths for the built-in catalog (mirrors the old CFS loader).
const char* BUILTIN_PATHS[] = {"assets/filaments.json", "../assets/filaments.json",
                               "/opt/helixscreen/assets/filaments.json"};
const char* USER_PATHS[] = {"config/user_filaments.json", "../config/user_filaments.json"};

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

std::vector<nlohmann::json> read_products(const char* const* paths, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::ifstream f(paths[i]);
        if (!f.is_open())
            continue;
        try {
            auto doc = nlohmann::json::parse(f);
            if (doc.is_object() && doc.contains("filaments") && doc["filaments"].is_array())
                return object_entries(doc["filaments"], paths[i]);
            if (doc.is_array()) // user overlay is a bare array
                return object_entries(doc, paths[i]);
        } catch (const std::exception& e) {
            spdlog::warn("[filament] parse failed {}: {}", paths[i], e.what());
        }
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
    const char* paths[] = {path.c_str()};
    FilamentCatalog cat;
    for (const auto& jp : read_products(paths, 1)) {
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
    const char* bpaths[] = {builtin_path.c_str()};
    const char* opaths[] = {overlay_path.c_str()};

    // Raw product JSON keyed by id, so overlay can override before resolution.
    std::unordered_map<std::string, nlohmann::json> merged;
    std::vector<std::string> order;
    for (const auto& jp : read_products(bpaths, 1)) {
        std::string id = helix::json_util::safe_string(jp, "id");
        if (merged.find(id) == merged.end())
            order.push_back(id);
        merged[id] = jp;
    }
    for (const auto& jp : read_products(opaths, 1)) {
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
    for (const auto& jp : read_products(BUILTIN_PATHS, std::size(BUILTIN_PATHS))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    // User overlay may add coded products too.
    for (const auto& jp : read_products(USER_PATHS, std::size(USER_PATHS))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

namespace {

/// First path in the list that exists on disk, or "" if none do.
std::string first_existing(const char* const* paths, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::ifstream f(paths[i]);
        if (f.is_open())
            return paths[i];
    }
    return "";
}

} // namespace

FilamentCatalog FilamentCatalog::load_full() {
    return load_with_overlay(first_existing(BUILTIN_PATHS, std::size(BUILTIN_PATHS)),
                             first_existing(USER_PATHS, std::size(USER_PATHS)));
}

std::map<std::string, std::string> FilamentCatalog::load_user_orca_type_map() {
    return load_user_orca_type_map_from(first_existing(USER_PATHS, std::size(USER_PATHS)));
}

std::map<std::string, std::string>
FilamentCatalog::load_user_orca_type_map_from(const std::string& path) {
    std::map<std::string, std::string> out;
    if (path.empty())
        return out;
    std::ifstream f(path);
    if (!f.is_open())
        return out;
    try {
        auto doc = nlohmann::json::parse(f);
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
    } catch (const std::exception& e) {
        spdlog::warn("[filament] user orca_type_map parse failed {}: {}", path, e.what());
    }
    return out;
}

std::string FilamentCatalog::choose_overlay_write_path(const char* const* paths, std::size_t n) {
    std::string path = first_existing(paths, n);
    // Fresh install: no overlay exists yet, so first_existing() is empty. Fall
    // back to the primary path so the file can be created — without this the
    // very first save (from the edit modal) has nowhere to write and fails.
    if (path.empty() && n > 0)
        path = paths[0];
    return path;
}

bool FilamentCatalog::save_user_products(const std::vector<nlohmann::json>& products) {
    return save_user_products_to(products,
                                 choose_overlay_write_path(USER_PATHS, std::size(USER_PATHS)));
}

std::vector<nlohmann::json> FilamentCatalog::load_user_products() {
    return read_products(USER_PATHS, std::size(USER_PATHS));
}

std::vector<nlohmann::json> FilamentCatalog::load_user_products_from(const std::string& path) {
    const char* paths[] = {path.c_str()};
    return read_products(paths, 1);
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
    if (path.empty()) {
        spdlog::warn("[filament] save_user_products: no overlay path configured");
        return false;
    }

    // Read-modify-write: preserve any existing `orca_type_map`. If the file is
    // missing, a bare array (legacy), or unparseable, start fresh with an empty
    // object — a corrupt existing file must not block the user's save.
    nlohmann::json doc = nlohmann::json::object();
    {
        std::ifstream in(path);
        if (in.is_open()) {
            try {
                auto parsed = nlohmann::json::parse(in);
                if (parsed.is_object())
                    doc = std::move(parsed);
                // Bare array or other shape: silently start fresh. The legacy
                // bare-array form carried only products, never orca_type_map,
                // so nothing is lost by replacing it with object form here.
            } catch (const std::exception& e) {
                // The existing file is unparseable — we must not block the save,
                // but the user may have hand-authored an orca_type_map in there.
                // Preserve the original as a .bak (best-effort) so it stays
                // recoverable, then start fresh with an empty object.
                const std::string bak = path + ".bak";
                std::error_code bak_ec;
                std::filesystem::copy_file(
                    path, bak, std::filesystem::copy_options::overwrite_existing, bak_ec);
                spdlog::warn("[filament] existing overlay parse failed on save ({}): {}; {} to {}",
                             path, e.what(), bak_ec ? "could not back up" : "backed up", bak);
            }
        }
    }

    // Atomic write: tmp file + rename. POSIX rename is atomic within a single
    // filesystem. The tmp file lives next to the target so the rename never
    // crosses a mount boundary.
    std::filesystem::path target(path);
    std::filesystem::path tmp = target;
    tmp += ".tmp";

    // Ensure parent dir exists (the on-device runtime config dir is created
    // elsewhere, but tests / fresh installs may hit this path first).
    std::error_code ec;
    if (auto parent = target.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        // Ignore "already exists"; report everything else.
        if (ec && !std::filesystem::is_directory(parent)) {
            spdlog::warn("[filament] save_user_products: cannot create parent dir {}: {}",
                         parent.string(), ec.message());
            return false;
        }
    }

    doc["filaments"] = products;

    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            spdlog::warn("[filament] save_user_products: cannot open {} for writing", tmp.string());
            return false;
        }
        out << doc.dump(2);
        if (!out) {
            spdlog::warn("[filament] save_user_products: error writing to {}", tmp.string());
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return false;
        }
    } // ofstream closed here, buffers flushed, before rename

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        spdlog::warn("[filament] save_user_products: rename failed ({} -> {}): {}", tmp.string(),
                     target.string(), ec.message());
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
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

} // namespace helix::printer
