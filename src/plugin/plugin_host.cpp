// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_host.h"

#include "ui_callback_helpers.h"
#include "ui_icon_image.h"
#include "ui_nav.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "core/text_io.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "helix-xml/src/xml/lv_xml_translation.h"
#include "lua_panel_widget.h"
#include "lvgl/lvgl.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "plugin_canvas.h"
#include "plugin_settings_overlay.h"
#include "plugin_table.h"
#include "plugin_xml_policy.h"
#include "translation_loader.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace helix::plugin {

namespace {

PluginHost* g_live_host = nullptr;

// Named for its one job: generic names like join() at file scope register as
// shipped free functions and turn every test helper of the same name into a
// mirror-gate finding.
std::string join_errors(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Reads `<dir>/<name>/manifest.json` into a PluginInfo: Invalid with the
/// joined errors, Invalid for a directory/id mismatch, manifest set otherwise.
PluginInfo read_plugin_info(const std::string& dir, const std::string& name) {
    PluginInfo info;
    info.dir_name = name;
    auto parsed = parse_manifest(read_file(std::filesystem::path(dir) / name / "manifest.json"));
    if (!parsed.manifest) {
        info.status = PluginStatus::Invalid;
        info.reason = join_errors(parsed.errors, "; ");
    } else if (parsed.manifest->id != name) {
        info.status = PluginStatus::Invalid;
        info.reason = "directory name must match id '" + parsed.manifest->id + "'";
    } else {
        info.manifest = std::move(parsed.manifest);
    }
    return info;
}

} // namespace

void register_plugin_event_callback() {
    static bool registered = false;
    if (registered)
        return;
    register_xml_callbacks({
        {"plugin_event",
         [](lv_event_t* e) {
             auto* user_data = static_cast<const char*>(lv_event_get_user_data(e));
             if (g_live_host && user_data)
                 g_live_host->dispatch_event(user_data);
         }},
    });
    register_plugin_settings_callbacks();
    register_plugin_canvas_widget();
    register_plugin_table_widget();
    registered = true;
}

size_t plugin_memory_budget(uint64_t mem_total_bytes) {
    return static_cast<size_t>(std::min<uint64_t>(mem_total_bytes / 16, uint64_t(64) << 20));
}

std::vector<std::string> loadable_plugin_ids(const std::vector<std::string>& candidates,
                                             const std::vector<PluginInfo>& infos) {
    std::vector<std::string> out;
    for (const auto& id : candidates) {
        for (const auto& info : infos) {
            if (info.dir_name != id)
                continue;
            if (info.manifest.has_value() &&
                (info.status == PluginStatus::Loaded || info.status == PluginStatus::Disabled ||
                 info.status == PluginStatus::NeedsApproval))
                out.push_back(id);
            break;
        }
    }
    return out;
}

// TR_NOOP marks the literals for the extractor; the display site calls lv_tr
// on the stored pointer (plugins_overlay.cpp), so the lookup happens at render
// time against the loaded language pack.
const char* plugin_status_name(PluginStatus s) {
    switch (s) {
    case PluginStatus::Disabled:
        return TR_NOOP("disabled");
    case PluginStatus::Loaded:
        return TR_NOOP("loaded");
    case PluginStatus::NeedsApproval:
        return TR_NOOP("needs approval");
    case PluginStatus::Invalid:
        return TR_NOOP("invalid");
    case PluginStatus::Incompatible:
        return TR_NOOP("incompatible");
    case PluginStatus::OverBudget:
        return TR_NOOP("over memory budget");
    case PluginStatus::Faulted:
        return TR_NOOP("faulted");
    }
    return "?";
}

PluginHost::PluginHost(Deps deps) : deps_(std::move(deps)) {
    g_live_host = this;
    layout_hook_ = theme_manager_add_layout_refresh_hook(
        [this](lv_display_t* display) { refresh_layout_constants(display); });
}

void PluginHost::refresh_layout_constants(lv_display_t* display) {
    for (const auto& [id, l] : loaded_)
        register_plugin_consts(l.consts, display);
}

PluginHost* PluginHost::live() {
    return g_live_host;
}

PluginHost::~PluginHost() {
    theme_manager_remove_layout_refresh_hook(layout_hook_);
    unload_all();
    guard_.invalidate();
    if (g_live_host == this)
        g_live_host = nullptr;
}

void PluginHost::load_from(const std::string& dir) {
    unload_all();
    plugins_.clear();
    dir_ = dir;
    bulk_ = true;

    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        // Dot-named directories are the sync's scratch (a half-finished swap's
        // .old-<id>, the .staging download area), never plugin rows.
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.')
            continue;
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "manifest.json", ec))
            names.push_back(name);
    }
    if (ec)
        spdlog::warn("[PluginHost] cannot scan {}: {}", dir, ec.message());
    std::sort(names.begin(), names.end());

    for (const auto& name : names)
        plugins_.push_back(read_plugin_info(dir, name));

    for (auto& info : plugins_) {
        if (info.manifest && info.status == PluginStatus::Disabled)
            consider(info);
    }
    for (const auto& info : plugins_) {
        spdlog::info("[PluginHost] {}: {}{}", info.dir_name, plugin_status_name(info.status),
                     info.reason.empty() ? "" : " (" + info.reason + ")");
    }
    bulk_ = false;
    if (widget_defs_dirty_) {
        PanelWidgetManager::instance().notify_widget_defs_changed();
        widget_defs_dirty_ = false;
    }
}

