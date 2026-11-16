// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_setup_prompter.cpp
 * @brief choose_prompt(): which hardware prompt a discovery pass raises.
 *
 * The decision was three independent boolean conditions, each repeating the same
 * three gates. The table below spells those conditions out as the specification and
 * walks every combination of inputs, so the enum cannot disagree with them on any
 * input and no input can raise two prompts.
 *
 * The type mismatch cases cover what a Warn decision raises: a sticky toast and a
 * history entry, both leading to one review modal whose buttons alone settle it.
 */

#include "ui_modal.h"
#include "ui_panel_notification_history.h"
#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/armed_timers.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/hardware_setup_prompter_test_access.h"
#include "../test_helpers/notification_history_panel_test_access.h"
#include "../test_helpers/scoped_config_write_counter.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "hardware_setup_prompter.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "wizard_config_paths.h"

#include <memory>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::choose_prompt;
using helix::HardwarePrompt;
using helix::HardwarePromptGates;

namespace {

/// The three conditions as the discovery pass wrote them out, one per prompt.
struct Spec {
    bool reconfig;
    bool deferred;
    bool mismatch;
};

Spec spec_for(const HardwarePromptGates& g, bool deferred_steps_empty) {
    (void)deferred_steps_empty;
    return {
        g.hw_changed && g.reconfig_steps_pending && !g.print_active && !g.wizard_blocked &&
            !g.reconfig_shown,
        g.hardware_setup_deferred && g.hw_changed && !g.print_active && !g.wizard_blocked &&
            !g.reconfig_steps_pending && !g.deferred_prompt_shown,
        g.hw_changed && !g.print_active && !g.reconfig_steps_pending &&
            !g.hardware_setup_deferred && !g.wizard_blocked && !g.type_mismatch_shown,
    };
}

HardwarePromptGates gates_from_bits(unsigned bits) {
    HardwarePromptGates g;
    g.hw_changed = bits & 1u;
    g.print_active = bits & 2u;
    g.wizard_blocked = bits & 4u;
    g.reconfig_steps_pending = bits & 8u;
    g.hardware_setup_deferred = bits & 16u;
    g.reconfig_shown = bits & 32u;
    g.deferred_prompt_shown = bits & 64u;
    g.type_mismatch_shown = bits & 128u;
    return g;
}

} // namespace

TEST_CASE("choose_prompt agrees with the three gate conditions on every input",
          "[hardware_prompt][discovery]") {
    for (unsigned bits = 0; bits < 256; ++bits) {
        for (bool steps_empty : {false, true}) {
            const HardwarePromptGates g = gates_from_bits(bits);
            const Spec spec = spec_for(g, steps_empty);
            const HardwarePrompt got = choose_prompt(g, [steps_empty] { return steps_empty; });
            CAPTURE(bits, steps_empty);

            // Mutually exclusive: the encoding as one enum loses nothing.
            CHECK(static_cast<int>(spec.reconfig) + static_cast<int>(spec.deferred) +
                      static_cast<int>(spec.mismatch) <=
                  1);

            if (spec.reconfig) {
                CHECK(got == HardwarePrompt::Reconfig);
            } else if (spec.deferred) {
                CHECK(got == (steps_empty ? HardwarePrompt::DeferredSilent
                                          : HardwarePrompt::DeferredOffer));
            } else if (spec.mismatch) {
                CHECK(got == HardwarePrompt::TypeMismatch);
            } else {
                CHECK(got == HardwarePrompt::None);
            }
        }
    }
}

