// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/full_page_host.h"

#include "ui_next_tick.h"
#include "ui_panel_home.h"
#include "ui_utils.h"

#include "overlay_base.h"
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

} // namespace

FullPageHost::FullPageHost(TileSizing& sizing, std::string function,
                           std::function<OverlayBase&()> overlay, EmbedFn embed)
    : function_(std::move(function)), overlay_(std::move(overlay)), embed_(std::move(embed)) {
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
    if (!host || !overlay_) {
        return false;
    }
    OverlayBase& overlay = overlay_();
    return overlay.is_embedded() && lv_obj_get_parent(overlay.get_root()) == host;
}

void FullPageHost::set_placement(bool fills_extra_page, int page_index) {
    page_index_ = page_index;
    if (fills_extra_page == full_page_) {
        return;
    }
    full_page_ = fills_extra_page;
    lv_subject_set_int(&full_page_subject_, full_page_ ? 1 : 0);
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
    if (!active_ || !full_page_ || !host || !overlay_ || hosts_tree()) {
        return;
    }
    OverlayBase& overlay = overlay_();
    if (!(embed_ ? embed_(overlay, host) : overlay.embed(host))) {
        // The overlay is open on the stack. The frame keeps its compact face until
        // the page is shown again.
        return;
    }
    hosting_ = true;
    // Edit mode disarmed the page before this tree existed; disarm it the same
    // way so the session's restore re-arms it with everything else.
    if (tile_root_ && lv_obj_has_flag(tile_root_, helix::ui::kEditClickSuppressedFlag)) {
        helix::ui::disable_widget_clicks_recursive(host);
    }
}

void FullPageHost::release_tree() {
    if (hosts_tree()) {
        overlay_().release_embed();
    }
    hosting_ = false;
}

void FullPageHost::on_tile_deleted() {
    active_ = false;
    if (!std::exchange(hosting_, false)) {
        return;
    }
    // The overlay is a process-lifetime singleton, so the release is bound to no
    // object. A tree embedded again before the tick is someone else's to release.
    helix::ui::run_next_tick([overlay = overlay_]() {
        OverlayBase& o = overlay();
        if (!o.is_embedded()) {
            o.release_embed();
        }
    });
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