json PluginHost::enabled_entry(const std::string& id) const {
    json block = deps_.read_block();
    if (!block.is_object())
        return json();
    auto en = block.find("enabled");
    if (en == block.end() || !en->is_object())
        return json();
    auto it = en->find(id);
    return it == en->end() ? json() : *it;
}

size_t PluginHost::memory_in_use() const {
    size_t total = 0;
    for (const auto& [id, l] : loaded_)
        total += l.memory_bytes;
    return total;
}

void PluginHost::consider(PluginInfo& info) {
    const Manifest& m = *info.manifest;
    info.reason.clear();

    bool app_version_known = helix::version::parse_version(deps_.helix_version).has_value();
    if (!m.helix_version.empty() && app_version_known &&
        !helix::version::check_version_constraint(m.helix_version, deps_.helix_version)) {
        info.status = PluginStatus::Incompatible;
        info.reason = "needs HelixScreen " + m.helix_version;
        return;
    }

    json entry = enabled_entry(m.id);
    if (!entry.is_object()) {
        info.status = PluginStatus::Disabled;
        return;
    }

    auto grown = permission_growth(granted(m.id), m.permissions);
    if (!grown.empty()) {
        info.status = PluginStatus::NeedsApproval;
        info.reason = "asks for new permissions:";
        for (Permission p : grown)
            info.reason += std::string(" ") + permission_name(p);
        return;
    }

    size_t want = static_cast<size_t>(m.memory_mb) << 20;
    size_t in_use = memory_in_use();
    if (in_use + want > deps_.memory_budget) {
        size_t left = deps_.memory_budget > in_use ? deps_.memory_budget - in_use : 0;
        info.status = PluginStatus::OverBudget;
        info.reason = "needs " + std::to_string(m.memory_mb) + " MB, " +
                      std::to_string(left >> 20) + " MB left of the plugin budget";
        return;
    }

    info.status = load(info) ? PluginStatus::Loaded : info.status;
}

