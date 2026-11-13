// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix::detection {

enum class DetectionKind { Spaghetti, DirtyBed, Residue, DirtyNozzle, Unknown };

/// The Snapmaker U1 defect_detection module raises exception id 532, with
/// these codes (stock defect_detection.py).
inline constexpr int kU1DefectExceptionId = 532;
inline constexpr int kU1DefectCodeDirtyBed = 1;
inline constexpr int kU1DefectCodeNoodle = 2;
inline constexpr int kU1DefectCodeResidue = 3;
inline constexpr int kU1DefectCodeDirtyNozzle = 4;

/// Map a Snapmaker U1 `print_stats.exception.code` to a kind. 0 / -1 /
/// anything else = not a visual defect we surface.
inline DetectionKind kind_from_u1_code(int code) {
    switch (code) {
    case kU1DefectCodeDirtyBed:
        return DetectionKind::DirtyBed;
    case kU1DefectCodeNoodle:
        return DetectionKind::Spaghetti;
    case kU1DefectCodeResidue:
        return DetectionKind::Residue;
    case kU1DefectCodeDirtyNozzle:
        return DetectionKind::DirtyNozzle;
    default:
        return DetectionKind::Unknown;
    }
}

/// A code under any id but kU1DefectExceptionId belongs to a different
/// module; a frame that carried no id (-1) is judged by its code alone.
inline DetectionKind kind_from_u1_exception(int exception_id, int code) {
    if (exception_id >= 0 && exception_id != kU1DefectExceptionId) {
        return DetectionKind::Unknown;
    }
    return kind_from_u1_code(code);
}

/// Normalized "a detector reported a defect" event.
struct DetectionEvent {
    std::string source_id;
    DetectionKind kind = DetectionKind::Unknown;
    bool attributable = false;
    std::optional<float> confidence;
    bool already_paused = false;
    std::string message;
};

/// The printer's own stored on/off + pause choice, as the vendor stack keeps
/// it. Seeded into HelixScreen settings once, on the first start where a
/// capable source exists; after that the settings own both values.
struct DetectionPreference {
    bool enabled = true;
    bool pause = true;
};

/// A backend that can report print-failure detections.
class DetectionSource {
  public:
    using Callback = std::function<void(const DetectionEvent&)>;

    virtual ~DetectionSource() = default;
    virtual std::string id() const = 0;
    virtual bool available() const = 0;
    virtual bool can_tune() const {
        return false;
    }
    /// Whether the source's own firmware pauses the print on a detection.
    /// The default (false) is the common shape: HelixScreen sends the pause,
    /// so the pause-on-detect setting governs the source and its settings
    /// row shows. Sources paused in firmware override to true; the setting
    /// is inert for them, their row hides, and a warn-only response still
    /// escalates to the modal.
    virtual bool self_pauses() const {
        return false;
    }
    /// Lower the source's sensitivity (the response modal's Tune button).
    /// Called only while can_tune() is true, on the main thread.
    virtual void tune() {}
    /// Re-run the local capability probe. Called on every connect: answers
    /// that depend on state saved after boot (e.g. the wizard's printer
    /// type) can change without a restart. Sources whose capability arrives
    /// over the wire keep the base no-op.
    virtual void refresh_capability() {}
    /// Take this connection's printer.objects.list answer (every Klipper
    /// object name). Called on every connect, on the main thread. Sources
    /// whose capability is a Klipper module decide it here; the rest keep
    /// the base no-op.
    virtual void on_objects_listed(const std::vector<std::string>& objects) {
        (void)objects;
    }
    /// Make the printer's own detector follow the Spaghetti Detection switch,
    /// for sources whose firmware detects (and pauses) by itself. Called on
    /// the main thread while available(): whenever the switch changes, and on
    /// connect when read_detection_enabled() reports a different value.
    /// Sources that detect in HelixScreen keep the no-op and read the setting
    /// themselves.
    virtual void apply_detection_enabled(bool enabled) {
        (void)enabled;
    }
    /// Callback for a read of the printer's own detection switch: its state,
    /// or nullopt when the printer has none or did not answer. Called on any
    /// thread; the caller marshals.
    using DetectionEnabledReadCallback = std::function<void(std::optional<bool>)>;
    /// Ask the printer whether its own detection is on. Called on connect,
    /// while available(); sources without a printer-side switch answer nullopt.
    virtual void read_detection_enabled(DetectionEnabledReadCallback done) {
        done(std::nullopt);
    }
    /// The printer's stored preference, or nullopt when this source has none
    /// (settings then keep their defaults: on, pause on detect).
    virtual std::optional<DetectionPreference> printer_preference() const {
        return std::nullopt;
    }
    /// Whether the print is paused right now because this source detected
    /// something. Read by other pause consumers (the runout guidance) so one
    /// pause gets one explanation, whichever observer runs first.
    virtual bool explains_current_pause() const {
        return false;
    }

    /// The bed check as the printer reports it. A detection module switched off
    /// as a whole runs no bed check, whatever its own flag says.
    struct BedCheckState {
        bool enabled = false;
        bool module_enabled = true;
    };
    /// Callback for a read: the printer's state, or nullopt when the printer
    /// could not be asked or did not answer. Called on any thread; the caller
    /// marshals.
    using BedCheckReadCallback = std::function<void(std::optional<BedCheckState>)>;
    /// Callback for a switch: whether the printer accepted it, and when it did
    /// not, the printer's own reason (empty when it gave none). Any thread.
    using BedCheckSetCallback = std::function<void(bool accepted, std::string refusal)>;
    /// Whether the printer runs a pre-print check for objects left on the bed
    /// that the user can switch on and off.
    virtual bool has_bed_check() const {
        return false;
    }
    /// Ask the printer whether its bed check is on. Called only while
    /// has_bed_check() is true.
    virtual void read_bed_check(BedCheckReadCallback done) {
        done(std::nullopt);
    }
    /// Switch the printer's bed check. Called only while has_bed_check().
    virtual void set_bed_check(bool enabled, BedCheckSetCallback done) {
        (void)enabled;
        done(false, {});
    }
    /// Callback fires on the MAIN thread (already marshaled).
    virtual void set_callback(Callback cb) = 0;
};

} // namespace helix::detection
