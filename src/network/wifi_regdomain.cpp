// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_regdomain.h"

#include "core/run_capture.h"
#include "core/text_io.h"
#include "wifi_country.h"

#include <unistd.h>

namespace helix::wifi::regdomain {

namespace {

std::optional<std::string>& iw_override() {
    static std::optional<std::string> path;
    return path;
}

} // namespace

const char* find_iw() {
    if (const auto& forced = iw_override()) {
        return forced->empty() ? nullptr : forced->c_str();
    }
    for (const char* path : {"/usr/sbin/iw", "/sbin/iw", "/usr/bin/iw", "/bin/iw"}) {
        if (access(path, X_OK) == 0) {
            return path;
        }
    }
    return nullptr;
}

std::string exec_iw(const char* iw, const std::string& args, int& exit_code) {
    return helix::run_capture(std::string(iw) + " " + args + " 2>/dev/null", exit_code);
}

std::optional<std::string> read_country() {
    const char* iw = find_iw();
    if (!iw) {
        return std::nullopt;
    }
    int exit_code = -1;
    const std::string out = exec_iw(iw, "reg get", exit_code);
    if (exit_code != 0) {
        return std::nullopt;
    }
    return helix::wifi::country::parse_iw_reg_get(out);
}

bool process_may_set() {
    if (geteuid() == 0) {
        return true;
    }
    const auto status = helix::text_io::read_file("/proc/self/status");
    return status && helix::wifi::country::proc_status_has_capability(
                         *status, helix::wifi::country::kCapNetAdmin);
}

void set_iw_for_test(const std::string& path) {
    iw_override() = path;
}

void reset_iw_for_test() {
    iw_override().reset();
}

} // namespace helix::wifi::regdomain
