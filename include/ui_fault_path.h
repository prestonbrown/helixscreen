// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <optional>
#include <string>
#include <string_view>

/**
 * @file ui_fault_path.h
 * @brief Publishes a fault's stop point along the filament path to the
 *        `fault_path_segment` subject.
 *
 * The graphic itself lives in `ui_xml/components/fault_path.xml` — four labelled
 * stops (Spool, Lane, Hub, Toolhead), the three gaps between them, and a caption naming
 * the failing gap in words, all bound to this one int subject. C++ only decides *where*
 * the filament stopped, by asking `helix::faultpath` (`fault_path_position.h`), the
 * module that knows which firmwares name one; XML owns how it looks.
 *
 * Subject values are `PathSegment` (`include/ams_types.h`), with 0 (`NONE`) meaning
 * "no recognised position" — the component binds `hidden` to that, so an unrecognised
 * or unpositioned fault shows no strip at all.
 */

namespace helix::ui {

/**
 * @brief Register the `fault_path_segment` subject and the `fault_path` component.
 *
 * Call once from `register_xml_components()`, before any XML that embeds
 * `<fault_path/>` is registered. Idempotent. Self-registers its teardown with
 * `StaticSubjectRegistry` so the subject dies before `lv_deinit()`.
 */
void fault_path_register();

/**
 * @brief Publish the stop point the fault sentence in @p message names.
 *
 * Sets `fault_path_segment` to that `PathSegment`, or to 0 (graphic hidden) when
 * @p message names none. A modal that embeds `<fault_path/>` calls this with
 * its own text before it is built, so the strip always describes the text on
 * screen and never a fault some earlier modal published. Registers the subject
 * if it does not exist yet.
 *
 * Main thread only: it writes an LVGL subject, which fires observers.
 *
 * @return The recognised segment, or std::nullopt.
 */
std::optional<PathSegment> fault_path_publish(std::string_view message);

/// The value `fault_path_segment` holds now: a `PathSegment`, or 0 when none.
int fault_path_current();

/**
 * @brief Hand the strip back to the modal underneath when an overlying one closes.
 *
 * Sets `fault_path_segment` to @p previous only while it still holds @p published,
 * the value the closing modal set; a later publisher's value is left alone.
 */
void fault_path_restore(int published, int previous);

/**
 * @brief Publish a fault message's stop point and strip its text diagram.
 *
 * Sets `fault_path_segment` to the recognised `PathSegment`, or to 0 when the message
 * names no stop point (faultpath::fault_position) — which also hides the graphic, so
 * every caller must route its message through here, including the ones that turn out
 * to be unrecognised. Otherwise a previous fault's marker stays on screen.
 *
 * Main thread only: it writes an LVGL subject, which fires observers.
 *
 * @param message The fault message as it reached the modal.
 * @return The message with its diagram rows removed, or unchanged when unrecognised.
 */
std::string fault_path_apply(const std::string& message);

/**
 * @brief One modal's claim on `fault_path_segment`
 *
 * A modal that embeds `<fault_path/>` publishes its own text's stop point
 * before it is built and hands the strip back to whatever lay underneath when it
 * closes, so the strip always describes the text on screen. A second publish
 * while held keeps the value found the first time. Released on release() or
 * destruction. Main thread only.
 */
class FaultPathHold {
  public:
    FaultPathHold() = default;
    ~FaultPathHold() {
        release();
    }
    FaultPathHold(const FaultPathHold&) = delete;
    FaultPathHold& operator=(const FaultPathHold&) = delete;

    /// fault_path_publish(), remembering what to hand back.
    std::optional<PathSegment> publish(std::string_view message);

    /// fault_path_apply(), remembering what to hand back.
    std::string apply(const std::string& message);

    /// Hand the strip back (fault_path_restore()). No-op when not held.
    void release();

  private:
    void hold();

    int before_ = 0;    ///< fault_path_segment when first published
    int published_ = 0; ///< the value this hold published
    bool held_ = false; ///< published and not yet handed back
};

} // namespace helix::ui
