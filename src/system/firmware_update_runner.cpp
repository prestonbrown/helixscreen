// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_update_runner.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

namespace helix::fwupdate {

namespace {
/// The launcher only forks the flasher into its own session and exits.
constexpr int kLaunchTimeoutSeconds = 30;
} // namespace

StartResult start_vendor_apply(const Provider& p, const ApplyPlan& plan, ApplyInputs inputs,
                               bool unverified_allowed, const ProcessRunner& run) {
    namespace fs = std::filesystem;
    StartResult result;
    if (!apply_enabled(p, unverified_allowed) || plan.argv.empty()) {
        result.outcome = StartOutcome::ApplyDisabled;
        spdlog::warn("[FirmwareUpdate] {}: apply is not enabled; not starting", provider_id(p));
        return result;
    }
    std::error_code ec;
    if (!plan.lock_path.empty() && fs::exists(plan.lock_path, ec)) {
        inputs.update_running = true;
    }
    if (!plan.unpack_dir.empty() && !inputs.free_bytes) {
        const fs::space_info space = fs::space(plan.unpack_dir, ec);
        if (!ec) {
            inputs.free_bytes = space.available;
        }
    }
    result.blocker = first_blocker(inputs);
    if (result.blocker != Blocker::None) {
        result.outcome = StartOutcome::Blocked;
        spdlog::warn("[FirmwareUpdate] {}: not starting, blocker {}", provider_id(p),
                     static_cast<int>(result.blocker));
        return result;
    }
    spdlog::warn("[FirmwareUpdate] {}: starting the vendor flasher, log {}", provider_id(p),
                 plan.log_path);
    const CommandResult launched = run(plan.argv, kLaunchTimeoutSeconds);
    if (!launched.succeeded()) {
        result.outcome = StartOutcome::LaunchFailed;
        result.error = !launched.error.empty() ? launched.error : last_output_line(launched.output);
        spdlog::error("[FirmwareUpdate] {}: launcher failed (exit {}): {}", provider_id(p),
                      launched.exit_code, result.error);
        return result;
    }
    result.outcome = StartOutcome::Started;
    return result;
}

std::optional<std::string> make_private_run_dir(const std::string& parent) {
    std::string tmpl = parent + "/helixscreen-fwupdate.XXXXXX";
    // mkdtemp creates the directory with mode 0700, atomically and never through a link.
    if (::mkdtemp(tmpl.data()) == nullptr) {
        spdlog::error("[FirmwareUpdate] Cannot create a run directory under {}: {}", parent,
                      std::strerror(errno));
        return std::nullopt;
    }
    return tmpl;
}

bool pid_file_process_alive(const std::string& pid_file) {
    std::ifstream in(pid_file);
    long pid = 0;
    if (!(in >> pid) || pid <= 1) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

VendorRunCheck check_vendor_run(bool process_alive, bool reached_no_return,
                                std::chrono::seconds silent_for,
                                std::chrono::seconds silence_limit) {
    if (!process_alive) {
        return reached_no_return ? VendorRunCheck::Restarting : VendorRunCheck::Unknown;
    }
    return silent_for >= silence_limit ? VendorRunCheck::Unknown : VendorRunCheck::Running;
}

std::vector<std::string> LogTail::poll() {
    std::vector<std::string> lines;
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return lines;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) {
        return lines;
    }
    const auto size = static_cast<uint64_t>(end);
    if (size < offset_) {
        offset_ = 0;
        partial_.clear();
    }
    if (size == offset_) {
        return lines;
    }
    in.seekg(static_cast<std::streamoff>(offset_));
    std::string chunk(static_cast<size_t>(size - offset_), '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    chunk.resize(static_cast<size_t>(in.gcount()));
    offset_ += chunk.size();
    partial_ += chunk;
    size_t start = 0;
    for (size_t nl = partial_.find('\n'); nl != std::string::npos;
         nl = partial_.find('\n', start)) {
        std::string line = partial_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    partial_.erase(0, start);
    return lines;
}

} // namespace helix::fwupdate
