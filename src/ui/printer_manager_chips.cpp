// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_manager_chips.h"

#include "ui_bed_drying_modal.h"
#include "ui_chamber_dryer_modal.h"
#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_overlay_retraction_settings.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_advanced.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_power.h"
#include "ui_probe_overlay.h"
#include "ui_settings_led.h"
#include "ui_settings_sensors.h"
#include "ui_settings_sound.h"
#include "ui_settings_system.h"

#include "accel_sensor_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "job_queue_start.h"
#include "led/ui_led_control_overlay.h"
#include "lvgl.h"
#include "printer_capabilities_state.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "probe_sensor_types.h"
#include "tool_state.h"
#include "translation_loader.h"
#include "width_sensor_manager.h"

#if !defined(HELIX_PLATFORM_ESP32)
#include "ui_overlay_timelapse_settings.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_calibration_pa.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_screws_tilt.h"
#include "ui_panel_spoolman.h"
#endif

#include <spdlog/spdlog.h>

namespace helix {

namespace {

lv_obj_t* screen() {
    return lv_display_get_screen_active(nullptr);
}

void open_probe() {
    auto& overlay = get_global_probe_overlay();
    overlay.set_api(get_moonraker_api());
    overlay.show(screen());
}

void open_temp_graph_bed() {
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, screen());
}

void open_temp_graph_chamber() {
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Chamber, screen());
}

void open_chamber_dryer() {
    ui::ChamberDryerModal::show_owned();
}

void open_bed_drying() {
    ui::BedDryingModal::show_owned();
}

void open_fan_control() {
    open_fan_control_overlay(screen());
}

void open_ams_detail() {
    ui::open_ams_detail_panel();
}

void open_sensor_settings() {
    settings::get_sensor_settings_overlay().show(screen());
}

void open_retraction() {
    get_global_retraction_settings().show(screen());
}

void open_job_queue() {
    open_job_queue_modal();
}

void open_system_settings() {
    settings::get_system_settings_overlay().show(screen());
}

void open_power() {
    ui::open_power_panel(screen());
}

void open_led_settings() {
    settings::get_led_settings_overlay().show(screen());
}

void open_led_control() {
    open_led_control_overlay(screen());
}

void open_sound_settings() {
    settings::get_sound_settings_overlay().show(screen());
}

// The macro and plugin rows live on the Advanced panel, under this overlay.
void open_advanced() {
    nav::go_back();
    nav::set_active(PanelId::Advanced);
}

#if !defined(HELIX_PLATFORM_ESP32)
void open_bed_mesh() {
    ui::open_bed_mesh_panel(screen());
}

void open_input_shaper() {
    get_global_input_shaper_panel().show(screen());
}

void open_screws_tilt() {
    ui::open_screws_tilt_panel(screen());
}

void open_belt_tension() {
    ui::open_belt_tension_panel(screen());
}

void open_pa_cal() {
    ui::open_pa_cal_panel(screen());
}

void open_tool_offsets() {
    ui::open_tool_offset_cal_panel(screen());
}

void open_spoolman() {
    get_global_spoolman_panel().show(screen());
}

void open_timelapse() {
    get_global_timelapse_settings().show(screen());
}

#define PM_DESKTOP_SCREEN(fn) fn
#else
// These screens are not in the ESP32 image; their chips show the info popup.
#define PM_DESKTOP_SCREEN(fn) nullptr
#endif

using C = CapChip;

