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

#include <optional>
#include <string>

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
    void set_enabled(bool on);
    /// The setting is on and the row can write: Saves go per material.
    [[nodiscard]] bool active() const;
    [[nodiscard]] const Table& table() const {
        return table_;
    }

    /// The screen is starting a print of @p material most by weight. Sends the
    /// row's hint, where it has one, so the start macro applies that material
    /// instead of the slicer's first filament; nothing otherwise.
    void note_screen_start(IMoonrakerAPI* api, const std::string& material);
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
    using Done = helix::SharedCallback<void(bool ok, const std::string& message)>;
    void save_offset(IMoonrakerAPI* api, const std::string& material, int microns, Done done);

    /// Save the loaded mesh as @p material's profile. Klipper keeps mesh
    /// profiles in printer.cfg, so this ends in SAVE_CONFIG, which the caller
    /// runs through its own restart watch; this returns the G-code to send
    /// before it, or empty when the row stores no mesh for that material.
    [[nodiscard]] std::string mesh_save_gcode(const std::string& material) const;

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
    void write_config_file(IMoonrakerAPI* api, const std::string& material, int microns,
                           const std::string& message, Done done);
    void locate_config_file(
        IMoonrakerAPI* api,
        helix::SharedCallback<void(const std::string& path, const std::string& error)> then);

    Table table_;
    /// The current or last print's first filament; "known" once a print was seen.
    std::string print_first_;
    bool print_seen_ = false;
    /// The config file holding the section, once found this session.
    std::string config_file_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix::material_zoffset
