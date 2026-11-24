// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notice.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix {

namespace {

// Constant-initialized, so a subscriber installed by another translation unit's
// static initializer cannot be overwritten by this one's.
NoticeSubscriber s_subscriber = nullptr;
UiRequestHandler s_ui_request_handlers[static_cast<int>(UiRequestKind::kCount)] = {};

} // namespace

NoticeSubscriber set_notice_subscriber(NoticeSubscriber subscriber) {
    NoticeSubscriber previous = s_subscriber;
    s_subscriber = subscriber;
    return previous;
}

void publish_notice(const Notice& notice) {
    if (!s_subscriber) {
        spdlog::warn("[Notice] No subscriber; dropping: {}", notice.message);
        return;
    }
    s_subscriber(notice);
}

void publish_notification(NoticeSeverity severity, std::string message, std::string title) {
    Notice notice;
    notice.severity = severity;
    notice.message = std::move(message);
    notice.delivery = NoticeDelivery::Notification;
    notice.title = std::move(title);
    publish_notice(notice);
}

void publish_detail_notification(NoticeSeverity severity, std::string message, std::string detail,
                                 bool sticky) {
    Notice notice;
    notice.severity = severity;
    notice.message = std::move(message);
    notice.delivery = NoticeDelivery::Notification;
    notice.detail = std::move(detail);
    notice.sticky = sticky;
    publish_notice(notice);
}

void publish_history_entry(NoticeSeverity severity, std::string title, std::string message,
                           std::string action) {
    Notice notice;
    notice.severity = severity;
    notice.message = std::move(message);
    notice.delivery = NoticeDelivery::HistoryOnly;
    notice.title = std::move(title);
    notice.history_action = std::move(action);
    publish_notice(notice);
}

void publish_alert(std::string title, std::string message) {
    Notice notice;
    notice.severity = NoticeSeverity::ERROR;
    notice.message = std::move(message);
    notice.delivery = NoticeDelivery::Alert;
    notice.title = std::move(title);
    publish_notice(notice);
}

UiRequestHandler set_ui_request_handler(UiRequestKind kind, UiRequestHandler handler) {
    UiRequestHandler& slot = s_ui_request_handlers[static_cast<int>(kind)];
    UiRequestHandler previous = slot;
    slot = handler;
    return previous;
}

void publish_ui_request(const UiRequest& request) {
    UiRequestHandler handler = s_ui_request_handlers[static_cast<int>(request.kind)];
    if (!handler) {
        spdlog::warn("[Notice] No handler for UI request {}; dropping",
                     static_cast<int>(request.kind));
        return;
    }
    handler(request);
}

} // namespace helix
