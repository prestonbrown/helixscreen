// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/run_capture.h"

#include <array>
#include <cstdio>
#include <sys/wait.h>

namespace helix {

std::string run_capture(const std::string& cmd, int& exit_code) {
    exit_code = -1;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return {};
    }
    std::string out;
    std::array<char, 512> buffer;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
    const int ret = pclose(pipe);
    exit_code = WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
    return out;
}

} // namespace helix
