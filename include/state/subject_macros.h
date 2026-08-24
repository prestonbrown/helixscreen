// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file subject_macros.h
 * @brief DRY macros for LVGL subject initialization
 *
 * These macros consolidate the repetitive 3-line subject initialization pattern:
 * 1. lv_subject_init_*(subject, value)
 * 2. subjects.register_subject(subject)
 * 3. if (register_xml) lv_xml_register_subject(nullptr, "name", subject)
 *
 * The macros use naming convention: name_ for subject, name_buf_ for string buffer.
 * This allows short, readable initialization code:
 *
 * @code
 * // Before: 3 lines per subject
 * lv_subject_init_int(&temperature_, 0);
 * subjects_.register_subject(&temperature_);
 * lv_xml_register_subject(nullptr, "temperature", &temperature_);
 *
 * // After: 1 line per subject
 * INIT_SUBJECT_INT(temperature, 0, subjects_, true);
 * @endcode
 *
 * @note These macros expect variable naming convention:
 *       - Integer subject: `name_` (lv_subject_t)
 *       - String subject: `name_` (lv_subject_t) and `name_buf_` (char array)
 */

#pragma once

#include "helix/xml/scoped_subject_registry.h"
#include "lvgl/lvgl.h"
#include "state/volatile_subjects.h"

/**
 * @brief Initialize an integer subject with optional XML registration
 *
 * Initializes the subject named `name_` with the given default value,
 * registers it with the SubjectManager, and optionally registers it
 * with the LVGL XML binding system.
 *
 * @param name Base name (without trailing underscore). The actual subject
 *             variable must be named `name_` (e.g., name=temp -> temp_)
 * @param default_val Initial integer value
 * @param subjects SubjectManager instance to register with
 * @param register_xml If true, register with lv_xml_register_subject()
 *
 * @example
 * lv_subject_t my_count_;
 * INIT_SUBJECT_INT(my_count, 42, subjects, true);
 * // Equivalent to:
 * //   lv_subject_init_int(&my_count_, 42);
 * //   subjects.register_subject(&my_count_, "my_count");
 * //   lv_xml_register_subject(nullptr, "my_count", &my_count_);
 *
 * The XML name is handed to register_subject() as well as registered in the
 * scope, because SubjectManager::deinit_all() withdraws each name before
 * freeing its subject. Owners that do not live for the whole process - a
 * PrinterState held by a stack-allocated test fixture, a torn-down panel -
 * depend on that: without the name the registry keeps resolving to freed
 * storage and the next lookup hands out a dangling lv_subject_t.
 */
#define INIT_SUBJECT_INT(name, default_val, subjects, register_xml)                                \
    do {                                                                                           \
        const bool helix_reg_xml_ = (register_xml);                                                \
        lv_subject_init_int(&name##_, (default_val));                                              \
        (subjects).register_subject(&name##_, helix_reg_xml_ ? #name : nullptr);                   \
        if (helix_reg_xml_) {                                                                      \
            helix::xml::register_subject_in_current_scope(#name, &name##_);                        \
        }                                                                                          \
    } while (0)

/**
 * @brief Initialize a string subject with optional XML registration
 *
 * Initializes the subject named `name_` with the buffer `name_buf_`,
 * registers it with the SubjectManager, and optionally registers it
 * with the LVGL XML binding system.
 *
 * @param name Base name (without trailing underscore). The actual subject
 *             variable must be named `name_` and buffer `name_buf_`
 * @param default_val Initial string value (can be "" for empty)
 * @param subjects SubjectManager instance to register with
 * @param register_xml If true, register with lv_xml_register_subject()
 *
 * @example
 * lv_subject_t status_text_;
 * char status_text_buf_[64];
 * INIT_SUBJECT_STRING(status_text, "Ready", subjects, true);
 * // Equivalent to:
 * //   lv_subject_init_string(&status_text_, status_text_buf_, NULL,
 * //                          sizeof(status_text_buf_), "Ready");
 * //   subjects.register_subject(&status_text_, "status_text");
 * //   lv_xml_register_subject(nullptr, "status_text", &status_text_);
 *
 * See INIT_SUBJECT_INT for why the name is passed to register_subject().
 */
#define INIT_SUBJECT_STRING(name, default_val, subjects, register_xml)                             \
    do {                                                                                           \
        const bool helix_reg_xml_ = (register_xml);                                                \
        lv_subject_init_string(&name##_, name##_buf_, nullptr, sizeof(name##_buf_),                \
                               (default_val));                                                     \
        (subjects).register_subject(&name##_, helix_reg_xml_ ? #name : nullptr);                   \
        if (helix_reg_xml_) {                                                                      \
            helix::xml::register_subject_in_current_scope(#name, &name##_);                        \
        }                                                                                          \
    } while (0)

/**
 * @brief Initialize an integer subject AND mark it Klippy-volatile
 *
 * Identical to INIT_SUBJECT_INT, plus it records the subject and its default in
 * @p volatiles so a Klippy state transition can restore it. Use for subjects fed by
 * Moonraker DELTA-only status fields whose stale value gates behaviour (#1129).
 *
 * The default is written ONCE, here — init and reset cannot drift apart.
 *
 * @note @p default_val is expanded twice. Pass a literal or a constant expression,
 *       never a call with side effects.
 *
 * @param name        Base name (without trailing underscore)
 * @param default_val Initial integer value; also the value reset restores
 * @param subjects    SubjectManager instance to register with
 * @param volatiles   helix::subjects::VolatileSubjects instance to register with
 * @param register_xml If true, register with the LVGL XML binding system
 */
#define INIT_SUBJECT_INT_VOLATILE(name, default_val, subjects, volatiles, register_xml)            \
    do {                                                                                           \
        INIT_SUBJECT_INT(name, (default_val), subjects, register_xml);                             \
        (volatiles).register_subject(&name##_, (default_val));                                     \
    } while (0)
