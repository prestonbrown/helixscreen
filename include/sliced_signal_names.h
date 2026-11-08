// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The visible signal names a printer turned out to need beyond what its config
// calls. A sliced file's start G-code may call HELIX_READY itself; the config
// scan cannot see that, so the first print that does answers
// `Unknown command:"HELIX_READY"`. SlicedSignalWatcher reads that response
// during a print, records the names when the print ends, and the next macro
// update forwards them (names_to_alias's `extra_names`).

#include "ui_observer_guard.h"

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

/// Records the visible signal names given for the active printer with one
/// settings save, none when nothing is new. Returns the newly recorded ones in
/// kVisibleNames order; a name that is not a signal name is ignored.
std::vector<std::string> record_signal_names(const std::vector<std::string>& visible);

/// The toast for a print that called @p names. @p update_offered: the helper
/// macros row offers the update that forwards them, so the text points there.
[[nodiscard]] std::string sliced_signal_notice(const std::vector<std::string>& names,
                                               bool update_offered);

} // namespace signal_macros

/// Watches the console for visible signal names Klipper does not know while a
/// print runs, and records them when it ends. Construct and destroy on the
/// main thread.
class SlicedSignalWatcher {
  public:
    /// Called on the main thread when a print ends that called names this
    /// watcher has not announced yet, with every name that print called, in
    /// kVisibleNames order.
    using NoticeFn = std::function<void(const std::vector<std::string>& visible)>;

    SlicedSignalWatcher(IMoonrakerClient* client, NoticeFn notice);
    ~SlicedSignalWatcher();

    SlicedSignalWatcher(const SlicedSignalWatcher&) = delete;
    SlicedSignalWatcher& operator=(const SlicedSignalWatcher&) = delete;

    /// Follows @p print_active (non-zero while the printer holds a job).
    void watch_print_active(lv_subject_t* print_active, const SubjectLifetime& lifetime);

    /// A print started or ended. At the end, the names it called are recorded
    /// and announced.
    void on_print_active(bool active);

    /// The names of `Unknown command` lines; kept only while a print runs.
    void on_unknown_names(const std::vector<std::string>& visible);

  private:
    void finish_print();

    IMoonrakerClient* client_;
    NoticeFn notice_;
    bool print_active_ = false;
    std::set<std::string> pending_;
    std::set<std::string> noticed_;
    ObserverGuard print_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
