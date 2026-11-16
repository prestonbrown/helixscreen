// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file led_wled_json.h
 * @brief Internal helper for unwrapping Moonraker's WLED strip payloads.
 *
 * NOT part of the public LED API. Included by src/led/led_controller.cpp and by
 * the unit tests that pin the unwrap behaviour.
 */

#include "hv/json.hpp" // JSON_HPP_OK: inline readers of the json

namespace helix::led::detail {

/**
 * @brief Resolve the object mapping strip name -> strip detail from a
 *        `GET /machine/wled/strips` payload.
 *
 * Moonraker answers with a two-level envelope — a `result` wrapper around a
 * `strips` wrapper around the actual map:
 *
 * @code
 * {"result": {"strips": {"TopLight": {"strip": "TopLight", "status": "off",
 *                                     "brightness": 255, "preset": -1}}}}
 * @endcode
 *
 * Unwrapping only `result` and iterating yields one bogus entry whose key is the
 * literal string `strips`. That fake id is then POSTed back to
 * `/machine/wled/strip` and Moonraker answers HTTP 400, so every WLED action
 * silently fails and every polled state reads off/255/-1
 * (prestonbrown/helixscreen#1241).
 *
 * Disambiguation: a user may legitimately configure `[wled strips]`, in which
 * case `result.strips` is that strip's own detail object rather than a wrapper.
 * A detail object always carries a `strip` member and a wrapper never does, so
 * that member decides whether to descend.
 *
 * @param data Raw response payload (with or without the `result` wrapper).
 * @return Reference to the strip map. Never a temporary — unusable input yields
 *         a reference to a static empty object. The reference borrows from
 *         @p data, so it stays valid only as long as @p data does.
 */
inline const nlohmann::json& wled_strip_map(const nlohmann::json& data) {
    static const nlohmann::json empty = nlohmann::json::object();

    const nlohmann::json* level = &data;
    if (data.is_object() && data.contains("result") && data["result"].is_object()) {
        level = &data["result"];
    }

    if (!level->is_object()) {
        return empty;
    }

    auto strips_it = level->find("strips");
    if (strips_it != level->end() && strips_it->is_object() && !strips_it->contains("strip")) {
        return *strips_it;
    }

    return *level;
}

} // namespace helix::led::detail
