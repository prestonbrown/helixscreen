// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_switch_flow.h"

#include "ui_modal.h"
#include "ui_toast_manager.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "print_lifecycle_state.h"
#include "printer_cache_registry.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

/// RAII latch for PrinterSwitchFlow::m_soft_restart_in_progress.
///
/// Clearing the re-entrancy flag by hand on each exit path leaves it stuck true whenever an
/// exit is not the one that was hand-coded, and a stuck flag makes every later printer
/// switch or add a silent no-op until the process restarts.
class SoftRestartLatch {
  public:
    explicit SoftRestartLatch(bool& flag) : m_flag(flag) {
        m_flag = true;
    }
    ~SoftRestartLatch() {
        m_flag = false;
    }

    SoftRestartLatch(const SoftRestartLatch&) = delete;
    SoftRestartLatch& operator=(const SoftRestartLatch&) = delete;
    SoftRestartLatch(SoftRestartLatch&&) = delete;
    SoftRestartLatch& operator=(SoftRestartLatch&&) = delete;

  private:
    bool& m_flag;
};

} // namespace

namespace {

/// A job the user would be walking away from: preparing, printing or paused.
bool active_printer_is_printing() {
    return PrintLifecycleState::is_active(get_printer_state().print_state().get_print_lifecycle());
}

} // namespace

PrinterSwitchFlow::PrinterSwitchFlow(Config*& config, AsyncLifetimeGuard& async, Restart restart)
    : m_config(config), m_async(async), m_restart(std::move(restart)) {}

void PrinterSwitchFlow::request_switch(const std::string& printer_id) {
    if (m_soft_restart_in_progress || m_confirm_pending) {
        spdlog::warn("[PrinterSwitchFlow] Ignoring switch to '{}': a switch is already running",
                     printer_id);
        return;
    }
    if (printer_id == m_connected_printer_id) {
        return;
    }
    // A connected printer that is no longer in the list was removed, and its removal was
    // already confirmed; nothing is left to ask about.
    const auto ids = m_config->get_printer_ids();
    const bool connected_removed =
        std::find(ids.begin(), ids.end(), m_connected_printer_id) == ids.end();
    if (connected_removed || !active_printer_is_printing()) {
        switch_printer(printer_id);
        return;
    }

    m_confirm_pending = true;
    const std::string message = fmt::format(
        fmt::runtime(lv_tr("{} is still printing. The print keeps running after you switch.")),
        m_config->get_active_printer_name());
    ui::ConfirmOptions options;
    options.on_cancel = [this] { m_confirm_pending = false; };
    options.on_dismiss = [this] { m_confirm_pending = false; };
    options.owner_token = m_async.token();
    ui::modal_confirm(
        lv_tr("Switch Printer"), message.c_str(), ModalSeverity::Warning, lv_tr("Switch Printer"),
        [this, printer_id] {
            m_confirm_pending = false;
            // Out of the dialog's click handler: the restart tears down the
            // screen the dialog sits on.
            m_async.defer("PrinterSwitchFlow::confirmed_switch",
                          [this, printer_id] { switch_printer(printer_id); });
        },
        options);
}

