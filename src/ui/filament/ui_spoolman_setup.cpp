// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spoolman_setup.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace helix::ui {

bool SpoolmanSetup::validate_port(const std::string& port) {
    if (port.empty() || std::isspace(static_cast<unsigned char>(port.front()))) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(port.c_str(), &end, 10);
    // Must parse entire string (no trailing chars) and be in valid TCP port range
    if (errno != 0 || end != port.c_str() + port.size()) {
        return false;
    }
    return value >= 1 && value <= 65535;
}

bool SpoolmanSetup::validate_host(const std::string& host) {
    if (host.empty()) {
        return false;
    }
    // Reject strings that are entirely whitespace
    bool all_whitespace =
        std::all_of(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c); });
    return !all_whitespace;
}

std::string SpoolmanSetup::build_url(const std::string& host, const std::string& port) {
    return "http://" + host + ":" + port;
}

std::string SpoolmanSetup::build_probe_url(const std::string& host, const std::string& port) {
    return build_url(host, port) + "/api/v1/health";
}

std::vector<std::pair<std::string, std::string>>
SpoolmanSetup::build_spoolman_config_entries(const std::string& host, const std::string& port) {
    return {{"server", build_url(host, port)}};
}

std::pair<std::string, std::string> SpoolmanSetup::parse_url_components(const std::string& url) {
    if (url.empty()) {
        return {"", kDefaultSpoolmanPort};
    }

    std::string working = url;

    // Strip scheme (e.g. "http://")
    auto scheme_end = working.find("://");
    if (scheme_end != std::string::npos) {
        working = working.substr(scheme_end + 3);
    }

    // Strip trailing path at first '/'
    auto path_start = working.find('/');
    if (path_start != std::string::npos) {
        working = working.substr(0, path_start);
    }

    // Split host and port at last ':'
    auto colon_pos = working.rfind(':');
    if (colon_pos == std::string::npos) {
        // No port in URL — return host with default port
        return {working, kDefaultSpoolmanPort};
    }

    std::string host = working.substr(0, colon_pos);
    std::string port_part = working.substr(colon_pos + 1);

    // Validate the extracted port; fall back to default if invalid
    if (!validate_port(port_part)) {
        return {host, kDefaultSpoolmanPort};
    }

    return {host, port_part};
}

} // namespace helix::ui
