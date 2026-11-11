// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_modal.h"
#include "ui_multiselect.h"

#include "ams_backend.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "filament_action_set.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief The head picker: which heads a Load, Unload, Mount or seat-by-hand acts on.
 *
 * One row per head (decide_head_picker_rows()), naming the slot the op will
 * use. A row the backend cannot serve is shown disabled with its reason. The
 * selection is capped at the backend's max_heads_per_filament_op(), one for a
 * single_select() op. The picker decides nothing itself: its primary button hands the
 * chosen rows to the caller, which dispatches.
 */
class BatchFilamentModal : public Modal {
  public:
    using Confirm = std::function<void(const std::vector<HeadPickRow>& chosen)>;

    /// One-shot owned show for @p op: create, name the title and the primary
    /// button after it, build the rows from the active backend, and hand the
    /// instance to ModalStack, which frees it when its entry goes.
    static bool show_owned(HeadPickOp op, Confirm on_confirm);

    const char* get_name() const override {
        return "Head Picker";
    }
    const char* component_name() const override {
        return "batch_filament_modal";
    }

    // Pure: multiselect keys are head indices as decimal strings.
    static std::vector<int> selected_slots(const std::vector<std::string>& keys);

    // Pure: row text. The lane name, plus what is in the lane and where it
    // stands: a lane carrying material says loaded (at the toolhead), ready to
    // load or empty by its sensor, or the material alone with no sensor; a
    // lane with no material known empty says Empty, and an unanswerable
    // presence leaves the lane name bare. The tick state stops
    // describing the machine the moment the user changes it, so the contents
    // are named in the row.
    static std::string row_label(LaneNoun noun, int slot, const SlotInfo& info,
                                 std::optional<bool> present, bool at_toolhead);

    /// What one head row says: the head, the slot it names where that is not
    /// the head's own, then either why the row is disabled or the slot's
    /// contents (row_label()'s suffix).
    struct HeadRowText {
        LaneNoun head_noun = LaneNoun::Tool;
        LaneNoun slot_noun = LaneNoun::Slot;
        int head = -1;
        int slot = -1;
        bool show_slot = false;
        SlotInfo info;
        std::optional<bool> present;
        bool at_toolhead = false;
        std::string reason;
    };
    static std::string head_row_label(const HeadRowText& text);

    /// Whether @p head's row reads its @p slot as loaded rather than ready to
    /// load: the slot is at the toolhead, or, where each head has one slot, the
    /// backend knows the head holds filament (a docked hot end keeps the strand
    /// its load fed).
    static bool row_at_toolhead(const AmsBackend& backend, int head, int slot);

    // Pure: the selected slots the backend says can take this operation,
    // plus the first one it refused and why. Eligibility is
    // direction-dependent, so this runs on button press, not when the rows
    // are built. Out-of-range slots are dropped, not trusted — and there is
    // no eligibility value to read past the table's end, so no refusal to
    // name either.
    struct EligibilitySift {
        std::vector<int> eligible; ///< selected slots that can run
        int dropped = -1;          ///< first selected slot refused, -1 when none
        AmsBackend::FilamentOpEligibility drop_reason = AmsBackend::FilamentOpEligibility::Busy;
    };
    static EligibilitySift
    sift_eligible(const std::vector<int>& selected,
                  const std::vector<AmsBackend::FilamentOpEligibility>& per_slot);

    /// Run @p slots as one batch on @p backend: drop the slots it says cannot
    /// take the op (naming the first), then send the rest together.
    /// @return true when a batch was sent
    static bool dispatch_batch(AmsBackend& backend, const std::vector<int>& slots, bool load);

  protected:
    void on_show() override;

    /// btn_primary: hands the chosen rows to the caller.
    void on_ok() override;

  private:
    HeadPickOp op_ = HeadPickOp::Load;
    Confirm on_confirm_;
    std::vector<HeadPickRow> rows_;

    UiMultiselect multiselect_;
};

} // namespace helix::ui
