// SPDX-License-Identifier: GPL-3.0-or-later

#include "material_z_offset_state.h"

#include "app_globals.h"
#include "config.h"
#include "core/subject.h"
#include "i_moonraker_api.h"
#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "state/subject_macros.h"
#include "widget_subjects.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <lvgl.h>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "hv/json.hpp"

namespace helix::material_zoffset {
namespace {

helix::WidgetSubjects s_subjects{"MaterialZOffsetState"};
helix::Subject s_status;
helix::Subject s_enabled;
helix::Subject s_rev;

/// A save whose file changes under it is edited again this many times in all.
constexpr int kEditAttempts = 2;

/// How many config files a search for the section reads at most.
constexpr size_t kMaxScannedFiles = 64;
/// A loaded config file larger than this is not read, and stops the search: it
/// could hold a definition the search would then miss.
constexpr uint64_t kMaxScannedBytes = 1024 * 1024;

std::string enabled_key(const Config& config) {
    return config.df() + "material_z_offset/enabled";
}

const PrinterDiscovery& hw() {
    return get_printer_state().get_discovery();
}

std::string format_mm(int microns) {
    return fmt::format("{:+.3f}", static_cast<double>(microns) / 1000.0);
}

/// "dir/" of a config-root-relative path, "" at the root.
std::string dir_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// The search through printer.cfg and everything it includes. Each pending
/// read holds the Scan; next() holds only a raw pointer, so nothing cycles.
struct Scan : std::enable_shared_from_this<Scan> {
    std::function<void()> next;
    std::map<std::string, uint64_t> files; ///< Every file under the config root, with its size
    /// A loaded file too large to read: the search cannot be complete.
    std::string oversized;
    std::deque<std::string> queue;
    std::set<std::string> visited;
    std::vector<std::string> holders; ///< Files holding the section, once per occurrence
    std::string section;
    helix::SharedCallback<void(const std::string&, const std::string&)> then;
};

} // namespace

MaterialZOffsetState& MaterialZOffsetState::instance() {
    static MaterialZOffsetState s_instance;
    return s_instance;
}

void MaterialZOffsetState::init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_status, 0, "material_zoffset_status", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_enabled, 0, "material_zoffset_enabled", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_rev, 0, "material_zoffset_rev", s_subjects);
    });
    publish();
}

lv_subject_t* MaterialZOffsetState::rev_subject() {
    return s_rev.is_live() ? &s_rev : nullptr;
}

SubjectLifetime MaterialZOffsetState::rev_lifetime() const {
    return s_subjects.get_subjects_lifetime();
}

void MaterialZOffsetState::publish() {
    if (!s_status.is_live()) {
        return;
    }
    lv_subject_set_int(&s_status, static_cast<int>(status()));
    lv_subject_set_int(&s_enabled, enabled() ? 1 : 0);
}

void MaterialZOffsetState::bump() {
    publish();
    if (s_rev.is_live()) {
        lv_subject_set_int(&s_rev, lv_subject_get_int(&s_rev) + 1);
    }
}

void MaterialZOffsetState::on_discovery(const PrinterDiscovery& discovered) {
    table_ = {};
    config_file_.clear();
    if (supported(discovered)) {
        spdlog::info("[MaterialZOffset] Per-material Z offset row: {}{}", provider_name(discovered),
                     write_ready(discovered) ? "" : " (needs setup)");
    }
    bump();
}

void MaterialZOffsetState::update_from_status(const nlohmann::json& status) {
    if (merge_status(hw(), status, table_)) {
        spdlog::debug("[MaterialZOffset] Table: {} offsets, {} meshes, applied '{}' (#{})",
                      table_.offsets.size(), table_.meshes.size(), table_.applied,
                      table_.apply_count);
        bump();
    }
}

Status MaterialZOffsetState::status() const {
    const PrinterDiscovery& discovered = hw();
    if (!supported(discovered)) {
        return Status::None;
    }
    return write_ready(discovered) ? Status::Ready : Status::NeedsSetup;
}

bool MaterialZOffsetState::enabled() const {
    const Config* config = Config::get_instance();
    return config && config->get<bool>(enabled_key(*config), false);
}

void MaterialZOffsetState::set_enabled(IMoonrakerAPI* api, bool on) {
    Config* config = Config::get_instance();
    if (config && on != enabled()) {
        config->set<bool>(enabled_key(*config), on);
        config->save();
        spdlog::info("[MaterialZOffset] Z offset by material {}", on ? "on" : "off");
    }
    // The start macro obeys the printer-side switch, not this screen: turning
    // the setting off has to stop it applying, or a later machine-wide save
    // would fold an offset the next print adds again.
    send_enable(api, on);
    bump();
}

