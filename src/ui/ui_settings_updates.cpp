// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_updates.h"

#include "ui_callback_helpers.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "helix_version.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "platform_info.h"
#include "system/config_trust.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
#include "system/android_update_source.h"

#include <SDL.h>
#elif defined(HELIX_PLATFORM_X86_DESKTOP)
#include "system/release_page.h"

#include <SDL.h>
#endif

namespace helix::settings {

#if defined(__ANDROID__) || defined(HELIX_PLATFORM_X86_DESKTOP)
namespace {
/// The release the update check is offering, for builds that open a page
/// instead of running the tarball updater.
helix::OfferedRelease offered_release() {
    auto& checker = UpdateChecker::instance();
    helix::OfferedRelease offered;
    if (auto info = checker.get_cached_update()) {
        offered.tag_name = info->tag_name;
        offered.version = info->version;
    }
    offered.on_github = checker.get_channel() != UpdateChecker::UpdateChannel::Dev;
    return offered;
}
} // namespace
#endif

void UpdatesSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_about_update_channel_changed",
         [](lv_event_t* e) {
             lv_obj_t* dropdown = lv_event_get_current_target_obj(e);
             const int index = helix::ui::event_selected(e);

             if (index == 2 && helix::config_trust::read_update_urls().dev_url.empty()) {
                 spdlog::warn("[UpdatesSettings] Dev channel selected but no dev_url configured");
                 int current = SystemSettingsManager::instance().get_update_channel();
                 lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current));
                 ToastManager::instance().show(
                     ToastSeverity::WARNING,
                     lv_tr("Dev channel requires dev_url in update_urls.json"), 3000);
                 return;
             }

             spdlog::info("[UpdatesSettings] Update channel changed: {} ({})", index,
                          index == 0 ? "Stable" : (index == 1 ? "Beta" : "Dev"));
             SystemSettingsManager::instance().set_update_channel(index);
             // Drops the previous channel's cached verdict, re-snapshots the config
             // for the debug bundle's off-thread reader, and starts a fresh check.
             // The re-check matters when the new channel is BEHIND this install and
             // the only way forward is an explicit switch back.
             UpdateChecker::instance().on_channel_changed();
         }},
        {"on_about_check_updates_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Check for updates requested");
             UpdateChecker::instance().check_for_updates();
         }},
        {"on_about_install_update_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Install update requested");

             // Moving backward is never what someone means by "install update", so it is
             // never the one-tap path. Settings written by the newer build are not
             // migrated back either: the older build reads what it recognizes and
             // leaves the rest alone.
             auto cached = UpdateChecker::instance().get_cached_update();
             if (cached && cached->is_downgrade) {
                 spdlog::info(
                     "[UpdatesSettings] Install target v{} is older than installed v{}, confirming",
                     cached->version, HELIX_VERSION);
                 std::string msg =
                     fmt::format(lv_tr("This channel offers v{}, older than the installed v{}. "
                                       "Anything added since then will be removed."),
                                 cached->version, HELIX_VERSION);
                 helix::ui::modal_confirm(
                     lv_tr("Install Older Version?"), msg.c_str(), ModalSeverity::Warning,
                     lv_tr("Install"),
                     [] { get_updates_settings_overlay().show_update_download_modal(); });
             } else {
                 get_updates_settings_overlay().show_update_download_modal();
             }
         }},
        {"on_about_updates_unavailable_clicked",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Updates-unavailable notice tapped");

             // Reached only when self_update_supported() is false and updates are not
             // firmware-managed: this box can see that a new version exists but cannot
             // apply one itself. The command is the whole payload; without it the row
             // states a problem and offers nothing. The QR points at the docs for the
             // longer story.
             const std::string url = helix::docs_url("upgrading/");
             helix::ui::InfoQrModal::show_owned({
                 .icon = "console",
                 .title = lv_tr("Update from a Terminal"),
                 // No command in here on purpose. The one-liner is not portable across the
                 // platforms this runs on (BusyBox firmwares such as K1, K2, AD5M and CC1
                 // ship ash with no bash, and several have wget but no curl), so any single
                 // literal would be wrong somewhere and baked into a binary. The docs can
                 // say the right thing per platform and be corrected without a release.
                 .message = lv_tr("Run the HelixScreen installer with --update from a "
                                  "terminal on this printer. Scan for the command for "
                                  "your platform."),
                 .url = url,
                 .url_text = helix::docs_url_text(url),
             });
         }},
        {"on_update_download_start",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Starting update download");
             UpdateChecker::instance().start_download();
         }},
        {"on_update_download_cancel",
         [](lv_event_t*) {
             spdlog::info("[UpdatesSettings] Download cancelled by user");
             UpdateChecker::instance().cancel_download();
             get_updates_settings_overlay().hide_update_download_modal();
         }},
        {"on_update_download_dismiss",
         [](lv_event_t*) { get_updates_settings_overlay().hide_update_download_modal(); }},
    });
}

void UpdatesSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    sync_update_channel_rows(overlay_root_,
                             static_cast<int>(UpdateChecker::instance().get_channel()));
}

