// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_presenter.h"

#include "ui_modal.h"

#include "display_manager.h"

#include <algorithm>

namespace helix::plugin {

namespace {
// Covers live in several unrelated owners, so a waiting queue re-reads them on a
// short timer that exists only while something waits.
constexpr uint32_t kRetryMs = 200;
} // namespace

Decision decide(bool app_blocked, const std::string& showing_dialog_owner,
                const std::string& requester, bool earlier_waiting) {
    if (app_blocked)
        return Decision::Wait;
    if (showing_dialog_owner == requester)
        return Decision::Present;
    if (!showing_dialog_owner.empty() || earlier_waiting)
        return Decision::Wait;
    return Decision::Present;
}

PluginPresenter::PluginPresenter()
    : covers_([] { return ui::current_app_covers(); }), wake_([] {
          if (auto* dm = DisplayManager::instance())
              dm->wake_display();
      }) {}

PluginPresenter::~PluginPresenter() = default;

void PluginPresenter::set_covers_source(CoversFn covers) {
    covers_ = std::move(covers);
}

void PluginPresenter::set_wake(WakeFn wake) {
    wake_ = std::move(wake);
}

bool PluginPresenter::may_present(const std::string& plugin_id, bool earlier_waiting) const {
    return decide(ui::app_blocked(covers_()), ModalStack::instance().top_visible_tag(), plugin_id,
                  earlier_waiting) == Decision::Present;
}

bool PluginPresenter::wake_and_recheck() {
    wake_();
    if (ui::app_blocked(covers_()))
        return false; // waking engaged the lock screen: keep it queued
    return true;
}

int PluginPresenter::request(const std::string& plugin_id, SurfaceKind kind, PresentFn present,
                             CancelFn cancelled) {
    Entry e{next_ticket_++, plugin_id, kind, std::move(present), std::move(cancelled)};
    if (!presenting_ && may_present(plugin_id, !queue_.empty())) {
        presenting_ = true;
        const bool woke_clear = wake_and_recheck();
        bool shown = false;
        if (woke_clear)
            shown = e.present();
        presenting_ = false;
        if (woke_clear) {
            rearm();
            return shown ? e.ticket : 0;
        }
    }
    const int ticket = e.ticket;
    queue_.push_back(std::move(e));
    rearm();
    return ticket;
}

bool PluginPresenter::withdraw(int ticket) {
    auto it = std::find_if(queue_.begin(), queue_.end(),
                           [ticket](const Entry& e) { return e.ticket == ticket; });
    if (it == queue_.end())
        return false;
    CancelFn cancelled = std::move(it->cancelled);
    queue_.erase(it);
    rearm();
    if (cancelled)
        cancelled();
    return true;
}

bool PluginPresenter::is_queued(int ticket) const {
    return std::any_of(queue_.begin(), queue_.end(),
                       [ticket](const Entry& e) { return e.ticket == ticket; });
}

size_t PluginPresenter::queued_dialogs(const std::string& plugin_id) const {
    return static_cast<size_t>(std::count_if(queue_.begin(), queue_.end(), [&](const Entry& e) {
        return e.plugin_id == plugin_id && e.kind == SurfaceKind::Dialog;
    }));
}

void PluginPresenter::drop_plugin(const std::string& plugin_id) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&](const Entry& e) { return e.plugin_id == plugin_id; }),
                 queue_.end());
    rearm();
}

void PluginPresenter::pump() {
    if (presenting_)
        return;
    presenting_ = true;
    while (!queue_.empty() && may_present(queue_.front().plugin_id, false)) {
        if (!wake_and_recheck())
            break;
        // Out of the queue before it runs: presenting may request or withdraw.
        Entry e = std::move(queue_.front());
        queue_.pop_front();
        if (!e.present() && e.cancelled)
            e.cancelled();
    }
    presenting_ = false;
    rearm();
}

void PluginPresenter::rearm() {
    if (queue_.empty()) {
        retry_.reset();
        return;
    }
    if (retry_)
        return;
    // One-shot, re-armed by the pump while anything still waits. LVGL deletes a
    // spent one-shot itself, so the guard lets go before pumping.
    lv_timer_t* t = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PluginPresenter*>(lv_timer_get_user_data(timer));
            self->retry_.release();
            self->pump();
        },
        kRetryMs, this);
    lv_timer_set_repeat_count(t, 1);
    retry_.reset(t);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
