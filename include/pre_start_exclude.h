#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"
#include "printer_excluded_objects_state.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class IMoonrakerAPI; // NAMESPACE_OK: forward declaration of the global API interface

namespace helix {
class PrinterState;
}

namespace helix::ui {

/// Whether the printer has [exclude_object], read from the capability subject
/// both hosts observe, so a printer switch reaches them together. False for null.
bool printer_has_exclude_object(const PrinterState* printer_state);

/// Whether objects can be skipped: the printer has [exclude_object] and the
/// file is G-code defining at least two objects. Print status asks the same
/// question of a running print.
bool pre_start_exclude_available(bool printer_has_exclude_object, bool is_3mf,
                                 size_t defined_count);

/// The defined spelling of @p name, compared upper-case as Klipper stores
/// object names; "" when no defined object matches.
std::string canonical_object_name(const std::vector<std::string>& defined, const std::string& name);

/// Whether @p picks cover every defined object, counted upper-case the way
/// Klipper would. False when nothing is defined.
bool every_object_picked(const std::vector<std::string>& defined,
                         const std::unordered_set<std::string>& picks);

/// The details object list: the scan's objects in file order, then any the
/// full parse found that the scan did not (compared upper-case).
std::vector<gcode::GCodeObject>
merge_defined_objects(const std::vector<gcode::GCodeObject>& scanned,
                      const gcode::ParsedGCodeFile* parsed);

std::vector<PrinterExcludedObjectsState::ObjectInfo>
object_infos_from(const std::vector<gcode::GCodeObject>& objects);

/// Send EXCLUDE_OBJECT for each picked name. Main thread, after Moonraker
/// confirmed the start: Klipper clears exclude_object state as a print starts.
/// One error toast names whatever failed, unless the print has already ended.
void send_pre_start_exclusions(IMoonrakerAPI* api, std::vector<std::string> names);

/// @p on_confirmed, followed by the exclusions for @p names on the main
/// thread. Callable from any thread. It holds only the names and reads the
/// current API when it fires, so the view that started the print may be gone.
std::function<void()> with_pre_start_exclusions(std::function<void()> on_confirmed,
                                                std::vector<std::string> names);

} // namespace helix::ui