namespace {

// An i18n/ folder of LVGL translation packs registers into the global table
// LVGL already resolves every string through. Plugin packs sit at the END of
// the lookup order, behind every app pack whenever it was loaded, so a pack
// supplies only the tags the app does not translate and an empty slot in it
// cannot hide an app string. English is the tag itself and has no app pack to
// win against, so a pack declaring `en` is refused. Packs cannot be removed,
// so a path registers once: a plugin re-enable or hot reload reuses what is
// loaded, and an edited pack takes effect on the next app start.
constexpr std::uintmax_t kMaxI18nPackBytes = 256 * 1024;

bool declares_english(const std::string& xml) {
    size_t at = xml.find("languages=\"");
    if (at == std::string::npos)
        return false;
    at += 11;
    std::string langs = " " + xml.substr(at, xml.find('"', at) - at) + " ";
    return langs.find(" en ") != std::string::npos;
}

void register_plugin_translations(const std::filesystem::path& root) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(root / "i18n", ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".xml")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    static std::set<std::string> registered;
    for (const auto& path : files) {
        if (registered.count(path.string()))
            continue;
        std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxI18nPackBytes) {
            spdlog::warn("[PluginHost] translation pack {} skipped: over {} bytes", path.string(),
                         kMaxI18nPackBytes);
            continue;
        }
        auto xml = helix::text_io::read_file(path.string(), kMaxI18nPackBytes);
        if (!xml)
            continue;
        if (declares_english(*xml)) {
            spdlog::warn("[PluginHost] translation pack {} skipped: English is the tag itself, "
                         "drop 'en' from its languages",
                         path.string());
            continue;
        }
        lv_translation_set_new_packs_at_tail(true);
        lv_result_t res = lv_xml_register_translation_from_data(xml->c_str());
        lv_translation_set_new_packs_at_tail(false);
        if (res == LV_RESULT_OK) {
            registered.insert(path.string());
            spdlog::info("[PluginHost] translations registered: {}", path.string());
        }
    }
}

/// `rel` (already a checked plugin-relative path) inside `root`, or empty when a symlink
/// resolves it outside the plugin folder.
std::string path_inside(const std::filesystem::path& root, const std::string& rel) {
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(root, ec);
    if (ec)
        return {};
    const auto full = std::filesystem::weakly_canonical(root / rel, ec);
    if (ec)
        return {};
    auto mismatch = std::mismatch(base.begin(), base.end(), full.begin(), full.end());
    return mismatch.first == base.end() ? full.string() : std::string();
}

} // namespace

