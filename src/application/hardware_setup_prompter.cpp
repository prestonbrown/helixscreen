// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file hardware_setup_prompter.cpp
 * @brief The prompts a discovery pass can raise about hardware: reconfig wizard,
 *        deferred-setup offer, printer-type mismatch warning.
 */

#include "hardware_setup_prompter.h"

#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_panel_notification_history.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "hardware_role_registry.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "wizard_config_paths.h"
#include "wizard_step_logic.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdint>

using namespace helix;

namespace {

/// The warning toast's button. user_data carries the warning's generation, so the
/// action dispatched is that warning's own, exactly as its history entry's is.
void review_toast_clicked(void* user_data) {
    const auto generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user_data));
    const std::string action = fmt::format("{}:{}", kReviewPrinterTypeAction, generation);
    if (auto review = helix::ui::notification_action_handler(action)) {
        review();
    }
}

/// The warning toast's X button. Closing it is a decision — keep the saved
/// type — so it dispatches through the same action registry as the review
/// button and inherits its lifetime and staleness guarantees.
void mismatch_toast_closed(void* user_data) {
    const auto generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user_data));
    const std::string action = fmt::format("{}:{}", kMismatchToastClosedAction, generation);
    if (auto settle = helix::ui::notification_action_handler(action)) {
        settle();
    }
}

/// Both toast actions carry "<key>:<generation>"; a bad parse has nothing to
/// dispatch on.
bool parse_action_generation(const std::string& arg, uint32_t& gen) {
    const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), gen);
    return parsed.ec == std::errc{} && parsed.ptr == arg.data() + arg.size();
}

/// PRINTER_TYPE and TYPE_MISMATCH_SHOWN_FOR hold display names, which a
/// database rename orphans; every comparison reads them through the alias list.
std::string canonical_saved_type() {
    auto* cfg = Config::get_instance();
    return PrinterDetector::canonical_type_name(
        cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, ""));
}

std::string canonical_shown_for() {
    auto* cfg = Config::get_instance();
    return PrinterDetector::canonical_type_name(
        cfg->get<std::string>(cfg->df() + helix::wizard::TYPE_MISMATCH_SHOWN_FOR, ""));
}

void* toast_generation(uint32_t generation) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
}

// The format strings are translated handles, not compile-time literals, hence
// fmt::runtime. Two entries for one machine are worded as the firmware mod or
// add-on that separates them: naming the same printer twice reads as nonsense.

std::string type_mismatch_toast_text(const std::string& saved, const std::string& detected) {
    using Relation = PrinterDetector::TypeRelation;
    const auto rel = PrinterDetector::relate_types(saved, detected);
    switch (rel.relation) {
    case Relation::DetectedVariant:
        return fmt::format(
            fmt::runtime(lv_tr("Your {0} looks like it has {1}, but it is set up as stock.")),
            rel.base, rel.detected_label);
    case Relation::SavedVariant:
        return fmt::format(fmt::runtime(lv_tr("Your {0} is set up for {1}, but it looks stock.")),
                           rel.base, rel.saved_label);
    case Relation::SiblingVariants:
        return fmt::format(
            fmt::runtime(lv_tr("Your {0} looks like it has {1}, but it is set up for {2}.")),
            rel.base, rel.detected_label, rel.saved_label);
    case Relation::DifferentModel:
        break;
    }
    return fmt::format(fmt::runtime(lv_tr("This looks like a {}, but it is set up as a {}")),
                       detected, saved);
}

std::string type_mismatch_review_text(const std::string& saved, const std::string& detected,
                                      int confidence) {
    using Relation = PrinterDetector::TypeRelation;
    const auto rel = PrinterDetector::relate_types(saved, detected);
    switch (rel.relation) {
    case Relation::DetectedVariant:
        return fmt::format(
            fmt::runtime(lv_tr("This {0} looks like it has {1} ({2}% confidence), but it is set "
                               "up as stock. A wrong profile applies incorrect pre-print options "
                               "and presets.")),
            rel.base, rel.detected_label, confidence);
    case Relation::SavedVariant:
        return fmt::format(
            fmt::runtime(lv_tr("This {0} is set up for {1}, but it looks stock ({2}% "
                               "confidence). A wrong profile applies incorrect pre-print options "
                               "and presets.")),
            rel.base, rel.saved_label, confidence);
    case Relation::SiblingVariants:
        return fmt::format(
            fmt::runtime(lv_tr("This {0} looks like it has {1} ({2}% confidence), but it is set "
                               "up for {3}. A wrong profile applies incorrect pre-print options "
                               "and presets.")),
            rel.base, rel.detected_label, confidence, rel.saved_label);
    case Relation::DifferentModel:
        break;
    }
    return fmt::format(
        fmt::runtime(lv_tr("This printer looks like a {} ({}% confidence), but it is "
                           "set up as a {}. A wrong type applies incorrect pre-print "
                           "options and presets.")),
        detected, confidence, saved);
}

} // namespace

