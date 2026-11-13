// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/full_page_host.h"

#include "ui_obj_flags.h"
#include "ui_panel_home.h"
#include "ui_utils.h"

#include "src/ui/panel_widgets/tile_sizing.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace helix {

namespace {

/// Every live host, for jump lookups. Main thread only.
std::vector<FullPageHost*>& live_hosts() {
    static std::vector<FullPageHost*> hosts;
    return hosts;
}

/// Give back the clicks edit mode took from a tree leaving the page: a lent tree
/// lives on outside it, where the session's restore never reaches.
void rearm_clicks(lv_obj_t* root) {
    if (!root) {
        return;
    }
    if (helix::ui::obj_has_flags(root, helix::ui::kEditClickSuppressedFlag)) {
        helix::ui::obj_set_flags(root, helix::ui::kEditClickSuppressedFlag, false);
        lv_obj_set_clickable(root, true);
    }
    helix::ui::enable_widget_clicks_recursive(root);
}

} // namespace

FullPageHost::FullPageHost(TileSizing& sizing, std::string function,
                           std::function<FullPageContent&()> content, EmbedFn embed)
    : function_(std::move(function)), content_(std::move(content)), embed_(std::move(embed)) {
    // Published before the manager parses the tile's component, which drops a
    // binding whose subject is missing at parse time.
    lv_subject_init_int(&full_page_subject_, 0);
    sizing.publish_instance_subject(subjects_, &full_page_subject_, "full_page_subject",
                                    "full_page");
    live_hosts().push_back(this);
}

FullPageHost::~FullPageHost() {
    auto& hosts = live_hosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), this), hosts.end());
    release_tree();
}

lv_obj_t* FullPageHost::body() const {
    return tile_root_ ? lv_obj_find_by_name(tile_root_, "full_page_body") : nullptr;
}

bool FullPageHost::hosts_tree() const {
    lv_obj_t* host = body();
    if (!host || !content_) {
        return false;
    }
    lv_obj_t* root = content_().embedded_root();
    return root && lv_obj_get_parent(root) == host;
}

void FullPageHost::set_placement(bool fills_extra_page, int page_index) {
    page_index_ = page_index;
    if (fills_extra_page == full_page_) {
        return;
    }
    full_page_ = fills_extra_page;
    publish_state();
    spdlog::debug("[FullPageHost] '{}' {} on page {}", function_,
                  full_page_ ? "is a full page" : "is a compact tile", page_index_);
    if (full_page_) {
        embed_if_shown();
    } else {
        release_tree();
    }
}

void FullPageHost::attach(lv_obj_t* tile_root) {
    tile_root_ = tile_root;
}

void FullPageHost::detach() {
    release_tree();
    active_ = false;
    tile_root_ = nullptr;
}

void FullPageHost::activate() {
    spdlog::trace("[FullPageHost] '{}' page {} shown (full page: {})", function_, page_index_,
                  full_page_);
    active_ = true;
    embed_if_shown();
}

void FullPageHost::deactivate() {
    spdlog::trace("[FullPageHost] '{}' page {} hidden", function_, page_index_);
    active_ = false;
    release_tree();
}

void FullPageHost::embed_if_shown() {
    lv_obj_t* host = body();
    if (!active_ || !full_page_ || !host || !content_ || hosts_tree()) {
        return;
    }
    FullPageContent& content = content_();
    if (!(embed_ ? embed_(content, host) : content.embed(host))) {
        // The view is open elsewhere. The frame keeps its placeholder until the
        // page is shown again.
        return;
    }
    hosting_ = true;
    publish_state();
    // Edit mode disarmed the page before this tree existed; disarm it the same
    // way so the session's restore re-arms it with everything else.
    if (tile_root_ && helix::ui::obj_has_flags(tile_root_, helix::ui::kEditClickSuppressedFlag)) {
        helix::ui::disable_widget_clicks_recursive(host);
    }
}

void FullPageHost::release_tree() {
    if (hosts_tree()) {
        FullPageContent& content = content_();
        rearm_clicks(content.embedded_root());
        content.release_embed();
    }
    hosting_ = false;
    publish_state();
}

void FullPageHost::publish_state() {
    const FullPageState state = !full_page_ ? FullPageState::Compact
                                : hosting_  ? FullPageState::Hosting
                                            : FullPageState::Empty;
    lv_subject_set_int(&full_page_subject_, static_cast<int>(state));
}

void FullPageHost::on_tile_deleted() {
    active_ = false;
    if (!std::exchange(hosting_, false)) {
        return;
    }
    FullPageContent& content = content_();
    rearm_clicks(content.embedded_root());
    content.host_deleting();
}

std::optional<int> full_page_for(std::string_view function) {
    for (const FullPageHost* host : live_hosts()) {
        if (host->is_jump_target() && host->function() == function) {
            return host->page_index();
        }
    }
    return std::nullopt;
}

bool jump_to_full_page(std::string_view function) {
    const std::optional<int> page = full_page_for(function);
    if (!page) {
        return false;
    }
    return get_global_home_panel().show_page(*page);
}

} // namespace helix
