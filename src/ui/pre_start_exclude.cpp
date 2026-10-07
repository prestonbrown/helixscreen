// SPDX-License-Identifier: GPL-3.0-or-later

#include "pre_start_exclude.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <optional>

namespace helix::ui {

bool printer_has_exclude_object(const PrinterState* printer_state) {
    return printer_state && lv_subject_get_int(printer_state->capabilities_state().subject(
                                Capability::HasExcludeObject)) != 0;
}

bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf,
                                 size_t defined_count) {
    return printer_has_exclude_object && !is_3mf && defined_count >= 2;
}

std::string canonical_object_name(const std::vector<std::string>& defined,
                                  const std::string& name) {
    const std::string wanted = helix::text_io::to_upper(name);
    for (const auto& d : defined) {
        if (helix::text_io::to_upper(d) == wanted) {
            return d;
        }
    }
    return {};
}

bool every_object_picked(const std::vector<std::string>& defined,
                         const std::unordered_set<std::string>& picks) {
    if (defined.empty()) {
        return false;
    }
    std::unordered_set<std::string> picked;
    for (const auto& p : picks) {
        picked.insert(helix::text_io::to_upper(p));
    }
    return std::all_of(defined.begin(), defined.end(), [&](const std::string& d) {
        return picked.count(helix::text_io::to_upper(d)) > 0;
    });
}

std::vector<gcode::GCodeObject>
merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                      const gcode::ParsedGCodeFile* parsed) {
    std::vector<gcode::GCodeObject> out = scanned;
    if (!parsed) {
        return out;
    }
    for (const auto& [name, obj] : parsed->objects) {
        const std::string upper = helix::text_io::to_upper(name);
        const bool known = std::any_of(out.begin(), out.end(), [&](const gcode::GCodeObject& o) {
            return helix::text_io::to_upper(o.name) == upper;
        });
        if (!known) {
            out.push_back(obj);
        }
    }
    return out;
}

std::vector<PrinterExcludedObjectsState::ObjectInfo>
object_infos_from(const std::vector<gcode::GCodeObject>& objects) {
    std::vector<PrinterExcludedObjectsState::ObjectInfo> out;
    out.reserve(objects.size());
    for (const auto& o : objects) {
        // A zero centre is the parser's "none", the same reading compute_object_badges() uses.
        std::optional<glm::vec2> center;
        if (o.center != glm::vec2(0.0f)) {
            center = o.center;
        }
        out.push_back(PrinterExcludedObjectsState::make_object_info(o.name, center, o.polygon));
    }
    return out;
}

namespace {

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += n;
    }
    return out;
}

void report_unsent(const std::vector<std::string>& names) {
    const PrintState state = get_printer_state().print_state().get_print_lifecycle();
    if (state == PrintState::Cancelled || state == PrintState::Error) {
        spdlog::info("[PreStartExclude] Print ended before {} could be skipped", join_names(names));
        return;
    }
    NOTIFY_ERROR(lv_tr("Could not skip {}"), join_names(names));
}

} // namespace

void send_pre_start_exclusions(IMoonrakerAPI* api, std::vector<std::string> names) {
    if (names.empty()) {
        return;
    }
    if (!api) {
        spdlog::warn("[PreStartExclude] No printer API to send {} picks", names.size());
        report_unsent(names);
        return;
    }

    // Answers come back on the HTTP thread; each one hops to the main thread,
    // where this batch is only ever touched.
    struct Batch {
        size_t pending = 0;
        std::vector<std::string> failed;
    };
    auto batch = std::make_shared<Batch>();
    batch->pending = names.size();
    auto settle = [batch](std::string failed_name) {
        if (!failed_name.empty()) {
            batch->failed.push_back(std::move(failed_name));
        }
        if (--batch->pending > 0 || batch->failed.empty()) {
            return;
        }
        report_unsent(batch->failed);
    };

    for (const auto& name : names) {
        spdlog::info("[PreStartExclude] Skipping '{}' in the print that just started", name);
        api->exclude_object(
            name,
            [settle]() {
                helix::ui::queue_update("PreStartExclude::sent", [settle]() { settle({}); });
            },
            [settle, name](const MoonrakerError& err) {
                const bool failed = !err.command_may_still_run();
                spdlog::warn("[PreStartExclude] EXCLUDE_OBJECT '{}' answered: {}", name,
                             err.message);
                helix::ui::queue_update("PreStartExclude::failed", [settle, name, failed]() {
                    settle(failed ? name : std::string{});
                });
            });
    }
}

std::function<void()> with_pre_start_exclusions(std::function<void()> on_confirmed,
                                                std::vector<std::string> names) {
    if (names.empty()) {
        return on_confirmed;
    }
    return [on_confirmed = std::move(on_confirmed), names = std::move(names)]() {
        if (on_confirmed) {
            on_confirmed();
        }
        helix::ui::queue_update("PreStartExclude::send", [names]() {
            send_pre_start_exclusions(get_moonraker_api(), names);
        });
    };
}

} // namespace helix::ui