namespace helix {

HardwarePrompt choose_prompt(const HardwarePromptGates& g,
                             const std::function<bool()>& deferred_steps_empty) {
    // Every prompt waits for a changed hardware shape, an idle printer and no first-run
    // wizard: a wizard over a live print or a second wizard is never right.
    if (!g.hw_changed || g.print_active || g.wizard_blocked) {
        return HardwarePrompt::None;
    }
    // The three reasons are mutually exclusive, in this precedence, so one pass never
    // stacks two dialogs.
    if (g.reconfig_steps_pending) {
        return g.reconfig_shown ? HardwarePrompt::None : HardwarePrompt::Reconfig;
    }
    if (g.hardware_setup_deferred) {
        if (g.deferred_prompt_shown) {
            return HardwarePrompt::None;
        }
        return deferred_steps_empty() ? HardwarePrompt::DeferredSilent
                                      : HardwarePrompt::DeferredOffer;
    }
    return g.type_mismatch_shown ? HardwarePrompt::None : HardwarePrompt::TypeMismatch;
}

HardwareSetupPrompter::~HardwareSetupPrompter() {
    cancel_launch_timer();
    // The handlers hold `this`; a toast or history row outliving the prompter then
    // finds nothing to call.
    if (review_handler_registered_) {
        helix::ui::set_notification_action_handler(kReviewPrinterTypeAction, nullptr);
        helix::ui::set_notification_action_handler(kMismatchToastClosedAction, nullptr);
    }
}

void HardwareSetupPrompter::cancel_launch_timer() {
    helix::ui::lv_timer_cancel_safe(launch_timer_);
    launch_timer_ = nullptr;
    pending_launch_ = nullptr;
}

void HardwareSetupPrompter::launch_after_modal_exit(void (HardwareSetupPrompter::*launch)()) {
    // Build the wizard AFTER the modal's exit animation, not inside the click that
    // started it: the dialog's own close only marks the backdrop exiting, so creating
    // the full-screen wizard here would put it underneath a still-fading backdrop.
    cancel_launch_timer();
    pending_launch_ = launch;
    launch_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<HardwareSetupPrompter*>(lv_timer_get_user_data(t));
            auto launch = self->pending_launch_;
            self->launch_timer_ = nullptr;
            self->pending_launch_ = nullptr;
            lv_timer_delete(t);
            if (launch) {
                (self->*launch)();
            }
        },
        300, this);
    lv_timer_set_repeat_count(launch_timer_, 1);
}

void HardwareSetupPrompter::reset_for_new_connection() {
    guards.reconfig_shown = false;
    guards.deferred_prompt_shown = false;
    guards.type_mismatch_shown = false;
    pending_steps_.clear();
    // A review captured for the previous printer must not open against the next one.
    pending_review_.reset();
    ++review_generation_;
    // A soft restart clears the modal stack without reporting a dismissal.
    review_open_ = false;
}