TEST_CASE("choose_prompt asks for the deferred steps only when an offer is due",
          "[hardware_prompt][discovery]") {
    int asked = 0;
    auto counting = [&asked] {
        ++asked;
        return false;
    };

    HardwarePromptGates due;
    due.hw_changed = true;
    due.hardware_setup_deferred = true;
    CHECK(choose_prompt(due, counting) == HardwarePrompt::DeferredOffer);
    CHECK(asked == 1);

    // Computing the steps touches filament-sensor discovery, so none of these may.
    asked = 0;
    HardwarePromptGates g = due;
    g.print_active = true;
    CHECK(choose_prompt(g, counting) == HardwarePrompt::None);
    g = due;
    g.wizard_blocked = true;
    CHECK(choose_prompt(g, counting) == HardwarePrompt::None);
    g = due;
    g.hw_changed = false;
    CHECK(choose_prompt(g, counting) == HardwarePrompt::None);
    g = due;
    g.reconfig_steps_pending = true;
    CHECK(choose_prompt(g, counting) == HardwarePrompt::Reconfig);
    g = due;
    g.deferred_prompt_shown = true;
    CHECK(choose_prompt(g, counting) == HardwarePrompt::None);
    CHECK(asked == 0);
}

TEST_CASE("choose_prompt: a type mismatch never stacks on a deferred debt",
          "[hardware_prompt][discovery]") {
    HardwarePromptGates g;
    g.hw_changed = true;
    CHECK(choose_prompt(g, [] { return true; }) == HardwarePrompt::TypeMismatch);

    g.hardware_setup_deferred = true;
    CHECK(choose_prompt(g, [] { return false; }) == HardwarePrompt::DeferredOffer);

    // The debt answered earlier this session: the mismatch still waits for the next boot.
    g.deferred_prompt_shown = true;
    CHECK(choose_prompt(g, [] { return false; }) == HardwarePrompt::None);
}

// ============================================================================
// Type mismatch: toast + history entry, then the review modal
// ============================================================================

namespace {

using helix::HardwareSetupPrompter;
using helix::HardwareSetupPrompterTestAccess;
using helix::kReviewPrinterTypeAction;

constexpr const char* SAVED = "Voron 2.4";
constexpr const char* DETECTED = "Creality K1";

struct ToastSeen {
    ToastSeverity severity;
    std::string message;
    uint32_t duration_ms;
};

struct HistorySeen {
    ToastSeverity severity;
    std::string message;
    std::string action;
};

class TypeMismatchFixture : public LVGLUITestFixture {
  public:
    TypeMismatchFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        set_config(helix::wizard::PRINTER_TYPE, SAVED);
        set_config(helix::wizard::TYPE_MISMATCH_SHOWN_FOR, "");
        set_config(helix::wizard::kTypeMismatchShownForDetected, "");

        helix::ui::set_test_toast_hook([this](ToastSeverity sev, const std::string& msg,
                                              uint32_t ms) { toast_ = ToastSeen{sev, msg, ms}; });
        helix::ui::set_test_notification_action_hook(
            [this](ToastSeverity sev, const std::string& msg, const std::string& action) {
                history_ = HistorySeen{sev, msg, action};
            });

        prompter_ = std::make_unique<HardwareSetupPrompter>(
            async_, [this] { return test_screen(); }, [] { return nullptr; });
    }

    ~TypeMismatchFixture() override {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            drain();
        }
        prompter_.reset();
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::set_test_notification_action_hook(nullptr);
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    void set_config(const char* key, const std::string& value) {
        cfg_->set<std::string>(cfg_->df() + key, value);
    }
    std::string dismissed_for() const {
        return cfg_->get<std::string>(cfg_->df() + helix::wizard::TYPE_MISMATCH_SHOWN_FOR, "");
    }
    std::string dismissed_for_detected() const {
        return cfg_->get<std::string>(cfg_->df() + helix::wizard::kTypeMismatchShownForDetected,
                                      "");
    }

    void warn() {
        HardwareSetupPrompterTestAccess::warn_type_mismatch(*prompter_, DETECTED, 92, SAVED);
        drain();
    }

    /// Tap the history entry the warning added, through the panel's dispatch.
    void tap_history_entry() {
        REQUIRE(history_.has_value());
        NotificationHistoryPanel panel(get_printer_state(), nullptr);
        NotificationHistoryPanelTestAccess::dispatch_action(panel, history_->action.c_str());
        drain();
    }

