// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard_language_chooser.h"

#include "ui_fonts.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "display_settings_manager.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

using namespace helix;

#ifndef HELIX_MAX_FONT_TIER
#define HELIX_MAX_FONT_TIER 6 // default: all tiers (micro=0 .. xxlarge=6)
#endif
#ifndef HELIX_HAS_HIDPI_FONTS
#define HELIX_HAS_HIDPI_FONTS 0 // conservative: assume the 48/64 faces are not linked
#endif

// External subject for enabling/disabling Next button
extern lv_subject_t connection_test_passed;

// ============================================================================
// Welcome Translations
// ============================================================================

// Welcome text in each supported language (cycles during animation)
static const char* WELCOME_TRANSLATIONS[] = {
    "Welcome!",          // en
    "Willkommen!",       // de
    "Bienvenue!",        // fr
    "¡Bienvenido!",      // es
    "Добро пожаловать!", // ru
    "Bem-vindo!",        // pt
    "Benvenuto!",        // it
    "欢迎！",            // zh
    "ようこそ！",        // ja
};
static constexpr int WELCOME_COUNT = 9;

// Language codes for saving to config (matches button order in XML)
static const char* LANGUAGE_CODES[] = {"en", "de", "fr", "es", "ru", "pt", "it", "zh", "ja"};

// Timer period for cycling welcome text
static constexpr uint32_t WELCOME_CYCLE_MS = 2500;

// Animation durations
static constexpr int32_t CROSSFADE_DURATION_MS = 150;

namespace helix {

const lv_font_t* wizard_welcome_header_font(UiBreakpoint bp) {
    switch (bp) {
    case UiBreakpoint::Micro:
        return &noto_sans_16;
    case UiBreakpoint::Tiny:
        return &noto_sans_18;
    case UiBreakpoint::Small:
        return &noto_sans_24;
    case UiBreakpoint::Medium:
#if HELIX_MAX_FONT_TIER >= 6 && HELIX_HAS_HIDPI_FONTS
        return &noto_sans_48;
#elif HELIX_MAX_FONT_TIER >= 3
        return &noto_sans_26;
#else
        return &noto_sans_24;
#endif
    default: // Large / XLarge / XXLarge
#if HELIX_MAX_FONT_TIER >= 6 && HELIX_HAS_HIDPI_FONTS
        return &noto_sans_64;
#elif HELIX_MAX_FONT_TIER >= 4
        return &noto_sans_28;
#elif HELIX_MAX_FONT_TIER >= 3
        return &noto_sans_26;
#else
        return &noto_sans_24;
#endif
    }
}

} // namespace helix

// The header wears the ladder as a local style resolved at create(); a resize
// that moves the ui_breakpoint subject mid-wizard must re-resolve it or the
// header keeps the tier the step was built at (prestonbrown/helixscreen#1612).
static void welcome_header_font_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* header = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    lv_obj_set_style_text_font(
        header, wizard_welcome_header_font(as_breakpoint(lv_subject_get_int(subject))),
        LV_PART_MAIN);
}

// ============================================================================
// Global Instance
// ============================================================================

static std::unique_ptr<WizardLanguageChooserStep> g_wizard_language_chooser_step;

// Flag to force language step to show (for visual testing)
static bool g_force_language_step = false;

void force_language_chooser_step(bool force) {
    g_force_language_step = force;
    if (force) {
        spdlog::debug("[WizardLanguageChooser] Force-showing step for visual testing");
    }
}

