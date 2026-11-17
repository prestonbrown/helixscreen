// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/subject.h"

#include "core/observer_guard.h"
#include "core/update_queue.h"
#include "helix_lvgl_anomaly.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <memory>
#include <utility>

namespace helix {

namespace {

/// user_data of a node whose Subject has been destroyed while its free is still queued.
/// subject_reset() on such a node resets it in place; nothing else reads it.
char g_orphan_tag;

std::atomic<uint32_t> g_anomalies{0};

/// Bumped by subjects_note_lvgl_deinit(): a node from an earlier generation was freed by
/// lv_deinit() and must not be touched, even once LVGL is initialized again.
std::atomic<uint32_t> g_generation{0};

uint32_t current_generation() {
    return g_generation.load(std::memory_order_acquire);
}

void report(const char* code, const char* context) {
    g_anomalies.fetch_add(1, std::memory_order_relaxed);
    helix_lvgl_anomaly(code, context);
}

/// Frees every observer on `subject` the way lv_subject_deinit() does: walk with the next
/// pointer taken first, delete each, then clear whatever the list still holds.
void remove_all_observers(lv_subject_t* subject) {
    auto* observer = static_cast<lv_observer_t*>(lv_ll_get_head(&subject->subs_ll));
    while (observer) {
        auto* next = static_cast<lv_observer_t*>(lv_ll_get_next(&subject->subs_ll, observer));
        lv_observer_delete(observer);
        observer = next;
    }
    lv_ll_clear(&subject->subs_ll);
}

/// Value, previous value and range exactly as lv_subject_init_<type>() leaves them, written
/// in place so the node keeps its address. No observers exist when this runs, so nothing
/// is notified and nothing is clamped.
void seed(lv_subject_t* subject, lv_subject_type_t type, lv_subject_value_t value) {
    subject->type = type;
    subject->size = 0;
    subject->value = value;
    subject->prev_value = value;
    subject->min_value = lv_subject_value_t{};
    subject->max_value = lv_subject_value_t{};
    if (type == LV_SUBJECT_TYPE_INT) {
        subject->min_value.num = INT32_MIN;
        subject->max_value.num = INT32_MAX;
    } else if (type == LV_SUBJECT_TYPE_FLOAT) {
        subject->min_value.float_v = -FLT_MAX;
        subject->max_value.float_v = FLT_MAX;
    }
}

/// Drops every observer from the list without freeing it, as LVGL's own init does on a
/// live subject. A guard still holding one of them deletes it later on its own; had
/// the observer been freed here, that delete would free it twice.
void orphan_all_observers(lv_subject_t* subject) {
    lv_ll_init(&subject->subs_ll, subject->subs_ll.n_size);
}

/// What reset() does to the node: observers freed, new ones refused until the next init.
void reset_node(lv_subject_t* subject) {
    remove_all_observers(subject);
    subject->deinitialized = 1;
}

constexpr bool same_text(const char* a, const char* b) {
    for (; *a != '\0' && *a == *b; ++a, ++b) {
    }
    return *a == *b;
}

bool on_main_thread() {
    return helix::ui::is_main_thread();
}

/// Frees `node` once the update queue drains, never inline. An owner's members die in
/// reverse declaration order, so a SubjectManager or an ObserverGuard declared above a
/// Subject still reaches the node after ~Subject has run; today's by-value lv_subject_t
/// keeps its bytes until the whole owner is gone, and the queue keeps that true. The queue
/// also moves a free that starts on a worker thread onto the main thread.
void release_node(lv_subject_t* node, uint32_t generation) {
    node->user_data = &g_orphan_tag;
    // Once the queue is shut down (app shutdown) or destroyed (the atexit chain) it would
    // only drop the free, and log that it did; LVGL keeps the node in its list and
    // lv_deinit() frees it.
    if (helix::ui::UpdateQueue::destroyed() || helix::ui::UpdateQueue::instance().is_shut_down()) {
        return;
    }
    // queue_update tags are literals at the call; the assert ties this one to the constant.
    static_assert(same_text(kSubjectReleaseTag, "helix::Subject::release_node"),
                  "release tag literal and kSubjectReleaseTag differ");
    helix::ui::queue_update("helix::Subject::release_node", [node, generation]() {
        if (lv_is_initialized() && generation == current_generation()) {
            ObserverGuard::forget_subject_teardown_exempt(node);
            lv_subject_delete(node);
        }
    });
}

} // namespace

void subjects_note_lvgl_deinit() noexcept {
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ObserverGuard::forget_all_teardown_exempt_subjects();
}

Subject::Subject() noexcept {
    if (lv_is_initialized() && on_main_thread()) {
        handle_ = lv_subject_create(LV_SUBJECT_TYPE_INT);
        if (handle_) {
            generation_ = current_generation();
            handle_->user_data = this;
            handle_->deinitialized = 1;
        }
    }
}

bool Subject::node_usable() const noexcept {
    return handle_ != nullptr && lv_is_initialized() && generation_ == current_generation();
}

Subject::~Subject() {
    if (node_usable()) {
        release_node(handle_, generation_);
    }
}

Subject::Subject(Subject&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      generation_(std::exchange(other.generation_, 0)), live_(std::exchange(other.live_, false)) {
    if (node_usable()) {
        handle_->user_data = this;
    }
}

Subject& Subject::operator=(Subject&& other) noexcept {
    if (this != std::addressof(other)) {
        if (node_usable()) {
            release_node(handle_, generation_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
        generation_ = std::exchange(other.generation_, 0);
        live_ = std::exchange(other.live_, false);
        if (node_usable()) {
            handle_->user_data = this;
        }
    }
    return *this;
}

lv_subject_t* Subject::prepare(lv_subject_type_t type) {
    if (!lv_is_initialized()) {
        report("subject_init_before_lvgl", "helix::Subject::init_* before lv_init()");
        handle_ = nullptr;
        live_ = false;
        return nullptr;
    }
    if (handle_ != nullptr && !node_usable()) {
        // lv_deinit() freed this node; LVGL is up again, so start a new one.
        handle_ = nullptr;
        live_ = false;
    }
    if (!on_main_thread()) {
        report("subject_init_off_main_thread", "helix::Subject::init_* off the LVGL thread");
        if (!handle_) {
            // Creating would insert into LVGL's global subject list from this thread.
            live_ = false;
            return nullptr;
        }
    }
    if (!handle_) {
        handle_ = lv_subject_create(type);
        if (!handle_) {
            live_ = false;
            return nullptr;
        }
        generation_ = current_generation();
        handle_->user_data = this;
    } else if (live_) {
        report("subject_reinit_while_live",
               "helix::Subject::init_* on a live subject; observers orphaned");
        orphan_all_observers(handle_);
    } else if (lv_ll_get_head(&handle_->subs_ll) != nullptr) {
        // seed() rewrites the type in place, which is sound only with no observer on the
        // list. A node that is not live refuses observers, so this is a breach.
        report("subject_retype_with_observers",
               "helix::Subject::init_* on a node that kept observers; observers orphaned");
        orphan_all_observers(handle_);
    }
    handle_->deinitialized = 0;
    live_ = true;
    return handle_;
}

void Subject::init_int(int32_t value) {
    if (auto* s = prepare(LV_SUBJECT_TYPE_INT)) {
        lv_subject_value_t v{};
        v.num = value;
        seed(s, LV_SUBJECT_TYPE_INT, v);
    }
}

void Subject::init_float(float value) {
    if (auto* s = prepare(LV_SUBJECT_TYPE_FLOAT)) {
        lv_subject_value_t v{};
        v.float_v = value;
        seed(s, LV_SUBJECT_TYPE_FLOAT, v);
    }
}

void Subject::init_pointer(void* value) {
    if (auto* s = prepare(LV_SUBJECT_TYPE_POINTER)) {
        lv_subject_value_t v{};
        v.pointer = value;
        seed(s, LV_SUBJECT_TYPE_POINTER, v);
    }
}

void Subject::init_color(lv_color_t value) {
    if (auto* s = prepare(LV_SUBJECT_TYPE_COLOR)) {
        lv_subject_value_t v{};
        v.color = value;
        seed(s, LV_SUBJECT_TYPE_COLOR, v);
    }
}

void Subject::init_string(char* buf, char* prev_buf, size_t size, const char* value) {
    auto* s = prepare(LV_SUBJECT_TYPE_STRING);
    if (!s) {
        return;
    }
    seed(s, LV_SUBJECT_TYPE_STRING, lv_subject_value_t{});
    if (!buf || size == 0) {
        return;
    }
    // memmove, not strlcpy: the managed macros pass the buffer itself as the value.
    const char* src = value ? value : "";
    const size_t len = std::min(std::strlen(src), size - 1);
    std::memmove(buf, src, len);
    buf[len] = '\0';
    if (prev_buf) {
        std::memcpy(prev_buf, buf, len + 1);
    }
    s->size = static_cast<uint32_t>(size);
    s->value.pointer = buf;
    s->prev_value.pointer = prev_buf;
}

void Subject::reset() {
    if (!live_) {
        return;
    }
    live_ = false;
    if (!node_usable()) {
        return;
    }
    reset_node(handle_);
}

uint32_t Subject::anomalies_reported() noexcept {
    return g_anomalies.load(std::memory_order_relaxed);
}

bool subject_is_live(const lv_subject_t* subject) noexcept {
    if (subject == nullptr || subject->user_data == &g_orphan_tag) {
        return false;
    }
    if (const Subject* owner = Subject::owner_of(subject)) {
        return owner->is_live();
    }
    return subject->type != LV_SUBJECT_TYPE_INVALID && subject->deinitialized == 0;
}

Subject* Subject::owner_of(const lv_subject_t* subject) noexcept {
    if (!subject || subject->user_data == &g_orphan_tag) {
        return nullptr;
    }
    return static_cast<Subject*>(subject->user_data);
}

void subject_reset(lv_subject_t* subject) {
    if (!subject) {
        return;
    }
    if (subject->user_data == &g_orphan_tag) {
        reset_node(subject); // its Subject is gone and the free is queued
        return;
    }
    if (Subject* owner = Subject::owner_of(subject)) {
        owner->reset();
        return;
    }
    report("subject_reset_unowned", "helix::subject_reset on a subject no helix::Subject owns");
}

bool subject_has_observer(const lv_subject_t* subject, const lv_observer_t* observer) {
    if (!subject || !observer) {
        return false;
    }
    auto* list = const_cast<lv_ll_t*>(&subject->subs_ll);
    for (auto* it = static_cast<lv_observer_t*>(lv_ll_get_head(list)); it;
         it = static_cast<lv_observer_t*>(lv_ll_get_next(list, it))) {
        if (it == observer) {
            return true;
        }
    }
    return false;
}

void Subject::publish_int(int32_t value) {
    if (live_) {
        lv_subject_set_int(handle_, value);
    } else {
        init_int(value);
    }
}

void subject_init_int(Subject& subject, int32_t value) {
    subject.init_int(value);
}

void subject_init_pointer(Subject& subject, void* value) {
    subject.init_pointer(value);
}

void subject_init_color(Subject& subject, lv_color_t value) {
    subject.init_color(value);
}

void subject_init_string(Subject& subject, char* buf, char* prev_buf, size_t size,
                         const char* value) {
    subject.init_string(buf, prev_buf, size, value);
}

} // namespace helix
