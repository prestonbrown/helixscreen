// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The visible signal names a printer turned out to need beyond what its config
// calls. A sliced file's start G-code may call HELIX_READY itself; the config
// scan cannot see that, so the first print that does answers
// `Unknown command:"HELIX_READY"`. SlicedSignalWatcher reads that response,
// records the name for the printer, and the next macro update forwards it
// (names_to_alias's `extra_names`).

#include "async_lifetime_guard.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace helix {

class IMoonrakerClient;

namespace signal_macros {

/// The names recorded for the active printer, in kVisibleNames order.
[[nodiscard]] std::vector<std::string> recorded_signal_names();

/// Records a visible signal name for the active printer and saves the
/// settings. False when it was already recorded or is not a signal name.
bool record_signal_name(const std::string& visible);

} // namespace signal_macros

/// Watches the console for a visible signal name Klipper does not know, and
/// records it. Construct and destroy on the main thread.
class SlicedSignalWatcher {
  public:
    /// Called on the main thread once per name per watcher.
    using NoticeFn = std::function<void(const std::string& visible)>;

    SlicedSignalWatcher(IMoonrakerClient* client, NoticeFn notice);
    ~SlicedSignalWatcher();

    SlicedSignalWatcher(const SlicedSignalWatcher&) = delete;
    SlicedSignalWatcher& operator=(const SlicedSignalWatcher&) = delete;

    /// Handles one console line; the main-thread half of the registered callback.
    void on_line(const std::string& line);

  private:
    IMoonrakerClient* client_;
    NoticeFn notice_;
    std::set<std::string> noticed_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