    static void click(lv_obj_t* dialog, const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(dialog, name);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    helix::Config* cfg_ = nullptr;
    helix::AsyncLifetimeGuard async_;
    std::unique_ptr<HardwareSetupPrompter> prompter_;
    std::optional<ToastSeen> toast_;
    std::optional<HistorySeen> history_;
    ScopedConfigWriteCounter writes_;

  private:
    nlohmann::json saved_data_;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a Warn raises a sticky toast and a history entry, not a modal",
                 "[type-mismatch][hardware_setup]") {
    warn();

    CHECK(Modal::get_top() == nullptr);

    REQUIRE(toast_.has_value());
    CHECK(toast_->severity == ToastSeverity::WARNING);
    CHECK(toast_->duration_ms == 0);
    CHECK(toast_->message.find(DETECTED) != std::string::npos);
    CHECK(toast_->message.find(SAVED) != std::string::npos);

    REQUIRE(history_.has_value());
    CHECK(history_->severity == ToastSeverity::WARNING);
    CHECK(history_->action.rfind(std::string(kReviewPrinterTypeAction) + ":", 0) == 0);
    CHECK(history_->message == toast_->message);

    // Warning is not answering: nothing is settled yet.
    CHECK(dismissed_for().empty());
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: an ambiguous detection warns about the saved machine's contender",
                 "[type-mismatch][hardware_setup][mismatch_subject]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = false;
    cfg_->set<bool>(cfg_->df() + "wizard_completed", true);
    cfg_->set<bool>("/wizard_completed", true);

    // A K2 on k2-improvements that declares no bed ties the Plus and Pro mod entries.
    helix::PrinterDiscovery hw;
    hw.parse_objects(
        nlohmann::json::array({"extruder", "heater_bed", "motor_control", "fan_feedback", "load_ai",
                               "gcode_macro _START_PRINT_VARS", "gcode_macro START_PRINT"}));
    hw.set_hostname("k2plus");
    hw.set_kinematics("corexy");
    REQUIRE(PrinterDetector::auto_detect(hw).ambiguous());

    // Whichever entry detection ranks first, one of these two saved models is the other one.
    const bool saved_plus = GENERATE(false, true);
    const std::string saved = saved_plus ? "Creality K2 Plus" : "Creality K2 Pro";
    const std::string other_model = saved_plus ? "Creality K2 Pro" : "Creality K2 Plus";
    set_config(helix::wizard::PRINTER_TYPE, saved);

    REQUIRE(prompter_->run_discovery_prompts(hw, true, false, false) ==
            helix::HardwarePrompt::TypeMismatch);
    drain();

    REQUIRE(toast_.has_value());
    INFO(toast_->message);
    CHECK(toast_->message.find("k2-improvements") != std::string::npos);
    CHECK(toast_->message.find(saved) != std::string::npos);
    CHECK(toast_->message.find(other_model) == std::string::npos);
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: two entries for one machine are worded as the variant",
                 "[type-mismatch][hardware_setup]") {
    const std::string base = "Creality K2 Plus";
    const std::string variant = "Creality K2 Plus (k2-improvements)";
    const bool saved_is_variant = GENERATE(false, true);
    const std::string saved = saved_is_variant ? variant : base;
    const std::string detected = saved_is_variant ? base : variant;
    set_config(helix::wizard::PRINTER_TYPE, saved);
    HardwareSetupPrompterTestAccess::warn_type_mismatch(*prompter_, detected, 92, saved);
    drain();

    REQUIRE(toast_.has_value());
    INFO(toast_->message);
    CHECK(toast_->message.find("k2-improvements") != std::string::npos);
    CHECK(toast_->message.find(base) != std::string::npos);
    // The base is named once, not as both the detected and the saved printer.
    CHECK(toast_->message.find(variant) == std::string::npos);
    CHECK(toast_->message.find(base) == toast_->message.rfind(base));

    REQUIRE(helix::ui::fire_last_toast_action());
    drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    lv_obj_t* body = lv_obj_find_by_name(dialog, "dialog_message");
    REQUIRE(body != nullptr);
    const std::string text = lv_label_get_text(body);
    INFO(text);
    CHECK(text.find("k2-improvements") != std::string::npos);
    CHECK(text.find("92%") != std::string::npos);
    CHECK(text.find(variant) == std::string::npos);
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: Review on the toast opens the review; Keep current settles it",
                 "[type-mismatch][hardware_setup]") {
    warn();
    REQUIRE(helix::ui::fire_last_toast_action());
    drain();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(dismissed_for().empty());

    CHECK_FALSE(helix::ui::last_action_toast_dismissed());

    click(dialog, "btn_secondary");

    CHECK(Modal::get_top() == nullptr);
    CHECK(dismissed_for() == SAVED);
    // The answer is for this pair only (prestonbrown/helixscreen#2075).
    CHECK(dismissed_for_detected() == DETECTED);
    CHECK(writes_.writes() > 0);
    // Answered, so the sticky toast goes too.
    CHECK(helix::ui::last_action_toast_dismissed());
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: the history entry opens the same review; Choose Model settles it",
                 "[type-mismatch][hardware_setup]") {
    warn();
    tap_history_entry();

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(dismissed_for().empty());

    click(dialog, "btn_primary");

    CHECK(dismissed_for() == SAVED);
    CHECK(dismissed_for_detected() == DETECTED);
    CHECK(helix::ui::last_action_toast_dismissed());
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: dismissing the review another way leaves it armed",
                 "[type-mismatch][hardware_setup]") {
    warn();
    tap_history_entry();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    Modal::hide(dialog, ModalCloseReason::BackdropTap);
    drain();

    REQUIRE(Modal::get_top() == nullptr);
    CHECK(dismissed_for().empty());
    CHECK_FALSE(helix::ui::last_action_toast_dismissed());

    // Still reachable from either entry point: the next tap asks again.
    const bool from_toast = GENERATE(false, true);
    if (from_toast) {
        REQUIRE(helix::ui::fire_last_toast_action());
        drain();
    } else {
        tap_history_entry();
    }
    CHECK(Modal::get_top() != nullptr);
}

// Closing the toast is an answer too: the X means "keep my type, stop asking",
// and must persist across boots the way a Keep-current answer does
// (prestonbrown/helixscreen#1881).
TEST_CASE_METHOD(TypeMismatchFixture, "Type mismatch: closing the toast keeps the saved type",
                 "[type-mismatch][hardware_setup]") {
    warn();
    REQUIRE(toast_.has_value());
    REQUIRE(dismissed_for().empty());
    REQUIRE(helix::ui::last_shown_toast_has_closed_cb());

    // Drive the dispatch the X button routes through, with the warning's own
    // generation, parsed from the review action the same warning recorded.
    REQUIRE(history_.has_value());
    const size_t colon = history_->action.find(':');
    REQUIRE(colon != std::string::npos);
    const std::string closed = "type_mismatch_closed" + history_->action.substr(colon);
    NotificationHistoryPanel panel(get_printer_state(), nullptr);
    NotificationHistoryPanelTestAccess::dispatch_action(panel, closed.c_str());
    drain();

    CHECK(dismissed_for() == SAVED);
    CHECK(dismissed_for_detected() == DETECTED);
    CHECK(writes_.writes() > 0);
    CHECK(helix::ui::last_action_toast_dismissed());
}

// The real X on the real toast, not the handler by name: everything from
// warn_type_mismatch's show_with_action call to the persisted flag.
TEST_CASE_METHOD(TypeMismatchFixture, "Type mismatch: the toast's own X settles it",
                 "[type-mismatch][hardware_setup]") {
    auto& tm = ToastManager::instance();
    tm.init();
    warn();
    REQUIRE(toast_.has_value());
    REQUIRE(helix::ui::last_shown_toast_has_closed_cb());

    lv_obj_t* btn = lv_obj_find_by_name(lv_layer_top(), "toast_close_btn");
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    drain();

    CHECK(dismissed_for() == SAVED);
    CHECK_FALSE(tm.is_visible());

    tm.deinit_subjects();
    drain();
}

// The prompter unregisters its close handler; a toast outliving it closes to
// nothing, not into a destroyed owner.
TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a close after the prompter is gone settles nothing",
                 "[type-mismatch][hardware_setup]") {
    warn();
    REQUIRE(history_.has_value());
    prompter_.reset();
    drain();

    const size_t colon = history_->action.find(':');
    REQUIRE(colon != std::string::npos);
    const std::string closed = "type_mismatch_closed" + history_->action.substr(colon);
    // The handler went with the prompter; nothing dispatches into a dead owner.
    CHECK_FALSE(helix::ui::notification_action_handler(closed));
    NotificationHistoryPanel panel(get_printer_state(), nullptr);
    NotificationHistoryPanelTestAccess::dispatch_action(panel, closed.c_str());
    drain();

    CHECK(dismissed_for().empty());
}

