// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock_spoolman.h"

#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <tuple>

namespace {

using json = nlohmann::json;

/// Spoolman serializes an unset optional field as null, not as an empty value.
json or_null(const std::string& s) {
    return s.empty() ? json(nullptr) : json(s);
}
json or_null(double v) {
    return v > 0 ? json(v) : json(nullptr);
}
json or_null(int v) {
    return v > 0 ? json(v) : json(nullptr);
}

json vendor_json(const VendorInfo& v, const std::string& comment = "") {
    return {{"id", v.id},
            {"registered", "2025-01-01T00:00:00Z"},
            {"name", v.name},
            {"comment", or_null(comment)},
            {"empty_spool_weight", nullptr},
            {"external_id", nullptr},
            {"extra", json::object()}};
}

/// Spoolman keeps one temperature per filament. A range held in the mock's
/// records is served as its midpoint.
json one_temp(int a, int b) {
    if (a > 0 && b > 0) {
        return (a + b) / 2;
    }
    return or_null(std::max(a, b));
}

json filament_json(const FilamentInfo& f) {
    json j = {{"id", f.id},
              {"registered", "2025-01-01T00:00:00Z"},
              {"name", or_null(f.filament_name)},
              {"material", or_null(f.material)},
              {"density", f.density > 0 ? json(f.density) : json(1.24)},
              {"diameter", f.diameter},
              {"weight", or_null(static_cast<double>(f.weight))},
              {"spool_weight", or_null(static_cast<double>(f.spool_weight))},
              {"color_hex", or_null(f.color_hex)},
              {"settings_extruder_temp", one_temp(f.nozzle_temp_min, f.nozzle_temp_max)},
              {"settings_bed_temp", one_temp(f.bed_temp_min, f.bed_temp_max)},
              {"multi_color_hexes", or_null(f.multi_color_hexes)},
              {"external_id", nullptr},
              {"extra", json::object()}};
    if (f.vendor_id > 0 || !f.vendor_name.empty()) {
        j["vendor"] = vendor_json(VendorInfo{f.vendor_id, f.vendor_name});
    } else {
        j["vendor"] = nullptr;
    }
    return j;
}

/// A Spoolman HTTP error as Moonraker's proxy relays it: raise_for_status()
/// raises ServerError(<HTTP reason phrase>, status), and the JSON-RPC layer
/// sends a 404 as code -32601. Spoolman's own response body is not passed on.
MoonrakerError spoolman_error(int status) {
    const char* reason = status == 404   ? "Not Found"
                         : status == 422 ? "Unprocessable Entity"
                         : status == 400 ? "Bad Request"
                                         : "Internal Server Error";
    const int code = status == 404 ? -32601 : status;
    return MoonrakerError::from_json_rpc({{"code", code}, {"message", reason}},
                                         "server.spoolman.proxy");
}

/// Spoolman's request models type settings_extruder_temp and settings_bed_temp
/// as an optional integer and answer anything else with 422.
bool temps_valid(const json& body, MoonrakerError& err) {
    for (const char* key : {"settings_extruder_temp", "settings_bed_temp"}) {
        if (!body.contains(key)) {
            continue;
        }
        const json& v = body[key];
        const bool integral =
            v.is_number_integer() ||
            (v.is_number_float() && v.get<double>() == std::floor(v.get<double>()));
        if (!v.is_null() && !integral) {
            spdlog::debug("[MockSpoolman] 422: {} is not an integer", key);
            err = spoolman_error(422);
            return false;
        }
    }
    return true;
}

/// "/v1/spool/12" with prefix "/v1/spool/" -> 12; nullopt when not that shape.
std::optional<int> id_after(const std::string& path, const std::string& prefix) {
    if (path.rfind(prefix, 0) != 0 || path.size() == prefix.size()) {
        return std::nullopt;
    }
    const std::string rest = path.substr(prefix.size());
    if (!std::all_of(rest.begin(), rest.end(), ::isdigit)) {
        return std::nullopt;
    }
    return std::atoi(rest.c_str());
}

/// The value of @p key in a query string, or "" when absent.
std::string query_value(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string pair =
            query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (pair.rfind(key + "=", 0) == 0) {
            return pair.substr(key.size() + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return "";
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

/// A slice of SpoolmanDB in Spoolman's ExternalFilament shape, enough for
/// search to have matches across manufacturers, materials and a multi-colour.
const json& mock_external_catalog() {
    static const json catalog = json::parse(R"([
      {"id": "polymaker_pla_polyliteplablack_1000_175_n", "manufacturer": "Polymaker",
       "name": "PolyLite PLA Black", "material": "PLA", "density": 1.24, "weight": 1000,
       "spool_weight": 140, "diameter": 1.75, "color_hex": "1A1A1A", "extruder_temp": 210,
       "bed_temp": 60, "translucent": false, "glow": false},
      {"id": "polymaker_pla_polyterracottonwhite_1000_175_n", "manufacturer": "Polymaker",
       "name": "PolyTerra PLA Cotton White", "material": "PLA", "density": 1.31, "weight": 1000,
       "spool_weight": 140, "diameter": 1.75, "color_hex": "E8E4D8", "extruder_temp": 210,
       "bed_temp": 55, "translucent": false, "glow": false},
      {"id": "polymaker_petg_polylitepetgblue_1000_175_n", "manufacturer": "Polymaker",
       "name": "PolyLite PETG Blue", "material": "PETG", "density": 1.25, "weight": 1000,
       "spool_weight": 140, "diameter": 1.75, "color_hex": "1E5AA8", "extruder_temp": 240,
       "bed_temp": 75, "translucent": false, "glow": false},
      {"id": "polymaker_asa_polyliteasared_1000_175_n", "manufacturer": "Polymaker",
       "name": "PolyLite ASA Red", "material": "ASA", "density": 1.07, "weight": 1000,
       "spool_weight": 140, "diameter": 1.75, "color_hex": "C8102E", "extruder_temp": 255,
       "bed_temp": 100, "translucent": false, "glow": false},
      {"id": "prusament_pla_galaxyblack_1000_175_n", "manufacturer": "Prusament",
       "name": "PLA Galaxy Black", "material": "PLA", "density": 1.24, "weight": 1000,
       "spool_weight": 201, "diameter": 1.75, "color_hex": "26262B", "extruder_temp": 215,
       "bed_temp": 60, "translucent": false, "glow": false},
      {"id": "prusament_petg_jetblack_1000_175_n", "manufacturer": "Prusament",
       "name": "PETG Jet Black", "material": "PETG", "density": 1.27, "weight": 1000,
       "spool_weight": 201, "diameter": 1.75, "color_hex": "1B1B1B", "extruder_temp": 240,
       "bed_temp": 85, "translucent": false, "glow": false},
      {"id": "esun_pla+_white_1000_175_n", "manufacturer": "eSUN", "name": "PLA+ White",
       "material": "PLA", "density": 1.23, "weight": 1000, "spool_weight": 220, "diameter": 1.75,
       "color_hex": "F4F4F4", "extruder_temp": 215, "bed_temp": 60, "translucent": false,
       "glow": false},
      {"id": "esun_silkpla_rainbow_1000_175_n", "manufacturer": "eSUN",
       "name": "Silk PLA Rainbow", "material": "PLA", "density": 1.24, "weight": 1000,
       "spool_weight": 220, "diameter": 1.75,
       "color_hexes": ["E53935", "FFEB3B", "43A047", "1E88E5"],
       "multi_color_direction": "coaxial", "extruder_temp": 210,
       "bed_temp": 60, "translucent": false, "glow": false},
      {"id": "bambulab_pla_basicorange_1000_175_n", "manufacturer": "Bambu Lab",
       "name": "PLA Basic Orange", "material": "PLA", "density": 1.26, "weight": 1000,
       "spool_weight": 250, "diameter": 1.75, "color_hex": "FF6A13", "extruder_temp": 220,
       "bed_temp": 55, "translucent": false, "glow": false},
      {"id": "bambulab_tpu_95ablack_1000_175_n", "manufacturer": "Bambu Lab",
       "name": "TPU 95A Black", "material": "TPU", "density": 1.22, "weight": 1000,
       "spool_weight": 250, "diameter": 1.75, "color_hex": "000000", "extruder_temp": 230,
       "bed_temp": 35, "translucent": false, "glow": false},
      {"id": "overture_abs_grey_1000_175_n", "manufacturer": "Overture", "name": "ABS Grey",
       "material": "ABS", "density": 1.04, "weight": 1000, "diameter": 1.75,
       "color_hex": "8A8D8F", "extruder_temp": 250, "bed_temp": 100, "translucent": false,
       "glow": false},
      {"id": "sunlu_petg_transparent_1000_175_n", "manufacturer": "Sunlu",
       "name": "PETG Transparent", "material": "PETG", "density": 1.27, "weight": 1000,
       "spool_weight": 160, "diameter": 1.75, "color_hex": "FFFFFF33", "translucent": true,
       "glow": false}
    ])",
                                            nullptr, false);
    return catalog;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// The curated entries plus synthetic ones, so a common query such as "pla"
/// or "poly" fills a whole page of results the way the real catalog does.
const json& mock_external_catalog_full() {
    static const json catalog = [] {
        json all = mock_external_catalog();
        static constexpr const char* kMakers[] = {"Polymaker", "Prusament", "eSUN", "Sunlu"};
        static constexpr const char* kLines[] = {"PolyLite", "PolyTerra", "Basic", "Matte"};
        static constexpr const char* kMaterials[] = {"PLA", "PETG", "ASA"};
        static constexpr const char* kColors[][2] = {
            {"Red", "C62828"},  {"Orange", "EF6C00"}, {"Yellow", "F9A825"}, {"Green", "2E7D32"},
            {"Teal", "00838F"}, {"Navy", "1A237E"},   {"Purple", "6A1B9A"}, {"Pink", "AD1457"},
            {"Grey", "757575"}, {"Brown", "5D4037"}};
        for (size_t m = 0; m < std::size(kMakers); ++m) {
            for (const char* material : kMaterials) {
                for (const auto& color : kColors) {
                    const std::string name =
                        std::string(kLines[m]) + " " + material + " " + color[0];
                    all.push_back({{"id", lower(std::string(kMakers[m]) + "_" + name)},
                                   {"manufacturer", kMakers[m]},
                                   {"name", name},
                                   {"material", material},
                                   {"density", 1.24},
                                   {"weight", 1000},
                                   {"spool_weight", 200},
                                   {"diameter", 1.75},
                                   {"color_hex", color[1]},
                                   {"extruder_temp", 215},
                                   {"bed_temp", 60},
                                   {"translucent", false},
                                   {"glow", false}});
                }
            }
        }
        return all;
    }();
    return catalog;
}

/// Spoolman's search: every whitespace-separated word of the query must appear,
/// case-insensitively, in the manufacturer, name and material together.
bool catalog_matches(const json& entry, const std::string& query) {
    const std::string haystack = lower(entry.value("manufacturer", "") + " " +
                                       entry.value("name", "") + " " + entry.value("material", ""));
    size_t pos = 0;
    const std::string q = lower(query);
    while (pos < q.size()) {
        const size_t start = q.find_first_not_of(' ', pos);
        if (start == std::string::npos) {
            break;
        }
        const size_t end = q.find(' ', start);
        const std::string word = q.substr(start, end == std::string::npos ? end : end - start);
        if (haystack.find(word) == std::string::npos) {
            return false;
        }
        pos = end == std::string::npos ? q.size() : end;
    }
    return true;
}

} // namespace

MockSpoolmanServer::MockSpoolmanServer() {
    init_mock_spools();
    if (const char* env = std::getenv("HELIX_MOCK_SPOOLMAN_DB_SEARCH");
        env && std::string(env) == "0") {
        external_search_supported_ = false;
        spdlog::info("[MockSpoolman] SpoolmanDB search off via HELIX_MOCK_SPOOLMAN_DB_SEARCH=0 "
                     "(an older Spoolman)");
    }
    if (const char* env = std::getenv("HELIX_MOCK_SPOOLMAN_DB_SEARCH_LATENCY_MS")) {
        external_search_latency_ms_ = std::clamp(std::atoi(env), 0, 10000);
    }
}

void MockSpoolmanServer::add_vendor(int id, std::string name) {
    VendorInfo v;
    v.id = id;
    v.name = std::move(name);
    vendors_.push_back(std::move(v));
}

void MockSpoolmanServer::add_filament(int id, int vendor_id, std::string material,
                                      std::string color_hex, std::string name) {
    FilamentInfo f;
    f.id = id;
    f.vendor_id = vendor_id;
    f.material = std::move(material);
    f.color_hex = std::move(color_hex);
    f.filament_name = std::move(name);
    filaments_.push_back(std::move(f));
}

std::vector<VendorInfo> MockSpoolmanServer::vendor_list() const {
    // Seeded and created vendors first (stable IDs for tests), then one per
    // distinct spool vendor on IDs they leave free.
    std::vector<VendorInfo> vendors = vendors_;
    std::set<std::string> seen;
    std::set<int> seen_ids;
    int next_id = 1;
    for (const auto& v : vendors) {
        seen.insert(v.name);
        seen_ids.insert(v.id);
        next_id = std::max(next_id, v.id + 1);
    }
    for (const auto& spool : spools_) {
        if (!spool.vendor.empty() && seen.insert(spool.vendor).second) {
            while (seen_ids.count(next_id)) {
                ++next_id;
            }
            VendorInfo v;
            v.id = next_id++;
            v.name = spool.vendor;
            vendors.push_back(v);
        }
    }
    return vendors;
}

std::vector<FilamentInfo> MockSpoolmanServer::filament_list() const {
    // Seeded and created filaments first, then one per distinct
    // vendor+material+name among the spools.
    std::vector<FilamentInfo> filaments = filaments_;
    std::map<std::string, int> vendor_ids;
    for (const auto& v : vendor_list()) {
        vendor_ids.emplace(v.name, v.id);
    }
    std::set<std::string> seen;
    int next_id = 1;
    for (const auto& f : filaments_) {
        seen.insert(f.vendor_name + "|" + f.material + "|" + f.filament_name);
        next_id = std::max(next_id, f.id + 1);
    }
    for (const auto& spool : spools_) {
        if (seen.insert(spool.vendor + "|" + spool.material + "|" + spool.filament_name).second) {
            FilamentInfo f;
            f.id = next_id++;
            f.vendor_name = spool.vendor;
            auto vid = vendor_ids.find(spool.vendor);
            f.vendor_id = vid == vendor_ids.end() ? 0 : vid->second;
            f.material = spool.material;
            f.filament_name = spool.filament_name;
            f.color_hex = spool.color_hex;
            f.diameter = 1.75f;
            f.weight = static_cast<float>(spool.initial_weight_g);
            f.nozzle_temp_min = f.nozzle_temp_max = spool.nozzle_temp_recommended;
            f.bed_temp_min = f.bed_temp_max = spool.bed_temp_recommended;
            filaments.push_back(f);
        }
    }
    return filaments;
}

json MockSpoolmanServer::spool_json(const SpoolInfo& s) const {
    json filament = {
        {"id", s.filament_id},
        {"registered", "2025-01-01T00:00:00Z"},
        {"name", or_null(s.filament_name)},
        {"material", or_null(s.material)},
        {"density", 1.24},
        {"diameter", 1.75},
        {"weight", or_null(s.initial_weight_g)},
        {"spool_weight", or_null(s.spool_weight_g)},
        {"color_hex", or_null(s.color_hex)},
        {"multi_color_hexes", or_null(s.multi_color_hexes)},
        {"settings_extruder_temp", s.nozzle_temp_recommended > 0
                                       ? json(s.nozzle_temp_recommended)
                                       : one_temp(s.nozzle_temp_min, s.nozzle_temp_max)},
        {"settings_bed_temp", s.bed_temp_recommended > 0
                                  ? json(s.bed_temp_recommended)
                                  : one_temp(s.bed_temp_min, s.bed_temp_max)},
        {"external_id", nullptr},
        {"extra", filament_extra(s.filament_id)}};
    filament["vendor"] = (!s.vendor.empty() || s.vendor_id > 0)
                             ? vendor_json(VendorInfo{s.vendor_id, s.vendor})
                             : json(nullptr);
    const double used = std::max(0.0, s.initial_weight_g - s.remaining_weight_g);
    return {{"id", s.id},
            {"registered", s.registered.empty() ? "2025-01-01T00:00:00Z" : s.registered},
            {"first_used", or_null(s.last_used)},
            {"last_used", or_null(s.last_used)},
            {"filament", std::move(filament)},
            {"price", or_null(s.price)},
            {"remaining_weight", s.remaining_weight_g},
            {"initial_weight", or_null(s.initial_weight_g)},
            {"spool_weight", or_null(s.spool_weight_g)},
            {"used_weight", used},
            {"remaining_length", s.remaining_length_m * 1000.0},
            {"used_length", 0.0},
            {"location", or_null(s.location)},
            {"lot_nr", or_null(s.lot_nr)},
            {"comment", or_null(s.comment)},
            {"archived", archived_spool_ids_.count(s.id) > 0},
            {"extra", json::object()}};
}

json MockSpoolmanServer::filament_extra(int filament_id) const {
    auto it = filament_extra_.find(filament_id);
    return it == filament_extra_.end() ? json::object() : it->second;
}

bool MockSpoolmanServer::filament_field_defined(const std::string& key) const {
    return std::any_of(filament_fields_.begin(), filament_fields_.end(),
                       [&key](const json& f) { return f.value("key", "") == key; });
}

SpoolInfo* MockSpoolmanServer::find_spool(int id) {
    auto it = std::find_if(spools_.begin(), spools_.end(),
                           [id](const SpoolInfo& s) { return s.id == id; });
    return it == spools_.end() ? nullptr : &*it;
}

bool MockSpoolmanServer::proxy(const json& params, json& result, MoonrakerError& err) {
    const std::string method = params.value("request_method", "GET");
    const std::string full_path = params.value("path", "");
    const json body = params.contains("body") ? params["body"] : json::object();
    const size_t q = full_path.find('?');
    const std::string path = full_path.substr(0, q);
    const std::string query = q == std::string::npos ? "" : full_path.substr(q + 1);

    spdlog::debug("[MockSpoolman] {} {}", method, full_path);

    auto with_extra = [this](json filament, int filament_id) {
        filament["extra"] = filament_extra(filament_id);
        return filament;
    };

    auto not_found = [&err](const std::string& kind, int id) {
        spdlog::debug("[MockSpoolman] 404: no {} with ID {}", kind, id);
        err = spoolman_error(404);
        return false;
    };

    if (method == "GET" && path == "/v1/spool") {
        std::vector<SpoolInfo> listed;
        for (const auto& spool : spools_) {
            if (archived_spool_ids_.count(spool.id) == 0) {
                listed.push_back(spool);
            }
        }
        result = json::array();
        for (const auto& spool : listed) {
            result.push_back(spool_json(spool));
        }
        return true;
    }
    if (auto id = id_after(path, "/v1/spool/")) {
        SpoolInfo* spool = find_spool(*id);
        if (method == "GET") {
            if (!spool) {
                return not_found("spool", *id);
            }
            result = spool_json(*spool);
            return true;
        }
        if (method == "PATCH") {
            if (refuse_spool_patch_key && body.contains(*refuse_spool_patch_key)) {
                spdlog::debug("[MockSpoolman] 500: refusing PATCH of spool {} carrying '{}'", *id,
                              *refuse_spool_patch_key);
                err = spoolman_error(500);
                return false;
            }
            if (body.size() == 1 && body.contains("remaining_weight") &&
                body["remaining_weight"].is_number()) {
                weight_updates.push_back({*id, body["remaining_weight"].get<double>()});
            } else {
                spool_updates.push_back({*id, body});
            }
            if (!spool) {
                return not_found("spool", *id);
            }
            if (body.contains("archived") && body["archived"].is_boolean()) {
                if (body["archived"].get<bool>()) {
                    archived_spool_ids_.insert(*id);
                } else {
                    archived_spool_ids_.erase(*id);
                }
            }
            auto num = [&body](const char* key, double& out) {
                if (body.contains(key) && body[key].is_number()) {
                    out = body[key].get<double>();
                }
            };
            auto str = [&body](const char* key, std::string& out) {
                if (body.contains(key) && body[key].is_string()) {
                    out = body[key].get<std::string>();
                }
            };
            num("remaining_weight", spool->remaining_weight_g);
            num("spool_weight", spool->spool_weight_g);
            num("price", spool->price);
            str("lot_nr", spool->lot_nr);
            str("comment", spool->comment);
            str("location", spool->location);
            if (body.contains("filament_id") && body["filament_id"].is_number_integer()) {
                spool->filament_id = body["filament_id"].get<int>();
                for (const auto& f : filament_list()) {
                    if (f.id == spool->filament_id) {
                        spool->material = f.material;
                        spool->filament_name = f.filament_name;
                        spool->color_hex = f.color_hex;
                        spool->vendor_id = f.vendor_id;
                        spool->vendor = f.vendor_name;
                        spool->nozzle_temp_recommended = f.nozzle_temp_max;
                        spool->bed_temp_recommended = f.bed_temp_max;
                        break;
                    }
                }
            }
            result = spool_json(*spool);
            return true;
        }
        if (method == "DELETE") {
            if (!spool) {
                return not_found("spool", *id);
            }
            spools_.erase(std::remove_if(spools_.begin(), spools_.end(),
                                         [&](const SpoolInfo& s) { return s.id == *id; }),
                          spools_.end());
            result = json::object();
            return true;
        }
    }
    if (method == "POST" && path == "/v1/spool") {
        created_spools.push_back(body);
        SpoolInfo spool;
        spool.id = next_created_spool_id > 0 ? next_created_spool_id
                                             : static_cast<int>(spools_.size()) + 1;
        spool.initial_weight_g = body.value("initial_weight", 1000.0);
        spool.remaining_weight_g = body.value("remaining_weight", spool.initial_weight_g);
        spool.spool_weight_g = body.value("spool_weight", 0.0);
        if (body.contains("filament_id") && body["filament_id"].is_number_integer()) {
            spool.filament_id = body["filament_id"].get<int>();
            for (const auto& f : filament_list()) {
                if (f.id == spool.filament_id) {
                    spool.material = f.material;
                    spool.filament_name = f.filament_name;
                    spool.color_hex = f.color_hex;
                    spool.vendor_id = f.vendor_id;
                    spool.vendor = f.vendor_name;
                    spool.nozzle_temp_recommended = f.nozzle_temp_max;
                    spool.bed_temp_recommended = f.bed_temp_max;
                    break;
                }
            }
        }
        if (body.contains("location") && body["location"].is_string()) {
            spool.location = body["location"].get<std::string>();
        }
        spools_.push_back(spool);
        result = spool_json(spool);
        return true;
    }

    if (method == "GET" && path == "/v1/vendor") {
        result = json::array();
        for (const auto& v : vendor_list()) {
            auto c = vendor_comments_.find(v.id);
            result.push_back(vendor_json(v, c == vendor_comments_.end() ? "" : c->second));
        }
        return true;
    }
    if (method == "POST" && path == "/v1/vendor") {
        created_vendors.push_back(body);
        VendorInfo vendor;
        vendor.id = next_created_vendor_id > 0 ? next_created_vendor_id
                                               : static_cast<int>(spools_.size()) + 100;
        vendor.name = body.value("name", "");
        const std::string comment = body.contains("comment") && body["comment"].is_string()
                                        ? body["comment"].get<std::string>()
                                        : "";
        vendor_comments_[vendor.id] = comment;
        vendors_.push_back(vendor);
        result = vendor_json(vendor, comment);
        return true;
    }
    if (method == "DELETE" && id_after(path, "/v1/vendor/")) {
        const int id = *id_after(path, "/v1/vendor/");
        vendors_.erase(std::remove_if(vendors_.begin(), vendors_.end(),
                                      [id](const VendorInfo& v) { return v.id == id; }),
                       vendors_.end());
        result = json::object();
        return true;
    }

    if (method == "GET" && path == "/v1/field/filament") {
        result = filament_fields_;
        return true;
    }
    if (method == "POST" && path.rfind("/v1/field/filament/", 0) == 0) {
        const std::string key = path.substr(std::string("/v1/field/filament/").size());
        defined_filament_fields.emplace_back(key, body);
        json field = body;
        field["key"] = key;
        field["entity_type"] = "filament";
        bool replaced = false;
        for (auto& existing : filament_fields_) {
            if (existing.value("key", "") == key) {
                existing = field;
                replaced = true;
            }
        }
        if (!replaced) {
            filament_fields_.push_back(field);
        }
        result = filament_fields_;
        return true;
    }
    if (method == "GET" && path == "/v1/filament") {
        result = json::array();
        const std::string vendor_filter = query_value(query, "vendor.id");
        if (vendor_filter.empty()) {
            for (const auto& f : filament_list()) {
                result.push_back(with_extra(filament_json(f), f.id));
            }
            return true;
        }
        const int vendor_id = std::atoi(vendor_filter.c_str());
        for (const auto& f : filament_list()) {
            if (f.vendor_id == vendor_id) {
                result.push_back(with_extra(filament_json(f), f.id));
            }
        }
        return true;
    }
    if (method == "POST" && path == "/v1/filament") {
        created_filaments.push_back(body);
        if (!body.contains("density") || !body.contains("diameter")) {
            spdlog::debug("[MockSpoolman] 422: density and diameter are required");
            err = spoolman_error(422);
            return false;
        }
        if (!temps_valid(body, err)) {
            return false;
        }
        FilamentInfo filament;
        filament.id = next_created_filament_id > 0 ? next_created_filament_id : next_filament_id_++;
        filament.material = body.value("material", "");
        filament.filament_name = body.value("name", "");
        filament.color_hex = body.value("color_hex", "");
        filament.multi_color_hexes = body.value("multi_color_hexes", "");
        filament.diameter = body.value("diameter", 1.75f);
        filament.weight = body.value("weight", 0.0f);
        filament.spool_weight = body.value("spool_weight", 0.0f);
        for (const auto& [key, lo, hi] :
             {std::tuple{"settings_extruder_temp", &filament.nozzle_temp_min,
                         &filament.nozzle_temp_max},
              std::tuple{"settings_bed_temp", &filament.bed_temp_min, &filament.bed_temp_max}}) {
            if (body.contains(key) && body[key].is_number()) {
                *lo = *hi = static_cast<int>(body[key].get<double>());
            }
        }
        if (body.contains("vendor_id") && body["vendor_id"].is_number_integer()) {
            filament.vendor_id = body["vendor_id"].get<int>();
            for (const auto& v : vendor_list()) {
                if (v.id == filament.vendor_id) {
                    filament.vendor_name = v.name;
                    break;
                }
            }
        }
        filaments_.push_back(filament);
        result = with_extra(filament_json(filament), filament.id);
        return true;
    }
    if (auto id = id_after(path, "/v1/filament/")) {
        if (method == "GET") {
            if (next_filament_get_result) {
                result = *next_filament_get_result;
                next_filament_get_result.reset();
                return true;
            }
            for (const auto& f : filament_list()) {
                if (f.id == *id) {
                    result = with_extra(filament_json(f), f.id);
                    return true;
                }
            }
            const bool on_a_spool =
                std::any_of(spools_.begin(), spools_.end(),
                            [&id](const SpoolInfo& sp) { return sp.filament_id == *id; });
            if (on_a_spool || filament_extra_.count(*id) > 0) {
                result = {{"id", *id}, {"extra", filament_extra(*id)}};
                return true;
            }
            return not_found("filament", *id);
        }
        if (method == "PATCH") {
            filament_updates.push_back({*id, body});
            if (!temps_valid(body, err)) {
                return false;
            }
            if (body.contains("extra") && body["extra"].is_object()) {
                // Spoolman refuses a key with no field definition.
                for (auto it = body["extra"].begin(); it != body["extra"].end(); ++it) {
                    if (!filament_field_defined(it.key())) {
                        spdlog::debug("[MockSpoolman] 400: unknown extra field {}", it.key());
                        err = spoolman_error(400);
                        return false;
                    }
                }
                // Older Spoolman replaces the whole dict, the stricter of the two
                // behaviours a writer has to survive.
                filament_extra_[*id] = body["extra"];
            }
            result = {{"id", *id}};
            for (auto& f : filaments_) {
                if (f.id == *id) {
                    if (body.contains("color_hex") && body["color_hex"].is_string()) {
                        f.color_hex = body["color_hex"].get<std::string>();
                    }
                    if (body.contains("settings_extruder_temp")) {
                        f.nozzle_temp_min = f.nozzle_temp_max =
                            body["settings_extruder_temp"].is_null()
                                ? 0
                                : static_cast<int>(body["settings_extruder_temp"].get<double>());
                    }
                    if (body.contains("settings_bed_temp")) {
                        f.bed_temp_min = f.bed_temp_max =
                            body["settings_bed_temp"].is_null()
                                ? 0
                                : static_cast<int>(body["settings_bed_temp"].get<double>());
                    }
                    result = with_extra(filament_json(f), f.id);
                }
            }
            return true;
        }
        if (method == "DELETE") {
            filaments_.erase(std::remove_if(filaments_.begin(), filaments_.end(),
                                            [&](const FilamentInfo& f) { return f.id == *id; }),
                             filaments_.end());
            result = json::object();
            return true;
        }
    }

    if (method == "GET" && path == "/v1/external/filament/search" && external_search_supported_) {
        ++external_search_count_;
        const std::string q = url_decode(query_value(query, "query"));
        const int limit = std::clamp(std::atoi(query_value(query, "limit").c_str()), 1, 100);
        result = json::array();
        for (const auto& entry : mock_external_catalog_full()) {
            if (static_cast<int>(result.size()) >= limit) {
                break;
            }
            if (catalog_matches(entry, q)) {
                result.push_back(entry);
            }
        }
        return true;
    }

    err = spoolman_error(404);
    return false;
}

void MockSpoolmanServer::init_mock_spools() {
    // Create a realistic mock spool inventory
    spools_.clear();

    // The active spool is auto-assigned to the active tool on a changer, so on
    // a MedusaHC mock it must be the spool lane T0 already names, or the
    // "Current" card and the T0 lane describe different filament.
    if (MoonrakerClientMock::mock_medusa_variant() != MoonrakerClientMock::MedusaVariant::NONE) {
        active_spool_id_ = 2;
    }

    // Spool 1: Polymaker PLA - Jet Black (active, 85% remaining)
    SpoolInfo spool1;
    spool1.id = 1;
    spool1.vendor = "Polymaker";
    spool1.material = "PLA";
    spool1.filament_name = "Jet Black";
    spool1.color_hex = "1A1A2E";
    spool1.remaining_weight_g = 850.0;
    spool1.initial_weight_g = 1000.0;
    spool1.remaining_length_m = 290.0;
    spool1.spool_weight_g = 140.0;
    spool1.nozzle_temp_recommended = 210;
    spool1.bed_temp_recommended = 60;
    spools_.push_back(spool1);

    // Spool 2: eSUN Silk PLA - Silk Blue (75% remaining)
    SpoolInfo spool2;
    spool2.id = 2;
    spool2.vendor = "eSUN";
    spool2.material = "Silk PLA";
    spool2.filament_name = "Silk Blue";
    spool2.color_hex = "26DCD9";
    spool2.remaining_weight_g = 750.0;
    spool2.initial_weight_g = 1000.0;
    spool2.remaining_length_m = 258.0;
    spool2.spool_weight_g = 240.0;
    spool2.nozzle_temp_recommended = 210;
    spool2.bed_temp_recommended = 50;
    spools_.push_back(spool2);

    // Spool 3: Elegoo ASA - Pop Blue (50% remaining)
    SpoolInfo spool3;
    spool3.id = 3;
    spool3.vendor = "Elegoo";
    spool3.material = "ASA";
    spool3.filament_name = "Pop Blue";
    spool3.color_hex = "00AEFF";
    spool3.remaining_weight_g = 500.0;
    spool3.initial_weight_g = 1000.0;
    spool3.remaining_length_m = 185.0;
    spool3.spool_weight_g = 170.0;
    spool3.nozzle_temp_recommended = 260;
    spool3.bed_temp_recommended = 100;
    spools_.push_back(spool3);

    // Spool 4: Flashforge ABS - Fire Engine Red (LOW: 10% remaining)
    SpoolInfo spool4;
    spool4.id = 4;
    spool4.vendor = "Flashforge";
    spool4.material = "ABS";
    spool4.filament_name = "Fire Engine Red";
    spool4.color_hex = "D20000";
    spool4.remaining_weight_g = 100.0;
    spool4.initial_weight_g = 1000.0;
    spool4.remaining_length_m = 39.0;
    spool4.spool_weight_g = 160.0;
    spool4.nozzle_temp_recommended = 260;
    spool4.bed_temp_recommended = 100;
    spools_.push_back(spool4);

    // Spool 5: Kingroon PETG - Signal Yellow (NEW: 100% remaining)
    SpoolInfo spool5;
    spool5.id = 5;
    spool5.vendor = "Kingroon";
    spool5.material = "PETG";
    spool5.filament_name = "Signal Yellow";
    spool5.color_hex = "F4E111";
    spool5.remaining_weight_g = 1000.0;
    spool5.initial_weight_g = 1000.0;
    spool5.remaining_length_m = 333.0;
    spool5.spool_weight_g = 155.0;
    spool5.nozzle_temp_recommended = 235;
    spool5.bed_temp_recommended = 70;
    spools_.push_back(spool5);

    // Spool 6: Overture TPU - Clear (60% remaining)
    SpoolInfo spool6;
    spool6.id = 6;
    spool6.vendor = "Overture";
    spool6.material = "TPU";
    spool6.filament_name = "Clear";
    spool6.color_hex = "E8E8E8";
    spool6.remaining_weight_g = 600.0;
    spool6.initial_weight_g = 1000.0;
    spool6.remaining_length_m = 198.0;
    spool6.spool_weight_g = 230.0;
    spool6.nozzle_temp_recommended = 220;
    spool6.bed_temp_recommended = 50;
    spools_.push_back(spool6);

    // === Additional spools from real Spoolman inventory for realistic testing ===

    // Spool 7: Bambu Lab ASA - Gray (NEW: 100%)
    SpoolInfo spool7;
    spool7.id = 7;
    spool7.vendor = "Bambu Lab";
    spool7.material = "ASA";
    spool7.filament_name = "Gray ASA";
    spool7.color_hex = "8A949E";
    spool7.remaining_weight_g = 1000.0;
    spool7.initial_weight_g = 1000.0;
    spool7.remaining_length_m = 370.0;
    spool7.spool_weight_g = 250.0;
    spool7.nozzle_temp_recommended = 250;
    spool7.bed_temp_recommended = 90;
    spools_.push_back(spool7);

    // Spool 8: Polymaker PC - Grey (67% - Polycarbonate engineering material)
    SpoolInfo spool8;
    spool8.id = 8;
    spool8.vendor = "Polymaker";
    spool8.material = "PC";
    spool8.filament_name = "PolyMax PC Grey";
    spool8.color_hex = "A2AAAD";
    spool8.remaining_weight_g = 500.0;
    spool8.initial_weight_g = 750.0;
    spool8.remaining_length_m = 152.0;
    spool8.spool_weight_g = 125.0;
    spool8.nozzle_temp_recommended = 270;
    spool8.bed_temp_recommended = 100;
    spools_.push_back(spool8);

    // Spool 9: Polymaker PA12-CF15 - Carbon Fiber Nylon (100% - HIGH TEMP)
    SpoolInfo spool9;
    spool9.id = 9;
    spool9.vendor = "Polymaker";
    spool9.material = "PA-CF";
    spool9.filament_name = "Fiberon PA12-CF15 Black";
    spool9.color_hex = "000000";
    spool9.remaining_weight_g = 500.0;
    spool9.initial_weight_g = 500.0;
    spool9.remaining_length_m = 170.0;
    spool9.spool_weight_g = 190.0;
    spool9.nozzle_temp_recommended = 290;
    spool9.bed_temp_recommended = 50;
    spools_.push_back(spool9);

    // Spool 10: Tinmorry TPU - Blue (90% - Flexible)
    SpoolInfo spool10;
    spool10.id = 10;
    spool10.vendor = "Tinmorry";
    spool10.material = "TPU";
    spool10.filament_name = "Blue TPU";
    spool10.color_hex = "435FCC";
    spool10.remaining_weight_g = 900.0;
    spool10.initial_weight_g = 1000.0;
    spool10.remaining_length_m = 297.0;
    spool10.spool_weight_g = 200.0;
    spool10.nozzle_temp_recommended = 230;
    spool10.bed_temp_recommended = 50;
    spools_.push_back(spool10);

    // Spool 11: eSUN ABS - Black (40%)
    SpoolInfo spool11;
    spool11.id = 11;
    spool11.vendor = "eSUN";
    spool11.material = "ABS";
    spool11.filament_name = "Black ABS+HS";
    spool11.color_hex = "000000";
    spool11.remaining_weight_g = 400.0;
    spool11.initial_weight_g = 1000.0;
    spool11.remaining_length_m = 148.0;
    spool11.spool_weight_g = 160.0;
    spool11.nozzle_temp_recommended = 260;
    spool11.bed_temp_recommended = 100;
    spools_.push_back(spool11);

    // Spool 12: Flashforge ASA - Dark Green Sparkle (35%)
    SpoolInfo spool12;
    spool12.id = 12;
    spool12.vendor = "Flashforge";
    spool12.material = "ASA";
    spool12.filament_name = "Dark Green Sparkle ASA";
    spool12.color_hex = "276E27";
    spool12.remaining_weight_g = 350.0;
    spool12.initial_weight_g = 1000.0;
    spool12.remaining_length_m = 129.5;
    spool12.spool_weight_g = 175.0;
    spool12.nozzle_temp_recommended = 260;
    spool12.bed_temp_recommended = 100;
    spools_.push_back(spool12);

    // Spool 13: Bambu Lab PETG - Translucent Green (100%)
    SpoolInfo spool13;
    spool13.id = 13;
    spool13.vendor = "Bambu Lab";
    spool13.material = "PETG";
    spool13.filament_name = "Translucent Green PETG";
    spool13.color_hex = "29A261";
    spool13.remaining_weight_g = 1000.0;
    spool13.initial_weight_g = 1000.0;
    spool13.remaining_length_m = 333.0;
    spool13.spool_weight_g = 250.0;
    spool13.nozzle_temp_recommended = 250;
    spool13.bed_temp_recommended = 70;
    spools_.push_back(spool13);

    // Spool 14: Eryone Silk PLA - Gold/Silver/Copper (49% - tri-color)
    SpoolInfo spool14;
    spool14.id = 14;
    spool14.vendor = "Eryone";
    spool14.material = "Silk PLA";
    spool14.filament_name = "Gold/Silver/Copper Tri-Color";
    spool14.color_hex = "D4AF37";                          // Primary color (gold)
    spool14.multi_color_hexes = "#D4AF37,#C0C0C0,#B87333"; // Gold, Silver, Copper
    spool14.remaining_weight_g = 494.0;
    spool14.initial_weight_g = 1000.0;
    spool14.remaining_length_m = 170.0;
    spool14.spool_weight_g = 150.0;
    spool14.nozzle_temp_recommended = 220;
    spool14.bed_temp_recommended = 60;
    spools_.push_back(spool14);

    // Spool 15: Bambu Lab PLA - Red (100%)
    SpoolInfo spool15;
    spool15.id = 15;
    spool15.vendor = "Bambu Lab";
    spool15.material = "PLA";
    spool15.filament_name = "Red PLA";
    spool15.color_hex = "C12E1F";
    spool15.remaining_weight_g = 1000.0;
    spool15.initial_weight_g = 1000.0;
    spool15.remaining_length_m = 340.0;
    spool15.spool_weight_g = 250.0;
    spool15.nozzle_temp_recommended = 220;
    spool15.bed_temp_recommended = 60;
    spools_.push_back(spool15);

    // Spool 16: Polymaker ABS - Metallic Blue (17%)
    SpoolInfo spool16;
    spool16.id = 16;
    spool16.vendor = "Polymaker";
    spool16.material = "ABS";
    spool16.filament_name = "PolyLite ABS Metallic Blue";
    spool16.color_hex = "333C64";
    spool16.remaining_weight_g = 174.0;
    spool16.initial_weight_g = 1000.0;
    spool16.remaining_length_m = 64.0;
    spool16.spool_weight_g = 140.0;
    spool16.nozzle_temp_recommended = 260;
    spool16.bed_temp_recommended = 100;
    spools_.push_back(spool16);

    // Spool 17: Sunlu PETG - Black (55%)
    SpoolInfo spool17;
    spool17.id = 17;
    spool17.vendor = "Sunlu";
    spool17.material = "PETG";
    spool17.filament_name = "Black PETG";
    spool17.color_hex = "000000";
    spool17.remaining_weight_g = 550.0;
    spool17.initial_weight_g = 1000.0;
    spool17.remaining_length_m = 183.0;
    spool17.spool_weight_g = 130.0;
    spool17.nozzle_temp_recommended = 255;
    spool17.bed_temp_recommended = 80;
    spools_.push_back(spool17);

    // Spool 18: eSUN PLA+ - White (30%)
    SpoolInfo spool18;
    spool18.id = 18;
    spool18.vendor = "eSUN";
    spool18.material = "PLA+";
    spool18.filament_name = "PLA+ White";
    spool18.color_hex = "FFFFFF";
    spool18.remaining_weight_g = 300.0;
    spool18.initial_weight_g = 1000.0;
    spool18.remaining_length_m = 103.0;
    spool18.spool_weight_g = 170.0;
    spool18.nozzle_temp_recommended = 220;
    spool18.bed_temp_recommended = 60;
    spools_.push_back(spool18);

    // Spool 19: TTYT3D Marble PLA - Black/White (85% - dual-color marble)
    SpoolInfo spool19;
    spool19.id = 19;
    spool19.vendor = "TTYT3D";
    spool19.material = "Marble PLA";
    spool19.filament_name = "Black/White Marble";
    spool19.color_hex = "202020";                  // Primary color (dark base)
    spool19.multi_color_hexes = "#202020,#F0F0F0"; // Black, White
    spool19.remaining_weight_g = 850.0;
    spool19.initial_weight_g = 1000.0;
    spool19.remaining_length_m = 292.0;
    spool19.spool_weight_g = 200.0;
    spool19.nozzle_temp_recommended = 210;
    spool19.bed_temp_recommended = 60;
    spools_.push_back(spool19);

    // HELIX_MOCK_SPOOLMAN_SPOOLS: pad the inventory with deterministic
    // synthetic spools so search/filter cost can be measured at realistic
    // sizes -- real Spoolman databases run to hundreds of spools, while the
    // hand-written inventory above tops out at 19. Padding only extends: the
    // curated spools the mock backends link against keep their ids
    // (test_mock_spool_consistency pins those).
    if (const char* pad_env = std::getenv("HELIX_MOCK_SPOOLMAN_SPOOLS")) {
        const long target = atol(pad_env);
        static constexpr int kMaxPadded = 5000;
        static constexpr const char* kVendors[] = {"Polymaker", "eSUN",     "Prusament",
                                                   "Bambu Lab", "Elegoo",   "Overture",
                                                   "Sunlu",     "Hatchbox", "Kexcelled"};
        static constexpr const char* kMaterials[] = {"PLA", "PETG", "ASA", "ABS", "TPU", "PA-CF"};
        static constexpr const char* kColors[] = {"1A1A2E", "2E6F40", "6F2E2E", "2E406F",
                                                  "6F6F2E", "402E6F", "2E6F6F", "6F402E"};
        const size_t limit = std::min<size_t>(std::max<long>(target, 0), kMaxPadded);
        for (size_t i = spools_.size(); i < limit; ++i) {
            SpoolInfo spool;
            spool.id = 1000 + static_cast<int>(i);
            spool.vendor = kVendors[i % std::size(kVendors)];
            spool.material = kMaterials[i % std::size(kMaterials)];
            spool.filament_name = "Synthetic " + spool.material + " " + std::to_string(i);
            spool.color_hex = kColors[i % std::size(kColors)];
            spool.remaining_weight_g = 100.0 + (i % 9) * 100.0;
            spool.initial_weight_g = 1000.0;
            spool.remaining_length_m = 40.0 + (i % 7) * 40.0;
            spool.spool_weight_g = 140.0;
            spool.nozzle_temp_recommended = 220;
            spool.bed_temp_recommended = 60;
            spools_.push_back(spool);
        }
        if (spools_.size() > 19) {
            spdlog::info("[MockSpoolman] Padded mock spool inventory to {} via "
                         "HELIX_MOCK_SPOOLMAN_SPOOLS",
                         spools_.size());
        }
    }

    spdlog::debug("[MockSpoolman] Initialized {} mock spools", spools_.size());
}