constexpr std::array<CapChipSpec, kCapChipCount> kSpecs = {{
    {C::Kinematics, "pm_chip_kinematics", "pm_chip_text_kinematics", TR_NOOP("Kinematics"),
     TR_NOOP("How the printer's axes are driven, as its Klipper config declares it. HelixScreen "
             "uses it to decide how homing works and which belt and motion tools apply."),
     nullptr},
    {C::BuildVolume, "pm_chip_build_volume", "pm_chip_text_build_volume", TR_NOOP("Build Volume"),
     TR_NOOP("The printable size in millimeters (X × Y × Z), from the axis limits in the "
             "printer's Klipper config."),
     nullptr},
    {C::Probe, "pm_chip_probe", "pm_chip_text_probe", TR_NOOP("Probe"),
     TR_NOOP("A sensor that measures the distance to the bed. HelixScreen uses it for Z offset "
             "calibration and probe tests on the Probe screen."),
     open_probe},
    {C::BedMesh, "pm_chip_bed_mesh", "pm_chip_text_bed_mesh", TR_NOOP("Bed Mesh"),
     TR_NOOP("A height map of the bed that Klipper uses to make up for an uneven surface. View "
             "and calibrate it on the Bed Mesh screen."),
     PM_DESKTOP_SCREEN(open_bed_mesh)},
    {C::Qgl, "pm_chip_qgl", "pm_chip_text_qgl", TR_NOOP("QGL"),
     TR_NOOP("Quad Gantry Level squares a gantry driven by four Z motors to the bed. HelixScreen "
             "offers it on the Controls and Motion screens."),
     nullptr},
    {C::ZTilt, "pm_chip_z_tilt", "pm_chip_text_z_tilt", TR_NOOP("Z-Tilt"),
     TR_NOOP("Z-Tilt levels the gantry or bed using its separate Z motors. HelixScreen offers it "
             "on the Controls and Motion screens."),
     nullptr},
    {C::ScrewsTilt, "pm_chip_screws_tilt", "pm_chip_text_screws_tilt", TR_NOOP("Screws Tilt"),
     TR_NOOP("Measures the bed at each adjustment screw and says how far to turn it. Run it on "
             "the Screws Tilt screen."),
     PM_DESKTOP_SCREEN(open_screws_tilt)},
    {C::Accelerometer, "pm_chip_accelerometer", "pm_chip_text_accelerometer",
     TR_NOOP("Accelerometer"),
     TR_NOOP("Measures vibration for input shaping. Run resonance tests on the Input Shaper "
             "screen."),
     PM_DESKTOP_SCREEN(open_input_shaper)},
    {C::BeltTension, "pm_chip_belt_tension", "pm_chip_text_belt_tension", TR_NOOP("Belt Tension"),
     TR_NOOP("Compares the two belt paths of a CoreXY printer with the accelerometer, so you can "
             "even out their tension. Run it on the Belt Tension screen."),
     PM_DESKTOP_SCREEN(open_belt_tension)},
    {C::ShakeTune, "pm_chip_shake_tune", "pm_chip_text_shake_tune", TR_NOOP("Shake&Tune"),
     TR_NOOP("Klippain Shake&Tune macros are installed on the printer. HelixScreen detects them "
             "but does not use them; run them from Mainsail or Fluidd."),
     nullptr},
    {C::HeatedBed, "pm_chip_heated_bed", "pm_chip_text_heated_bed", TR_NOOP("Heated Bed"),
     TR_NOOP("The bed heater. Set its temperature and watch its graph on the bed temperature "
             "screen."),
     open_temp_graph_bed},
    {C::ChamberSensor, "pm_chip_chamber_sensor", "pm_chip_text_chamber_sensor",
     TR_NOOP("Chamber Sensor"),
     TR_NOOP("A sensor that reads the air temperature inside the enclosure. Watch it on the "
             "chamber temperature screen."),
     open_temp_graph_chamber},
    {C::ChamberHeater, "pm_chip_chamber_heater", "pm_chip_text_chamber_heater",
     TR_NOOP("Chamber Heater"),
     TR_NOOP("A heater that warms the air inside the enclosure. Set its temperature on the "
             "chamber temperature screen."),
     open_temp_graph_chamber},
    {C::ChamberDryer, "pm_chip_chamber_dryer", "pm_chip_text_chamber_dryer",
     TR_NOOP("Chamber Dryer"),
     TR_NOOP("The chamber heater can run a filament drying cycle. Start one from the drying "
             "dialog."),
     open_chamber_dryer},
    {C::FilterFan, "pm_chip_filter_fan", "pm_chip_text_filter_fan", TR_NOOP("Filter Fan"),
     TR_NOOP("A fan that pulls the enclosure air through a filter. Control it on the chamber "
             "temperature screen."),
     open_temp_graph_chamber},
    {C::BedDrying, "pm_chip_bed_drying", "pm_chip_text_bed_drying", TR_NOOP("Bed Drying"),
     TR_NOOP("The heated bed and enclosure can dry spools placed on the bed. Start a cycle from "
             "the bed drying dialog."),
     open_bed_drying},
    {C::Fans, "pm_chip_fans", "pm_chip_text_fans", TR_NOOP("Fans"),
     TR_NOOP("Fans besides the part cooling fan. Control them on the Fans screen."),
     open_fan_control},
    {C::FanTach, "pm_chip_fan_tach", "pm_chip_text_fan_tach", TR_NOOP("Fan Tach"),
     TR_NOOP("The printer reports measured fan speeds. HelixScreen shows them on the Fans "
             "screen."),
     open_fan_control},
    {C::FilamentSystem, "pm_chip_filament_system", "pm_chip_text_filament_system",
     TR_NOOP("Multi-Filament"),
     TR_NOOP("A system that feeds several filaments to the toolhead, shown with its slot count. "
             "Manage the slots on the filament system screen."),
     open_ams_detail},
    {C::ToolChanger, "pm_chip_tool_changer", "pm_chip_text_tool_changer", TR_NOOP("Tool Changer"),
     TR_NOOP("The printer swaps between several toolheads. Manage the tools on the filament "
             "system screen."),
     open_ams_detail},
    {C::FilamentSensors, "pm_chip_filament_sensors", "pm_chip_text_filament_sensors",
     TR_NOOP("Filament Sensors"),
     TR_NOOP("Sensors that detect filament running out or jamming. Set what each one does in "
             "the sensor settings."),
     open_sensor_settings},
    {C::WidthSensors, "pm_chip_width_sensors", "pm_chip_text_width_sensors",
     TR_NOOP("Width Sensors"),
     TR_NOOP("Sensors that measure the filament diameter as it feeds. Configure them in the "
             "sensor settings."),
     open_sensor_settings},
    {C::Retraction, "pm_chip_retraction", "pm_chip_text_retraction", TR_NOOP("Retraction"),
     TR_NOOP("Klipper handles retraction itself (G10/G11). Adjust the lengths and speeds in the "
             "retraction settings."),
     open_retraction},
    {C::PaCal, "pm_chip_pa_cal", "pm_chip_text_pa_cal", TR_NOOP("PA Calibration"),
     TR_NOOP("The printer can measure pressure advance on its own. Run it on the PA "
             "calibration screen."),
     PM_DESKTOP_SCREEN(open_pa_cal)},
    {C::ToolOffsets, "pm_chip_tool_offsets", "pm_chip_text_tool_offsets", TR_NOOP("Tool Offsets"),
     TR_NOOP("The printer can measure each tool's offset automatically. Run it on the tool "
             "offset calibration screen."),
     PM_DESKTOP_SCREEN(open_tool_offsets)},
    {C::NozzleClean, "pm_chip_nozzle_clean", "pm_chip_text_nozzle_clean", TR_NOOP("Nozzle Clean"),
     TR_NOOP("A macro that wipes the nozzle. HelixScreen can run it before a print from the "
             "pre-print options."),
     nullptr},
    {C::PurgeLine, "pm_chip_purge_line", "pm_chip_text_purge_line", TR_NOOP("Purge Line"),
     TR_NOOP("The printer can draw a priming line before a print. HelixScreen offers it in the "
             "pre-print options."),
     nullptr},
    {C::HeatSoak, "pm_chip_heat_soak", "pm_chip_text_heat_soak", TR_NOOP("Heat Soak"),
     TR_NOOP("Heat soak lets the enclosure warm up before a print. It is listed because the "
             "printer has a heat soak macro or it was turned on in settings; HelixScreen does not "
             "run it."),
     nullptr},
    {C::ExcludeObject, "pm_chip_exclude_object", "pm_chip_text_exclude_object",
     TR_NOOP("Exclude Object"),
     TR_NOOP("Exclude Object lets you skip a failed part mid-print from the print status "
             "screen."),
     nullptr},
    {C::LayerPause, "pm_chip_layer_pause", "pm_chip_text_layer_pause", TR_NOOP("Layer Pause"),
     TR_NOOP("Pauses a print at a chosen layer, for a filament swap or an insert. During a "
             "print, set the layer from the preview card on the print status screen."),
     nullptr},
    {C::JobQueue, "pm_chip_job_queue", "pm_chip_text_job_queue", TR_NOOP("Job Queue"),
     TR_NOOP("Moonraker keeps a queue of prints to run one after another. View it in the job "
             "queue."),
     open_job_queue},
    {C::Mcus, "pm_chip_mcus", "pm_chip_text_mcus", TR_NOOP("MCUs"),
     TR_NOOP("The controller boards Klipper runs on. See their load in the system settings."),
     open_system_settings},
    {C::Power, "pm_chip_power", "pm_chip_text_power", TR_NOOP("Power"),
     TR_NOOP("Power devices Moonraker can switch. Turn them on and off on the Power screen."),
     open_power},
    {C::Leds, "pm_chip_leds", "pm_chip_text_leds", TR_NOOP("LEDs"),
     TR_NOOP("Lights the printer can control. Choose which ones HelixScreen uses in the LED "
             "settings."),
     open_led_settings},
    {C::LedEffects, "pm_chip_led_effects", "pm_chip_text_led_effects", TR_NOOP("LED Effects"),
     TR_NOOP("Animated light effects configured on the printer. Turn them on and off from the "
             "LED controls."),
     open_led_control},
    {C::Speaker, "pm_chip_speaker", "pm_chip_text_speaker", TR_NOOP("Speaker"),
     TR_NOOP("A buzzer or speaker for alerts and sounds. Set what plays in the sound settings."),
     open_sound_settings},
    {C::Webcam, "pm_chip_webcam", "pm_chip_text_webcam", TR_NOOP("Webcam"),
     TR_NOOP("A camera Moonraker streams. Add the camera widget to the home screen to watch it, "
             "and see it during a print."),
     nullptr},
    {C::Spoolman, "pm_chip_spoolman", "pm_chip_text_spoolman", TR_NOOP("Spoolman"),
     TR_NOOP("Moonraker is connected to Spoolman, which tracks your spools and how much "
             "filament is left. Browse them on the Spoolman screen."),
     PM_DESKTOP_SCREEN(open_spoolman)},
    {C::Timelapse, "pm_chip_timelapse", "pm_chip_text_timelapse", TR_NOOP("Timelapse"),
     TR_NOOP("The Moonraker timelapse plugin is installed. Set how it records in the timelapse "
             "settings."),
     PM_DESKTOP_SCREEN(open_timelapse)},
    {C::HelixMacros, "pm_chip_helix_macros", "pm_chip_text_helix_macros",
     TR_NOOP("HelixScreen Macros"),
     TR_NOOP("HelixScreen's helper macros are installed in the printer's config. Install "
             "updates on the Advanced screen."),
     open_advanced},
    {C::HelixPlugin, "pm_chip_helix_plugin", "pm_chip_text_helix_plugin",
     TR_NOOP("HelixPrint Plugin"),
     TR_NOOP("HelixScreen's Moonraker plugin is installed, which it uses to run pre-print "
             "options. Manage it on the Advanced screen."),
     open_advanced},
}};