void UpdatesSettingsOverlay::show_update_download_modal(bool start_immediately) {
#ifdef __ANDROID__
    // Android never runs the tarball updater. Every install intent (the Install
    // Update row and the "New Version Available" notification) opens wherever this
    // APK came from: Play's in-app update flow (or its listing) for a Play install,
    // the GitHub release for a sideload.
    if (helix::is_android_platform()) {
        const std::string installer = helix::android::installer_package();
        if (helix::android::installed_by_play(installer) && helix::android::start_play_update()) {
            spdlog::info("[UpdatesSettings] Installer '{}', starting the Play update flow",
                         installer);
            (void)start_immediately;
            return;
        }
        const std::string url = helix::android::update_url(installer, offered_release());
        spdlog::info("[UpdatesSettings] Installer '{}', opening {}", installer, url);
        if (SDL_OpenURL(url.c_str()) != 0) {
            spdlog::warn("[UpdatesSettings] Opening {} failed: {}", url, SDL_GetError());
            if (url == helix::android::kPlayStoreMarketUrl) {
                SDL_OpenURL(helix::android::kPlayStoreWebUrl);
            }
        }
        (void)start_immediately;
        return;
    }
#elif defined(HELIX_PLATFORM_X86_DESKTOP)
    // The desktop build never rewrites its own install: every install intent opens
    // the release page in the user's browser, where they download the new tarball.
    {
        const std::string url = helix::release_page_url(offered_release());
        spdlog::info("[UpdatesSettings] Desktop build, opening {}", url);
        if (SDL_OpenURL(url.c_str()) != 0) {
            spdlog::warn("[UpdatesSettings] Opening {} failed: {}", url, SDL_GetError());
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("Could not open a browser for the update"), 4000);
        }
        (void)start_immediately;
        return;
    }
#endif

    // The modal can open before the overlay ever has (update notification).
    register_callbacks();

    if (!download_status_observer_) {
        auto& checker = UpdateChecker::instance();
        download_status_observer_ = helix::ui::observe<int>(
            checker.download_status_subject(), this,
            [](UpdatesSettingsOverlay* self, int status) { self->on_download_status(status); },
            checker.get_subjects_lifetime());
    }
    download_modal_closed_by_user_ = false;

    if (!update_download_modal_) {
        // Clear any stale Error/Complete status carried over from a prior
        // download attempt — otherwise the modal briefly flashes that
        // content before the status update below takes effect.
        UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0,
                                                         "");
        update_download_modal_ = helix::ui::modal_show("update_download_modal");
    }

    if (start_immediately) {
        // User already confirmed on the "New Version Available" notification —
        // skip the redundant Confirming state and begin the download directly.
        UpdateChecker::instance().start_download();
        return;
    }

    // Set to Confirming state with version info
    auto info = UpdateChecker::instance().get_cached_update();
    std::string text = info ? fmt::format(lv_tr("Download v{}?"), info->version)
                            : std::string(lv_tr("Download update?"));
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Confirming, 0,
                                                     text);
}

void UpdatesSettingsOverlay::on_download_status(int status) {
    if (status != static_cast<int>(UpdateChecker::DownloadStatus::Error) ||
        download_modal_closed_by_user_) {
        return;
    }
    // Asked of the stack, not the pointer: a dismissed dialog stays a valid
    // object for its exit animation, and its address can be reused after.
    const auto& stack = ModalStack::instance();
    lv_obj_t* backdrop =
        update_download_modal_ ? stack.backdrop_for(update_download_modal_) : nullptr;
    if (backdrop && !stack.is_exiting(backdrop)) {
        return;
    }
    // Reopened as-is: the status, text and error are already the failure's, and
    // the modal binds straight to them.
    spdlog::info("[UpdatesSettings] Update failed after its dialog closed - reopening it");
    update_download_modal_ = helix::ui::modal_show("update_download_modal");
}

void UpdatesSettingsOverlay::hide_update_download_modal() {
    download_modal_closed_by_user_ = true;
    if (update_download_modal_) {
        helix::ui::modal_hide(update_download_modal_);
        update_download_modal_ = nullptr;
    }
    // Reset download state
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
}

void UpdatesSettingsOverlay::sync_update_channel_rows(lv_obj_t* root, int effective_channel) {
    if (!root || effective_channel < 0) {
        return;
    }

    const auto selected = static_cast<uint32_t>(effective_channel);

    for (const char* row_name : {"row_update_channel", "row_update_channel_dev"}) {
        lv_obj_t* row = helix::ui::find_optional(root, row_name);
        if (!row) {
            continue;
        }
        lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", "UpdatesSettingsOverlay");
        if (!dropdown) {
            continue;
        }
        // A row too short for this channel is the one that is hidden right now.
        // Skipping it rather than letting LVGL clamp keeps Dev's index 2 from
        // rendering as Beta on the two-entry row if it ever became visible.
        if (selected < lv_dropdown_get_option_count(dropdown)) {
            lv_dropdown_set_selected(dropdown, selected);
        }
    }
}

} // namespace helix::settings