bool PluginHost::load(PluginInfo& info) {
    sweep_retired_subjects();
    const Manifest& m = *info.manifest;
    const std::string id = m.id;
    auto root = std::filesystem::path(dir_) / info.dir_name;

    std::vector<std::filesystem::path> xmls;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root / "ui", ec)) {
        if (e.path().extension() == ".xml")
            xmls.push_back(e.path());
    }
    std::sort(xmls.begin(), xmls.end());
    for (const auto& p : xmls) {
        if (!is_owned_name(id, p.stem().string())) {
            info.status = PluginStatus::Invalid;
            info.reason = "component '" + p.stem().string() + "' must be named " + id + "__<name>";
            return false;
        }
    }
    std::vector<std::string> stems;
    std::vector<std::string> buffers;
    stems.reserve(xmls.size());
    buffers.reserve(xmls.size());
    for (const auto& p : xmls) {
        stems.push_back(p.stem().string());
        buffers.push_back(read_file(p));
    }
    // An app callback or an app subject named here would bypass every permission, so the
    // policy runs before anything is registered, and the bytes it checked are the bytes
    // that get registered.
    for (size_t i = 0; i < xmls.size(); ++i) {
        std::string why = check_plugin_xml(id, stems, buffers[i]);
        if (!why.empty()) {
            info.status = PluginStatus::Invalid;
            info.reason = xmls[i].filename().string() + ": " + why;
            return false;
        }
    }
    // A plugin registering an existing name would replace the app's component (and unloading
    // would then remove it), so nothing is registered until every stem is free.
    for (const auto& stem : stems) {
        if (lv_xml_component_get_scope(stem.c_str())) {
            info.status = PluginStatus::Invalid;
            info.reason = "component '" + stem + "' already exists";
            return false;
        }
    }
    // Every declared widget resolves to one of the plugin's own component files, or the
    // home grid would later hand lv_xml_create a name nothing registered.
    for (const WidgetDecl& d : m.widgets) {
        if (std::find(stems.begin(), stems.end(), d.component) == stems.end()) {
            info.status = PluginStatus::Invalid;
            info.reason =
                "widget '" + d.id + "' names component '" + d.component + "', which is not in ui/";
            return false;
        }
    }

    // Constants resolve when a component is created, so they are in place before
    // any of the plugin's components is registered.
    PluginConsts consts = collect_plugin_consts(buffers);
    if (auto errors = missing_tier_errors(consts.px); !errors.empty()) {
        info.status = PluginStatus::Invalid;
        info.reason = errors.front();
        return false;
    }
    register_plugin_consts(consts, lv_display_get_default());

    auto [it, inserted] = loaded_.try_emplace(id);
    Loaded& l = it->second;
    l.gen = next_load_gen_++;
    l.consts = std::move(consts);
    for (size_t i = 0; i < xmls.size(); ++i) {
        if (lv_xml_register_component_from_data(stems[i].c_str(), buffers[i].c_str()) !=
            LV_RESULT_OK) {
            info.status = PluginStatus::Invalid;
            info.reason = "cannot load " + xmls[i].filename().string();
            unload(id);
            return false;
        }
        l.components.emplace_back(stems[i], lv_xml_component_get_scope(stems[i].c_str()));
    }

    // An icon that cannot draw its image draws its fallback; the plugin still loads.
    for (const IconDecl& d : m.icons) {
        const std::string path = path_inside(root, d.src);
        const auto status = helix::ui::icon::register_image_icon(d.name, path, d.fallback);
        l.icons.push_back(d.name);
        if (status != helix::ui::icon::ImageIconStatus::Ok)
            spdlog::warn("[plugin {}] icon '{}' ({}) {}; drawing '{}' instead", id, d.name, d.src,
                         path.empty() ? "is outside the plugin folder"
                         : status == helix::ui::icon::ImageIconStatus::Missing ? "is missing"
                         : status == helix::ui::icon::ImageIconStatus::TooLarge
                             ? "is over 64 KB or 256x256"
                             : "is not a readable PNG",
                         d.fallback);
    }

    json block = deps_.read_block();
    if (block.is_object()) {
        if (auto s = block.find("settings"); s != block.end() && s->is_object()) {
            if (auto e = s->find(id); e != s->end() && e->is_object())
                l.settings = *e;
        }
    }
    if (!l.settings.is_object())
        l.settings = json::object();

    l.memory_bytes = static_cast<size_t>(m.memory_mb) << 20;
    LuaRuntime::Limits limits;
    limits.memory_bytes = l.memory_bytes;
    LifetimeToken token = guard_.token();
    l.rt = std::make_unique<LuaRuntime>(
        id, root.string(), limits, [this, token, id, gen = l.gen](const std::string& reason) {
            token.defer("plugin_fault", [this, id, gen, reason] { on_fault(id, gen, reason); });
        });
    l.ctx = std::make_unique<PluginContext>(
        PluginContext{*l.rt, deps_.backend, m, &l.settings, [this, id] { save_settings(id); },
                      plugin_storage_path(deps_.settings_path, id)});
    l.ctx->setting_changed = [this, id, gen = l.gen](const std::string& key, const json& value) {
        for (auto& screen : settings_screens_) {
            if (screen->plugin_id() == id && screen->load_gen() == gen)
                screen->show_setting(key, value);
        }
    };
    l.ui.open = [this, id](const std::string& component, PluginUi::Callbacks cb,
                           const PluginUi::Attrs& attrs) {
        return surfaces_.open_overlay(id, component, std::move(cb), attrs);
    };
    l.ui.dialog = [this, id](const std::string& component, PluginUi::Callbacks cb,
                             const PluginUi::Attrs& attrs) {
        return surfaces_.open_dialog(id, component, std::move(cb), attrs);
    };
    l.ui.open_count = [this](const std::string& plugin_id) {
        return surfaces_.overlay_count(plugin_id);
    };
    l.ui.dialog_count = [this](const std::string& plugin_id) {
        return surfaces_.dialog_count(plugin_id);
    };
    l.ui.close = [this](int handle) { surfaces_.close(handle); };
    l.ui.is_open = [this](int handle) { return surfaces_.is_open(handle); };
    l.ui.queue_confirm = [this, id](std::function<bool()> present,
                                    std::function<void()> cancelled) {
        surfaces_.queue_confirm(id, std::move(present), std::move(cancelled));
    };
    l.ctx->ui = &l.ui;
    register_plugin_translations(root);
    for (Installer install :
         {&install_core_bindings, &install_ui_bindings, &install_printer_bindings,
          &install_moonraker_bindings, &install_io_bindings, &install_widget_bindings,
          &install_canvas_bindings, &install_sound_bindings, &install_table_bindings})
        install(*l.ctx);

    if (!l.rt->run_file("main.lua")) {
        info.status = PluginStatus::Faulted;
        info.reason = l.rt->faulted() ? l.rt->fault_reason() : "main.lua failed; see the log";
        unload(id);
        return false;
    }

    for (const WidgetDecl& d : m.widgets) {
        helix::RuntimeWidgetDef def;
        def.id = d.id;
        def.display_name = d.name;
        def.icon = d.icon.empty() ? kDefaultPluginIcon : d.icon;
        def.description = d.description;
        def.colspan = d.colspan;
        def.rowspan = d.rowspan;
        def.max_colspan = d.max_colspan;
        def.max_rowspan = d.max_rowspan;
        def.supports_half_col = d.half_cells;
        def.supports_half_row = d.half_cells;
        def.factory = [pid = id, wid = d.id, comp = d.component,
                       tok = l.rt->token()](const std::string&) {
            return std::make_unique<LuaPanelWidget>(pid, wid, comp, tok);
        };
        if (helix::register_runtime_widget_def(std::move(def))) {
            l.widget_ids.push_back(d.id);
        } else {
            spdlog::warn("[PluginHost] {}: widget id '{}' is taken", id, d.id);
        }
    }
    if (!l.widget_ids.empty()) {
        widget_defs_dirty_ = true;
        if (!bulk_) {
            helix::PanelWidgetManager::instance().notify_widget_defs_changed();
            widget_defs_dirty_ = false;
        }
    }
    return true;
}

