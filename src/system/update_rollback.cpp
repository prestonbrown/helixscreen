// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/update_rollback.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "lvgl.h"
#include "platform_info.h"
#include "platform_table.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
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

std::optional<bool>& device_proven_override() {
    static std::optional<bool> value;
    return value;
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
    return device_proven_override().value_or(kRollbackDeviceProven) &&
           support == RollbackSupport::Supported;
}

void set_auto_rollback_supported_for_test(std::optional<bool> value) {
    rollback_supported_override() = value;
}

void set_rollback_device_proven_for_test(std::optional<bool> value) {
    device_proven_override() = value;
}

std::string rollback_dir() {
    const char* dir = std::getenv(kRollbackDirEnv);
    return dir != nullptr ? std::string(dir) : std::string();
}

void disarm_rollback_guard() {
    const std::string dir = rollback_dir();
    if (dir.empty()) {
        return;
    }
    for (const char* name :
         {kRollbackArmedFile, "update_rollback.starts", kRollbackHealthyFile, kRollbackRequestFile,
          kRollbackStoppingFile, kRollbackBusyFile, "update_rollback.phase",
          "update_rollback.handoff", "update_rollback.guard"}) {
        remove_rollback_file(dir, name);
    }
}

bool rollback_guard_armed() {
    const std::string dir = rollback_dir();
    std::error_code ec;
    return !dir.empty() && std::filesystem::exists(path_in(dir, kRollbackArmedFile), ec);
}

bool keep_previous_for_install(bool automatic, bool rollback_supported, bool guard_listening) {
    return automatic && rollback_supported && guard_listening;
}

bool is_kept_previous_of(const std::string& install_root, const std::string& previous) {
    if (install_root.empty() || install_root == "/") {
        return false;
    }
    const std::string base = install_root + ".old";
    if (previous == base) {
        return true;
    }
    if (previous.size() <= base.size() + 1 ||
        previous.compare(0, base.size() + 1, base + ".") != 0) {
        return false;
    }
    for (size_t i = base.size() + 1; i < previous.size(); ++i) {
        if (previous[i] < '0' || previous[i] > '9') {
            return false;
        }
    }
    return true;
}

void discard_kept_previous(const std::string& install_root, const std::string& previous) {
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(install_root, ec).string();
    const auto prev = ec ? std::string() : std::filesystem::weakly_canonical(previous, ec).string();
    if (ec || !is_kept_previous_of(root, prev)) {
        spdlog::warn("[UpdateRollback] Not removing {}: not the kept copy of {}", previous,
                     install_root);
        return;
    }
    std::filesystem::remove_all(prev, ec);
    if (ec) {
        spdlog::warn("[UpdateRollback] Could not remove {}: {}", prev, ec.message());
    } else {
        spdlog::info("[UpdateRollback] Removed the unused kept copy {}", prev);
    }
}

std::string format_rollback_notice(const RollbackResult& result) {
    return result.to_version + "|" + result.from_version + "|" + result.reason;
}

std::optional<RollbackResult> parse_rollback_notice(const std::string& stored) {
    const auto first = stored.find('|');
    if (first == std::string::npos || first == 0) {
        return std::nullopt;
    }
    const auto second = stored.find('|', first + 1);
    if (second == std::string::npos) {
        return std::nullopt;
    }
    RollbackResult r;
    r.to_version = stored.substr(0, first);
    r.from_version = stored.substr(first + 1, second - first - 1);
    r.reason = stored.substr(second + 1);
    return r;
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

namespace {
/// The timer behind refresh_rollback_busy_file(); deletes itself once disarmed.
void update_busy_file(lv_timer_t* timer) {
    const std::string dir = rollback_dir();
    std::string unused;
    if (dir.empty() || !read_rollback_file(dir, kRollbackArmedFile, unused)) {
        if (!dir.empty()) {
            remove_rollback_file(dir, kRollbackBusyFile);
        }
        if (timer != nullptr) {
            lv_timer_delete(timer);
        }
        return;
    }
    auto& print_state = get_printer_state().print_state();
    const bool busy =
        job_holds_machine(print_state.get_print_lifecycle()) || print_state.is_plr_snapshot_valid();
    if (busy) {
        write_rollback_file(dir, kRollbackBusyFile, "");
    } else {
        remove_rollback_file(dir, kRollbackBusyFile);
    }
}

} // namespace

void refresh_rollback_busy_file() {
    update_busy_file(nullptr);
}

void on_ui_ready(bool safe_mode) {
    auto& checker = UpdateChecker::instance();
    const std::string dir = rollback_dir();

    std::string text;
    if (!dir.empty() && read_rollback_file(dir, kRollbackResultFile, text)) {
        remove_rollback_file(dir, kRollbackResultFile);
        if (auto result = parse_rollback_result(text)) {
            checker.report_rollback(*result);
        }
    }
    // Shown on every start until it has been up long enough to be seen, so a
    // service restart right after the rollback does not swallow it.
    checker.show_pending_rollback_notice();

    if (dir.empty()) {
        return;
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

    update_busy_file(nullptr);
    // A print starting is seen at once; the timer catches a power-loss resume
    // and deletes itself once the rollback is disarmed.
    auto& ps = get_printer_state();
    static int s_anchor = 0;
    static ObserverGuard s_lifecycle_observer;
    s_lifecycle_observer = helix::ui::observe<int>(
        ps.print_state().get_print_lifecycle_subject(), &s_anchor,
        [](int*, int) { refresh_rollback_busy_file(); }, ps.get_subjects_lifetime());
    // TIMER_DTOR_OK: no owner, captures nothing, deletes itself once disarmed
    lv_timer_create([](lv_timer_t* t) { update_busy_file(t); }, kRollbackBusyPollMs, nullptr);
}

} // namespace helix::update