// Only the warning's own close settles anything; a dispatch from a stale one
// leaves the flag unwritten.
TEST_CASE_METHOD(TypeMismatchFixture, "Type mismatch: a stale close settles nothing",
                 "[type-mismatch][hardware_setup]") {
    warn();
    REQUIRE(history_.has_value());
    const size_t colon = history_->action.find(':');
    REQUIRE(colon != std::string::npos);
    const std::string stale =
        fmt::format("type_mismatch_closed:{}", std::stoi(history_->action.substr(colon + 1)) + 1);
    NotificationHistoryPanel panel(get_printer_state(), nullptr);
    NotificationHistoryPanelTestAccess::dispatch_action(panel, stale.c_str());
    drain();

    // The dispatch plumbing itself writes (go_back); the settle must not have.
    CHECK(dismissed_for().empty());
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a second Review does not stack a second modal",
                 "[type-mismatch][hardware_setup]") {
    warn();
    tap_history_entry();
    lv_obj_t* first = Modal::get_top();
    REQUIRE(first != nullptr);

    REQUIRE(helix::ui::fire_last_toast_action());
    drain();

    CHECK(Modal::get_top() == first);
    Modal::hide(first);
    drain();
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(TypeMismatchFixture, "Type mismatch: a stale Review shows nothing",
                 "[type-mismatch][hardware_setup]") {
    warn();

    const int stale = GENERATE(0, 1, 2);
    if (stale == 0) {
        // The type was changed some other way since the warning.
        set_config(helix::wizard::PRINTER_TYPE, "Creality K1");
    } else if (stale == 1) {
        // Already answered for this saved and detected pair.
        set_config(helix::wizard::TYPE_MISMATCH_SHOWN_FOR, SAVED);
        set_config(helix::wizard::kTypeMismatchShownForDetected, DETECTED);
    } else {
        // Switched to another printer.
        prompter_->reset_for_new_connection();
    }

    tap_history_entry();
    CHECK(Modal::get_top() == nullptr);
    REQUIRE(helix::ui::fire_last_toast_action());
    drain();
    CHECK(Modal::get_top() == nullptr);
}

// A type chosen in Settings or Printer Manager spends the warning: the sticky
// toast must not stay up saying "set up as stock" over the type the user just
// picked (prestonbrown/helixscreen#2087).
TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a type changed elsewhere takes the warning's toast down",
                 "[type-mismatch][hardware_setup][2087]") {
    state().set_printer_type_sync(SAVED);
    drain();
    warn();
    REQUIRE(toast_.has_value());

    SECTION("re-applying the same type leaves it up") {
        state().set_printer_type_sync(SAVED);
        drain();
        CHECK_FALSE(helix::ui::last_action_toast_dismissed());
    }
    SECTION("a different saved type takes it down, and settles nothing") {
        set_config(helix::wizard::PRINTER_TYPE, DETECTED);
        state().set_printer_type_sync(DETECTED);
        drain();
        CHECK(helix::ui::last_action_toast_dismissed());
        CHECK(dismissed_for().empty());

        // The Review that was armed with it is spent as well.
        tap_history_entry();
        CHECK(Modal::get_top() == nullptr);
    }
}