void PluginHost::unload(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    Loaded& l = it->second;
    if (l.rt && !l.rt->faulted()) {
        l.ctx->unloading = true;
        lua_State* L = l.rt->state();
        lua_getglobal(L, "on_unload");
        if (lua_isfunction(L, -1)) {
            int ref = l.rt->ref_value(L, -1);
            lua_pop(L, 1);
            l.rt->invoke(ref);
            l.rt->unref(ref);
        } else {
            lua_pop(L, 1);
        }
    }
    // Overlays go before the runtime does too, and silently: the plugin's on_close
    // hooks point at a Lua state that is about to close.
    surfaces_.drop_plugin(id);
    overlays_.close_all(id);
    // Its generated settings screen leaves the same way, through navigation, so
    // the close callback that erases it runs on every path.
    close_settings_screens(id);
    // Widget definitions go before the runtime does: the async home rebuild this
    // schedules dereferences nothing of the plugin's, and the tiles it retires are
    // handed to deferred deletion while their subjects are still alive.
    if (!l.widget_ids.empty()) {
        for (const auto& wid : l.widget_ids)
            helix::unregister_runtime_widget_def(wid);
        l.widget_ids.clear();
        widget_defs_dirty_ = true;
        if (!bulk_) {
            helix::PanelWidgetManager::instance().notify_widget_defs_changed();
            widget_defs_dirty_ = false;
        }
    }
    l.rt.reset();
    for (const auto& name : l.icons)
        helix::ui::icon::unregister_image_icon(name);
    for (const auto& [name, scope] : l.components) {
        if (lv_xml_component_find_scope(name.c_str()) == scope)
            lv_xml_component_unregister(name.c_str());
    }
    l.ctx.reset();
    loaded_.erase(it);
    sweep_retired_subjects();
}

void PluginHost::unload_all() {
    std::vector<std::string> ids;
    for (const auto& [id, l] : loaded_)
        ids.push_back(id);
    // No notify at the end: shutdown tears the UI down after this, and a printer switch
    // reloads through load_from, whose own end-of-scan notify covers what went here.
    // widget_defs_dirty_ stays set for that caller.
    bulk_ = true;
    for (const auto& id : ids)
        unload(id);
    bulk_ = false;
}

