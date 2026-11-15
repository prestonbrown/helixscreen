// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_database_merge.h"

#include "json_utils.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix::printer_db {

using json = nlohmann::json;

BundledIds index_bundled(const json& printers) {
    BundledIds ids;
    if (!printers.is_array()) {
        return ids;
    }
    for (std::size_t i = 0; i < printers.size(); ++i) {
        const std::string id = json_util::safe_string(printers[i], "id");
        if (!id.empty()) {
            ids.index[id] = i;
        }
    }
    for (const auto& printer : printers) {
        const std::string id = json_util::safe_string(printer, "id");
        const json* aliases = json_util::find_member(printer, "id_aliases");
        if (id.empty() || aliases == nullptr || !aliases->is_array()) {
            continue;
        }
        for (const auto& alias : *aliases) {
            if (alias.is_string() && ids.index.count(alias.get<std::string>()) == 0) {
                ids.superseding.emplace(alias.get<std::string>(), id);
            }
        }
    }
    return ids;
}

void merge_extension_printers(json& printers, const json& extension_printers, BundledIds& ids,
                              const std::string& file_path, MergeCounts& counts,
                              std::vector<std::string>& errors) {
    for (const auto& printer : extension_printers) {
        const std::string id = json_util::safe_string(printer, "id");
        if (id.empty()) {
            errors.push_back(fmt::format("{}: printer missing 'id' field", file_path));
            spdlog::warn("[PrinterDetector] {}", errors.back());
            continue;
        }

        const bool enabled = json_util::safe_bool(printer, "enabled", true);

        if (auto it = ids.index.find(id); it != ids.index.end()) {
            if (!enabled) {
                // Filtered out of the list and detection.
                printers[it->second]["enabled"] = false;
                spdlog::debug("[PrinterDetector] Disabled bundled printer '{}'", id);
            } else {
                printers[it->second] = printer;
                spdlog::debug("[PrinterDetector] User override for '{}'", id);
            }
            counts.overrides++;
            continue;
        }
        // Another publisher's id for a printer a bundled entry already
        // describes. Loading it would put a second entry of the same name up
        // against the bundled one on detection score, so the bundled entry
        // stands; an extension changes it under the bundled id.
        if (auto sup = ids.superseding.find(id); sup != ids.superseding.end()) {
            spdlog::info("[PrinterDetector] {}: '{}' is the bundled '{}' under another id; "
                         "keeping the bundled entry (override it as '{}')",
                         file_path, id, sup->second, sup->second);
            continue;
        }
        if (!enabled) {
            continue;
        }
        const std::string name = json_util::safe_string(printer, "name");
        if (name.empty()) {
            errors.push_back(fmt::format("{}: printer '{}' missing 'name' field", file_path, id));
            spdlog::warn("[PrinterDetector] {}", errors.back());
            continue;
        }
        printers.push_back(printer);
        ids.index[id] = printers.size() - 1;
        spdlog::debug("[PrinterDetector] Added user printer '{}'", name);
        counts.additions++;
    }
}

} // namespace helix::printer_db
