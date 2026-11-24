// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "src/ui/panel_widgets/tiled_panel_widget.h"
#include "subject_managed_panel.h"

#include <optional>
#include <string>

#include "hv/json.hpp" // JSON_HPP_OK: config_ held by value

namespace helix {

/// A home tile showing one sensor reading followed by a unit, whose unit text
/// the user can replace per tile (prestonbrown/helixscreen#939). The humidity
/// and width-sensor tiles are two Specs of this class.
///
/// The tile's component binds `value_subject`, the reading with its unit
/// already attached, so a unit override never changes the number: it only
/// changes the text after it.
class UnitTileWidget : public TiledPanelWidget {
  public:
    /// What differs between two unit tiles.
    struct Spec {
        const char* id;           ///< PanelWidgetDef id; the component is panel_widget_<id>
        const char* default_unit; ///< shown while no override is set
        const char* unit_gap;     ///< between the number and a non-empty unit
        const char* label;        ///< the tile's caption, measured for sizing
        const char* widest_number;
        /// The reading's raw subject.
        lv_subject_t* (*raw_subject)();
        /// Death signal of raw_subject()'s owner.
        SubjectLifetime (*raw_lifetime)();
        /// The reading's number as shown, or false when the sensor has none.
        bool (*format_number)(int raw, char* buf, size_t size);
    };

    /// The humidity and width-sensor tiles' Specs.
    static const Spec& humidity_spec();
    static const Spec& width_spec();

    UnitTileWidget(const std::string& instance_id, const Spec& spec);
    ~UnitTileWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;

    const char* id() const override {
        return instance_id_.c_str();
    }

    /// The component is per type; an instance id may carry a suffix.
    std::string get_component_name() const override {
        return std::string("panel_widget_") + spec_.id;
    }

    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Replace the unit text after this tile's reading, or restore the default
    /// with nullopt. An empty string draws no unit. Persists in the widget's
    /// config as "unit_override".
    void set_unit_override(std::optional<std::string> unit);
    const std::optional<std::string>& unit_override() const {
        return unit_override_;
    }

    /// The unit text drawn for @p unit: the override when set, else the default.
    std::string unit_text(const std::optional<std::string>& unit) const;

    /// The configure card's Unit row: edit the text.
    static void unit_row_cb(lv_event_t* e);
    /// The configure card's Default button: drop the override.
    static void unit_reset_cb(lv_event_t* e);

    /// Register the card's XML callbacks and publish its subjects.
    static void register_xml();

  private:
    class ConfigureCard : public helix::ui::ContextMenu {
        HELIX_CONTEXT_MENU_KIND(ConfigureCard)

      public:
        explicit ConfigureCard(UnitTileWidget& owner) : owner_(owner) {}

        /// Publish the Unit row's text and whether an override is set.
        void fill_unit_row();
        /// Close the card and ask for the unit text.
        void edit_unit();
        void reset_unit();

      protected:
        const char* xml_component_name() const override {
            return "unit_tile_configure_card";
        }
        CardWidth card_width() const override {
            return {40, 200, 320};
        }

      private:
        UnitTileWidget& owner_;
    };

    /// Recompute the drawn reading from the raw subject and the unit.
    void refresh_value();
    void apply_unit();
    void save_config();

    std::string instance_id_;
    Spec spec_;
    nlohmann::json config_ = nlohmann::json::object();
    std::optional<std::string> unit_override_; ///< nullopt = the default unit

    SubjectManager subjects_;
    helix::Subject value_subject_{};
    char value_buffer_[48] = {};
    ObserverGuard raw_observer_;

    helix::AsyncLifetimeGuard lifetime_;
    ConfigureCard configure_card_{*this};
};

/// Register the unit-tile factories (humidity, width_sensor).
void register_unit_tile_widgets();

} // namespace helix