void PluginHost::rescan(const std::vector<std::string>& ids) {
    if (dir_.empty()) {
        spdlog::warn("[PluginHost] rescan before any load_from; nothing to do");
        return;
    }
    bulk_ = true;
    for (const auto& id : ids) {
        // The ids come from a directory listing or a caller; only the id grammar
        // keeps them inside dir_ (a "../x" would resolve outside it).
        if (!is_valid_plugin_id(id)) {
            spdlog::warn("[PluginHost] ignoring invalid plugin id '{}'", id);
            continue;
        }
        unload(id);
        auto it = std::find_if(plugins_.begin(), plugins_.end(),
                               [&](const PluginInfo& p) { return p.dir_name == id; });
        std::error_code ec;
        bool present =
            std::filesystem::exists(std::filesystem::path(dir_) / id / "manifest.json", ec);
        if (!present) {
            if (it != plugins_.end())
                plugins_.erase(it);
            spdlog::info("[PluginHost] {}: removed", id);
            continue;
        }
        PluginInfo info = read_plugin_info(dir_, id);
        if (it != plugins_.end()) {
            *it = std::move(info);
        } else {
            auto pos = std::lower_bound(
                plugins_.begin(), plugins_.end(), id,
                [](const PluginInfo& p, const std::string& n) { return p.dir_name < n; });
            it = plugins_.insert(pos, std::move(info));
        }
        if (it->manifest && it->status == PluginStatus::Disabled)
            consider(*it);
        spdlog::info("[PluginHost] {}: {}{}", it->dir_name, plugin_status_name(it->status),
                     it->reason.empty() ? "" : " (" + it->reason + ")");
    }
    bulk_ = false;
    if (widget_defs_dirty_) {
        PanelWidgetManager::instance().notify_widget_defs_changed();
        widget_defs_dirty_ = false;
    }
}

void PluginHost::on_fault(const std::string& id, uint64_t gen, const std::string& reason) {
    auto it = loaded_.find(id);
    if (it == loaded_.end() || it->second.gen != gen)
        return; // unloaded by the load path, or a newer load of the same id
    unload(id);
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Faulted;
        info->reason = reason;
        std::string detail = info->manifest->name + ": " + reason;
        ToastManager::instance().show_with_detail(ToastSeverity::WARNING, lv_tr("Plugin disabled"),
                                                  detail.c_str());
    }
}

void PluginHost::save_settings(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["settings"].is_object())
        block["settings"] = json::object();
    block["settings"][id] = it->second.settings;
    deps_.write_block(block);
}

PluginInfo* PluginHost::find(const std::string& id) {
    for (auto& p : plugins_) {
        if (p.manifest && p.manifest->id == id && p.dir_name == id)
            return &p;
    }
    return nullptr;
}

LuaRuntime* PluginHost::runtime(const std::string& id) {
    auto it = loaded_.find(id);
    return it == loaded_.end() ? nullptr : it->second.rt.get();
}

bool PluginHost::enable(const std::string& id) {
    PluginInfo* info = find(id);
    if (!info || info->status == PluginStatus::Invalid)
        return false;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["enabled"].is_object())
        block["enabled"] = json::object();
    json perms = json::array();
    for (Permission p : info->manifest->permissions)
        perms.push_back(permission_name(p));
    block["enabled"][id] = {{"version", info->manifest->version}, {"permissions", perms}};
    deps_.write_block(block);
    unload(id);
    info->status = PluginStatus::Disabled;
    consider(*info);
    return info->status == PluginStatus::Loaded;
}

PermissionSet PluginHost::granted(const std::string& id) const {
    PermissionSet out;
    json entry = enabled_entry(id);
    if (!entry.is_object())
        return out;
    auto p = entry.find("permissions");
    if (p == entry.end() || !p->is_array())
        return out;
    for (const auto& n : *p) {
        if (!n.is_string())
            continue;
        if (auto perm = permission_from_string(n.get<std::string>()))
            out.insert(*perm);
    }
    return out;
}

void PluginHost::disable(const std::string& id) {
    unload(id);
    json block = deps_.read_block();
    if (block.is_object() && block.contains("enabled") && block["enabled"].is_object()) {
        block["enabled"].erase(id);
        deps_.write_block(block);
    }
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Disabled;
        info->reason.clear();
    }
}

