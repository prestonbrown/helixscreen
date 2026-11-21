// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_update.h"

#include "app_globals.h"
#include "firmware_update_check.h"
#include "install_provenance.h"
#include "printer_state.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace helix::fwupdate {

namespace {

using board::Board;

/// One output line that moves an update forward.
struct ProgressMark {
    const char* needle; ///< substring of the line
    Stage stage;
    int percent; ///< -1: the line reports no percentage
    int index;
    bool point_of_no_return;
};

/// systemUpgrade.sh `upgrade all` on stock Snapmaker U1 firmware. The percentages
/// are the ones the script itself publishes over MQTT at the same steps; each line
/// names the step that starts there.
constexpr ProgressMark kSnapmakerU1Marks[] = {
    {"Upgrade abort.", Stage::Failed, -1, 0, false},
    {"The upgrade is in progress.", Stage::Failed, -1, 0, false},
    {"doesn't exist, please check.", Stage::Failed, -1, 0, false},
    {"setsid: not found", Stage::Failed, -1, 0, false},
    {"upgrade soc finish, prepare to reboot", Stage::Rebooting, 99, 0, true},
    {"Prepare to upgrade SOC.", Stage::MainSystem, 93, 0, false},
    {"Upgrade all start", Stage::Unpacking, 1, 0, false},
    {"Shutdown klipper process.", Stage::MainBoard, 9, 0, false},
    {"The MCU0 upgrade was successful.", Stage::Toolhead, 18, 1, false},
    {"head0 upgrade was successful", Stage::Toolhead, 37, 2, false},
    {"head0 offline", Stage::Toolhead, 37, 2, false},
    {"head1 upgrade was successful", Stage::Toolhead, 56, 3, false},
    {"head1 offline", Stage::Toolhead, 56, 3, false},
    {"head2 upgrade was successful", Stage::Toolhead, 75, 4, false},
    {"head2 offline", Stage::Toolhead, 75, 4, false},
    {"head3 upgrade was successful", Stage::MainSystem, 93, 0, false},
    {"head3 offline", Stage::MainSystem, 93, 0, false},
};

ProgressEvent parse_marks(const ProgressMark* begin, const ProgressMark* end,
                          std::string_view line) {
    for (const ProgressMark* m = begin; m != end; ++m) {
        if (line.find(m->needle) != std::string_view::npos) {
            ProgressEvent ev;
            ev.stage = m->stage;
            if (m->percent >= 0) {
                ev.percent = m->percent;
            }
            ev.index = m->index;
            ev.point_of_no_return = m->point_of_no_return;
            return ev;
        }
    }
    return {};
}

ProgressEvent parse_snapmaker_u1(std::string_view line) {
    return parse_marks(std::begin(kSnapmakerU1Marks), std::end(kSnapmakerU1Marks), line);
}

/// Starts systemUpgrade.sh in its own session, reading /dev/null and writing only to
/// the log, then returns. No pipe back to HelixScreen: the script stops services and
/// reboots, and a write to a pipe whose reader died would kill it mid-flash.
/// $1 script, $2 image, $3 log.
constexpr const char* kSnapmakerU1Launcher =
    "setsid \"$1\" upgrade all \"$2\" </dev/null >\"$3\" 2>&1 &";

std::vector<std::string> launch_snapmaker_u1(const char* script, const std::string& image,
                                             const std::string& log) {
    return {"/bin/sh", "-c", kSnapmakerU1Launcher, "fwupdate", script, image, log};
}

bool is_snapmaker_u1_stock(const HostFacts& h) {
    return h.board == Board::SnapmakerU1 && h.host_mod != "paxx";
}
bool is_snapmaker_u1_paxx(const HostFacts& h) {
    return h.board == Board::SnapmakerU1 && h.host_mod == "paxx";
}
bool is_k1(const HostFacts& h) {
    return h.board == Board::K1;
}
bool is_k2(const HostFacts& h) {
    return h.board == Board::K2;
}
bool is_ad5m(const HostFacts& h) {
    return h.board == Board::Ad5m;
}
bool is_ad5x(const HostFacts& h) {
    return h.board == Board::Ad5x;
}
bool is_creator5(const HostFacts& h) {
    return h.board == Board::Creator5;
}
bool is_creator5_pro(const HostFacts& h) {
    return h.board == Board::Creator5Pro;
}
bool is_cc1(const HostFacts& h) {
    return h.board == Board::Cc1;
}
bool is_qidi(const HostFacts& h) {
    std::string lower = h.printer_type;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("qidi", 0) == 0;
}

constexpr uint64_t kMiB = 1024ull * 1024ull;

} // namespace

struct Provider {
    const char* id;
    const char* owner;
    bool (*matches)(const HostFacts&);
    Mechanism mechanism;
    Readiness readiness;

    /// Image rule: extension (nullptr: the row takes no image file), name prefixes
    /// (any one; none listed: any name), minimum size.
    const char* extension;
    const char* prefixes[3];
    uint64_t min_bytes;

