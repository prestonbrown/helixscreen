// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_legacy_import.cpp
 * @brief AmsState: the one-shot import of HelixScreen 1.0's spool links
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_state.h"
#include "ams_state_internal.h"
#include "config.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "helix_fs.h"
#include "i_moonraker_api.h"
#include "legacy_spool_import.h"
#include "system/helix_paths.h"

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
    legacy_import_.generation = ++legacy_import_generations_;
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
    const Config* config = Config::get_instance();
    const bool file_imported =
        config && config->get<bool>(config->df() + legacy::kLegacyToolImportDonePath, false);
    if (file_imported) {
        spdlog::debug("{} tool links already imported; {} ignored", kTag, legacy_import_.file_path);
    } else if (fs::exists(legacy_import_.file_path)) {
        if (const auto text = text_io::read_file(legacy_import_.file_path)) {
            legacy_import_.file = nlohmann::json::parse(*text, nullptr, false);
            if (!legacy_import_.file.is_discarded()) {
                from_file =
                    legacy::tool_spools_from_file(legacy_import_.file, legacy_import_.printer_id);
            }
        }
        legacy_import_.file_unreadable = !legacy_import_.file.is_object();
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
            token.defer("AmsState::legacy_tool_spools", [from_file, from_db, value]() {
                AmsState& self = AmsState::instance();
                self.legacy_import_.tool_spools = from_db.empty() ? from_file : from_db;
                self.legacy_import_.db_key_seen = true;
                self.legacy_import_.db_value = value;
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
    const uint64_t generation = legacy_import_.generation;
    auto token = async_lifetime_.token();

    // Per-tool links: only where a tool had a lane of its own was the 1.0
    // ToolState the record (elsewhere it mirrored the lanes, and copying it back
    // would relink lanes the firmware has since cleared). Written once the
    // lanes have been read, so "no link" is what Moonraker holds. A link leaves
    // the 1.0 copies only once Moonraker echoes it, or once a lane is found
    // already linked.
    if (legacy_import_.tool_spools && !legacy_import_.tools_done && backend != nullptr &&
        backend->lane_records_ready()) {
        legacy_import_.tools_done = true;
        const std::map<int, int> links = std::move(*legacy_import_.tool_spools);
        if (backend->supports_per_tool_spool_assignment()) {
            for (const auto& [tool, spool_id] : links) {
                const auto lane = lane_feeding_tool(tool);
                if (!lane || lane->backend_index != 0) {
                    spdlog::info("{} tool {} spool {}: no lane feeds it, kept", kTag, tool,
                                 spool_id);
                    legacy_import_.owed[tool] = spool_id;
                    continue;
                }
                if (lane->info.spoolman_id > 0) {
                    continue;
                }
                const helix::ams::LaneId lane_id = backend->lane_id(lane->slot);
                const std::uint64_t filed_before = helix::ams::pending_filings(lane_id);
                SlotInfo linked = lane->info;
                linked.spoolman_id = spool_id;
                const AmsError err = commit_slot_edit(lane->slot, lane->info, linked);
                if (!err.success()) {
                    spdlog::info("{} tool {} spool {} -> lane {}: {}; kept for the next connect",
                                 kTag, tool, spool_id, lane->slot, err.technical_msg);
                    legacy_import_.owed[tool] = spool_id;
                    continue;
                }
                legacy_import_.owed[tool] = spool_id;
                ++legacy_import_.awaiting;
                const int written_tool = tool;
                helix::ams::await_pending(
                    lane_id, filed_before,
                    [token, generation, written_tool](helix::ams::PendingOutcome outcome,
                                                      const std::string&) {
                        token.defer("AmsState::legacy_tool_settled",
                                    [generation, written_tool, outcome]() {
                                        AmsState::instance().legacy_tool_settled(
                                            generation, written_tool,
                                            outcome == helix::ams::PendingOutcome::Echoed);
                                    });
                    });
            }
        }
        if (legacy_import_.awaiting == 0) {
            finish_legacy_tool_import();
        }
    }

    // The bypass spool goes into an empty bypass record, once that record has
    // been read. The parked block goes once the record holds it.
    if (legacy_import_.bypass_parked && !legacy_import_.bypass_done && bypass_record_known()) {
        legacy_import_.bypass_done = true;
        if (legacy_import_.bypass && !bypass_record().has_value()) {
            spdlog::info("{} bypass spool {} ({}) -> bypass record", kTag,
                         legacy_import_.bypass->spoolman_id, legacy_import_.bypass->material);
            const std::uint64_t filed_before =
                helix::ams::pending_filings(helix::ams::kBypassLaneId);
            set_external_spool_info(*legacy_import_.bypass);
            helix::ams::await_pending(
                helix::ams::kBypassLaneId, filed_before,
                [token, generation](helix::ams::PendingOutcome outcome, const std::string&) {
                    token.defer("AmsState::legacy_bypass_settled", [generation, outcome]() {
                        AmsState::instance().legacy_bypass_settled(
                            generation, outcome == helix::ams::PendingOutcome::Echoed);
                    });
                });
        } else {
            legacy_bypass_settled(generation, true);
        }
    }
}

void AmsState::legacy_tool_settled(uint64_t generation, int tool, bool written) {
    assert_main_thread();
    if (generation != legacy_import_.generation || legacy_import_.awaiting <= 0) {
        return;
    }
    if (written) {
        spdlog::info("{} tool {} spool {}: linked", kTag, tool, legacy_import_.owed[tool]);
        legacy_import_.owed.erase(tool);
    } else {
        spdlog::info("{} tool {} spool {}: not written; kept for the next connect", kTag, tool,
                     legacy_import_.owed[tool]);
    }
    if (--legacy_import_.awaiting == 0) {
        finish_legacy_tool_import();
    }
}

void AmsState::finish_legacy_tool_import() {
    assert_main_thread();
    const std::map<int, int>& owed = legacy_import_.owed;
    if (api_ != nullptr && legacy_import_.db_key_seen) {
        const nlohmann::json kept = legacy::set_keeping_tools(legacy_import_.db_value, owed);
        if (legacy::tool_spools_from_set(kept).empty()) {
            api_->database_delete_item(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey);
        } else if (kept != legacy_import_.db_value) {
            api_->database_post_item(legacy::kToolSpoolsDbNamespace, legacy::kToolSpoolsDbKey,
                                     kept);
        }
    }
    if (legacy_import_.file.is_object()) {
        if (const auto rest =
                legacy::file_keeping_tools(legacy_import_.file, legacy_import_.printer_id, owed)) {
            if (*rest != legacy_import_.file) {
                text_io::write_file_atomic(legacy_import_.file_path, rest->dump(2));
            }
        } else {
            // The file is a link into the user config store on most installs, and the
            // restore script re-links any store copy that survives.
            const std::string target = helix::paths::write_target(legacy_import_.file_path);
            if (target != legacy_import_.file_path) {
                fs::remove(target);
            }
            fs::remove(legacy_import_.file_path);
        }
    }
    if (owed.empty() && !legacy_import_.file_unreadable) {
        if (Config* config = Config::get_instance()) {
            config->set<bool>(config->df() + legacy::kLegacyToolImportDonePath, true);
            config->save();
        }
    }
}

void AmsState::legacy_bypass_settled(uint64_t generation, bool written) {
    assert_main_thread();
    if (generation != legacy_import_.generation) {
        return;
    }
    if (!written) {
        spdlog::info("{} bypass spool not written; kept for the next connect", kTag);
        return;
    }
    if (Config* config = Config::get_instance()) {
        nlohmann::json& filament = config->get_json(config->df() + "filament");
        if (filament.is_object()) {
            filament.erase("legacy_external_spool");
            config->save();
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
