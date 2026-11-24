// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_dialog.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::plugin {

namespace {
constexpr const char* kName = "PluginDialog";
} // namespace

const char* close_how_name(CloseHow how) {
    switch (how) {
    case CloseHow::User:
        return "user";
    case CloseHow::Plugin:
        return "plugin";
    case CloseHow::App:
        return "app";
    case CloseHow::Cancelled:
        return "cancelled";
    }
    return "app";
}

CloseHow close_how_for(ModalCloseReason reason) {
    switch (reason) {
    case ModalCloseReason::BackdropTap:
    case ModalCloseReason::EscKey:
    case ModalCloseReason::ButtonPress:
        return CloseHow::User;
    case ModalCloseReason::Programmatic:
        return CloseHow::Plugin;
    case ModalCloseReason::HotReload:
    case ModalCloseReason::External:
        return CloseHow::App;
    }
    return CloseHow::App;
}

PluginDialog::PluginDialog(std::string plugin_id, std::string component,
                           std::vector<std::pair<std::string, std::string>> attrs,
                           std::function<void(CloseHow)> on_closed)
    : plugin_id_(std::move(plugin_id)), component_(std::move(component)), attrs_(std::move(attrs)),
      on_closed_(std::move(on_closed)) {}

const char* PluginDialog::get_name() const {
    return kName;
}

const char* PluginDialog::component_name() const {
    return component_.c_str();
}

lv_obj_t* PluginDialog::present(std::unique_ptr<PluginDialog> d) {
    std::vector<const char*> pairs;
    pairs.reserve(d->attrs_.size() * 2 + 1);
    for (const auto& [name, value] : d->attrs_) {
        pairs.push_back(name.c_str());
        pairs.push_back(value.c_str());
    }
    pairs.push_back(nullptr);

    PluginDialog* raw = d.get();
    const std::string tag = raw->plugin_id_;
    if (!Modal::show_owned(std::move(d), lv_screen_active(), pairs.data())) {
        spdlog::warn("[PluginDialog] '{}' could not be created", tag);
        return nullptr;
    }
    lv_obj_t* dialog = raw->dialog();
    ModalStack::instance().set_tag(ModalStack::instance().backdrop_for(dialog), tag);
    return dialog;
}

PluginDialog* PluginDialog::find(lv_obj_t* dialog) {
    if (!dialog) {
        return nullptr;
    }
    Modal* owner = ModalStack::instance().owner_for(dialog);
    if (!owner || std::strcmp(owner->get_name(), kName) != 0) {
        return nullptr;
    }
    return static_cast<PluginDialog*>(owner);
}

bool PluginDialog::close_by_plugin(lv_obj_t* dialog) {
    PluginDialog* self = find(dialog);
    if (!self || !self->is_visible()) {
        return false;
    }
    self->hide(ModalCloseReason::Programmatic);
    return true;
}

void PluginDialog::forget(lv_obj_t* dialog) {
    if (PluginDialog* self = find(dialog)) {
        self->on_closed_ = nullptr;
    }
}

void PluginDialog::on_hide() {
    // Taken out first: the callback may reach this dialog again.
    auto cb = std::move(on_closed_);
    on_closed_ = nullptr;
    if (cb) {
        cb(close_how_for(close_reason_));
    }
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