    /// What applying does to the printer, for the confirm dialog.
    bool flashes;
    bool stops_klipper;
    bool reboots;
    bool removes_helixscreen;

    /// Apply plan: the flasher (nullptr: no apply path), its lock file and where it
    /// unpacks, the launcher argv and the parser for its output.
    const char* script;
    const char* lock_path;
    const char* unpack_dir;
    std::vector<std::string> (*launch)(const char* script, const std::string& image,
                                       const std::string& log);
    ProgressEvent (*parse)(std::string_view line);

    /// Where to look for new firmware remotely, or nullptr.
    const CheckSource* check;
    /// The docs page that reinstalls HelixScreen after an update removed it, or nullptr.
    const char* reinstall_guide;
};

namespace {

// First match wins. A row with no apply plan still answers what the printer has,
// so the page can say why it offers nothing.
const Provider kProviders[] = {
    // PAXX Extended Firmware owns updates through its firmware-config API; HelixScreen
    // calls it only under a contract agreed with its maintainer.
    {"snapmaker_u1_paxx",
     "PAXX",
     is_snapmaker_u1_paxx,
     Mechanism::OwnerTool,
     Readiness::NeedsVendorCooperation,
     nullptr,
     {nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    // Read from a third-party copy of the stock script, never run by us: the apply
    // stays behind unverified_apply_allowed(). The script deletes /oem/.debug before
    // it unpacks, which removes a standalone HelixScreen even when the update fails.
    {"snapmaker_u1_stock",
     "Snapmaker",
     is_snapmaker_u1_stock,
     Mechanism::VendorScript,
     Readiness::NeedsDeviceVerification,
     ".bin",
     {nullptr},
     50 * kMiB,
     true,
     true,
     true,
     true,
     "/home/lava/bin/systemUpgrade.sh",
     "/tmp/.system_upgrade.lock",
     "/tmp",
     launch_snapmaker_u1,
     parse_snapmaker_u1,
     nullptr,
     "guide/install-u1/"},
    // /etc/ota_bin/local_ota_update.sh <img>, progress through OTA_CB_APP. No apply
    // path until it has run on a K1.
    {"creality_k1",
     "Creality",
     is_k1,
     Mechanism::VendorScript,
     Readiness::NeedsDeviceVerification,
     ".img",
     {nullptr},
     0,
     true,
     true,
     false,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    // The same script, handing the image to swupdate (A/B with recovery).
    {"creality_k2",
     "Creality",
     is_k2,
     Mechanism::VendorScript,
     Readiness::NeedsDeviceVerification,
     ".img",
     {nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    // The stock firmware installs a model-prefixed .tgz from the stick at power-on.
    {"flashforge_ad5m",
     "FlashForge",
     is_ad5m,
     Mechanism::UsbAtBoot,
     Readiness::NeedsDeviceVerification,
     ".tgz",
     {"Adventurer5M-", "Adventurer5MPro-", nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    {"flashforge_ad5x",
     "FlashForge",
     is_ad5x,
     Mechanism::UsbAtBoot,
     Readiness::NeedsDeviceVerification,
     ".tgz",
     {"AD5X-", nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    {"flashforge_creator5",
     "FlashForge",
     is_creator5,
     Mechanism::UsbAtBoot,
     Readiness::NeedsDeviceVerification,
     ".tgz",
     {"Creator5-", nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    {"flashforge_creator5_pro",
     "FlashForge",
     is_creator5_pro,
     Mechanism::UsbAtBoot,
     Readiness::NeedsDeviceVerification,
     ".tgz",
     {"Creator5Pro-", nullptr},
     0,
     true,
     true,
     true,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    // Updates arrive as Klipper action prompts the app already renders.
    {"elegoo_cc1",
     "OpenCentauri",
     is_cc1,
     Mechanism::SelfUpdating,
     Readiness::NoActionNeeded,
     nullptr,
     {nullptr},
     0,
     false,
     false,
     false,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    // qidiclient installs QIDI's zip (firmware_manifest.json, a SoC .deb through
    // dpkg, board images) from QIDI's CDN or a USB QD_Update/ folder. The
    // community Valve Oil updater already runs that outside qidiclient (pinned
    // sha256, idle and heaters-off gate, resumable systemd unit): the partner to
    // agree a contract with before HelixScreen offers it.
    {"qidi",
     "QIDI",
     is_qidi,
     Mechanism::VendorPackage,
     Readiness::NeedsVendorCooperation,
     nullptr,
     {nullptr},
     0,
     true,
     true,
     false,
     false,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
};

bool ends_with(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return std::equal(suffix.begin(), suffix.end(), s.end() - suffix.size(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

} // namespace

HostFacts current_host_facts() {
    HostFacts facts;
    facts.board = board::current();
    facts.host_mod = install_provenance::detect_host_mod("");
    facts.printer_type = get_printer_state().profile_state().printer_type();
    return facts;
}

const Provider* provider_for(const HostFacts& host) {
    for (const Provider& p : kProviders) {
        if (p.matches(host)) {
            return &p;
        }
    }
    return nullptr;
}

const char* provider_id(const Provider& p) {
    return p.id;
}
const char* provider_owner(const Provider& p) {
    return p.owner;
}
Mechanism provider_mechanism(const Provider& p) {
    return p.mechanism;
}
Readiness provider_readiness(const Provider& p) {
    return p.readiness;
}
const char* provider_reinstall_guide(const Provider& p) {
    return p.reinstall_guide != nullptr ? p.reinstall_guide : "";
}

bool unverified_apply_allowed() {
    const char* v = std::getenv("HELIX_FWUPDATE_UNVERIFIED_APPLY");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool apply_enabled(const Provider& p, bool unverified_allowed) {
    if (p.launch == nullptr || p.script == nullptr) {
        return false;
    }
    return p.readiness == Readiness::Supported ||
           (p.readiness == Readiness::NeedsDeviceVerification && unverified_allowed);
}

const CheckSource* check_source(const Provider& p) {
    return p.check != nullptr && p.check->url != nullptr ? p.check : nullptr;
}

bool vendor_apply_available(const Provider* p) {
    return p != nullptr && apply_enabled(*p, unverified_apply_allowed());
}

ImageVerdict check_image(const Provider& p, const FileEntry& file) {
    if (p.extension == nullptr || !ends_with(file.name, p.extension)) {
        return ImageVerdict::WrongType;
    }
    if (p.prefixes[0] != nullptr) {
        bool prefixed = false;
        for (const char* const* pre = p.prefixes; *pre != nullptr; ++pre) {
            if (file.name.rfind(*pre, 0) == 0) {
                prefixed = true;
                break;
            }
        }
        if (!prefixed) {
            return ImageVerdict::WrongModel;
        }
    }
    if (file.size < p.min_bytes) {
        return ImageVerdict::TooSmall;
    }
    return ImageVerdict::Ok;
}

std::vector<FileEntry> find_images(const Provider& p, const std::vector<std::string>& roots) {
    namespace fs = std::filesystem;
    std::vector<FileEntry> found;
    for (const std::string& root : roots) {
        std::error_code ec;
        fs::directory_iterator it(root, ec);
        if (ec) {
            continue;
        }
        for (const fs::directory_entry& entry : it) {
            std::error_code fec;
            if (!entry.is_regular_file(fec) || fec) {
                continue;
            }
            const uint64_t size = entry.file_size(fec);
            if (fec) {
                continue;
            }
            FileEntry file{entry.path().string(), entry.path().filename().string(), size};
            if (check_image(p, file) == ImageVerdict::Ok) {
                found.push_back(std::move(file));
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.name != b.name ? a.name < b.name : a.path < b.path;
    });
    return found;
}

bool print_stats_state_permits_update(std::string_view state) {
    return state == "standby" || state == "complete" || state == "cancelled" || state == "error";
}

Blocker first_blocker(const ApplyInputs& in) {
    if (job_holds_machine(in.lifecycle) || in.print_stats_state == "printing" ||
        in.print_stats_state == "paused") {
        return Blocker::JobRunning;
    }
    if (!print_stats_state_permits_update(in.print_stats_state)) {
        return Blocker::PrintStateUnknown;
    }
    if (in.update_running) {
        return Blocker::UpdateRunning;
    }
    if (in.required_bytes > 0) {
        if (!in.free_bytes) {
            return Blocker::SpaceUnknown;
        }
        if (*in.free_bytes < in.required_bytes) {
            return Blocker::NotEnoughSpace;
        }
    }
    return Blocker::None;
}

std::vector<ConfirmPoint> confirm_points(const Provider& p) {
    std::vector<ConfirmPoint> points;
    if (p.flashes) {
        points.push_back(ConfirmPoint::KeepPowerOn);
    }
    if (p.stops_klipper) {
        points.push_back(ConfirmPoint::StopsKlipper);
    }
    if (p.reboots) {
        points.push_back(ConfirmPoint::PrinterReboots);
    }
    if (p.removes_helixscreen) {
        points.push_back(ConfirmPoint::RemovesHelixScreen);
    }
    return points;
}

ApplyPlan apply_plan(const Provider& p, const std::string& image_path,
                     const std::string& log_path) {
    ApplyPlan plan;
    plan.log_path = log_path;
    if (p.launch == nullptr || p.script == nullptr) {
        return plan;
    }
    plan.argv = p.launch(p.script, image_path, log_path);
    plan.lock_path = p.lock_path != nullptr ? p.lock_path : "";
    plan.unpack_dir = p.unpack_dir != nullptr ? p.unpack_dir : "";
    return plan;
}

ProgressEvent parse_progress(const Provider& p, std::string_view line) {
    return p.parse != nullptr ? p.parse(line) : ProgressEvent{};
}

} // namespace helix::fwupdate