constexpr bool specs_in_enum_order() {
    for (size_t i = 0; i < kCapChipCount; ++i) {
        if (static_cast<size_t>(kSpecs[i].chip) != i) {
            return false;
        }
    }
    return true;
}

} // namespace

const std::array<CapChipSpec, kCapChipCount>& cap_chip_specs() {
    return kSpecs;
}

const CapChipSpec* find_cap_chip_spec(std::string_view name) {
    for (const auto& spec : kSpecs) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

CapabilityChipFacts gather_capability_chip_facts() {
    CapabilityChipFacts f;
    PrinterState& ps = get_printer_state();
    IMoonrakerAPI* api = get_moonraker_api();
    const PrinterDiscovery& hw = api ? api->hardware() : ps.get_discovery();

    f.kinematics = hw.kinematics();
    f.build_volume = hw.build_volume();
    f.mcus = hw.mcu_list();
    f.fan_count = PrinterFanState::count_extra_controllable_fans(hw.fans());
    f.led_count = static_cast<int>(hw.leds().size());
    f.led_effect_count = static_cast<int>(hw.led_effects().size());

    const auto probes = sensors::ProbeSensorManager::instance().get_sensors();
    if (!probes.empty() && probes.front().type != sensors::ProbeSensorType::STANDARD) {
        f.probe_type = sensors::probe_type_to_display_string(probes.front().type);
    }

    f.accel_count = static_cast<int>(sensors::AccelSensorManager::instance().sensor_count());
    f.filament_sensor_count = static_cast<int>(FilamentSensorManager::instance().sensor_count());
    f.width_sensor_count = static_cast<int>(sensors::WidthSensorManager::instance().sensor_count());
    const PrinterCapabilitiesState& caps = ps.capabilities_state();
    f.power_device_count = lv_subject_get_int(caps.subject(Capability::PowerDeviceCount));
    f.webcam_count = lv_subject_get_int(caps.subject(Capability::WebcamCount));
    f.head_count = static_cast<int>(ToolState::instance().physical_heads().size());
    f.has_tool_changer = hw.has_tool_changer();

    auto& ams = AmsState::instance();
    for (int i = 0; i < ams.backend_count(); ++i) {
        const auto info = ams.system_info(i);
        if (!info || !info->presents_filament_system()) {
            continue;
        }
        const bool tool_changer = is_tool_changer(info->type);
        f.has_tool_changer = f.has_tool_changer || tool_changer;
        f.filament_systems.push_back({info->type_name, info->total_slots, tool_changer});
    }

    // Only the capabilities whose subject an override drives: there, a chip
    // showing what detection missed is the user's doing, and a support check
    // needs to tell the two apart.
    const CapabilityOverrides& overrides = ps.get_capability_overrides();
    const std::pair<CapChip, const char*> overridable[] = {
        {C::BedMesh, capability::BED_MESH},   {C::Qgl, capability::QGL},
        {C::ZTilt, capability::Z_TILT},       {C::NozzleClean, capability::NOZZLE_CLEAN},
        {C::HeatSoak, capability::HEAT_SOAK}, {C::Speaker, capability::SPEAKER},
    };
    for (const auto& [chip, name] : overridable) {
        if (overrides.is_forced_on(name)) {
            f.forced_on.insert(chip);
        }
    }
    return f;
}

void open_cap_chip(const CapChipSpec& spec) {
    spdlog::debug("[Printer Manager] {} chip clicked", spec.name);
    if (spec.open) {
        spec.open();
        return;
    }
    ui::modal_alert(lv_tr(spec.info_title), lv_tr(spec.info_message), ModalSeverity::Info);
}

static_assert(specs_in_enum_order());

} // namespace helix