WizardLanguageChooserStep* get_wizard_language_chooser_step() {
    if (!g_wizard_language_chooser_step) {
        g_wizard_language_chooser_step = std::make_unique<WizardLanguageChooserStep>();
        StaticPanelRegistry::instance().register_destroy(
            "WizardLanguageChooserStep", []() { g_wizard_language_chooser_step.reset(); });
    }
    return g_wizard_language_chooser_step.get();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WizardLanguageChooserStep::WizardLanguageChooserStep() {
    spdlog::debug("[{}] Instance created", get_name());
}

WizardLanguageChooserStep::~WizardLanguageChooserStep() {
    // Timer guard handles cleanup automatically via RAII

    screen_root_ = nullptr;
}

// ============================================================================
// ============================================================================
// Subject Initialization
// ============================================================================

void WizardLanguageChooserStep::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize welcome text subject with string buffer
    strncpy(welcome_buffer_, WELCOME_TRANSLATIONS[0], sizeof(welcome_buffer_) - 1);
    welcome_buffer_[sizeof(welcome_buffer_) - 1] = '\0';
    UI_MANAGED_SUBJECT_STRING(welcome_text_, welcome_buffer_, welcome_buffer_,
                              "wizard_welcome_text", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

// Helper to update visual selection state of language list items
static void update_language_list_selection(lv_obj_t* selected_btn) {
    if (!selected_btn)
        return;

    // Get the parent list container
    lv_obj_t* list = lv_obj_get_parent(selected_btn);
    if (!list)
        return;

    // Iterate through all children and update their visual state
    uint32_t child_count = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* btn = lv_obj_get_child(list, static_cast<int32_t>(i));
        if (!btn)
            continue;

        // Get the label inside the button (first child)
        lv_obj_t* label = lv_obj_get_child(btn, 0);
        bool is_selected = (btn == selected_btn);

        if (is_selected) {
            // Selected: primary color background
            lv_obj_set_style_bg_color(btn, theme_manager_get_color("primary"), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            if (label) {
                // Contrast text color based on background luminance
                lv_color_t primary = theme_manager_get_color("primary");
                uint8_t lum = lv_color_luminance(primary);
                lv_color_t text_color = (lum > 140) ? lv_color_black() : lv_color_white();
                lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);
            }
        } else {
            // Unselected: transparent background
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
            if (label) {
                lv_obj_set_style_text_color(label, theme_manager_get_color("text"), LV_PART_MAIN);
            }
        }
    }
}

static void on_language_selected(lv_event_t* e) {
    // Get the language index from user_data (set in XML via event_cb user_data attribute)
    // Note: user_data is a string in LVGL 9 XML, not an integer
    const char* user_data_str = static_cast<const char*>(lv_event_get_user_data(e));
    if (!user_data_str) {
        spdlog::warn("[Wizard Language Chooser] No user_data in event");
        return;
    }

    int index = std::atoi(user_data_str);
    if (index < 0 || index >= 9) { // 9 languages total
        spdlog::warn("[Wizard Language Chooser] Invalid language index: {}", index);
        return;
    }

    spdlog::info("[Wizard Language Chooser] Language selected: {} ({})", LANGUAGE_CODES[index],
                 WELCOME_TRANSLATIONS[index % WELCOME_COUNT]);

    // Update visual selection
    lv_obj_t* clicked_btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    update_language_list_selection(clicked_btn);

    // Apply language immediately via SystemSettingsManager (hot-reload)
    // This updates the subject, calls lv_translation_set_language(), and persists to config
    SystemSettingsManager::instance().set_language(LANGUAGE_CODES[index]);

    // Refresh the wizard header with new translations
    ui_wizard_refresh_header_translations();

    // Update step state
    WizardLanguageChooserStep* step = get_wizard_language_chooser_step();
    if (step) {
        step->stop_cycle_timer();
        step->set_language_selected(true);
    }

    // Enable Next button
    lv_subject_set_int(&connection_test_passed, 1);
}

void WizardLanguageChooserStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());

    lv_xml_register_event_cb(nullptr, "on_language_selected", on_language_selected);
}

// ============================================================================
// Welcome Text Cycling
// ============================================================================

void WizardLanguageChooserStep::cycle_timer_cb(lv_timer_t* timer) {
    WizardLanguageChooserStep* step =
        static_cast<WizardLanguageChooserStep*>(lv_timer_get_user_data(timer));
    if (step) {
        step->cycle_welcome_text();
    }
}

void WizardLanguageChooserStep::cycle_welcome_text() {
    // Advance to next language
    current_welcome_index_ = (current_welcome_index_ + 1) % WELCOME_COUNT;
    const char* new_text = WELCOME_TRANSLATIONS[current_welcome_index_];

    spdlog::trace("[{}] Cycling to welcome text: {}", get_name(), new_text);

    // Animate the crossfade
    animate_crossfade(new_text);
}

void WizardLanguageChooserStep::animate_crossfade(const char* new_text) {
    if (!screen_root_) {
        return;
    }

    lv_obj_t* welcome_header = lv_obj_find_by_name(screen_root_, "welcome_header");
    if (!welcome_header) {
        // Fallback: just update the subject directly
        lv_subject_copy_string(&welcome_text_, new_text);
        return;
    }

    // Check if animations are enabled
    if (!DisplaySettingsManager::instance().get_animations_enabled()) {
        // No animation: just update the text immediately
        lv_subject_copy_string(&welcome_text_, new_text);
        return;
    }

    // Store new text for the completion callback
    // We capture it via the subject update in the completion callback
    struct CrossfadeCtx {
        WizardLanguageChooserStep* step;
        const char* new_text;
    };

    // Allocate context (will be cleaned up in completion callback)
    auto* ctx = new CrossfadeCtx{this, new_text};

    // Fade out animation (opacity: current -> 0)
    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, welcome_header);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_out, CROSSFADE_DURATION_MS);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade_out, [](void* obj, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                             LV_PART_MAIN);
    });
    lv_anim_set_user_data(&fade_out, ctx);
    lv_anim_set_completed_cb(&fade_out, [](lv_anim_t* anim) {
        auto* ctx = static_cast<CrossfadeCtx*>(anim->user_data);
        lv_obj_t* header = static_cast<lv_obj_t*>(anim->var);

        if (ctx && ctx->step) {
            // Update the text while invisible
            lv_subject_copy_string(ctx->step->get_welcome_text_subject(), ctx->new_text);

            // Fade in animation (opacity: 0 -> 255)
            lv_anim_t fade_in;
            lv_anim_init(&fade_in);
            lv_anim_set_var(&fade_in, header);
            lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&fade_in, CROSSFADE_DURATION_MS);
            lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&fade_in, [](void* obj, int32_t value) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                                     LV_PART_MAIN);
            });
            lv_anim_start(&fade_in);
        }

        // Clean up context
        delete ctx;
    });
    lv_anim_start(&fade_out);
}