void MaterialZOffsetState::reconcile(IMoonrakerAPI* api) {
    if (enabled() && status() == Status::Ready && table_.enabled != true) {
        send_enable(api, true);
    }
}

void MaterialZOffsetState::send_enable(IMoonrakerAPI* api, bool on) {
    const std::string gcode = enable_gcode(hw(), on);
    if (!api || gcode.empty() || status() != Status::Ready) {
        return;
    }
    api->execute_gcode(
        gcode, nullptr,
        [](const MoonrakerError& err) {
            spdlog::warn("[MaterialZOffset] Could not switch the start macro: {}", err.message);
        },
        0, /*silent=*/true, nullptr, /*caller_surfaces_errors=*/false);
}

bool MaterialZOffsetState::active() const {
    // A row with a printer-side switch is active only once the switch reads on:
    // a value saved while the macro ignores the table would never apply.
    const bool switched = enable_gcode(hw(), true).empty() || table_.enabled == true;
    return enabled() && status() == Status::Ready && switched;
}

bool MaterialZOffsetState::start_macro_applies() const {
    return material_zoffset::start_macro_applies(hw(), table_);
}

std::string MaterialZOffsetState::print_material() const {
    // Where the start macro reports what it applied, that is the entry; else
    // the start macro applied the slicer's first filament.
    return reports_application(hw()) ? table_.applied : print_first_;
}

bool MaterialZOffsetState::material_known() const {
    return reports_application(hw()) ? table_.apply_count > 0 : print_seen_;
}

std::optional<SaveTarget> MaterialZOffsetState::save_target() const {
    if (!active() || !material_known()) {
        return std::nullopt;
    }
    const std::string material = print_material();
    if (!material_key(hw(), material)) {
        return std::nullopt;
    }
    SaveTarget target;
    target.material = material.empty() ? display_name(kDefaultKey) : material;
    target.stored = applied_offset(hw(), table_, material);
    return target;
}

bool MaterialZOffsetState::save_withheld() const {
    return active() && !save_target();
}

void MaterialZOffsetState::note_screen_start(IMoonrakerAPI* api, const std::string& material,
                                             const std::string& filename) {
    if (!api) {
        return;
    }
    // Sent on every screen start, the clearing form when there is nothing to
    // pass, so no earlier hint is left standing.
    const std::string hint = start_hint_gcode(hw(), active() ? material : std::string(), filename);
    if (hint.empty()) {
        return;
    }
    spdlog::info("[MaterialZOffset] Start hint for '{}': {}", filename,
                 active() && !material.empty() ? material : std::string("(cleared)"));
    api->execute_gcode(
        hint, nullptr,
        [](const MoonrakerError& err) {
            spdlog::warn("[MaterialZOffset] Could not pass the material: {}", err.message);
        },
        0, /*silent=*/true, nullptr, /*caller_surfaces_errors=*/false);
}

void MaterialZOffsetState::note_screen_start_failed(IMoonrakerAPI* api) {
    if (!api) {
        return;
    }
    // An empty material is the hint's cleared state.
    const std::string clear = start_hint_gcode(hw(), "", "");
    if (clear.empty()) {
        return;
    }
    api->execute_gcode(clear, nullptr, nullptr, 0, /*silent=*/true, nullptr,
                       /*caller_surfaces_errors=*/false);
}

void MaterialZOffsetState::note_print_file(const std::string& first) {
    if (print_seen_ && first == print_first_) {
        return;
    }
    print_first_ = first;
    print_seen_ = true;
    bump();
}

std::optional<std::pair<std::string, std::string>>
MaterialZOffsetState::mesh_offer(const std::vector<std::string>& materials, int bed_c) const {
    if (!active() || !stores_mesh(hw())) {
        return std::nullopt;
    }
    const std::string material = closest_material(materials, bed_c);
    const std::string profile = mesh_profile_name(hw(), material);
    if (material.empty() || profile.empty()) {
        return std::nullopt;
    }
    return std::make_pair(material, profile);
}

void MaterialZOffsetState::save_offset(IMoonrakerAPI* api, const std::string& material, int microns,
                                       Done done) {
    // One save at a time, whichever surface asked: two edits of the same file
    // interleaved would each upload the other's line away.
    pending_saves_.push_back([this, api, material, microns, done]() {
        run_save(api, material, microns, [this, done](bool ok, const std::string& message) {
            if (done) {
                done(ok, message);
            }
            save_running_ = false;
            run_next_save();
        });
    });
    run_next_save();
}

void MaterialZOffsetState::run_next_save() {
    if (save_running_ || pending_saves_.empty()) {
        return;
    }
    save_running_ = true;
    auto next = std::move(pending_saves_.front());
    pending_saves_.pop_front();
    next();
}

