// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "hv/json.hpp"

/**
 * @brief Response from a generic REST API call
 *
 * Used for communicating with Moonraker extension plugins that expose
 * REST endpoints (e.g., ACE backend via ValgACE's Moonraker bridge at /server/ace/). Encapsulates
 * both success and error cases in a single structure.
 */
struct RestResponse {
    bool success = false; ///< true if HTTP 2xx response
    int status_code = 0;  ///< HTTP status code
    json data; ///< Parsed JSON response. If response isn't JSON, contains {"_raw_body": "..."}
    std::string error; ///< Error message (empty on success)
};
