// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The visible signal names a printer turned out to need beyond what its config
// calls. A sliced file's start G-code may call HELIX_READY itself; the config
// scan cannot see that, so the first print that does answers
// `Unknown command:"HELIX_READY"`. SlicedSignalWatcher reads that response
// during a print, records the names when the print ends, and the next macro
// update forwards them (names_to_alias's `extra_names`).

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"

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

/// The forwarders no print has used over kAliasDropAfterPrints completed prints.
[[nodiscard]] std::vector<std::string> retired_signal_names();

/// Settles a print against the per-printer quiet counts. A name in @p heard is
/// used, so its count clears. With @p counted, every name in @p loaded that was
/// not heard gains one print. Returns the names that reached the limit in this
/// call; saves once, and only when a count moved.
std::vector<std::string> note_alias_print(const std::vector<std::string>& heard,
                                          const std::vector<std::string>& loaded, bool counted);

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

    /// Reads the printer at a print edge. Unset members mean no print is counted.
    struct PrintProbes {
        std::function<std::vector<std::string>()> announcing_forwarders;
        std::function<bool()> print_just_started; ///< false when joined part-way through
        std::function<bool()> print_completed;
        std::function<void()> aliases_retired; ///< a forwarder reached the drop limit
    };

    SlicedSignalWatcher(IMoonrakerClient* client, NoticeFn notice);
    ~SlicedSignalWatcher();

    SlicedSignalWatcher(const SlicedSignalWatcher&) = delete;
    SlicedSignalWatcher& operator=(const SlicedSignalWatcher&) = delete;

    /// Follows @p print_active (non-zero while the printer holds a job).
    void watch_print_active(lv_subject_t* print_active, const SubjectLifetime& lifetime);

    void set_print_probes(PrintProbes probes);

    /// A print started or ended. At the end, the names it called are recorded
    /// and announced.
    void on_print_active(bool active);

    /// The names of `Unknown command` lines; kept only while a print runs.
    void on_unknown_names(const std::vector<std::string>& visible);

    /// Names forwarders announced; used wherever they ran, inside a print or not.
    void on_alias_names(const std::vector<std::string>& visible);

  private:
    void finish_print();
    void settle_aliases();

    IMoonrakerClient* client_;
    NoticeFn notice_;
    bool print_active_ = false;
    std::set<std::string> pending_;
    std::set<std::string> noticed_;
    std::set<std::string> heard_;
    std::vector<std::string> armed_; ///< forwarders loaded at this print's start
    PrintProbes probes_;
    ObserverGuard print_observer_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