void PrinterSwitchFlow::switch_printer(const std::string& printer_id) {
    if (m_confirm_pending) {
        spdlog::warn(
            "[PrinterSwitchFlow] Ignoring switch_printer while a switch is being confirmed");
        return;
    }
    if (m_soft_restart_in_progress) {
        spdlog::warn("[PrinterSwitchFlow] Ignoring switch_printer during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

    spdlog::info("[PrinterSwitchFlow] Switching to printer '{}'...", printer_id);

    const std::string previous_id = m_config->get_active_printer_id();
    if (!m_config->set_active_printer(printer_id)) {
        spdlog::error("[PrinterSwitchFlow] Failed to switch — unknown printer '{}'", printer_id);
        return;
    }
    // A switch the config does not remember would come back as the old printer after a
    // restart, so an unsaved switch does not happen.
    if (!save_or_report()) {
        m_config->set_active_printer(previous_id);
        return;
    }

    // Per-printer state lives at /printers/<active>/… and is reached via Config::df().
    // The active printer just changed, so df() now points at the new printer — fire every
    // registered per-printer cache invalidator BEFORE teardown, while df() is already
    // correct, so nothing keeps serving the previous printer's values (#804).
    PrinterCacheRegistry::instance().invalidate_all();

    m_restart.teardown();
    m_restart.rebuild();

    m_restart.land_home();
    m_connected_printer_id = printer_id;

    // Show toast with the new printer name
    const std::string printer_name = m_config->get_active_printer_name();
    std::string toast_msg = fmt::format(fmt::runtime(lv_tr("Switched to {}")), printer_name);
    ToastManager::instance().show(ToastSeverity::INFO, toast_msg.c_str());

    spdlog::info("[PrinterSwitchFlow] Switched to printer '{}'", printer_id);
}

void PrinterSwitchFlow::add_printer_via_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn(
            "[PrinterSwitchFlow] Ignoring add_printer_via_wizard during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

    const std::string new_id = m_config->next_printer_id();
    std::string previous_id = m_config->get_active_printer_id();

    // Create empty printer entry with wizard_completed=false so is_wizard_required()
    // returns true (without this, root-level wizard_completed fallback blocks the wizard)
    nlohmann::json printer_data = {{"wizard_completed", false}};
    m_config->add_printer(new_id, printer_data);
    m_config->set_active_printer(new_id);
    if (!save_or_report()) {
        m_config->remove_printer(new_id);
        m_config->set_active_printer(previous_id);
        return;
    }

    // Store previous ID so wizard cancellation can recover
    m_wizard_previous_printer_id = previous_id;

    spdlog::info("[PrinterSwitchFlow] Adding new printer '{}' via wizard (previous: '{}')", new_id,
                 previous_id);

    // Same active-printer change as switch_printer(): Config::df() has moved to the new
    // entry, so every per-printer cache must be dropped before teardown.
    PrinterCacheRegistry::instance().invalidate_all();

    // The rebuild runs the wizard itself when is_wizard_required() returns true (it does for
    // the new empty entry), so the wizard must not be launched again here.
    m_restart.teardown();

    // Registered after the teardown (which clears it) and before the rebuild (which runs the
    // wizard).
    set_wizard_cancel_callback([this]() { cancel_add_printer_wizard(); });

    m_restart.rebuild();
    m_connected_printer_id = new_id;
}

void PrinterSwitchFlow::cancel_add_printer_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn(
            "[PrinterSwitchFlow] Ignoring cancel_add_printer_wizard during active soft restart");
        return;
    }

    if (m_wizard_previous_printer_id.empty()) {
        spdlog::debug("[PrinterSwitchFlow] No add-printer recovery state — ignoring cancel");
        return;
    }

    std::string failed_id = m_config->get_active_printer_id();
    std::string restore_id = m_wizard_previous_printer_id;
    spdlog::info(
        "[PrinterSwitchFlow] Cancelling add-printer wizard — removing '{}', restoring '{}'",
        failed_id, restore_id);

    m_config->remove_printer(failed_id);
    m_config->set_active_printer(restore_id);
    // Unsaved, the abandoned entry reappears after a restart; the restore still runs.
    save_or_report();
    m_wizard_previous_printer_id.clear();

    // Defer wizard teardown + soft restart — we're called from a wizard button click handler,
    // so the wizard_container must survive until the event callback returns.
    m_async.defer("PrinterSwitchFlow::cancel_add_printer_wizard", [this]() {
        SoftRestartLatch soft_restart(m_soft_restart_in_progress);

        set_wizard_active(false);
        ui_wizard_deinit_subjects();

        // set_active_printer() above restored the previous printer, so Config::df() moved
        // again — drop every per-printer cache before teardown.
        PrinterCacheRegistry::instance().invalidate_all();

        m_restart.teardown();
        m_restart.rebuild();
        m_restart.land_home();
        m_connected_printer_id = m_config->get_active_printer_id();
    });
}

void PrinterSwitchFlow::add_printer(const std::string& host, int port) {
    const std::string existing = m_config->find_printer_by_host(host, port);
    if (!existing.empty()) {
        spdlog::info("[PrinterSwitchFlow] {}:{} is already printer '{}'", host, port, existing);
        request_switch(existing);
        return;
    }

    const std::string id = m_config->next_printer_id();
    m_config->add_printer(id, {{"moonraker_host", host}, {"moonraker_port", port}});
    spdlog::info("[PrinterSwitchFlow] Added printer '{}' at {}:{}", id, host, port);
    // Kept in the list unsaved rather than dropped, and not switched to.
    if (!save_or_report()) {
        return;
    }
    request_switch(id);
}

bool PrinterSwitchFlow::save_or_report() {
    if (m_config->save()) {
        return true;
    }
    spdlog::error("[PrinterSwitchFlow] Saving the printer list failed");
    ToastManager::instance().show(ToastSeverity::ERROR,
                                  lv_tr("Failed to save printer configuration"));
    return false;
}

} // namespace helix