void MaterialZOffsetState::run_save(IMoonrakerAPI* api, const std::string& material, int microns,
                                    Done done) {
    auto plan = write_plan(hw(), table_, material, microns);
    const std::string name = material.empty() ? display_name(kDefaultKey) : material;
    if (!api || !plan) {
        if (done) {
            done(false,
                 fmt::format(fmt::runtime(lv_tr("{} cannot be stored on this printer.")), name));
        }
        return;
    }
    std::string message =
        fmt::format(fmt::runtime(lv_tr("Saved {} mm for {}")), format_mm(microns), name);
    if (!plan->live_now) {
        message += ". ";
        message += lv_tr("It applies after the next Klipper restart.");
    }
    spdlog::info("[MaterialZOffset] Saving {} mm for {} ({})", format_mm(microns), name,
                 provider_name(hw()));

    const bool edits_file = plan->edits_config_file;
    auto after_gcode = [this, api, material, microns, message, edits_file, done]() {
        if (edits_file) {
            write_config_file(api, material, microns, message, done);
        } else if (done) {
            done(true, message);
        }
    };
    if (plan->gcode.empty()) {
        after_gcode();
        return;
    }
    // ERROR_OWNERSHIP_OK: the failure goes to done(), which the Save surface shows.
    api->execute_gcode(plan->gcode, lifetime_.bg_cb("MaterialZOffset::gcode_done", after_gcode),
                       lifetime_.bg_cb("MaterialZOffset::gcode_failed",
                                       [done](const MoonrakerError& err) {
                                           spdlog::error("[MaterialZOffset] Save failed: {}",
                                                         err.message);
                                           if (done) {
                                               done(false, err.localized_message());
                                           }
                                       }),
                       0, false, nullptr, /*caller_surfaces_errors=*/true);
}

void MaterialZOffsetState::write_config_file(IMoonrakerAPI* api, const std::string& material,
                                             int microns, const std::string& message, Done done) {
    const std::string section = config_section(hw());
    auto fail = [done](const std::string& why) {
        spdlog::warn("[MaterialZOffset] Config file not written: {}", why);
        if (done) {
            done(false, fmt::format(fmt::runtime(lv_tr("Applied for this session only: {}")), why));
        }
    };
    locate_config_file(api, [this, api, section, material, microns, message, done,
                             fail](const std::string& path, const std::string& error) {
        if (path.empty()) {
            fail(error);
            return;
        }
        edit_and_upload(api, path, section, material, microns, message, done, fail, kEditAttempts);
    });
}

void MaterialZOffsetState::edit_and_upload(IMoonrakerAPI* api, const std::string& path,
                                           const std::string& section, const std::string& material,
                                           int microns, const std::string& message, Done done,
                                           helix::SharedCallback<void(const std::string&)> fail,
                                           int attempts) {
    // Read fresh: the file may have changed since the search found it.
    api->transfers().download_file(
        "config", path,
        lifetime_.bg_cb(
            "MaterialZOffset::config_read",
            [this, api, path, section, material, microns, message, done, fail,
             attempts](const std::string& content) {
                ConfigEdit edit = edit_config_text(content, section, material, microns);
                if (!edit.text) {
                    config_file_.clear();
                    fail(edit.error);
                    return;
                }
                // Moonraker has no conditional upload, so read once more right
                // before writing: another writer's change since the first read
                // is redone on top of, never uploaded away.
                api->transfers().download_file(
                    "config", path,
                    lifetime_.bg_cb(
                        "MaterialZOffset::config_reread",
                        [this, api, path, section, material, microns, message, done, fail, attempts,
                         content, text = *edit.text](const std::string& now) {
                            if (now != content) {
                                if (attempts > 1) {
                                    spdlog::info("[MaterialZOffset] {} changed while saving; "
                                                 "editing it again",
                                                 path);
                                    edit_and_upload(api, path, section, material, microns, message,
                                                    done, fail, attempts - 1);
                                } else {
                                    fail(fmt::format(
                                        fmt::runtime(lv_tr("{} kept changing while saving")),
                                        path));
                                }
                                return;
                            }
                            api->transfers().upload_file(
                                "config", path, text,
                                lifetime_.bg_cb("MaterialZOffset::config_written",
                                                [path, message, done]() {
                                                    spdlog::info("[MaterialZOffset] Wrote {}",
                                                                 path);
                                                    if (done) {
                                                        done(true, message);
                                                    }
                                                }),
                                lifetime_.bg_cb(
                                    "MaterialZOffset::config_write_failed",
                                    [fail](const MoonrakerError& err) { fail(err.message); }));
                        }),
                    lifetime_.bg_cb("MaterialZOffset::config_reread_failed",
                                    [fail](const MoonrakerError& err) { fail(err.message); }));
            }),
        lifetime_.bg_cb("MaterialZOffset::config_read_failed",
                        [this, fail](const MoonrakerError& err) {
                            config_file_.clear();
                            fail(err.message);
                        }));
}