namespace {

LuaRuntime::PushFn push_context(const json& context) {
    return [&context](lua_State* co) {
        push_json(co, context);
        return 1;
    };
}

/// What a menu check decided for one item.
struct CheckVerdict {
    bool valid = false;
    bool visible = false;
    bool enabled = true;
    std::string reason;
};

/// Reads a check's return value: a boolean, or a plain table whose `visible` and `enabled`
/// are booleans (absent means true) and whose `reason` is a string (absent means none).
/// call_sync runs it between the check's resume and closing its thread, outside any
/// protected call, so nothing here may run plugin code: an error would reach the panic
/// handler. Fields are read raw, and a table with a metatable is not a valid result.
CheckVerdict read_check_value(lua_State* co, int nres) {
    CheckVerdict v;
    if (nres < 1)
        return v;
    const int at = lua_absindex(co, -nres);
    if (lua_type(co, at) == LUA_TBOOLEAN) {
        v.valid = true;
        v.visible = lua_toboolean(co, at);
        return v;
    }
    if (lua_type(co, at) != LUA_TTABLE)
        return v;
    if (lua_getmetatable(co, at)) {
        lua_pop(co, 1);
        return v;
    }
    auto raw_field = [&](const char* key) {
        lua_pushstring(co, key);
        return lua_rawget(co, at);
    };
    auto flag = [&](const char* key, bool& out) {
        int t = raw_field(key);
        bool ok = t == LUA_TNIL || t == LUA_TBOOLEAN;
        out = t == LUA_TNIL || lua_toboolean(co, -1);
        lua_pop(co, 1);
        return ok;
    };
    bool ok = flag("visible", v.visible) && flag("enabled", v.enabled);
    int t = raw_field("reason");
    if (t == LUA_TSTRING)
        v.reason = lua_tostring(co, -1);
    lua_pop(co, 1);
    v.valid = ok && (t == LUA_TNIL || t == LUA_TSTRING);
    return v;
}

} // namespace

bool PluginHost::menu_declared(MenuKind kind, std::string_view plugin_id) const {
    for (const auto& [id, l] : loaded_) {
        if (!l.rt || l.rt->faulted() || (!plugin_id.empty() && id != plugin_id))
            continue;
        const auto& items = l.ctx->manifest.menu_items;
        if (std::any_of(items.begin(), items.end(),
                        [kind](const MenuItemDecl& d) { return d.menu == kind; }))
            return true;
    }
    return false;
}

std::vector<plugin_menu::Entry> PluginHost::resolve_menu(MenuKind kind, const json& context,
                                                         std::string_view only_plugin) {
    std::vector<plugin_menu::Entry> out;
    const size_t cap = static_cast<size_t>(std::max(0, plugin_menu::row_cap(kind)));
    size_t dropped = 0;
    // plugins_ is in load order; loaded_ holds the runtimes.
    for (const auto& info : plugins_) {
        if (!only_plugin.empty() && info.dir_name != only_plugin)
            continue;
        auto it = loaded_.find(info.dir_name);
        if (it == loaded_.end())
            continue;
        Loaded& l = it->second;
        const Manifest& m = l.ctx->manifest;
        for (const auto& d : m.menu_items) {
            if (d.menu != kind)
                continue;
            if (!l.rt || l.rt->faulted())
                break;
            if (out.size() >= cap) {
                ++dropped;
                continue;
            }
            plugin_menu::Entry e;
            e.plugin_id = m.id;
            e.plugin_name = lv_tr(m.name.c_str());
            e.item_id = d.id;
            e.label = lv_tr(d.label.c_str());
            e.icon = d.icon;
            e.gen = l.gen;
            if (!d.check.empty()) {
                CheckVerdict v;
                if (!call_ui_handler_sync(
                        *l.rt, d.check, push_context(context),
                        [&v](lua_State* co, int nres) { v = read_check_value(co, nres); }))
                    continue;
                if (!v.valid) {
                    l.rt->report_error(
                        "menu check '" + d.check +
                        "' must return a boolean or a plain table {visible = bool, enabled = "
                        "bool, reason = string}");
                    continue;
                }
                if (!v.visible)
                    continue;
                e.enabled = v.enabled;
                if (!v.enabled && !v.reason.empty())
                    e.reason = lv_tr(v.reason.c_str());
            }
            out.push_back(std::move(e));
        }
    }
    if (dropped > 0)
        spdlog::debug("[PluginHost] {} menu: {} plugin row(s) past the cap of {} not shown",
                      menu_kind_name(kind), dropped, cap);
    return out;
}

