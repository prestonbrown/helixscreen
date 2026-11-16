// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pausing a running print at a layer boundary (prestonbrown/helixscreen#1703).
//
// A slicer bakes M600/PAUSE into the file at slice time; gcode_pause_scan.h
// finds those. This is the other kind: a pause the user schedules into a print
// that is already running, for a file with no pause in it.
//
// HelixScreen implements none of it. Printers that offer it define two macros,
// SET_PAUSE_NEXT_LAYER and SET_PAUSE_AT_LAYER, and a wrapper around
// SET_PRINT_STATS_INFO that fires the pause when the slicer's per-layer
// SET_PRINT_STATS_INFO CURRENT_LAYER=N call arrives. Without those macros the
// feature does not exist and the UI stays hidden. The known sources (the
// Mainsail and Fluidd client.cfg, and the U1's stock firmware, which ships the
// Fluidd one) agree on every name and parameter:
//
//   SET_PAUSE_NEXT_LAYER [ENABLE=0|1] [MACRO=<name>]
//   SET_PAUSE_AT_LAYER   [ENABLE=0|1] [LAYER=<n>] [MACRO=<name>]
//
// and keep the schedule as two variables on the wrapper macro:
//
//   pause_next_layer: { enable, call }
//   pause_at_layer:   { enable, layer, call }
//
// so the schedule reads back from `gcode_macro SET_PRINT_STATS_INFO`. A config
// whose wrapper keeps no such variables falls back to what this screen sent.
// Any source that ever differs is handled here and nowhere else.
//
// Layer numbers are the slicer's: the pause at layer N fires when the file's
// SET_PRINT_STATS_INFO CURRENT_LAYER=N line runs, the same value Klipper
// reports as print_stats.info.current_layer. A print whose slicer emits no
// such lines cannot fire either macro.

#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::layer_pause {

/// Whether the printer defines both scheduling macros.
bool supported(const PrinterDiscovery& hw);

/// The status object holding the schedule, as printer.cfg spells it (Klipper
/// never matches a subscription case-insensitively). Empty when unsupported or
/// when the wrapper macro is absent.
std::string state_object(const PrinterDiscovery& hw);

/// What discovery must subscribe to for the schedule to read back.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Gcode for each action. MACRO= is never sent, so the printer's configured
/// pause command (PAUSE or M600) stays in charge.
std::string schedule_next_layer_command();
std::string cancel_next_layer_command();
std::string schedule_at_layer_command(int layer);
std::string cancel_at_layer_command();

/// The pauses the printer will take.
struct Schedule {
    bool next_layer{false};
    int at_layer{0}; ///< 0 = none
    bool any() const {
        return next_layer || at_layer > 0;
    }
    bool operator==(const Schedule& o) const {
        return next_layer == o.next_layer && at_layer == o.at_layer;
    }
    bool operator!=(const Schedule& o) const {
        return !(*this == o);
    }
};

/// The schedule a status frame's state object reports, merged onto
/// @p previous: Moonraker sends only the variables that changed, so an absent
/// variable keeps its previous value. nullopt when the object carries neither
/// variable, which is a wrapper that keeps no readable state.
std::optional<Schedule> parse_state(const nlohmann::json& object, const Schedule& previous);

/// The layer the next pause lands on: the next layer when that is scheduled,
/// else the chosen layer while it is still ahead of @p current_layer. 0 = none.
int pending_layer(const Schedule& schedule, int current_layer);

/**
 * @brief The pending schedule, from the macros' state or from what was sent.
 *
 * Read-back state wins whenever the printer publishes it. Without it, the
 * commands this screen sent stand in, and expire on their own: the macros
 * clear themselves when they fire, which only read-back would report.
 */
class Tracker {
  public:
    /// A new print: what was sent belongs to the previous one. Read-back is
    /// kept, since the macros keep their state across prints too.
    void reset_for_new_print();

    /// A different printer or state object: nothing known still applies.
    void reset() {
        *this = Tracker{};
    }

    /// Record a command as sent. Read-back, when present, is updated at once
    /// so the UI does not flicker back until the next status frame.
    void note_next_layer(bool enable, int current_layer);
    void note_at_layer(int layer); ///< 0 = cancel

    /// Feed the state object from a status frame. True when the effective
    /// schedule changed.
    bool apply_status(const nlohmann::json& object);

    /// Feed the reported layer, which expires sent-only entries the print has
    /// passed. True when the effective schedule changed.
    bool on_layer(int current_layer);

    /// The print just paused at @p current_layer. The macros pause BEFORE the
    /// layer report goes through, so the layer still reads one short: a sent
    /// next-layer pause, or one for the layer after this, is the pause that
    /// just happened. True when the effective schedule changed.
    bool on_paused(int current_layer);

    Schedule effective() const {
        return readback_ ? *readback_ : sent_;
    }
    bool has_readback() const {
        return readback_.has_value();
    }

  private:
    Schedule sent_;
    int next_layer_sent_at_{0};
    std::optional<Schedule> readback_;
};

/// Whether the scheduling UI can act right now. Ordered: the UI shows the
/// first reason that applies.
enum class Availability : int {
    Hidden = 0,      ///< no print running (or printer lacks the macros)
    NoLayerData = 1, ///< the slicer reports no layers, so neither macro can fire
    LastLayer = 2,   ///< no layer left to pause before
    Ready = 3,
};

Availability classify(bool print_active, bool slicer_reports_layers, int current_layer,
                      int total_layers);

/// The layers a pause can still be scheduled at: (current, total].
struct LayerBounds {
    int min_layer;
    int max_layer;
};
LayerBounds layer_bounds(int current_layer, int total_layers);

} // namespace helix::layer_pause
