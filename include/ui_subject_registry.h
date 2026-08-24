// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_subject_registry.h
 * @brief Helper macros and functions for LVGL subject registration
 *
 * This header provides convenience macros to reduce boilerplate when declaring,
 * initializing, and registering LVGL subjects for reactive data binding.
 *
 * ## Usage Example:
 *
 * ```cpp
 * // In .cpp file (file scope):
 * static lv_subject_t temp_subject;
 * static char temp_buf[32];
 *
 * void my_panel_init_subjects() {
 *     // Initialize and register in one call
 *     UI_SUBJECT_INIT_AND_REGISTER_STRING(temp_subject, temp_buf, "25°C", "my_temp");
 *
 *     // Or for int subjects:
 *     static lv_subject_t count_subject;
 *     UI_SUBJECT_INIT_AND_REGISTER_INT(count_subject, 0, "my_count");
 * }
 * ```
 *
 * ## Before (15 lines):
 * ```cpp
 * void ui_panel_motion_init_subjects() {
 *     snprintf(pos_x_buf, sizeof(pos_x_buf), "X:    --  mm");
 *     snprintf(pos_y_buf, sizeof(pos_y_buf), "Y:    --  mm");
 *     snprintf(pos_z_buf, sizeof(pos_z_buf), "Z:    --  mm");
 *
 *     lv_subject_init_string(&pos_x_subject, pos_x_buf, nullptr, sizeof(pos_x_buf), pos_x_buf);
 *     lv_subject_init_string(&pos_y_subject, pos_y_buf, nullptr, sizeof(pos_y_buf), pos_y_buf);
 *     lv_subject_init_string(&pos_z_subject, pos_z_buf, nullptr, sizeof(pos_z_buf), pos_z_buf);
 *
 *     lv_xml_register_subject(NULL, "motion_pos_x", &pos_x_subject);
 *     lv_xml_register_subject(NULL, "motion_pos_y", &pos_y_subject);
 *     lv_xml_register_subject(NULL, "motion_pos_z", &pos_z_subject);
 *
 *     spdlog::info("[Motion] Subjects initialized: X/Y/Z position displays");
 * }
 * ```
 *
 * ## After (6 lines):
 * ```cpp
 * void ui_panel_motion_init_subjects() {
 *     UI_SUBJECT_INIT_AND_REGISTER_STRING(pos_x_subject, pos_x_buf, "X:    --  mm",
 * "motion_pos_x"); UI_SUBJECT_INIT_AND_REGISTER_STRING(pos_y_subject, pos_y_buf, "Y:    --  mm",
 * "motion_pos_y"); UI_SUBJECT_INIT_AND_REGISTER_STRING(pos_z_subject, pos_z_buf, "Z:    --  mm",
 * "motion_pos_z"); spdlog::info("[Motion] Subjects initialized: X/Y/Z position displays");
 * }
 * ```
 */

#pragma once

#include "helix/xml/scoped_subject_registry.h"
#include "subject_debug_registry.h"

#include <cstdio>
#include <cstring>
#include <lvgl.h>

/**
 * @brief Initialize and register a string subject with the XML system
 *
 * This macro combines subject initialization and registration into a single call.
 * It automatically handles buffer sizing and copies the initial value.
 *
 * @param subject The lv_subject_t variable (no &)
 * @param buffer The char[] buffer for string storage (no &)
 * @param initial_value C-string with initial value (will be copied to buffer)
 * @param name XML registration name (C-string)
 *
 * Example:
 * ```cpp
 * static lv_subject_t temp_subject;
 * static char temp_buf[32];
 * UI_SUBJECT_INIT_AND_REGISTER_STRING(temp_subject, temp_buf, "25°C", "temperature");
 * ```
 */
#define UI_SUBJECT_INIT_AND_REGISTER_STRING(subject, buffer, initial_value, name)                  \
    do {                                                                                           \
        /* A caller that seeds the buffer before registering passes it as its own initial          \
           value. snprintf with overlapping src and dst is undefined behaviour and empties the     \
           buffer on glibc, discarding the seed. Skip the copy when the two alias. */              \
        if (static_cast<const char*>(initial_value) != static_cast<const char*>(buffer)) {         \
            snprintf((buffer), sizeof(buffer), "%s", (initial_value));                             \
        }                                                                                          \
        lv_subject_init_string(&(subject), (buffer), nullptr, sizeof(buffer), (buffer));           \
        helix::xml::register_subject_in_current_scope((name), &(subject));                         \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (name), LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);                       \
    } while (0)