void MaterialZOffsetState::locate_config_file(
    IMoonrakerAPI* api,
    helix::SharedCallback<void(const std::string& path, const std::string& error)> then) {
    if (!config_file_.empty()) {
        then(config_file_, {});
        return;
    }
    auto scan = std::make_shared<Scan>();
    scan->section = config_section(hw());
    scan->then = then;

    // Walk printer.cfg's includes, so a backup copy of the section that nothing
    // loads (printer-20261118_101500.cfg) does not count as a second one.
    Scan* raw = scan.get();
    raw->next = [this, api, raw]() {
        if (raw->queue.empty() || raw->visited.size() >= kMaxScannedFiles) {
            // A search stopped by the cap has not seen every loaded file, so
            // one holder so far proves nothing: refuse rather than guess.
            if (!raw->oversized.empty()) {
                raw->then({}, fmt::format(fmt::runtime(lv_tr("{} is too large to search for [{}]")),
                                          raw->oversized, raw->section));
                return;
            }
            const bool unread =
                std::any_of(raw->queue.begin(), raw->queue.end(),
                            [raw](const std::string& f) { return raw->visited.count(f) == 0; });
            if (unread) {
                raw->then({}, fmt::format(
                                  fmt::runtime(lv_tr("printer.cfg loads more than {} config files; "
                                                     "[{}] was not searched for in all of them")),
                                  kMaxScannedFiles, raw->section));
                return;
            }
            if (raw->holders.size() == 1) {
                config_file_ = raw->holders.front();
                spdlog::info("[MaterialZOffset] [{}] is in {}", raw->section, config_file_);
                raw->then(config_file_, {});
            } else {
                raw->then({}, fmt::format(
                                  fmt::runtime(lv_tr("[{}] was found {} times in the config files "
                                                     "printer.cfg loads, expected once")),
                                  raw->section, raw->holders.size()));
            }
            return;
        }
        const std::string path = raw->queue.front();
        raw->queue.pop_front();
        if (!raw->visited.insert(path).second) {
            raw->next();
            return;
        }
        if (auto known = raw->files.find(path);
            known != raw->files.end() && known->second > kMaxScannedBytes) {
            raw->oversized = path;
            raw->queue.clear();
            raw->next();
            return;
        }
        auto held = raw->shared_from_this();
        api->transfers().download_file(
            "config", path,
            lifetime_.bg_cb("MaterialZOffset::scan_read",
                            [held, path](const std::string& content) {
                                if (content.size() > kMaxScannedBytes) {
                                    held->oversized = path;
                                    held->queue.clear();
                                    held->next();
                                    return;
                                }
                                for (int i = count_sections(content, held->section); i > 0; --i) {
                                    held->holders.push_back(path);
                                }
                                const std::string dir = dir_of(path);
                                for (const auto& target : include_targets(content)) {
                                    if (target[0] == '/') {
                                        continue; // outside the config root
                                    }
                                    const std::string pattern = dir + target;
                                    if (pattern.find_first_of("*?[") == std::string::npos) {
                                        held->queue.push_back(pattern);
                                        continue;
                                    }
                                    for (const auto& [f, size] : held->files) {
                                        if (include_matches(pattern, f)) {
                                            held->queue.push_back(f);
                                        }
                                    }
                                }
                                held->next();
                            }),
            lifetime_.bg_cb(
                "MaterialZOffset::scan_read_failed", [held, path](const MoonrakerError& err) {
                    spdlog::debug("[MaterialZOffset] Skipping {}: {}", path, err.message);
                    held->next();
                }));
    };

    api->files().list_files(
        "config", "", true,
        lifetime_.bg_cb("MaterialZOffset::scan_list",
                        [scan](const std::vector<FileInfo>& files) {
                            for (const auto& f : files) {
                                if (!f.is_dir) {
                                    scan->files[f.path.empty() ? f.filename : f.path] = f.size;
                                }
                            }
                            scan->queue.push_back("printer.cfg");
                            scan->next();
                        }),
        lifetime_.bg_cb("MaterialZOffset::scan_list_failed", [scan](const MoonrakerError& err) {
            spdlog::warn("[MaterialZOffset] Could not list config files: {}", err.message);
            scan->queue.push_back("printer.cfg");
            scan->next();
        }));
}

} // namespace helix::material_zoffset