HardwarePrompt HardwareSetupPrompter::run_discovery_prompts(const PrinterDiscovery& hw,
                                                            bool hw_changed, bool print_active,
                                                            bool hardware_setup_deferred) {
    // Route unresolved GUIDED hardware roles (a saved fan/heater role with no
    // confident live substitute) into the targeted reconfig wizard, only the
    // affected step(s). Skipped while the first-run wizard is required/active.
    // Unresolved steps are purely a function of (saved config, current hardware), so
    // an unchanged shape would only harass the user again.
    auto reconfig_steps = helix::unresolved_guided_steps(Config::get_instance(), hw);

    HardwarePromptGates gates;
    gates.hw_changed = hw_changed;
    gates.print_active = print_active;
    gates.wizard_blocked = Config::get_instance()->is_wizard_required() || is_wizard_active();
    gates.reconfig_steps_pending = !reconfig_steps.empty();
    gates.hardware_setup_deferred = hardware_setup_deferred;
    gates.reconfig_shown = guards.reconfig_shown;
    gates.deferred_prompt_shown = guards.deferred_prompt_shown;
    gates.type_mismatch_shown = guards.type_mismatch_shown;

    std::vector<wizard::StepId> deferred_steps;
    const HardwarePrompt prompt = choose_prompt(gates, [&deferred_steps] {
        deferred_steps = ui_wizard_deferred_hardware_steps();
        return deferred_steps.empty();
    });

    switch (prompt) {
    case HardwarePrompt::None:
        break;
    case HardwarePrompt::Reconfig:
        guards.reconfig_shown = true;
        launch_reconfig_wizard(std::move(reconfig_steps));
        break;
    case HardwarePrompt::DeferredOffer:
        guards.deferred_prompt_shown = true;
        prompt_deferred_hardware_setup(std::move(deferred_steps));
        break;
    case HardwarePrompt::DeferredSilent:
        // Nothing left to ask about (a preset already answers these, or the printer
        // has none of the optional hardware). Settle the debt silently rather than
        // showing a dead-end dialog.
        guards.deferred_prompt_shown = true;
        spdlog::info("[HardwareSetup] Deferred hardware setup has no steps to offer; "
                     "settling silently");
        settle_deferred_hardware_setup();
        break;
    case HardwarePrompt::TypeMismatch:
        // Saved printer type vs detected hardware: one actionable prompt per saved
        // type. maybe_warn_type_mismatch() applies its own further gates.
        maybe_warn_type_mismatch(hw);
        break;
    }
    return prompt;
}

void HardwareSetupPrompter::launch_reconfig_wizard(std::vector<wizard::StepId> steps) {
    IMoonrakerAPI* api = api_();
    if (!api) {
        return;
    }
    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();
    ui_wizard_init_subjects();
    // Ending the session — Cancel here, Finish in the on_complete callback
    // below — settles every guided role the session's steps could not
    // resolve: a step only offers controls for the roles it shows (the fan
    // step has no aux dropdown), so a preset-saved role with no live match
    // can never be satisfied inside the session and would relaunch the
    // wizard on every boot. settle_targeted_reconfig() writes "" (declined)
    // for each such role, read against the CURRENT discovered hardware.
    set_wizard_cancel_callback([api]() {
        // reapply_hardware_roles() is intentionally NOT called here:
        // ui_wizard_complete_targeted() fires the on_complete callback
        // (registered in ui_wizard_create_targeted below), which already
        // reapplies the roles. Calling it here too would be a redundant
        // double reapply.
        helix::settle_targeted_reconfig(Config::get_instance(), api->hardware());
        ui_wizard_complete_targeted();
        set_wizard_cancel_callback(nullptr);
    });
    ui_wizard_create_targeted(screen_(), std::move(steps), [this, api]() {
        set_wizard_cancel_callback(nullptr);
        helix::settle_targeted_reconfig(Config::get_instance(), api->hardware());
        reapply_hardware_roles();
    });
}

void HardwareSetupPrompter::reapply_hardware_roles() {
    lifetime_.defer("HardwareSetupPrompter::reapply_hardware_roles", [this]() {
        IMoonrakerAPI* api = api_();
        if (!api) {
            return;
        }
        const auto& fans = api->hardware().fans();
        const auto& heaters = api->hardware().heaters();
        // Re-resolve + persist fan roles, then rebind fan UI to the new mapping.
        // apply_roles, not init_fans: the wizard changed which fan plays which
        // role, not which fans exist, so the discovered list comes from what
        // discovery actually stored rather than being re-passed from here.
        auto roles = helix::FanRoleConfig::from_config(Config::get_instance(), fans);
        get_printer_state().fan_state().apply_roles(roles);
        // Heater roles persist back to config (no dedicated runtime fan-style consumer).
        helix::resolve_role_from_config(helix::HardwareRoleId::HotendHeater, Config::get_instance(),
                                        heaters, /*persist_autoheal=*/true);
        helix::resolve_role_from_config(helix::HardwareRoleId::BedHeater, Config::get_instance(),
                                        heaters, /*persist_autoheal=*/true);
    });
}

void HardwareSetupPrompter::settle_deferred_hardware_setup() {
    Config* config = Config::get_instance();
    if (!helix::wizard_clear_hardware_setup_deferred(config)) {
        return;
    }
    if (!config->save()) {
        spdlog::warn("[HardwareSetup] Failed to persist deferred hardware setup decision");
    }
}