/**
 * @brief Initialize and register a string subject with explicit size
 *
 * Use this variant when the buffer is passed via pointer (e.g., std::array::data())
 * where sizeof() would return pointer size instead of buffer size.
 *
 * @param subject The lv_subject_t variable (no &)
 * @param buffer Pointer to char buffer
 * @param size Size of the buffer in bytes
 * @param initial_value C-string with initial value (will be copied to buffer)
 * @param name XML registration name (C-string)
 *
 * Example:
 * ```cpp
 * std::array<char, 32> temp_buf;
 * lv_subject_t temp_subject;
 * UI_SUBJECT_INIT_AND_REGISTER_STRING_N(temp_subject, temp_buf.data(), temp_buf.size(), "25°C",
 *                                        "temperature");
 * ```
 */
#define UI_SUBJECT_INIT_AND_REGISTER_STRING_N(subject, buffer, size, initial_value, name)          \
    do {                                                                                           \
        /* See UI_SUBJECT_INIT_AND_REGISTER_STRING: aliasing buffer and initial_value is UB. */    \
        if (static_cast<const char*>(initial_value) != static_cast<const char*>(buffer)) {         \
            snprintf((buffer), (size), "%s", (initial_value));                                     \
        }                                                                                          \
        lv_subject_init_string(&(subject), (buffer), nullptr, (size), (buffer));                   \
        helix::xml::register_subject_in_current_scope((name), &(subject));                         \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (name), LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);                       \
    } while (0)

/**
 * @brief Initialize and register an integer subject with the XML system
 *
 * This macro combines subject initialization and registration into a single call.
 *
 * @param subject The lv_subject_t variable (no &)
 * @param initial_value Integer initial value
 * @param name XML registration name (C-string)
 *
 * Example:
 * ```cpp
 * static lv_subject_t count_subject;
 * UI_SUBJECT_INIT_AND_REGISTER_INT(count_subject, 0, "item_count");
 * ```
 */
#define UI_SUBJECT_INIT_AND_REGISTER_INT(subject, initial_value, name)                             \
    do {                                                                                           \
        lv_subject_init_int(&(subject), (initial_value));                                          \
        helix::xml::register_subject_in_current_scope((name), &(subject));                         \
        SubjectDebugRegistry::instance().register_subject(&(subject), (name), LV_SUBJECT_TYPE_INT, \
                                                          __FILE__, __LINE__);                     \
    } while (0)

/**
 * @brief Initialize and register a pointer subject with the XML system
 *
 * This macro combines subject initialization and registration into a single call.
 *
 * @param subject The lv_subject_t variable (no &)
 * @param initial_value Pointer initial value (can be nullptr)
 * @param name XML registration name (C-string)
 *
 * Example:
 * ```cpp
 * static lv_subject_t widget_subject;
 * UI_SUBJECT_INIT_AND_REGISTER_POINTER(widget_subject, nullptr, "active_widget");
 * ```
 */
#define UI_SUBJECT_INIT_AND_REGISTER_POINTER(subject, initial_value, name)                         \
    do {                                                                                           \
        lv_subject_init_pointer(&(subject), (initial_value));                                      \
        helix::xml::register_subject_in_current_scope((name), &(subject));                         \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (name), LV_SUBJECT_TYPE_POINTER, __FILE__, __LINE__);                      \
    } while (0)

/**
 * @brief Initialize and register a color subject with the XML system
 *
 * This macro combines subject initialization and registration into a single call.
 *
 * @param subject The lv_subject_t variable (no &)
 * @param initial_value lv_color_t initial value
 * @param name XML registration name (C-string)
 *
 * Example:
 * ```cpp
 * static lv_subject_t color_subject;
 * UI_SUBJECT_INIT_AND_REGISTER_COLOR(color_subject, lv_color_hex(0xFF0000), "accent_color");
 * ```
 */
#define UI_SUBJECT_INIT_AND_REGISTER_COLOR(subject, initial_value, name)                           \
    do {                                                                                           \
        lv_subject_init_color(&(subject), (initial_value));                                        \
        helix::xml::register_subject_in_current_scope((name), &(subject));                         \
        SubjectDebugRegistry::instance().register_subject(                                         \
            &(subject), (name), LV_SUBJECT_TYPE_COLOR, __FILE__, __LINE__);                        \
    } while (0)
