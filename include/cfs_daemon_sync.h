// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

/**
 * @file cfs_daemon_sync.h
 * @brief Best-effort notice to Creality's local daemon that a CFS slot changed.
 *
 * CFS-private: only AmsBackendCfs calls this. On Creality OS a daemon
 * (master-server / web-server) serves a WebSocket on 127.0.0.1:9999 that
 * Creality Print and the Creality Cloud app read slot state from. It does not
 * watch Klipper's tn_data, so a slot edited through BOX_MODIFY_TN_DATA stays
 * stale there until the daemon is told. Creality Print tells it with one text
 * frame (prestonbrown/helixscreen#2048, captured on a K1C):
 *
 *   {"method":"set","params":{"modifyMaterial":{"boxId":1,"id":0,"rfid":"",
 *    "type":"PLA","vendor":"Creality","name":"CR-PLA","color":"#0ffffff",
 *    "minTemp":190.0,"maxTemp":240.0,"pressure":"0.04"}}}
 *
 * boxId 1..4 is CFS unit T1..T4 and 0 the external spool holder; id 0..3 is
 * bay A..D (0 on the holder). The daemon's answer is not documented, so the
 * sender never reads one.
 *
 * The BOX_MODIFY_TN_DATA write stays the source of truth. This notice runs
 * after it, on an HttpExecutor worker, inside a fixed time budget, and every
 * failure (nothing listening, refused handshake, timeout) is a debug line.
 */
namespace helix {
struct SlotInfo;
}

namespace helix::cfs {

/// One slot as the daemon's modifyMaterial record describes it.
struct DaemonMaterial {
    int box_id = 0;     ///< 1..4 = unit T1..T4, 0 = external spool holder
    int slot_id = 0;    ///< 0..3 = bay A..D
    std::string rfid;   ///< 5-char catalog code of the material written, "" when none
    std::string type;   ///< material family ("PLA")
    std::string vendor; ///< brand ("Creality")
    std::string name;   ///< product label ("CR-PLA")
    uint32_t color_rgb = 0;
    int min_temp_c = 0;
    int max_temp_c = 0;
};

/// Where the daemon's record for a slot lives: boxId and id.
struct DaemonAddress {
    int box_id = 0;
    int slot_id = 0;
};

/// The pressure-advance value the daemon's record carries. HelixScreen keeps
/// no per-slot pressure advance, so every notice sends the value Creality
/// Print sent in the capture.
inline constexpr const char* kDaemonDefaultPressure = "0.04";

/// The daemon address for a global bay index (0..15), or for the external
/// spool sentinel @p external_slot. nullopt for anything else.
[[nodiscard]] std::optional<DaemonAddress> daemon_address(int global_index, int external_slot);

/// The daemon's color spelling: '#' + tn_data's color_value, "#0RRGGBB". The
/// leading nibble is the one tn_data.json and BOX_MODIFY_TN_DATA carry (always
/// 0 in every capture); the high byte of @p color_rgb is ignored.
[[nodiscard]] std::string daemon_color(uint32_t color_rgb);

/// The whole modifyMaterial frame text for @p material.
[[nodiscard]] std::string encode_modify_material(const DaemonMaterial& material);

/// The record for slot @p global_index after a BOX_MODIFY_TN_DATA write of
/// @p mat_code (the full firmware code, "" for a color-only write) and
/// @p color_rgb. @p slot is the slot as it now stands, or null. nullopt for an
/// index the daemon cannot address.
[[nodiscard]] std::optional<DaemonMaterial>
daemon_material_for(int global_index, const helix::SlotInfo* slot, const std::string& material,
                    const std::string& brand, const std::string& catalog_id,
                    const std::string& mat_code, uint32_t color_rgb);

struct DaemonEndpoint {
    std::string host = "127.0.0.1";
    int port = 9999;
    int connect_timeout_ms = 150; ///< TCP connect only
    int total_budget_ms = 750;    ///< connect + handshake + send + close, end to end
};

enum class DaemonSendResult {
    Delivered,       ///< handshake accepted, frame and close frame written
    ConnectFailed,   ///< nothing listening, or the connect timed out
    HandshakeFailed, ///< the peer answered, but not with 101 Switching Protocols
    SendFailed,      ///< the socket failed while writing
    TimedOut,        ///< the budget ran out waiting for the handshake answer
    Unsupported,     ///< a build with no socket transport (the ESP32 firmware)
};

[[nodiscard]] const char* to_string(DaemonSendResult result);

/// Blocking: connect, upgrade, send @p payload as one masked text frame, close.
/// Returns within about endpoint.total_budget_ms. Never call it on the UI
/// thread; post_daemon_frame is the way in.
[[nodiscard]] DaemonSendResult send_daemon_frame(const DaemonEndpoint& endpoint,
                                                 const std::string& payload);

/// Queue send_daemon_frame on HttpExecutor::fast() and log its result at
/// debug. Returns immediately; a pool that is not running drops the notice.
void post_daemon_frame(const DaemonEndpoint& endpoint, std::string payload);

} // namespace helix::cfs
