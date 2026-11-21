// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_firmware_update.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "display_format.h"
#include "helix_version.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "usb_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>

#include "hv/json.hpp"

namespace helix::settings {

namespace {

using json = nlohmann::json;
using namespace helix::fwupdate;

constexpr const char* kUpdateResponseMethod = "notify_update_response";
constexpr const char* kResponseHandler = "firmware_update_overlay";
/// Moonraker answers machine.update.upgrade when the update is done.
constexpr uint32_t kUpgradeTimeoutMs = 30u * 60u * 1000u;
constexpr uint32_t kRefreshTimeoutMs = 3u * 60u * 1000u;
constexpr uint32_t kVendorPollMs = 500;
constexpr const char* kVendorLogPath = "/tmp/helixscreen-firmware-update.log";

/// The row index an XML event_cb carries as its user_data string.
std::optional<std::size_t> event_index(lv_event_t* e) {
    const char* s = static_cast<const char*>(lv_event_get_user_data(e));
    if (s == nullptr || *s == '\0') {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::atoi(s));
}

std::string entry_detail(const ManagedEntry& e) {
    if (!e.valid) {
        return lv_tr("Needs repair from a web interface");
    }
    if (e.kind == "system") {
        return e.update_available
                   ? fmt::format(fmt::runtime(lv_tr("{} packages to update")), e.package_count)
                   : std::string(lv_tr("Up to date"));
    }
    if (e.update_available) {
        const std::string to = e.remote_version.empty() || e.remote_version == "?"
                                   ? std::string(lv_tr("a newer version"))
                                   : e.remote_version;
        const std::string available = fmt::format(fmt::runtime(lv_tr("{} available")), to);
        return e.version.empty() ? available : e.version + " \xC2\xB7 " + available;
    }
    return e.version.empty() ? std::string(lv_tr("Up to date"))
                             : fmt::format(fmt::runtime(lv_tr("Up to date ({})")), e.version);
}

/// The confirm dialog's question for update_manager entry @p e.
std::string confirm_question(const ManagedEntry& e) {
    if (e.kind == "system") {
        return fmt::format(fmt::runtime(lv_tr("Update {} system packages?")), e.package_count);
    }
    const bool remote_known = !e.remote_version.empty() && e.remote_version != "?";
    if (!e.version.empty() && remote_known) {
        return fmt::format(fmt::runtime(lv_tr("Update {} from {} to {}?")), e.name, e.version,
                           e.remote_version);
    }
    if (remote_known) {
        return fmt::format(fmt::runtime(lv_tr("Update {} to {}?")), e.name, e.remote_version);
    }
    return fmt::format(fmt::runtime(lv_tr("Update {}?")), e.name);
}

const char* blocker_text(Blocker b) {
    switch (b) {
    case Blocker::JobRunning:
        return lv_tr("Finish or cancel the print first");
    case Blocker::PrintStateUnknown:
        return lv_tr("The printer did not say whether it is printing. Try again.");
    case Blocker::UpdateRunning:
        return lv_tr("Another update is already running");
    case Blocker::NotEnoughSpace:
        return lv_tr("Not enough free space on the printer to unpack the firmware");
    case Blocker::SpaceUnknown:
        return lv_tr("Could not read the free space on the printer");
    case Blocker::None:
        break;
    }
    return "";
}

const char* confirm_point_text(ConfirmPoint p) {
    switch (p) {
    case ConfirmPoint::KeepPowerOn:
        return lv_tr("Do not turn the printer off until it finishes. Losing power while it "
                     "flashes can leave the printer unable to start.");
    case ConfirmPoint::StopsKlipper:
        return lv_tr("The printer stops and cannot print until the update is done.");
    case ConfirmPoint::PrinterReboots:
        return lv_tr("The printer restarts when the update finishes.");
    case ConfirmPoint::RemovesHelixScreen:
        return lv_tr("This update removes HelixScreen from the printer.");
    case ConfirmPoint::ServicesRestart:
        return lv_tr("Klipper or Moonraker may restart while it updates.");
    case ConfirmPoint::UpgradesOsPackages:
        return lv_tr("This upgrades the printer's operating system packages.");
    case ConfirmPoint::MayNeedReboot:
        return lv_tr("The printer may need a reboot afterwards.");
    case ConfirmPoint::StayOnUntilDone:
        return lv_tr("Do not turn the printer off until the update finishes.");
    }
    return "";
}

std::string readiness_note(const Provider& p) {
    const char* owner = provider_owner(p);
    switch (provider_readiness(p)) {
    case Readiness::NeedsDeviceVerification:
        return fmt::format(
            fmt::runtime(lv_tr("Installing {} firmware from this screen is not available yet.")),
            owner);
    case Readiness::NeedsVendorCooperation:
        return fmt::format(fmt::runtime(lv_tr("{} firmware updates are managed by its own tools.")),
                           owner);
    case Readiness::NotPossible:
        return fmt::format(
            fmt::runtime(lv_tr("{} firmware can only be updated from its own screen app.")), owner);
    case Readiness::NoActionNeeded:
        return lv_tr("This printer's firmware offers updates by itself when one is available.");
    case Readiness::Supported:
        break;
    }
    return "";
}

std::string stage_text(const ProgressEvent& ev) {
    switch (ev.stage) {
    case Stage::Unpacking:
        return lv_tr("Unpacking the firmware");
    case Stage::MainBoard:
        return lv_tr("Updating the main board");
    case Stage::Toolhead:
        return fmt::format(fmt::runtime(lv_tr("Updating toolhead {}")), ev.index);
    case Stage::MainSystem:
        return lv_tr("Updating the system");
    case Stage::Rebooting:
        return lv_tr("Restarting the printer");
    case Stage::Failed:
    case Stage::None:
        break;
    }
    return "";
}

void on_entry_update_clicked(lv_event_t* e) {
    if (const auto i = event_index(e)) {
        get_firmware_update_overlay().request_entry_update(*i);
    }
}

void on_image_install_clicked(lv_event_t* e) {
    if (const auto i = event_index(e)) {
        get_firmware_update_overlay().request_image_install(*i);
    }
}

} // namespace

FirmwareUpdateOverlay& get_firmware_update_overlay() {
    return lazy_global<FirmwareUpdateOverlay>("FirmwareUpdateOverlay");
}

FirmwareUpdateOverlay::~FirmwareUpdateOverlay() {
    unregister_response_handler();
    if (subjects_initialized_ && lv_is_initialized()) {
        subjects_.deinit_all();
    }
}

void FirmwareUpdateOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        provider_ = provider_for(current_host_facts());
        const bool vendor = vendor_apply_available(provider_);
        const std::string note =
            provider_ != nullptr && !vendor ? readiness_note(*provider_) : std::string();
        UI_MANAGED_SUBJECT_INT(view_, kViewList, "fwupdate_view", subjects_);
        UI_MANAGED_SUBJECT_INT(list_state_, kListLoading, "fwupdate_list_state", subjects_);
        UI_MANAGED_SUBJECT_INT(vendor_shown_, vendor ? 1 : 0, "fwupdate_vendor_shown", subjects_);
        UI_MANAGED_SUBJECT_STRING(vendor_status_, vendor_status_buf_, "", "fwupdate_vendor_status",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(vendor_note_, vendor_note_buf_, note.c_str(),
                                  "fwupdate_vendor_note", subjects_);
        UI_MANAGED_SUBJECT_INT(entry_count_, 0, "fwupdate_entry_count", subjects_);
        for (std::size_t i = 0; i < kMaxEntries; ++i) {
            const std::string base = "fwupdate_entry_" + std::to_string(i);
            entry_names_[i] = {base + "_name", base + "_detail", base + "_can_update"};
            UI_MANAGED_SUBJECT_STRING_N(entry_name_[i], entry_name_buf_[i].data(),
                                        entry_name_buf_[i].size(), "", entry_names_[i][0].c_str(),
                                        subjects_);
            UI_MANAGED_SUBJECT_STRING_N(entry_detail_[i], entry_detail_buf_[i].data(),
                                        entry_detail_buf_[i].size(), "", entry_names_[i][1].c_str(),
                                        subjects_);
            UI_MANAGED_SUBJECT_INT(entry_can_update_[i], 0, entry_names_[i][2].c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_INT(image_count_, 0, "fwupdate_image_count", subjects_);
        for (std::size_t i = 0; i < kMaxImages; ++i) {
            image_subject_names_[i] = "fwupdate_image_" + std::to_string(i) + "_name";
            UI_MANAGED_SUBJECT_STRING_N(image_name_[i], image_name_buf_[i].data(),
                                        image_name_buf_[i].size(), "",
                                        image_subject_names_[i].c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_INT(busy_, 0, "fwupdate_busy", subjects_);
        UI_MANAGED_SUBJECT_INT(run_state_, kRunRunning, "fwupdate_run_state", subjects_);
        UI_MANAGED_SUBJECT_STRING(run_title_, run_title_buf_, "", "fwupdate_run_title", subjects_);
        UI_MANAGED_SUBJECT_STRING(run_message_, run_message_buf_, "", "fwupdate_run_message",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(run_pct_, 0, "fwupdate_run_pct", subjects_);
        UI_MANAGED_SUBJECT_INT(run_has_pct_, 0, "fwupdate_run_has_pct", subjects_);
        UI_MANAGED_SUBJECT_INT(run_no_return_, 0, "fwupdate_run_no_return", subjects_);
    });
}

void FirmwareUpdateOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_fwupdate_entry_update", on_entry_update_clicked);
    lv_xml_register_event_cb(nullptr, "on_fwupdate_image_install", on_image_install_clicked);
    register_xml_callbacks({
        {"on_fwupdate_refresh", [](lv_event_t*) { get_firmware_update_overlay().refresh(true); }},
        {"on_fwupdate_done", [](lv_event_t*) { get_firmware_update_overlay().dismiss_progress(); }},
    });
}

void FirmwareUpdateOverlay::on_activate() {
    OverlayBase::on_activate();
    if (!running_) {
        refresh(false);
    }
}

void FirmwareUpdateOverlay::refresh(bool remote) {
    if (!subjects_initialized_ || running_) {
        return;
    }
    scan_images();
    IMoonrakerClient* client = get_moonraker_client();
    if (client == nullptr || !get_printer_state().is_update_manager_available()) {
        show_status(std::nullopt);
        return;
    }
    lv_subject_set_int(&list_state_, kListLoading);
    auto fetch_status = [this, client]() {
        client->send_jsonrpc(
            "machine.update.status", json::object(),
            lifetime_.bg_cb(
                "FirmwareUpdate::status",
                [this](const json& response) {
                    const json* result = helix::json_util::find_member(response, "result");
                    show_status(result != nullptr ? parse_update_status(*result) : std::nullopt);
                }),
            lifetime_.bg_cb("FirmwareUpdate::status_failed",
                            [this](const MoonrakerError& err) {
                                spdlog::warn("[{}] machine.update.status failed: {}", get_name(),
                                             err.message);
                                show_status(std::nullopt);
                            }),
            0, /*silent=*/true);
    };
    if (!remote) {
        fetch_status();
        return;
    }
    // Moonraker refuses a refresh during a print; the status read after it says
    // whatever it last knew either way.
    client->send_jsonrpc("machine.update.refresh", json::object(),
                         lifetime_.bg_cb("FirmwareUpdate::refreshed",
                                         [fetch_status](const json&) mutable { fetch_status(); }),
                         lifetime_.bg_cb("FirmwareUpdate::refresh_failed",
                                         [this, fetch_status](const MoonrakerError& err) mutable {
                                             spdlog::info(
                                                 "[{}] machine.update.refresh declined: {}",
                                                 get_name(), err.message);
                                             fetch_status();
                                         }),
                         kRefreshTimeoutMs, /*silent=*/true);
}

void FirmwareUpdateOverlay::show_status(const std::optional<UpdateStatus>& status) {
    if (!subjects_initialized_) {
        return;
    }
    entries_.clear();
    if (!status) {
        update_manager_busy_ = false;
        publish_busy();
        lv_subject_set_int(&entry_count_, 0);
        lv_subject_set_int(&list_state_, kListFailed);
        return;
    }
    update_manager_busy_ = status->busy;
    publish_busy();
    for (const ManagedEntry& e : status->entries) {
        if (entries_.size() == kMaxEntries) {
            break;
        }
        entries_.push_back(e);
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        lv_subject_set_string(&entry_name_[i], entries_[i].name.c_str());
        lv_subject_set_string(&entry_detail_[i], entry_detail(entries_[i]).c_str());
        lv_subject_set_int(&entry_can_update_[i], entries_[i].update_available ? 1 : 0);
    }
    lv_subject_set_int(&entry_count_, static_cast<int>(entries_.size()));
    lv_subject_set_int(&list_state_, kListReady);
}

void FirmwareUpdateOverlay::scan_images() {
    images_.clear();
    if (!vendor_apply_available(provider_)) {
        lv_subject_set_int(&image_count_, 0);
        return;
    }
    std::vector<std::string> roots;
    if (usb_manager_ != nullptr) {
        for (const auto& drive : usb_manager_->get_drives()) {
            roots.push_back(drive.mount_path);
        }
    }
    images_ = find_images(*provider_, roots);
    if (images_.size() > kMaxImages) {
        images_.resize(kMaxImages);
    }
    for (std::size_t i = 0; i < images_.size(); ++i) {
        const std::string label =
            images_[i].name + " (" +
            helix::display_format::format_file_size(static_cast<size_t>(images_[i].size)) + ")";
        lv_subject_set_string(&image_name_[i], label.c_str());
    }
    lv_subject_set_int(&image_count_, static_cast<int>(images_.size()));
    lv_subject_set_string(
        &vendor_status_,
        images_.empty()
            ? fmt::format(fmt::runtime(lv_tr("Insert a USB stick with the {} firmware file in its "
                                             "top folder.")),
                          provider_owner(*provider_))
                  .c_str()
            : "");
}

void FirmwareUpdateOverlay::toast_blocker(Blocker b) {
    ToastManager::instance().show(ToastSeverity::WARNING, blocker_text(b));
}

void FirmwareUpdateOverlay::request_entry_update(std::size_t index) {
    if (index >= entries_.size() || !entries_[index].update_available) {
        return;
    }
    if (running_ || update_manager_busy_) {
        toast_blocker(Blocker::UpdateRunning);
        return;
    }
    if (job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        toast_blocker(Blocker::JobRunning);
        return;
    }
    const ManagedEntry& e = entries_[index];
    std::string message = confirm_question(e);
    const std::vector<ConfirmPoint> points = confirm_points(e);
    message += "\n\n";
    for (std::size_t i = 0; i < points.size(); ++i) {
        message += i == 0 ? "" : " ";
        message += confirm_point_text(points[i]);
    }
    confirm_and_start({Job::Kind::Managed, e.name}, std::move(message),
                      confirm_is_destructive(points));
}

void FirmwareUpdateOverlay::request_image_install(std::size_t index) {
    if (index >= images_.size() || provider_ == nullptr) {
        return;
    }
    if (running_) {
        toast_blocker(Blocker::UpdateRunning);
        return;
    }
    if (job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        toast_blocker(Blocker::JobRunning);
        return;
    }
    std::string message =
        fmt::format(fmt::runtime(lv_tr("Install {} on this printer?")), images_[index].name);
    for (ConfirmPoint p : confirm_points(*provider_)) {
        message += "\n\n";
        message += confirm_point_text(p);
        if (p == ConfirmPoint::RemovesHelixScreen) {
            message += " ";
            const char* guide = provider_reinstall_guide(*provider_);
            message +=
                *guide != '\0'
                    ? fmt::format(fmt::runtime(lv_tr("Reinstall it afterwards by following {}.")),
                                  helix::docs_url_text(helix::docs_url(guide)))
                    : std::string(lv_tr("Reinstall it afterwards."));
        }
    }
    confirm_and_start({Job::Kind::Vendor, images_[index].path}, std::move(message),
                      confirm_is_destructive(confirm_points(*provider_)));
}

void FirmwareUpdateOverlay::confirm_and_start(Job job, std::string message, bool destructive) {
    helix::ui::ConfirmOptions options;
    options.owner_token = lifetime_.token();
    options.destructive = destructive;
    helix::ui::modal_confirm(
        lv_tr("Firmware Update"), message.c_str(), ModalSeverity::Warning,
        job.kind == Job::Kind::Vendor ? lv_tr("Install") : lv_tr("Update"),
        [this, job]() { check_and_start(job); }, options);
}

void FirmwareUpdateOverlay::check_and_start(Job job) {
    IMoonrakerClient* client = get_moonraker_client();
    if (client == nullptr || running_) {
        toast_blocker(running_ ? Blocker::UpdateRunning : Blocker::PrintStateUnknown);
        return;
    }
    // The decision reads print_stats as the printer reports it now, not the
    // last status frame.
    client->send_jsonrpc(
        "printer.objects.query", json{{"objects", {{"print_stats", json::array({"state"})}}}},
        lifetime_.bg_cb(
            "FirmwareUpdate::print_state",
            [this, job](const json& response) {
                const json* result = helix::json_util::find_member(response, "result");
                const json* status =
                    result != nullptr ? helix::json_util::find_member(*result, "status") : nullptr;
                const json* stats = status != nullptr
                                        ? helix::json_util::find_member(*status, "print_stats")
                                        : nullptr;
                ApplyInputs in;
                in.lifecycle = get_printer_state().print_state().get_print_lifecycle();
                in.print_stats_state =
                    stats != nullptr ? helix::json_util::safe_string(*stats, "state") : "";
                in.update_running = running_.has_value() || update_manager_busy_;
                if (job.kind == Job::Kind::Managed) {
                    const Blocker b = first_blocker(in);
                    if (b != Blocker::None) {
                        toast_blocker(b);
                        return;
                    }
                    start_managed(job.name);
                    return;
                }
                for (const FileEntry& image : images_) {
                    if (image.path == job.name) {
                        start_vendor(image, in);
                        return;
                    }
                }
            }),
        lifetime_.bg_cb(
            "FirmwareUpdate::print_state_failed",
            [this](const MoonrakerError&) { toast_blocker(Blocker::PrintStateUnknown); }),
        0, /*silent=*/true);
}

void FirmwareUpdateOverlay::set_progress(const std::string& message, std::optional<int> percent) {
    if (!message.empty()) {
        lv_subject_set_string(&run_message_, message.c_str());
    }
    if (percent) {
        lv_subject_set_int(&run_pct_, *percent);
        lv_subject_set_int(&run_has_pct_, 1);
    }
}

void FirmwareUpdateOverlay::start_managed(const std::string& name) {
    IMoonrakerClient* client = get_moonraker_client();
    if (client == nullptr) {
        toast_blocker(Blocker::PrintStateUnknown);
        return;
    }
    running_ = Job{Job::Kind::Managed, name};
    publish_busy();
    job_failed_ = false;
    lv_subject_set_string(&run_title_,
                          fmt::format(fmt::runtime(lv_tr("Updating {}")), name).c_str());
    lv_subject_set_string(&run_message_, lv_tr("Starting the update"));
    lv_subject_set_int(&run_has_pct_, 0);
    lv_subject_set_int(&run_no_return_, 0);
    lv_subject_set_int(&run_state_, kRunRunning);
    lv_subject_set_int(&view_, kViewProgress);
    spdlog::info("[{}] Updating update_manager entry {}", get_name(), name);

    job_client_ = client;
    client->register_method_callback(
        kUpdateResponseMethod, kResponseHandler,
        object_lifetime_.bg_cb("FirmwareUpdate::update_response", [this](const json& msg) {
            if (const auto r = parse_update_response(msg)) {
                on_update_response(*r);
            }
        }));
    client->send_jsonrpc(
        "machine.update.upgrade", json{{"name", name}},
        object_lifetime_.bg_cb("FirmwareUpdate::upgraded",
                               [this](const json&) { finish(!job_failed_, ""); }),
        object_lifetime_.bg_cb("FirmwareUpdate::upgrade_failed",
                               [this](const MoonrakerError& err) { finish(false, err.message); }),
        kUpgradeTimeoutMs, /*silent=*/true);
}

void FirmwareUpdateOverlay::on_update_response(const UpdateResponse& r) {
    if (!running_ || running_->kind != Job::Kind::Managed || r.application != running_->name) {
        return;
    }
    if (response_reports_failure(r)) {
        job_failed_ = true;
    }
    set_progress(r.message, percent_in_message(r.message));
    if (r.complete) {
        finish(!job_failed_, r.message);
    }
}

void FirmwareUpdateOverlay::start_vendor(const FileEntry& image, ApplyInputs in) {
    if (provider_ == nullptr) {
        return;
    }
    in.required_bytes = image.size;
    const ApplyPlan plan = apply_plan(*provider_, image.path, kVendorLogPath);
    std::error_code ec;
    std::filesystem::remove(plan.log_path, ec);
    running_ = Job{Job::Kind::Vendor, image.name};
    publish_busy();
    job_failed_ = false;
    lv_subject_set_string(&run_title_, lv_tr("Installing printer firmware"));
    lv_subject_set_string(&run_message_, lv_tr("Starting the update"));
    lv_subject_set_int(&run_has_pct_, 0);
    lv_subject_set_int(&run_no_return_, 0);
    lv_subject_set_int(&run_state_, kRunRunning);
    lv_subject_set_int(&view_, kViewProgress);
    vendor_log_ = std::make_unique<LogTail>(plan.log_path);

    const Provider* provider = provider_;
    const bool unverified = unverified_apply_allowed();
    auto tok = object_lifetime_.token();
    helix::http::HttpExecutor::fast().submit([tok, provider, plan, in, unverified]() {
        const StartResult started =
            start_vendor_apply(*provider, plan, in, unverified, helix::run_process);
        tok.defer("FirmwareUpdate::vendor_started", [tok, started]() {
            (void)tok;
            auto& self = get_firmware_update_overlay();
            if (started.outcome == StartOutcome::Started) {
                self.vendor_timer_.reset(lv_timer_create(
                    [](lv_timer_t*) { get_firmware_update_overlay().poll_vendor_log(); },
                    kVendorPollMs, nullptr));
                return;
            }
            if (started.outcome == StartOutcome::Blocked) {
                self.finish(false, blocker_text(started.blocker));
            } else if (started.outcome == StartOutcome::LaunchFailed) {
                self.finish(false, started.error);
            } else {
                self.finish(false, lv_tr("Installing firmware is not enabled on this printer"));
            }
        });
    });
}

void FirmwareUpdateOverlay::poll_vendor_log() {
    if (!running_ || !vendor_log_ || provider_ == nullptr) {
        return;
    }
    for (const std::string& line : vendor_log_->poll()) {
        spdlog::info("[{}] flasher: {}", get_name(), line);
        const ProgressEvent ev = parse_progress(*provider_, line);
        if (ev.stage == Stage::None) {
            continue;
        }
        if (ev.stage == Stage::Failed) {
            finish(false, line);
            return;
        }
        set_progress(stage_text(ev), ev.percent);
        if (ev.point_of_no_return) {
            lv_subject_set_int(&run_no_return_, 1);
        }
    }
}

void FirmwareUpdateOverlay::unregister_response_handler() {
    if (job_client_ != nullptr && job_client_ == get_moonraker_client()) {
        job_client_->unregister_method_callback(kUpdateResponseMethod, kResponseHandler);
    }
    job_client_ = nullptr;
}

void FirmwareUpdateOverlay::finish(bool ok, const std::string& message) {
    if (!running_) {
        return;
    }
    spdlog::info("[{}] {} {}: {}", get_name(), running_->name, ok ? "updated" : "failed", message);
    unregister_response_handler();
    vendor_timer_.reset();
    vendor_log_.reset();
    running_.reset();
    publish_busy();
    lv_subject_set_string(&run_message_, !message.empty() ? message.c_str()
                                                          : (ok ? lv_tr("Update complete")
                                                                : lv_tr("The update failed")));
    lv_subject_set_int(&run_state_, ok ? kRunDone : kRunFailed);
}

void FirmwareUpdateOverlay::publish_busy() {
    // Moonraker runs one update_manager job at a time, whoever started it.
    lv_subject_set_int(&busy_, running_ || update_manager_busy_ ? 1 : 0);
}

void FirmwareUpdateOverlay::dismiss_progress() {
    if (running_) {
        return;
    }
    lv_subject_set_int(&view_, kViewList);
    refresh(false);
}

} // namespace helix::settings
