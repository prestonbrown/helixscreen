// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/update_rollback.h"

#include "app_globals.h"
#include "lvgl.h"
#include "platform_info.h"
#include "platform_table.h"
#include "system/update_checker.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace helix::update {

namespace {

std::optional<bool>& rollback_supported_override() {
    static std::optional<bool> value;
    return value;
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0';
}

std::string path_in(const std::string& dir, const char* name) {
    return dir + "/" + name;
}

} // namespace

RollbackSupport rollback_support(const RollbackFacts& facts) {
    // Install layouts that cannot hold two versions side by side. The K2's
    // /opt overlay is too small for both, so its installer stages the old tree
    // on another partition, which cannot be renamed back. Desktop and Android
    // builds never run the in-app installer; ESP32 updates by OTA.
    if (facts.platform_key == "k2" || facts.platform_key == "x86-desktop" ||
        facts.platform_key == "android" || facts.platform_key == "esp32") {
        return RollbackSupport::Platform;
    }
    if (facts.install_suppressed) {
        return RollbackSupport::InstallSuppressed;
    }
    // A mod that owns the install tree updates it in place: no previous tree.
    if (facts.mod_payload) {
        return RollbackSupport::ModPayload;
    }
    if (!facts.guard_dir) {
        return RollbackSupport::NoGuard;
    }
    if (!facts.supervised) {
        return RollbackSupport::NotSupervised;
    }
    return RollbackSupport::Supported;
}

const char* rollback_support_name(RollbackSupport support) {
    switch (support) {
    case RollbackSupport::Supported:
        return "supported";
    case RollbackSupport::NoGuard:
        return "no_guard";
    case RollbackSupport::NotSupervised:
        return "not_supervised";
    case RollbackSupport::InstallSuppressed:
        return "install_suppressed";
    case RollbackSupport::ModPayload:
        return "mod_payload";
    case RollbackSupport::Platform:
        return "platform";
    }
    return "unknown";
}

RollbackFacts current_rollback_facts() {
    RollbackFacts facts;
    facts.platform_key = helix::platform::current_key();
    facts.guard_dir = env_set(kRollbackDirEnv);
    facts.supervised = env_set("HELIX_SUPERVISED");
    facts.install_suppressed = update_install_suppressed();
    facts.mod_payload = helix::firmware_mod_owns_install_tree();
    return facts;
}

bool auto_rollback_supported() {
    if (const auto& forced = rollback_supported_override()) {
        return *forced;
    }
    const RollbackSupport support = rollback_support(current_rollback_facts());
    spdlog::debug("[UpdateRollback] Automatic-update rollback: {}", rollback_support_name(support));
    return support == RollbackSupport::Supported;
}

void set_auto_rollback_supported_for_test(std::optional<bool> value) {
    rollback_supported_override() = value;
}

std::string rollback_dir() {
    const char* dir = std::getenv(kRollbackDirEnv);
    return dir != nullptr ? std::string(dir) : std::string();
}

std::string format_rollback_marker(const RollbackMarker& marker) {
    std::string out;
    out += "install_dir=" + marker.install_dir + "\n";
    out += "previous_dir=" + marker.previous_dir + "\n";
    out += "from_version=" + marker.from_version + "\n";
    out += "to_version=" + marker.to_version + "\n";
    return out;
}

std::string rollback_value(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return line.substr(prefix.size());
        }
    }
    return {};
}

bool write_rollback_file(const std::string& dir, const char* name, const std::string& content) {
    const std::string path = path_in(dir, name);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            spdlog::warn("[UpdateRollback] Could not write {}", tmp);
            return false;
        }
        out << content;
        if (!out.flush()) {
            spdlog::warn("[UpdateRollback] Could not write {}", tmp);
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        spdlog::warn("[UpdateRollback] Could not move {} into place", path);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool read_rollback_file(const std::string& dir, const char* name, std::string& out) {
    std::ifstream in(path_in(dir, name));
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void remove_rollback_file(const std::string& dir, const char* name) {
    std::error_code ec;
    std::filesystem::remove(path_in(dir, name), ec);
}

StartupHealth decide_startup_health(bool armed, bool safe_mode) {
    if (!armed) {
        return StartupHealth::NotArmed;
    }
    // Safe mode here means the crash-loop detector fired on this version.
    return safe_mode ? StartupHealth::RequestRollback : StartupHealth::ReportWhenSettled;
}

std::optional<RollbackResult> parse_rollback_result(const std::string& text) {
    RollbackResult result;
    result.from_version = rollback_value(text, "from_version");
    result.to_version = rollback_value(text, "to_version");
    result.reason = rollback_value(text, "reason");
    if (result.to_version.empty()) {
        return std::nullopt;
    }
    return result;
}

void on_ui_ready(bool safe_mode) {
    const std::string dir = rollback_dir();
    if (dir.empty()) {
        return;
    }

    std::string text;
    if (read_rollback_file(dir, kRollbackResultFile, text)) {
        remove_rollback_file(dir, kRollbackResultFile);
        if (auto result = parse_rollback_result(text)) {
            UpdateChecker::instance().report_rollback(*result);
        }
    }

    std::string armed_text;
    const bool armed = read_rollback_file(dir, kRollbackArmedFile, armed_text);
    switch (decide_startup_health(armed, safe_mode)) {
    case StartupHealth::NotArmed:
        return;
    case StartupHealth::RequestRollback:
        spdlog::warn("[UpdateRollback] v{} is crash-looping; asking the launcher to roll back",
                     rollback_value(armed_text, "to_version"));
        write_rollback_file(dir, kRollbackRequestFile, "reason=crash_loop\n");
        return;
    case StartupHealth::ReportWhenSettled:
        break;
    }

    spdlog::info("[UpdateRollback] Rollback armed; reporting healthy in {}s",
                 kRollbackHealthySeconds);
    // One-shot, captures nothing: the lv_timer is freed with LVGL at shutdown.
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t*) { // TIMER_DTOR_OK: one-shot, no owner, captures nothing
            const std::string d = rollback_dir();
            std::string unused;
            if (!d.empty() && read_rollback_file(d, kRollbackArmedFile, unused)) {
                spdlog::info("[UpdateRollback] Running for {}s; reporting healthy",
                             kRollbackHealthySeconds);
                write_rollback_file(d, kRollbackHealthyFile, "");
            }
        },
        static_cast<uint32_t>(kRollbackHealthySeconds) * 1000U, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

} // namespace helix::update