void PluginHost::activate_menu(const plugin_menu::Entry& e, const json& context) {
    auto it = loaded_.find(e.plugin_id);
    if (it == loaded_.end() || it->second.gen != e.gen || !it->second.rt ||
        it->second.rt->faulted()) {
        spdlog::debug("[PluginHost] menu item '{}' of '{}' is gone; ignoring the tap", e.item_id,
                      e.plugin_id);
        return;
    }
    const auto& items = it->second.ctx->manifest.menu_items;
    auto d = std::find_if(items.begin(), items.end(),
                          [&](const MenuItemDecl& m) { return m.id == e.item_id; });
    if (d == items.end())
        return;
    if (!dispatch_ui_handler(*it->second.rt, d->handler, push_context(context)))
        spdlog::debug("[PluginHost] plugin '{}' has no handler '{}'", e.plugin_id, d->handler);
}

void PluginHost::dispatch_event(std::string_view user_data) {
    PluginEventTarget target = parse_plugin_event(user_data);
    if (target.id.empty()) {
        spdlog::debug("[PluginHost] ignoring malformed plugin_event '{}'", user_data);
        return;
    }
    LuaRuntime* rt = runtime(target.id);
    if (!rt) {
        spdlog::debug("[PluginHost] plugin_event for '{}', which is not loaded", target.id);
        return;
    }
    if (!dispatch_ui_handler(*rt, target.name, target.arg))
        spdlog::debug("[PluginHost] plugin '{}' has no handler '{}'", target.id, target.name);
}

bool PluginHost::open_settings(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return false;
    const Manifest& m = it->second.ctx->manifest;
    if (!m.settings_overlay.empty())
        return overlays_.open(id, m.settings_overlay, {}) != 0;
    if (m.settings.empty())
        return false;

    auto screen =
        std::make_unique<PluginSettingsOverlay>(id, m, it->second.settings, it->second.gen);
    lv_obj_t* root = screen->create(lv_screen_active());
    if (!root)
        return false;
    PluginSettingsOverlay* raw = screen.get();
    settings_screens_.push_back(std::move(screen));

    overlays_.push(root, raw, [this, raw] {
        raw->on_nav_closed();
        for (auto it = settings_screens_.begin(); it != settings_screens_.end(); ++it) {
            if (it->get() == raw) {
                settings_screens_.erase(it);
                break;
            }
        }
    });
    return true;
}

bool PluginHost::set_setting(const std::string& id, const std::string& key, const json& value) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return false;
    return set_plugin_setting(*it->second.ctx, key, value);
}

PluginSettingsOverlay* PluginHost::settings_screen(const std::string& id) {
    for (auto& s : settings_screens_)
        if (s->plugin_id() == id)
            return s.get();
    return nullptr;
}

bool PluginHost::owns_row_binding(const void* ud) {
    for (auto& s : settings_screens_)
        if (s->binding_at(ud))
            return true;
    return false;
}

void PluginHost::close_settings_screens(const std::string& id) {
    // Newest first, matching pop order. Every screen leaves through navigation:
    // the on-top root takes go_back's restore path, a buried one is dropped by
    // close_overlay itself, and either way the close callback erases the screen.
    for (auto it = settings_screens_.rbegin(); it != settings_screens_.rend(); ++it) {
        if ((*it)->plugin_id() != id)
            continue;
        helix::nav::close_overlay((*it)->root());
    }
}

void PluginHost::handle_setting_row_event(lv_event_t* e, bool action) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (lv_obj_t* obj = target; obj; obj = lv_obj_get_parent(obj)) {
        void* ud = lv_obj_get_user_data(obj);
        if (!ud)
            continue;
        for (auto& s : settings_screens_) {
            if (auto* b = s->binding_at(ud)) {
                // A screen outlives its plugin until its close lands; a plugin
                // reloaded under the same id must not take its rows.
                auto lit = loaded_.find(s->plugin_id());
                if (lit == loaded_.end() || lit->second.gen != s->load_gen())
                    return;
                if (action)
                    s->on_row_action(*b);
                else
                    s->on_row_changed(*b, target);
                return;
            }
        }
    }
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
