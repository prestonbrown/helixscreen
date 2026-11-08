// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_legacy_import.cpp
 * @brief AmsState: the one-shot import of HelixScreen 1.0's spool links
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ui_update_queue.h"

#include "ams_state.h"
#include "ams_state_internal.h"
#include "config.h"
#include "helix_fs.h"
#include "i_moonraker_api.h"
#include "legacy_spool_import.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

namespace helix {
using ams_state_detail::assert_main_thread;
namespace legacy = helix::ams::legacy;

namespace {
constexpr const char* kTag = "[AmsState 1.0 import]";

std::string legacy_printer_id() {
    const Config* config = Config::get_instance();
    const std::string id = config ? config->get_active_printer_id() : std::string();
    return id.empty() ? "default" : id;
}
} // namespace

void AmsState::start_legacy_spool_import(const std::string& tool_spools_path) {
    assert_main_thread();
    legacy_import_ = LegacyImport{};
    legacy_import_.printer_id = legacy_printer_id();

    // The 1.0 bypass spool, parked by the v34 config migration.
    if (const Config* config = Config::get_instance()) {
        if (const auto* block =
                config->try_get_json(config->df() + legacy::kLegacyExternalSpoolPath)) {
            legacy_import_.bypass = legacy::external_spool_from_settings(*block);
            legacy_import_.bypass_parked = true;
        }
    }

    // The 1.0 per-tool links: the Moonraker key wins, the local file stands in.
    legacy_import_.file_path = tool_spools_path;
    std::map<int, int> from_file;
    if (fs::exists(legacy_import_.file_path)) {
        if (const auto text = text_io::read_file(legacy_import_.file_path)) {
            legacy_import_.file = nlohmann::json::parse(*text, nullptr, false);
            if (!legacy_import_.file.is_discarded()) {
                from_file =
                    legacy::tool_spools_from_file(legacy_import_.file, legacy_import_.printer_id);
            }
        }
    }
    if (api_ == nullptr) {
        legacy_import_.tool_spools = std::move(from_file);
        run_legacy_spool_import();
        return;
    }
    auto token = async_lifetime_.token();
    api_->database_get_item(
        legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey,
        [token, from_file](const nlohmann::json& value) {
            std::map<int, int> from_db = legacy::tool_spools_from_set(value);
            token.defer("AmsState::legacy_tool_spools", [from_file, from_db]() {
                AmsState& self = AmsState::instance();
                self.legacy_import_.tool_spools = from_db.empty() ? from_file : from_db;
                self.legacy_import_.db_key_seen = true;
                self.run_legacy_spool_import();
            });
        },
        [token, from_file](const MoonrakerError&) {
            token.defer("AmsState::legacy_tool_spools", [from_file]() {
                AmsState& self = AmsState::instance();
                self.legacy_import_.tool_spools = from_file;
                self.run_legacy_spool_import();
            });
        });
}

void AmsState::run_legacy_spool_import() {
    assert_main_thread();
    AmsBackend* backend = get_backend(0);

    // Per-tool links: only where a tool had a lane of its own was the 1.0
    // ToolState the record (elsewhere it mirrored the lanes, and copying it back
    // would relink lanes the firmware has since cleared). Written once the
    // lanes have been read, so "no link" is what Moonraker holds.
    if (legacy_import_.tool_spools && !legacy_import_.tools_done && backend != nullptr &&
        backend->lane_records_ready()) {
        legacy_import_.tools_done = true;
        const std::map<int, int> links = std::move(*legacy_import_.tool_spools);
        if (backend->supports_per_tool_spool_assignment()) {
            for (const auto& [tool, spool_id] : links) {
                const auto lane = lane_feeding_tool(tool);
                if (!lane || lane->backend_index != 0 || lane->info.spoolman_id > 0) {
                    continue;
                }
                SlotInfo linked = lane->info;
                linked.spoolman_id = spool_id;
                const AmsError err = commit_slot_edit(lane->slot, lane->info, linked);
                spdlog::info("{} tool {} spool {} -> lane {}: {}", kTag, tool, spool_id, lane->slot,
                             err.success() ? "linked" : err.technical_msg);
            }
        }
        if (api_ != nullptr && legacy_import_.db_key_seen) {
            api_->database_delete_item(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey);
        }
        if (legacy_import_.file.is_object()) {
            if (const auto rest =
                    legacy::file_without_printer(legacy_import_.file, legacy_import_.printer_id)) {
                text_io::write_file_atomic(legacy_import_.file_path, rest->dump(2));
            } else {
                fs::remove(legacy_import_.file_path);
            }
        }
    }

    // The bypass spool goes into an empty bypass record, once that record has
    // been read.
    if (legacy_import_.bypass_parked && !legacy_import_.bypass_done && bypass_record_known()) {
        legacy_import_.bypass_done = true;
        if (legacy_import_.bypass && !bypass_record().has_value()) {
            spdlog::info("{} bypass spool {} ({}) -> bypass record", kTag,
                         legacy_import_.bypass->spoolman_id, legacy_import_.bypass->material);
            set_external_spool_info(*legacy_import_.bypass);
        }
        if (Config* config = Config::get_instance()) {
            nlohmann::json& filament = config->get_json(config->df() + "filament");
            if (filament.is_object()) {
                filament.erase("legacy_external_spool");
                config->save();
            }
        }
    }
}

bool AmsState::bypass_record_known() const {
    if (AmsBackend* holder = bypass_record_holder()) {
        return holder->lane_records_ready();
    }
    return bypass_store_ != nullptr && bypass_store_read_;
}

} // namespace helix