void HardwareSetupPrompter::launch_deferred_hardware_setup() {
    auto steps = std::move(pending_steps_);
    pending_steps_.clear();
    if (steps.empty()) {
        return;
    }
    spdlog::info("[HardwareSetup] Launching deferred hardware setup ({} step(s))", steps.size());

    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();
    ui_wizard_init_subjects();
    // Back on the first targeted step has nothing to retreat to, so give it the
    // same dismiss semantics as the reconfig wizard.
    set_wizard_cancel_callback([]() {
        ui_wizard_complete_targeted();
        set_wizard_cancel_callback(nullptr);
    });
    HardwareSetupPrompter* app = this;
    ui_wizard_create_targeted(screen_(), std::move(steps), [app]() {
        set_wizard_cancel_callback(nullptr);
        // ui_wizard_complete_targeted() deliberately skips the expected-hardware
        // population, so record the user's fresh picks here.
        ui_wizard_record_expected_hardware(Config::get_instance());
        app->reapply_hardware_roles();
    });
}

void HardwareSetupPrompter::prompt_deferred_hardware_setup(
    std::vector<helix::wizard::StepId> steps) {
    // The steps to run are parked on the prompter for the confirm
    // callback's timer to consume (launch_deferred_hardware_setup() reads them
    // from there). on_dismiss clears them too, so a backdrop tap or ESC cannot
    // strand the offer.
    pending_steps_ = std::move(steps);
    spdlog::info("[HardwareSetup] Offering deferred hardware setup ({} step(s))",
                 pending_steps_.size());

    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [this] {
        pending_steps_.clear();
        // Declining is final for this printer. The offer is for optional
        // role assignments the app already has working defaults for, the
        // snapshot was written regardless so nothing is flagged either way,
        // and re-asking on every boot is the exact nag the surrounding
        // reconfig-wizard code records declines to avoid. `--wizard` still
        // re-runs setup, and a saved role that later breaks still routes to
        // the targeted reconfig wizard on its own.
        settle_deferred_hardware_setup();
        spdlog::info("[HardwareSetup] Deferred hardware setup declined");
    };
    opts.cancel_text = lv_tr("Not now");
    opts.on_dismiss = [this] { pending_steps_.clear(); };
    opts.owner_token = lifetime_.token();

    helix::ui::modal_confirm(
        lv_tr("Printer hardware detected"),
        lv_tr("Your printer was offline during setup, so hardware options were skipped. "
              "Set them up now?"),
        ModalSeverity::Info, lv_tr("Set up"),
        [this] {
            // Settle first: the wizard tears itself down asynchronously, and a
            // crash mid-run must not leave the offer pending forever.
            settle_deferred_hardware_setup();
            // The pending step list lives on the prompter until the launch consumes it.
            launch_after_modal_exit(&HardwareSetupPrompter::launch_deferred_hardware_setup);
        },
        opts);
}

void HardwareSetupPrompter::settle_type_mismatch_warning() {
    pending_review_.reset();
    review_open_ = false;
    // Answered: the sticky toast has nothing left to ask.
    ToastManager::instance().hide(review_toast_clicked, toast_generation(review_generation_));
    auto* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::TYPE_MISMATCH_SHOWN_FOR,
                          cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, ""));
    if (!cfg->save()) {
        spdlog::warn("[HardwareSetup] Failed to persist type mismatch decision");
    }
}

