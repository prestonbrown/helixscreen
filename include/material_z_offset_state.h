// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The live side of Z offset by material (prestonbrown/helixscreen#2142): the
// table read from status, the per-printer setting, which material the current
// print uses, and the writes. What each storage scheme looks like is
// material_z_offset.h's business; this class only asks it.

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "material_z_offset.h"

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

typedef struct _lv_subject_t lv_subject_t;

namespace helix {
class IMoonrakerAPI;
class PrinterDiscovery;
} // namespace helix

namespace helix::material_zoffset {

/// What Settings shows. Published as `material_zoffset_status`.
enum class Status : int {
    None = 0,       ///< No row: the one-time setup explanation
    NeedsSetup = 1, ///< HelixScreen's macro without [save_variables]: update the macros
    Ready = 2,
};

/// Where a per-material Save writes.
struct SaveTarget {
    std::string material;      ///< As the user reads it: "PETG", or "DEFAULT"
    std::optional<int> stored; ///< What the start macro applies for it now
};

class MaterialZOffsetState {
  public:
    static MaterialZOffsetState& instance();

    /// Publishes material_zoffset_status, material_zoffset_enabled and
    /// material_zoffset_rev. Main thread, before XML binds them.
    void init_subjects();

    /// A new discovery: drop the last printer's table and re-read the row.
    void on_discovery(const PrinterDiscovery& hw);

    /// Fold a status frame in. Main thread.
    void update_from_status(const nlohmann::json& status);

    [[nodiscard]] Status status() const;
    [[nodiscard]] bool enabled() const;
    /// Store the setting and switch the start macro to match, where it has a
    /// printer-side switch.
    void set_enabled(IMoonrakerAPI* api, bool on);
    /// Switch the start macro on when the setting is on and it reads off (a
    /// fresh [save_variables] file, or a switch written elsewhere).
    void reconcile(IMoonrakerAPI* api);
    /// The start macro applies per-material offsets at print start whatever
    /// the setting says, so no save may fold the live offset into the probe.
    [[nodiscard]] bool start_macro_applies() const;
    /// The setting is on and the row can write: Saves go per material.
    [[nodiscard]] bool active() const;
    [[nodiscard]] const Table& table() const {
        return table_;
    }

    /// The screen is starting @p filename (gcodes-relative), which prints
    /// @p material most by weight. Sends the row's hint, where it has one, so
    /// the start macro applies that material instead of the slicer's first
    /// filament; the clearing form when the feature is off or there is no
    /// material, so a hint never outlives the start it was for.
    void note_screen_start(IMoonrakerAPI* api, const std::string& material,
                           const std::string& filename);
    /// The screen's start did not go ahead: clear any hint it sent.
    void note_screen_start_failed(IMoonrakerAPI* api);
    /// The print running now names @p first as its first filament.
    void note_print_file(const std::string& first);

    /// Where a Save writes now, or nullopt for the machine-wide save. While
    /// active() and no material can be named, an empty optional's sibling
    /// save_withheld() is true and no save is offered at all.
    [[nodiscard]] std::optional<SaveTarget> save_target() const;
    [[nodiscard]] bool save_withheld() const;

    /// Persist @p microns for @p material (empty = DEFAULT). @p done runs on the
    /// main thread with a user-facing message: success, or why it failed.
    /// Saves run one at a time, in the order asked, whichever surface asked.
    using Done = helix::SharedCallback<void(bool ok, const std::string& message)>;
    void save_offset(IMoonrakerAPI* api, const std::string& material, int microns, Done done);

    /// The bed mesh profile a fresh mesh is offered under after a calibration:
    /// {material, profile} for the material whose usual bed temperature is
    /// closest to @p bed_c among @p materials, or nullopt when the feature is
    /// off or the row stores no mesh. Klipper keeps mesh profiles in
    /// printer.cfg, so saving one is the mesh panel's usual SAVE_CONFIG.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>>
    mesh_offer(const std::vector<std::string>& materials, int bed_c) const;

    /// Bumped on every change the UI shows: the table, the setting, the target.
    /// nullptr before init_subjects().
    lv_subject_t* rev_subject();
    /// The token an observer of rev_subject() carries.
    [[nodiscard]] SubjectLifetime rev_lifetime() const;

  private:
    MaterialZOffsetState() = default;
    void publish();
    void bump();
    [[nodiscard]] std::string print_material() const;
    [[nodiscard]] bool material_known() const;
    void send_enable(IMoonrakerAPI* api, bool on);
    void run_next_save();
    void run_save(IMoonrakerAPI* api, const std::string& material, int microns, Done done);
    void write_config_file(IMoonrakerAPI* api, const std::string& material, int microns,
                           const std::string& message, Done done);
    void edit_and_upload(IMoonrakerAPI* api, const std::string& path, const std::string& section,
                         const std::string& material, int microns, const std::string& message,
                         Done done, helix::SharedCallback<void(const std::string&)> fail,
                         int attempts);
    void locate_config_file(
        IMoonrakerAPI* api,
        helix::SharedCallback<void(const std::string& path, const std::string& error)> then);

    Table table_;
    /// The current or last print's first filament; "known" once a print was seen.
    std::string print_first_;
    bool print_seen_ = false;
    /// The config file holding the section, once found this session.
    std::string config_file_;
    /// Saves waiting for the one running to finish.
    std::deque<std::function<void()>> pending_saves_;
    bool save_running_ = false;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::material_zoffset