// ============================================================================
// Screen Creation
// ============================================================================

lv_obj_t* WizardLanguageChooserStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating language chooser screen", get_name());

    // Safety check: cleanup should have been called by wizard navigation
    if (screen_root_) {
        spdlog::warn("[{}] Screen pointer not null - cleanup may not have been called properly",
                     get_name());
        screen_root_ = nullptr; // Reset pointer, wizard framework handles deletion
    }

    // Create screen from XML
    screen_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "wizard_language_chooser", nullptr));
    if (!screen_root_) {
        spdlog::error("[{}] Failed to create screen from XML", get_name());
        return nullptr;
    }

    // Display-size face, a size class above the text_heading default
    // (prestonbrown/helixscreen#1599).
    if (lv_obj_t* header = lv_obj_find_by_name(screen_root_, "welcome_header")) {
        // The observer below fires on registration, superseding this set when
        // the breakpoint subject is live; this is the null-subject fallback.
        lv_obj_set_style_text_font(
            header, wizard_welcome_header_font(breakpoint_for(responsive_dimension(nullptr))),
            LV_PART_MAIN);

        // Follow a runtime breakpoint change. The observer is bound to the
        // header widget, so it unsubscribes itself when the wizard framework
        // deletes the step content — no cleanup wiring needed.
        if (lv_subject_t* bp_subject = theme_manager_get_breakpoint_subject()) {
            lv_subject_add_observer_obj(bp_subject, welcome_header_font_observer_cb, header,
                                        nullptr);
        }
    }

    // Start the welcome text cycling timer
    lv_timer_t* timer = lv_timer_create(cycle_timer_cb, WELCOME_CYCLE_MS, this);
    cycle_timer_.reset(timer);
    spdlog::debug("[{}] Started welcome text cycle timer ({}ms)", get_name(), WELCOME_CYCLE_MS);

    spdlog::debug("[{}] Screen created successfully", get_name());
    return screen_root_;
}

// ============================================================================
// Cleanup
// ============================================================================

void WizardLanguageChooserStep::cleanup() {
    spdlog::debug("[{}] Cleaning up resources", get_name());

    // Stop the cycling timer - prevents new crossfade animations from starting
    cycle_timer_.reset();

    // Cancel any running crossfade animations BEFORE widgets are deleted
    // Without this, a mid-animation cleanup would leave the animation timer
    // referencing a deleted widget, causing a crash in lv_obj_refresh_style
    if (screen_root_) {
        lv_obj_t* welcome_header = lv_obj_find_by_name(screen_root_, "welcome_header");
        if (welcome_header) {
            // Delete all animations on this widget (NULL = any exec_cb)
            lv_anim_delete(welcome_header, nullptr);
        }
    }

    // Reset UI references
    // Note: Do NOT call lv_obj_del() here - the wizard framework handles
    // object deletion when clearing wizard_content container
    screen_root_ = nullptr;

    spdlog::debug("[{}] Cleanup complete", get_name());
}

// ============================================================================
// Validation
// ============================================================================

bool WizardLanguageChooserStep::is_validated() const {
    return language_selected_;
}

// ============================================================================
// Skip Logic
// ============================================================================

bool WizardLanguageChooserStep::should_skip() const {
    // Force show if explicitly requested (for visual testing with --wizard-step 1)
    if (g_force_language_step) {
        spdlog::debug("[{}] Force-showing: --wizard-step 1 requested", get_name());
        return false;
    }

    Config* cfg = Config::get_instance();
    if (!cfg) {
        return false;
    }

    // Check if language has already been set in config
    std::string saved_language = cfg->get<std::string>("/language", "");

    // Skip if a language has been explicitly set (not empty and not just default "en")
    // We show the step for first-time setup even if no language is set
    if (!saved_language.empty() && saved_language != "en") {
        spdlog::info("[{}] Language already set to '{}', skipping step", get_name(),
                     saved_language);
        return true;
    }

    // Skip when adding a subsequent printer — language is already configured.
    // Multiple printers in config means at least one wizard has completed before.
    if (cfg->get_printer_ids().size() > 1 && !saved_language.empty()) {
        spdlog::info("[{}] Subsequent printer ({}), language '{}' already set, skipping step",
                     get_name(), cfg->get_printer_ids().size(), saved_language);
        return true;
    }

    spdlog::debug("[{}] No language preference saved, showing step", get_name());
    return false;
}