void HardwareSetupPrompter::maybe_warn_type_mismatch(const helix::PrinterDiscovery& hardware) {
    // The gates below all decline silently in normal operation. A debug bundle
    // is the only view we get of a reporter's run, so each one says why it
    // declined: without that there is no way to tell a 68%-confidence near-miss
    // apart from detection returning nothing at all (bundle TZT85MQ3).
    if (guards.type_mismatch_shown) {
        spdlog::debug("[HardwareSetup] Type mismatch check skipped: already prompted this session");
        return;
    }
    if (get_runtime_config()->should_mock_moonraker()) {
        // A mock printer's identity is whatever the persona declares, so a
        // mismatch against the saved type says nothing about real hardware.
        // Gate on the runtime-config predicate, not on HELIX_MOCK_PRINTER:
        // plain --test runs the mock client without that env var ever being
        // set, so the old getenv check let every --test launch open the
        // prompt against whatever type settings-test.json happened to carry.
        spdlog::debug("[HardwareSetup] Type mismatch check skipped: mock printer");
        return;
    }
    if (Config::get_instance()->is_wizard_required() || is_wizard_active()) {
        spdlog::debug("[HardwareSetup] Type mismatch check skipped: wizard required or active");
        return;
    }

    auto* cfg = Config::get_instance();
    const std::string stored = cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");

    // The saved type is a display name, so an entry renamed in the printer
    // database orphans every config written under the old one and detection
    // then contradicts a type that was never wrong. Resolve through the
    // database's alias list and heal the stored value, otherwise the stale
    // name keeps missing every other name-keyed lookup too (image, preset,
    // pre-print profile) long after this prompt is dismissed.
    const std::string saved = PrinterDetector::canonical_type_name(stored);
    if (saved != stored) {
        cfg->set<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, saved);
        if (!cfg->save()) {
            spdlog::warn("[HardwareSetup] Failed to persist renamed printer type '{}' -> '{}'",
                         stored, saved);
        }
    }

    // Canonicalised too: a dismissal recorded under the pre-rename name still
    // answers for the same printer.
    const std::string flag = canonical_shown_for();

    auto detected = PrinterDetector::auto_detect(hardware);
    const auto decision = PrinterDetector::classify_type_mismatch(saved, detected, flag);
    if (decision != PrinterDetector::MismatchDecision::Warn) {
        // info, not debug: this runs once per discovery pass, and it is the line
        // that answers "why was there no prompt?" in a bundle.
        spdlog::info(
            "[HardwareSetup] No type mismatch prompt: detected '{}' at {}% (runner-up '{}' "
            "at {}%, margin {}, {} tied), saved '{}', dismissed-for '{}', need >={}% and "
            "margin >={} - {}",
            detected.type_name, detected.confidence, detected.runner_up_type_name,
            detected.runner_up_confidence, detected.margin(), detected.tied_count, saved, flag,
            PrinterDetector::kMismatchMinConfidence, PrinterDetector::kDetectMinMargin,
            PrinterDetector::mismatch_decision_name(decision));
        return;
    }

    // Session guard: one warning per boot. The toast and history entry stay until the
    // user answers the review; only an answer persists TYPE_MISMATCH_SHOWN_FOR.
    guards.type_mismatch_shown = true;
    spdlog::info("[HardwareSetup] Printer type mismatch: saved '{}' but detected '{}' ({}%)", saved,
                 detected.type_name, detected.confidence);
    warn_type_mismatch({detected.type_name, detected.confidence, saved});
}

void HardwareSetupPrompter::warn_type_mismatch(TypeMismatchReview review) {
    const std::string message = type_mismatch_toast_text(review.saved, review.detected);
    pending_review_ = std::move(review);
    const uint32_t generation = ++review_generation_;

    // Toast and history row both reach the review through this one registration, so
    // neither holds the prompter: the destructor removes it. Each carries its own
    // generation, which the handler checks.
    helix::ui::set_notification_action_handler(
        kReviewPrinterTypeAction, [this](const std::string& arg) {
            uint32_t gen = 0;
            if (!parse_action_generation(arg, gen)) {
                spdlog::info("[HardwareSetup] Type mismatch review skipped: bad action '{}'", arg);
                return;
            }
            show_type_mismatch_review(gen);
        });
    helix::ui::set_notification_action_handler(
        kMismatchToastClosedAction, [this](const std::string& arg) {
            uint32_t gen = 0;
            if (!parse_action_generation(arg, gen)) {
                spdlog::info("[HardwareSetup] Type mismatch settle skipped: bad action '{}'", arg);
                return;
            }
            settle_type_mismatch_on_close(gen);
        });
    review_handler_registered_ = true;

    const std::string action = fmt::format("{}:{}", kReviewPrinterTypeAction, generation);
    ui_notification_warning_with_action(lv_tr("Printer type mismatch"), message.c_str(),
                                        action.c_str());
    // Sticky: a wrong type applies wrong pre-print options to every print, so the
    // warning stays until it is answered or closed; either way settles it.
    ToastManager::instance().show_with_action(
        ToastSeverity::WARNING, message.c_str(), lv_tr("Review"), review_toast_clicked,
        toast_generation(generation), 0, mismatch_toast_closed);
}