// A printer switch rebuilds PrinterState's subjects. The warning raised on the
// next printer must still come down when its type is fixed elsewhere.
TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: after a printer switch a type change still takes the toast down",
                 "[type-mismatch][hardware_setup][2087]") {
    state().set_printer_type_sync(SAVED);
    drain();
    warn();
    REQUIRE(toast_.has_value());

    state().deinit_subjects();
    state().init_subjects(true);
    prompter_->reset_for_new_connection();
    state().set_printer_type_sync(SAVED);
    drain();

    toast_.reset();
    warn();
    REQUIRE(toast_.has_value());
    REQUIRE_FALSE(helix::ui::last_action_toast_dismissed());

    set_config(helix::wizard::PRINTER_TYPE, DETECTED);
    state().set_printer_type_sync(DETECTED);
    drain();
    CHECK(helix::ui::last_action_toast_dismissed());
}

// `ctl demo type-mismatch-*` names a saved type the config does not hold. Its
// toast must stay up for the screenshot it exists to take.
TEST_CASE_METHOD(TypeMismatchFixture, "Type mismatch: the demo keeps its own toast up",
                 "[type-mismatch][hardware_setup][2087]") {
    state().set_printer_type_sync(SAVED);
    drain();
    prompter_->show_type_mismatch_demo(DETECTED, 92, "Not The Configured Printer", false);
    drain();
    REQUIRE(toast_.has_value());
    CHECK_FALSE(helix::ui::last_action_toast_dismissed());
}

