// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file subject.h
 * @brief helix::Subject, the one owner of an LVGL subject allocated by lv_subject_create()
 *
 * @pattern RAII. The LVGL node is allocated once and keeps its address for the whole life
 *          of the owning Subject: reset() and every later init_*() reuse it. The destructor
 *          queues the free on the update queue, so members declared above a Subject (a
 *          SubjectManager, an ObserverGuard) still reach a valid node while they die. Cached
 *          getters, the XML subject registry, SubjectDebugRegistry and ObserverGuard all
 *          key on that address across a printer switch's teardown and re-init.
 * @threading Main thread only, like every LVGL subject call.
 * @gotchas `&member` yields the lv_subject_t* (operator&), so pointer arithmetic over an
 *          array of Subjects compiles and is wrong: the nodes are separate allocations.
 *          Index the container instead. std::addressof() gives the Subject itself.
 *          Never logs: destructors run in the atexit chain, after spdlog is gone.
 */

#pragma once

#include "lvgl/lvgl.h"

#include <cstddef>
#include <cstdint>

namespace helix {

class Subject {
  public:
    /// Allocates the node when LVGL is up on the main thread, so `&member` taken in a
    /// constructor already has its final address. The node refuses observers until the
    /// first init_*(). Before lv_init() the handle stays null until init_*().
    Subject() noexcept;
    ~Subject();

    Subject(const Subject&) = delete;
    Subject& operator=(const Subject&) = delete;
    Subject(Subject&& other) noexcept;
    Subject& operator=(Subject&& other) noexcept;

    void init_int(int32_t value);
    void init_float(float value);
    void init_pointer(void* value);
    void init_color(lv_color_t value);
    /// `buf` (and `prev_buf` when non-null) must outlive this Subject; `value` may alias `buf`.
    void init_string(char* buf, char* prev_buf, size_t size, const char* value);
    template <size_t N> void init_string(char (&buf)[N], const char* value) {
        init_string(buf, nullptr, N, value);
    }

    /// Removes every observer and refuses new ones until the next init_*(); keeps the
    /// allocation. The replacement for lv_subject_deinit().
    void reset();

    [[nodiscard]] bool is_live() const noexcept {
        return live_;
    }
    [[nodiscard]] lv_subject_t* get() const noexcept {
        return handle_;
    }
    /// `&member` keeps yielding the lv_subject_t* every LVGL call takes.
    lv_subject_t* operator&() const noexcept {
        return handle_;
    }

    /// The Subject that owns `subject`, or nullptr for one no Subject owns (including a
    /// node whose Subject is gone and whose free is still queued).
    [[nodiscard]] static Subject* owner_of(const lv_subject_t* subject) noexcept;

    /// How many misuse anomalies Subject has reported in this process (re-init while live,
    /// init before lv_init, init off the main thread, retype with observers).
    [[nodiscard]] static uint32_t anomalies_reported() noexcept;

  private:
    lv_subject_t* prepare(lv_subject_type_t type);

    lv_subject_t* handle_ = nullptr;
    bool live_ = false;
};

/// The update-queue tag of a destroyed Subject's queued free. The callback touches only the
/// orphaned node, never its former owner, so it is safe to run at any later point.
constexpr const char* kSubjectReleaseTag = "helix::Subject::release_node";

/// lv_subject_deinit() for code holding only the pointer (SubjectManager, registries):
/// resets the owning Subject.
void subject_reset(lv_subject_t* subject);

/// The managed macros' entry points, overloaded on the storage so one macro serves an
/// unconverted lv_subject_t and a helix::Subject while the migration runs.
void subject_init_int(Subject& subject, int32_t value);
void subject_init_pointer(Subject& subject, void* value);
void subject_init_color(Subject& subject, lv_color_t value);
void subject_init_string(Subject& subject, char* buf, char* prev_buf, size_t size,
                         const char* value);
void subject_init_int(lv_subject_t& subject, int32_t value);
void subject_init_pointer(lv_subject_t& subject, void* value);
void subject_init_color(lv_subject_t& subject, lv_color_t value);
void subject_init_string(lv_subject_t& subject, char* buf, char* prev_buf, size_t size,
                         const char* value);

} // namespace helix
