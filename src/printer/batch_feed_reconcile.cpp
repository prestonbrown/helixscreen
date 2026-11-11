// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_feed_reconcile.h"

#include "i_moonraker_client.h"
#include "snapmaker_channel_state.h"
#include "spdlog/spdlog.h"

#include "hv/json.hpp"

namespace helix::batch_feeding {

namespace {

/// Every filament_feed channel the snapshot carries is at rest. False when any
/// channel is mid-load or mid-unload, and when the snapshot carries none: a
/// snapshot without channels cannot vouch that nothing is feeding.
bool feed_channels_at_rest(const nlohmann::json& status) {
    bool saw_channel = false;
    for (const auto& [object, channels] : status.items()) {
        if (object.rfind("filament_feed ", 0) != 0 || !channels.is_object()) {
            continue;
        }
        for (const auto& [name, channel] : channels.items()) {
            if (!channel.is_object()) {
                continue;
            }
            const auto state = channel.find("channel_state");
            if (state == channel.end() || !state->is_string()) {
                continue;
            }
            saw_channel = true;
            if (snapmaker::channel_state_in_progress(state->get_ref<const std::string&>())) {
                return false;
            }
        }
    }
    return saw_channel;
}

} // namespace

void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& status,
                          const std::string& macro_object, bool local_batch_active) {
    const auto macro = status.find(macro_object);
    if (macro == status.end() || !macro->is_object()) {
        return;
    }
    // `doing` is a save-variable whose JSON type is only as strict as the
    // firmware that wrote it: a non-bool value is "no reading", never a
    // crash — an exception here unwinds through the subscribe response
    // callback and discovery never completes on any connect.
    const auto doing = macro->find("doing");
    if (doing == macro->end() || !doing->is_boolean() || !doing->get<bool>()) {
        return;
    }
    // A batch this process dispatched and has not seen complete owns the
    // interlock: a filament batch is not a print, so print_stats stays
    // standby throughout and the print guards below cannot vouch for it,
    // yet ACTION=END restores the targets snapshotted at that batch's
    // START — zeroing mid-batch hotends. This runs on every rediscovery
    // (each reconnect, each klippy-ready), not only at startup, so a
    // WebSocket blip during a five-minute batch lands here too.
    if (local_batch_active) {
        spdlog::info(
            "[BatchFeed] Batch interlock held by a batch this session dispatched - leaving "
            "it alone");
        return;
    }
    // A print in flight owns the interlock: ACTION=END restores the targets
    // snapshotted at batch START, which mid-print are the wrong values to
    // restore. Only a printer positively confirmed idle gets the cleanup.
    // Klipper publishes an object as null until its first get_status, so an
    // unreadable print_stats means "cannot confirm", not "idle" - leaving an
    // interlock set is recoverable, ending a live batch is not.
    const auto print_stats = status.find("print_stats");
    if (print_stats == status.end() || !print_stats->is_object()) {
        return;
    }
    const auto state = print_stats->find("state");
    if (state == print_stats->end() || !state->is_string()) {
        return;
    }
    const std::string& print_state = state->get_ref<const std::string&>();
    if (print_state == "printing" || print_state == "paused") {
        spdlog::info("[BatchFeed] Batch interlock held with a print in flight - leaving it alone");
        return;
    }
    // virtual_sdcard is a second veto rather than a precondition: print_stats
    // is the authoritative state, and this object is absent on some setups.
    const auto sdcard = status.find("virtual_sdcard");
    if (sdcard != status.end() && sdcard->is_object()) {
        const auto active = sdcard->find("is_active");
        if (active != sdcard->end() && active->is_boolean() && active->get<bool>()) {
            spdlog::info(
                "[BatchFeed] Batch interlock held with a print in flight - leaving it alone");
            return;
        }
    }
    // A batch another client (Fluidd, a phone app) is running leaves every
    // print guard above reading idle too, and ending it restores the targets
    // its START snapshotted in the middle of a feed.
    if (!feed_channels_at_rest(status)) {
        spdlog::info(
            "[BatchFeed] Batch interlock held with a feed channel busy or unreadable - leaving "
            "it alone");
        return;
    }
    spdlog::info("[BatchFeed] Clearing a stranded AUTO_FEEDING_BATCH interlock");
    client.gcode_script(kEndGcode);
}

} // namespace helix::batch_feeding