// An answer for the saved type against an earlier detection does not spend a
// warning about a different detected type (prestonbrown/helixscreen#2075).
TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: an answer for another detection leaves the Review live",
                 "[type-mismatch][hardware_setup][2075]") {
    set_config(helix::wizard::TYPE_MISMATCH_SHOWN_FOR, SAVED);
    set_config(helix::wizard::kTypeMismatchShownForDetected, "Prusa MK4");
    warn();

    tap_history_entry();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    click(dialog, "btn_secondary");
    CHECK(dismissed_for() == SAVED);
    CHECK(dismissed_for_detected() == DETECTED);
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a toast or entry outliving the prompter does nothing",
                 "[type-mismatch][hardware_setup]") {
    warn();
    prompter_.reset();

    CHECK_FALSE(helix::ui::notification_action_handler(kReviewPrinterTypeAction));
    REQUIRE(helix::ui::fire_last_toast_action());
    drain();
    tap_history_entry();
    CHECK(Modal::get_top() == nullptr);
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: after a printer switch the old warning's Review is a no-op",
                 "[type-mismatch][hardware_setup]") {
    warn();
    REQUIRE(history_.has_value());
    const std::string old_action = history_->action;

    // The next printer mismatches too, under a different saved type.
    prompter_->reset_for_new_connection();
    set_config(helix::wizard::PRINTER_TYPE, "Prusa MK4");
    HardwareSetupPrompterTestAccess::warn_type_mismatch(*prompter_, "Bambu X1", 90, "Prusa MK4");
    drain();
    REQUIRE(history_->action != old_action);

    // The first printer's entry opens nothing, not the second printer's review.
    NotificationHistoryPanel panel(get_printer_state(), nullptr);
    NotificationHistoryPanelTestAccess::dispatch_action(panel, old_action.c_str());
    drain();
    CHECK(Modal::get_top() == nullptr);

    // The second's toast and entry still work.
    REQUIRE(helix::ui::fire_last_toast_action());
    drain();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    click(dialog, "btn_secondary");
    CHECK(dismissed_for() == "Prusa MK4");
}

TEST_CASE_METHOD(TypeMismatchFixture,
                 "Type mismatch: a prompter destroyed during Choose Model's delay launches nothing",
                 "[type-mismatch][hardware_setup]") {
    warn();
    tap_history_entry();
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    click(dialog, "btn_primary");

    const void* owner = prompter_.get();
    REQUIRE(helix::test::armed_timer_count(owner) == 1);
    prompter_.reset();
    CHECK(helix::test::armed_timer_count(owner) == 0);
}
