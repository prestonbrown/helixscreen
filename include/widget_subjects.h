// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_subject_registry.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <functional>
#include <utility>

namespace helix {

/**
 * @brief A module's static subjects plus their init-once lifecycle
 *
 * A home-panel widget keeps its subjects in file-scope statics that every
 * instance shares. They are created once, before any XML binds them, and torn
 * down by StaticSubjectRegistry before lv_deinit() or a printer switch; the
 * next init after that teardown creates them again.
 *
 * ```cpp
 * HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
 * helix::WidgetSubjects s_subjects{"MyWidgetSubjects"};
 *
 * void my_widget_init_subjects() {
 *     s_subjects.ensure_init([] {
 *         UI_MANAGED_SUBJECT_INT(s_rung_subject, 0, "my_widget_rung", s_subjects);
 *     });
 * }
 * ```
 *
 * It is the SubjectManager the init body publishes into, so the managed-subject
 * macros take it unchanged. It holds no subject storage of its own: the
 * lv_subject_t objects and their buffers stay where the module declares them,
 * which on the ESP32 image is PSRAM through HELIX_PSRAM_BSS.
 *
 * Main thread only, like every subject it manages.
 */
class WidgetSubjects : public SubjectManager {
  public:
    /// @param registry_name The StaticSubjectRegistry key; the registry keeps one
    ///        entry per name, so it must be unique to this module.
    explicit WidgetSubjects(const char* registry_name) : registry_name_(registry_name) {}

    // Its registry entry points at this object.
    WidgetSubjects(const WidgetSubjects&) = delete;
    WidgetSubjects& operator=(const WidgetSubjects&) = delete;
    WidgetSubjects(WidgetSubjects&&) = delete;
    WidgetSubjects& operator=(WidgetSubjects&&) = delete;

    /**
     * @brief Run @p init unless the subjects are already up
     *
     * After @p init returns, registers this module's deinit under the registry
     * name. That deinit, while LVGL is alive and the subjects are up, runs
     * @p teardown, then deinitializes every subject published here, and leaves
     * the next ensure_init() free to run @p init again.
     *
     * @param teardown What the module tears down besides its published subjects,
     *        such as an observer on another module's subject. Runs first.
     * @return true if @p init ran
     */
    template <typename Init>
    bool ensure_init(Init&& init, helix::SharedCallback<void()> teardown = {}) {
        if (initialized_) {
            return false;
        }
        std::forward<Init>(init)();
        initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit(
            registry_name_, [this, teardown = std::move(teardown)]() { deinit(teardown); });
        return true;
    }

    [[nodiscard]] bool initialized() const {
        return initialized_;
    }

    [[nodiscard]] const char* registry_name() const {
        return registry_name_;
    }

  private:
    void deinit(const helix::SharedCallback<void()>& teardown) {
        if (!initialized_ || !lv_is_initialized()) {
            return;
        }
        if (teardown) {
            teardown();
        }
        deinit_all();
        initialized_ = false;
        spdlog::trace("[{}] Subjects deinitialized", registry_name_);
    }

    const char* registry_name_;
    bool initialized_ = false;
};

} // namespace helix