bool HardwareSetupPrompter::pending_review_current(uint32_t generation, const char* what) {
    if (!pending_review_ || generation != review_generation_) {
        spdlog::info("[HardwareSetup] Type mismatch {} skipped: warning {} is not the pending "
                     "one ({}{})",
                     what, generation, review_generation_, pending_review_ ? "" : ", none pending");
        return false;
    }
    // A warning about a type the user has since replaced, or one already
    // answered, is spent: it settles nothing and stays disarmed for the new type.
    const std::string saved = canonical_saved_type();
    const std::string flag = canonical_shown_for();
    if (saved != pending_review_->saved || flag == saved) {
        spdlog::info("[HardwareSetup] Type mismatch {} skipped: warned for '{}', saved type is "
                     "now '{}', dismissed-for '{}'",
                     what, pending_review_->saved, saved, flag);
        pending_review_.reset();
        return false;
    }
    return true;
}

void HardwareSetupPrompter::settle_type_mismatch_on_close(uint32_t generation) {
    if (!pending_review_current(generation, "settle")) {
        return;
    }
    spdlog::info("[HardwareSetup] Type mismatch toast closed; keeping saved type '{}'",
                 pending_review_->saved);
    settle_type_mismatch_warning();
}

void HardwareSetupPrompter::show_type_mismatch_review(uint32_t generation) {
    if (pending_review_current(generation, "review")) {
        open_type_mismatch_review();
    }
}

void HardwareSetupPrompter::show_type_mismatch_demo(const std::string& detected, int confidence,
                                                    const std::string& saved, bool open_review) {
    warn_type_mismatch({detected, confidence, saved});
    if (open_review) {
        open_type_mismatch_review();
    }
}

void HardwareSetupPrompter::open_type_mismatch_review() {
    // The toast and its history row both open this; a second tap must not stack a
    // second copy over the first.
    if (!pending_review_ || review_open_) {
        spdlog::debug("[HardwareSetup] Type mismatch review already open or none pending");
        return;
    }
    const TypeMismatchReview& review = *pending_review_;

    // modal_confirm takes a plain const char*, so the body is composed first.
    const std::string body =
        type_mismatch_review_text(review.saved, review.detected, review.confidence);

    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [this] {
        // Declining is final for this saved type. Keeping the type is a
        // deliberate choice (a heavily modified printer can legitimately
        // outvote a heuristic), and the persisted flag stops the
        // prompt from re-appearing every boot. The model picker remains
        // available from Printer Manager and the full `--wizard` run.
        settle_type_mismatch_warning();
        spdlog::info("[HardwareSetup] Type mismatch warning declined");
    };
    // Not an answer: the review stays reachable, so the next tap must open it again.
    opts.on_dismiss = [this] { review_open_ = false; };
    opts.cancel_text = lv_tr("Keep current");
    opts.owner_token = lifetime_.token();
    // A backdrop tap or ESC settles nothing: the review stays reachable from history
    // and the warning returns next boot. Only a button settles it, and an accidental
    // tap must not permanently silence a wrong-printer warning.

    review_open_ =
        helix::ui::modal_confirm(
            lv_tr("Printer type mismatch"), body.c_str(), ModalSeverity::Warning,
            lv_tr("Choose Model"),
            [this] {
                // Settle first: the wizard tears itself down asynchronously, and a
                // crash mid-run must not leave the prompt pending forever.
                settle_type_mismatch_warning();
                launch_after_modal_exit(&HardwareSetupPrompter::launch_type_reidentify_wizard);
            },
            opts) != nullptr;
}

void HardwareSetupPrompter::launch_type_reidentify_wizard() {
    spdlog::info("[HardwareSetup] Launching printer re-identify wizard");
    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();
    ui_wizard_init_subjects();
    // Back on the first targeted step has nothing to retreat to, so give it the
    // same dismiss semantics as the deferred hardware-setup session.
    set_wizard_cancel_callback([]() {
        ui_wizard_complete_targeted();
        set_wizard_cancel_callback(nullptr);
    });
    HardwareSetupPrompter* app = this;
    ui_wizard_create_targeted(screen_(), {helix::wizard::StepId::PrinterIdentify}, [app]() {
        set_wizard_cancel_callback(nullptr);
        // The identify step's cleanup already persisted PRINTER_TYPE and applied
        // the new preset (ui_wizard_printer_identify.cpp cleanup). The preset
        // rewrote fan/heater role keys, so rebind the runtime mappings the same
        // way the deferred hardware-setup session does.
        app->reapply_hardware_roles();
    });
}

} // namespace helix
