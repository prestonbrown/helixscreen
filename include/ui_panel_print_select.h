// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_filament_mapping_modal.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"
#include "ui_plugin_install_modal.h"
#include "ui_print_select_card_view.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_select_file_provider.h"
#include "ui_print_select_file_sorter.h"
#include "ui_print_select_list_view.h"
#include "ui_print_select_path_navigator.h"
#include "ui_print_select_usb_source.h"
#include "ui_print_start_controller.h"

#include "async_lifetime_guard.h"
#include "helix_plugin_installer.h"
#include "in_flight_guard.h"
#include "print_file_data.h"
#include "print_history_manager.h"
#include "subject_managed_panel.h"
#include "usb_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file ui_panel_print_select.h
 * @brief Print file selection panel (class-based)
 *
 * Provides a file browser for G-code files with:
 * - Card view (grid of thumbnails) and list view (sortable table)
 * - View toggle button to switch between modes
 * - Sortable columns: filename, size, modified date, print time
 * - Detail overlay with file metadata and action buttons
 * - Delete confirmation dialog
 * - IMoonrakerAPI integration for file listing, deletion, and print start
 *
 * ## Reactive Subjects (6):
 * - selected_filename - Currently selected file name
 * - selected_thumbnail - Thumbnail path for detail view
 * - selected_print_time - Formatted print time string
 * - selected_filament_weight - Formatted filament weight string
 * - detail_view_visible - Controls detail overlay visibility
 * - print_select_view_mode - View mode (0=CARD, 1=LIST) - XML bindings control visibility
 *
 * ## Migration Notes:
 * This is the largest panel in the codebase (1167 lines). Key patterns:
 * - Static PrintFileData allocations in attach_*_click_handler() are now managed
 *   by storing data in the file_list_ and passing indices via user_data
 * - All lambdas converted to static trampolines with this pointer
 * - Resize callback uses static trampoline pattern
 *
 * @see PanelBase for base class documentation
 * @see docs/PANEL_MIGRATION.md for migration procedure
 */

/**
 * @brief View mode for print select panel
 */
enum class PrintSelectViewMode {
    CARD = 0, ///< Card grid view (default)
    LIST = 1  ///< List view with columns
};

/**
 * @brief Sort column for list view
 */
enum class PrintSelectSortColumn { FILENAME, SIZE, MODIFIED, PRINT_TIME, FILAMENT };

/**
 * @brief Sort direction
 */
enum class PrintSelectSortDirection { ASCENDING, DESCENDING };

// FileSource enum is defined in ui_print_select_usb_source.h

// FileHistoryStatus and PrintFileData are now in print_file_data.h

/**
 * @brief Card layout dimensions calculated from container size
 */
struct CardDimensions {
    int num_columns;
    int num_rows;
    int card_width;
    int card_height;
};

// Card width bounds, and the width a card wants to be. CARD_TARGET_WIDTH mirrors
// card_base_width in print_file_card.xml — the size the component was drawn around.
inline constexpr int CARD_WIDTH_MIN = 130;
inline constexpr int CARD_WIDTH_MAX = 230;
inline constexpr int CARD_WIDTH_TARGET = 170;

/**
 * @brief Choose how many card columns fit across a container.
 *
 * Considers every column count whose resulting card width is within
 * [CARD_WIDTH_MIN, CARD_WIDTH_MAX] and keeps the one landing nearest
 * CARD_WIDTH_TARGET. Ties keep the wider card (the lower column count wins).
 *
 * The previous rule walked column counts downward and took the first that fit,
 * which always maximized columns and therefore always produced the narrowest
 * legal card: an 800x480 screen (704px of content) got 5 cards of 132px when 4
 * of 167px is what print_file_card.xml is designed for.
 *
 * Pure function so the rule can be tested without building a panel.
 *
 * @param container_width Content width available for cards, in pixels
 * @param card_gap        Gap between columns, in pixels
 * @return Column count, or 0 when no count yields a card within the bounds
 */
inline int choose_card_columns(int container_width, int card_gap) {
    if (container_width <= 0 || card_gap < 0) {
        return 0;
    }

    int best_cols = 0;
    int best_distance = 0;

    for (int cols = 1; cols <= 10; cols++) {
        int total_gaps = (cols - 1) * card_gap;
        int card_width = (container_width - total_gaps) / cols;

        if (card_width < CARD_WIDTH_MIN || card_width > CARD_WIDTH_MAX) {
            continue;
        }

        int distance = std::abs(card_width - CARD_WIDTH_TARGET);
        if (best_cols == 0 || distance < best_distance) {
            best_cols = cols;
            best_distance = distance;
        }
    }

    return best_cols;
}

/**
 * @brief Card width for a given column count, matching choose_card_columns().
 */
inline int card_width_for_columns(int container_width, int card_gap, int columns) {
    if (columns <= 0) {
        return 0;
    }
    return (container_width - (columns - 1) * card_gap) / columns;
}

/**
 * @brief Decide whether a failed directory refresh should fall back to root.
 *
 * If the current directory no longer exists on the server, retrying it refreshes
 * nothing and the panel wedges (it re-requested a phantom doubled path every few
 * seconds in debug bundle TJVQDCZ6). When Moonraker reports the directory as
 * missing, drop back to root instead. Only reset when NOT already at root: if
 * root itself ever errors, resetting again would loop.
 *
 * Moonraker's file_manager raises "Directory does not exist (...)" (HTTP 400);
 * the message is server-side English and not localized, so a case-insensitive
 * substring match is safe. Extracted as a pure function so it can be tested
 * without the full PrintSelectPanel/LVGL fixture.
 */
inline bool dir_error_should_reset_to_root(const std::string& error_message, bool at_root) {
    if (at_root) {
        return false; // nowhere to fall back to; avoid an infinite reset loop
    }
    std::string lower = error_message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("does not exist") != std::string::npos;
}

/**
 * @brief Whether a notify_filelist_changed payload can change what this panel lists.
 *
 * PrintSelectFileProvider requests the "gcodes" root and nothing else, so a
 * change under any other root cannot alter this list. Moonraker fires the same
 * notification for every registered directory, and printers write to `config`
 * constantly: an AFC unit rewrites `AFC/AFC.var.unit` on every SET_* command and
 * a SAVE_VARIABLE delayed_gcode rewrites `saved_variables.cfg`, which together
 * cost 113 full server.files.get_directory round trips in one session in debug
 * bundle L53W5PKG — all while the user sat on the print-status panel.
 *
 * An empty root means the payload had no shape we recognise; refresh in that
 * case, because going stale is worse than one extra round trip.
 *
 * Exact match, not a prefix: a separately registered root such as
 * "gcodes_backup" is a different directory and would reintroduce the storm.
 * Extracted as a pure function so it can be tested without the full
 * PrintSelectPanel/LVGL fixture.
 */
inline bool filelist_change_affects_gcodes(const std::string& root) {
    return root.empty() || root == "gcodes";
}

/**
 * @brief Value to publish to a thumbnail POINTER subject (lv_image_bind_src).
 *
 * Returns @p buffer only when it holds a real (non-empty) thumbnail path;
 * otherwise nullptr. Never publishes an empty buffer: LVGL's
 * lv_image_src_get_type classifies a buffer whose first byte is < 0x20 (e.g. an
 * empty string) as LV_IMAGE_SRC_VARIABLE, then fails to decode it — logging
 * "lv_image_set_src: failed to get image info" (and previously crashing). Both
 * the subject init and set_selected_file() route through this so the
 * no-thumbnail sentinel is always nullptr (prestonbrown/helixscreen#990).
 *
 * Extracted as a pure function so it can be tested without the full
 * PrintSelectPanel/LVGL fixture.
 */
inline char* thumbnail_subject_value(char* buffer) {
    return (buffer && buffer[0] != '\0') ? buffer : nullptr;
}

struct PrintSelectPanelTestAccess; // test-only friend (tests/test_helpers/)

/**
 * @brief Print file selection panel with card/list views
 *
 * Displays G-code files from Moonraker with two view modes:
 * - Card view: Grid of file cards with thumbnails
 * - List view: Sortable table with file metadata
 *
 * Selected files show a detail overlay with print/delete options.
 */
class PrintSelectPanel : public PanelBase {
  public:
    /**
     * @brief Construct PrintSelectPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState for mock detection
     * @param api Pointer to IMoonrakerAPI for file operations
     */
    PrintSelectPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    /**
     * @brief Destructor - cleanup observers
     *
     * @note Does NOT call LVGL functions (static destruction order safety).
     *       Widget tree is cleaned up by LVGL.
     */
    ~PrintSelectPanel() override;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Initialize 15 reactive subjects for file selection state
     */
    void init_subjects() override;

    /**
     * @brief Deinitialize all subjects (call before destruction)
     *
     * Follows [L041] pattern for safe subject cleanup.
     */
    void deinit_subjects();

    /**
     * @brief Setup the print select panel
     *
     * Wires up view toggle, sort headers, creates detail view overlay,
     * registers resize callback, and initiates file loading.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen for overlay positioning
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Print Select Panel";
    }
    const char* get_xml_component_name() const override {
        return "print_select_panel";
    }

    /**
     * @brief Called when panel becomes visible
     *
     * Triggers lazy file refresh if file list is empty and API is connected.
     * This handles the case where set_api() was called before WebSocket connection.
     */
    void on_activate() override;
    void on_deactivate() override;

    //
    // === Public API ===
    //

    /**
     * @brief Toggle between card and list view
     */
    void toggle_view();

    /**
     * @brief Handle Printer source button click
     *
     * Switches to Printer (Moonraker) file source and refreshes file list.
     * Called from XML event_cb handler.
     */
    void on_source_printer_clicked();

    /**
     * @brief Handle USB source button click
     *
     * Switches to USB file source and refreshes file list.
     * Called from XML event_cb handler.
     */
    void on_source_usb_clicked();

    /**
     * @brief Sort files by specified column
     *
     * Toggles direction if same column, otherwise sorts ascending.
     *
     * @param column Column to sort by
     */
    void sort_by(PrintSelectSortColumn column);

    /**
     * @brief Refresh file list from Moonraker
     *
     * Fetches files from current directory, updates both views.
     * Metadata (print time, filament) is fetched asynchronously.
     * @param force If true, ignore the in-flight guard (use after reconnect
     *              when the previous RPC was likely lost with the old socket)
     */
    void refresh_files(bool force = false);

    /**
     * @brief Fetch metadata for a range of files (lazy loading)
     *
     * Only fetches metadata for files that haven't been fetched yet.
     * Called initially for visible items, then on scroll for newly visible items.
     *
     * @param start Start index (inclusive)
     * @param end End index (exclusive)
     */
    void fetch_metadata_range(size_t start, size_t end);

    /**
     * @brief Process metadata result and update file list
     *
     * Extracted from fetch_metadata_range to allow reuse from metascan fallback.
     * Handles thumbnail fetching, UI updates, and detail view synchronization.
     *
     * @param i Index in file_list_
     * @param filename Filename for validation
     * @param metadata Metadata to process
     */
    void process_metadata_result(size_t i, const std::string& filename,
                                 const FileMetadata& metadata);

    /**
     * @brief Navigate into a subdirectory
     *
     * @param dirname Directory name to navigate into
     */
    void navigate_to_directory(const std::string& dirname);

    /**
     * @brief Navigate up to parent directory
     *
     * Does nothing if already at root gcodes directory.
     */
    void navigate_up();

    /**
     * @brief Check if currently at root directory
     *
     * @return true if at root gcodes directory
     */
    bool is_at_root() const {
        return current_path_.empty();
    }

    /**
     * @brief Get current directory path
     *
     * @return Current path relative to gcodes root (empty = root)
     */
    const std::string& get_current_path() const {
        return current_path_;
    }

    /**
     * @brief Set IMoonrakerAPI and trigger file refresh
     *
     * Overrides base class to automatically refresh file list when API becomes available.
     *
     * @param api Pointer to IMoonrakerAPI (may be nullptr to disconnect)
     */
    void set_api(IMoonrakerAPI* api);

    /**
     * @brief Set selected file data and update subjects
     *
     * @param filename File name
     * @param thumbnail_src Pre-scaled thumbnail path for cards (.bin)
     * @param original_url Moonraker thumbnail URL (for detail view PNG lookup)
     * @param print_time Formatted print time
     * @param filament_weight Formatted filament weight
     * @param layer_count Formatted layer count (or "--" if unknown)
     * @param print_height Formatted print height (or "--" if unknown)
     * @param modified_timestamp File modification time (for cache validation)
     * @param layer_height Formatted layer height string (e.g., "0.24 mm")
     * @param filament_type Filament type/name (e.g., "ABS" or "PolyMaker PolyLite ABS")
     */
    void set_selected_file(const char* filename, const char* thumbnail_src,
                           const char* original_url, const char* print_time,
                           const char* filament_weight, const char* layer_count,
                           const char* print_height, time_t modified_timestamp,
                           const char* layer_height = "", const char* filament_type = "");

    /**
     * @brief Show detail view overlay for selected file
     */
    void show_detail_view();

    /// Forward the "Show sliced colors" toggle to the detail view.
    void forward_sliced_colors_toggle(bool checked);

    /**
     * @brief Programmatically select a file by name and show detail view
     *
     * Searches for the file in the current file list and opens its detail view.
     * Used by --select-file CLI flag for testing the detail view.
     *
     * @param filename File name to select (matches against filename field)
     * @return true if file was found and selected, false otherwise
     */
    bool select_file_by_name(const std::string& filename);

    /**
     * @brief Set a pending file selection (for --select-file flag)
     *
     * The file will be auto-selected when the file list is populated.
     * Used because files are loaded asynchronously.
     *
     * @param filename File name to select when list is loaded
     */
    void set_pending_file_selection(const std::string& filename);

    /**
     * @brief Set sort to "recently modified" (MODIFIED descending)
     *
     * Unlike sort_by(), this doesn't toggle — it forces MODIFIED descending.
     * Used by the home panel Library "Recent" action.
     */
    void set_sort_recent();

    /**
     * @brief Request navigation back to home when the detail view closes.
     *
     * Used by "Print Last" to skip the file browser on back navigation.
     * Consumed on next on_activate() after the detail overlay closes.
     */
    void set_return_to_home_on_close();

    /**
     * @brief Hide detail view overlay
     */
    void hide_detail_view();

    /**
     * @brief Access the print start controller (owned by this panel).
     *
     * Exposed so the print status panel's reprint path can route through the
     * controller's initiate_reprint() — sharing the Snapmaker U1 native
     * pre-print send instead of bypassing it.
     */
    [[nodiscard]] helix::ui::PrintStartController* get_print_start_controller() const {
        return print_controller_.get();
    }

    /**
     * @brief Show delete confirmation dialog
     */
    void show_delete_confirmation();

    /**
     * @brief Set reference to print status panel
     *
     * Required for navigating to print status after starting a print.
     *
     * @param panel Print status panel widget
     */
    void set_print_status_panel(lv_obj_t* panel);

    /**
     * @brief Set UsbManager for USB file access
     *
     * @param manager Pointer to UsbManager (may be nullptr)
     */
    void set_usb_manager(UsbManager* manager);

    /**
     * @brief Handle USB drive inserted event
     *
     * Called when a USB drive is inserted. Shows the USB tab in the source selector.
     */
    void on_usb_drive_inserted();

    /**
     * @brief Handle USB drive removal event
     *
     * Called when a USB drive is removed. Hides the USB tab in the source selector.
     * If currently viewing USB source, switches to Printer source and clears file list.
     */
    void on_usb_drive_removed();

    /**
     * @brief Start print of currently selected file.
     *
     * @param force If false (default) and the detail view reports an empty-
     *              filament-slot warning for any tool required by this file,
     *              shows a confirmation modal first. The modal's confirm
     *              callback re-enters with force=true to bypass the check.
     */
    void start_print(bool force = false);

    /**
     * @brief Show the enriched pre-flight filament check modal.
     *
     * Replaces the simple confirmation dialog. Renders a per-tool breakdown of
     * intended vs seated filament and offers Remap… / Cancel / Print Anyway.
     * Print Anyway re-enters start_print(true); Remap… routes by backend
     * strategy via on_preflight_remap().
     */
    void show_preflight_modal(const helix::PreflightResult& pf);

    /**
     * @brief Route a Remap… request to the single unified remap UI.
     *
     * Thin wrapper around open_remap_modal(). The per-backend difference lives
     * at the apply layer (apply_remap) and at print-start (already strategy-
     * dispatched), never at the opener.
     */
    void on_preflight_remap();

    /**
     * @brief Delete currently selected file
     */
    void delete_file();

    /**
     * @brief Hide delete confirmation dialog
     */
    void hide_delete_confirmation();

  private:
    friend struct PrintSelectPanelTestAccess;

    // Single unified remap entry point. Builds the picker UNIFORMLY for every
    // backend (tool_info from preflight checks, slots from AmsState) and shows
    // the one shared remap_modal_. Called by on_preflight_remap(), the detail
    // card tap (any non-None backend), and the embedded mapping card tap.
    void open_remap_modal();
    // Strategy-dispatched APPLY for the chosen mappings. The ONLY per-backend
    // branch: GcodeRewrite rewrites + prints; Native / SnapmakerNative push to
    // the shared card store (backend-specific send happens at print-start).
    void apply_remap(const std::vector<helix::ToolMapping>& updated);

    //
    // === Constants ===
    //

    // Card layout constants (used by calculate_card_dimensions).
    // Width bounds live at namespace scope as CARD_WIDTH_* so choose_card_columns()
    // can share them; these aliases keep the existing call sites reading the same.
    static constexpr int CARD_MIN_WIDTH = CARD_WIDTH_MIN;
    static constexpr int CARD_MAX_WIDTH = CARD_WIDTH_MAX;
    static constexpr int CARD_DEFAULT_HEIGHT = 245;
    static constexpr int ROW_COUNT_3_MIN_HEIGHT = 520;
    static constexpr const char* FOLDER_UP_ICON = "A:assets/images/folder-up.png";

    //
    // === Widget References ===
    //

    lv_obj_t* card_view_container_ = nullptr;
    lv_obj_t* list_view_container_ = nullptr;
    lv_obj_t* list_rows_container_ = nullptr;
    lv_obj_t* empty_state_container_ = nullptr;
    lv_obj_t* view_toggle_btn_ = nullptr;
    lv_obj_t* view_toggle_icon_ = nullptr;
    lv_obj_t* print_status_panel_widget_ = nullptr;

    //
    // === Subject Buffers ===
    //

    SubjectManager subjects_; ///< RAII manager for automatic subject cleanup

    lv_subject_t selected_filename_subject_; ///< Raw filename (for API/lookups)
    char selected_filename_buffer_[128];

    lv_subject_t selected_display_filename_subject_; ///< Display name (no .gcode extension)
    char selected_display_filename_buffer_[128];

    lv_subject_t selected_thumbnail_subject_;
    char selected_thumbnail_buffer_[256];

    lv_subject_t selected_detail_thumbnail_subject_; ///< Full-res PNG for detail view
    char selected_detail_thumbnail_buffer_[256];

    lv_subject_t selected_print_time_subject_;
    char selected_print_time_buffer_[32];

    lv_subject_t selected_filament_weight_subject_;
    char selected_filament_weight_buffer_[32];

    lv_subject_t selected_layer_count_subject_;
    char selected_layer_count_buffer_[32];

    lv_subject_t selected_print_height_subject_;
    char selected_print_height_buffer_[32];

    lv_subject_t selected_layer_height_subject_;
    char selected_layer_height_buffer_[32];

    lv_subject_t selected_filament_type_subject_;
    char selected_filament_type_buffer_[64]; // Longer for full names like "PolyMaker PolyLite ABS"

    lv_subject_t detail_view_visible_subject_;

    /// View mode subject: 0 = CARD, 1 = LIST (XML bindings control visibility)
    lv_subject_t view_mode_subject_;

    /// Can start print subject: 1 = can print, 0 = print in progress (disables button via XML
    /// binding)
    lv_subject_t can_print_subject_;

    //
    // === Panel State ===
    //

    std::vector<PrintFileData> file_list_;
    std::string current_path_;           ///< Current directory path (empty = root gcodes dir)
    std::string last_populated_path_;    ///< Track path for scroll preservation on refresh
    std::string selected_filament_type_; ///< Filament type of selected file (for dropdown default)
    std::vector<std::string> selected_filament_colors_; ///< Tool colors of selected file
    std::vector<std::string>
        selected_filament_materials_;        ///< Per-tool material types of selected file
    size_t selected_file_size_bytes_ = 0;    ///< File size of selected file (for safety checks)
    time_t selected_modified_timestamp_ = 0; ///< mtime of selected file (tools-used cache key)
    uint64_t selected_gcode_end_byte_ = 0;   ///< G-code body end offset (sizes the footer read)
    FileHistoryStatus selected_history_status_ =
        FileHistoryStatus::NEVER_PRINTED; ///< History status of selected file
    int selected_success_count_ = 0;      ///< Success count of selected file
    std::string
        pending_file_selection_; ///< File to auto-select when list is populated (--select-file)
    bool return_to_home_on_close_ = false;
    int return_home_activation_count_ = 0;
    PrintSelectViewMode current_view_mode_ = PrintSelectViewMode::CARD;
    PrintSelectSortColumn current_sort_column_ = PrintSelectSortColumn::MODIFIED;
    PrintSelectSortDirection current_sort_direction_ = PrintSelectSortDirection::DESCENDING;
    bool panel_initialized_ = false;               ///< Guard flag for resize callback
    bool first_activation_ = true;                 ///< Skip redundant refresh on first activation
    bool detail_view_open_ = false;                ///< True while detail view overlay is showing
    bool files_changed_while_detail_open_ = false; ///< True if filelist changed while detail open
    bool was_deactivated_ = false; ///< True if panel was fully deactivated (navigated away)
    /// Set in on_activate() before refresh_files(). Tells the on_files_ready merge to
    /// drop the carried-forward metadata_fetched flag for entries whose thumbnail_path
    /// is empty, giving them one retry per panel visit. Self-heals files whose metadata
    /// extraction failed transiently on Moonraker (e.g. JSON-RPC -32601 during upload).
    bool retry_missing_thumbnails_on_refresh_ = false;

    // Debounce timer for view refresh (prevents rebuilding views for each metadata callback)
    lv_timer_t* refresh_timer_ = nullptr;
    static constexpr uint32_t REFRESH_DEBOUNCE_MS = 50; ///< Debounce delay for view refresh

    // Periodic polling timer for file list (fallback when WebSocket notifications are missed)
    lv_timer_t* file_poll_timer_ = nullptr;
    static constexpr uint32_t FILE_POLL_INTERVAL_MS = 5000; ///< 5s polling fallback
    /// Single-flight guard for overlapping get_directory RPCs, with a 30s
    /// self-heal: if a response is silently lost the flag would otherwise wedge
    /// forever and the panel would stop refreshing. The threshold sits between
    /// worst-case slow embedded responses (K1C ~5-8s on large dirs) and the RPC
    /// layer's 60s ceiling (which normally clears the flag via the error
    /// callback; this is the panel-level safety net). See in_flight_guard.h.
    helix::InFlightGuard refresh_guard_{std::chrono::milliseconds(30000)};

    // Virtualized view modules (extracted for maintainability)
    std::unique_ptr<helix::ui::PrintSelectCardView> card_view_;
    std::unique_ptr<helix::ui::PrintSelectListView> list_view_;

    // Detail view overlay manager (handles file detail display, delete confirmation)
    std::unique_ptr<helix::ui::PrintSelectDetailView> detail_view_;

    // USB file source manager (handles Printer/USB source switching)
    std::unique_ptr<helix::ui::PrintSelectUsbSource> usb_source_;
    UsbManager* pending_usb_manager_ = nullptr;

    // File data provider (handles Moonraker file fetching and metadata)
    std::unique_ptr<helix::ui::PrintSelectFileProvider> file_provider_;

    // Print start controller (handles print initiation workflow, warnings)
    std::unique_ptr<helix::ui::PrintStartController> print_controller_;

    // THE single tool-remap picker for every backend. Embedded so its lifecycle
    // follows the panel; ModalStack manages the on-screen dialog. Reused across
    // all remap invocations (AFC/CFS/Happy Hare/AD5X-IFS native, ACE gcode
    // rewrite, Snapmaker U1 native). open_remap_modal() configures it; the apply
    // strategy lives in apply_remap().
    helix::ui::FilamentMappingModal remap_modal_;

    // File sorter (handles sorting logic for file list)
    helix::ui::PrintSelectFileSorter file_sorter_;

    // Path navigator (handles directory navigation logic)
    helix::ui::PrintSelectPathNavigator path_navigator_;

    // Observers for reactive updates (ObserverGuard handles cleanup)
    ObserverGuard connection_observer_;
    ObserverGuard print_state_observer_; ///< Observes print state to enable/disable print button
    ObserverGuard
        print_in_progress_observer_;      ///< Observes workflow in-progress for immediate disable
    ObserverGuard helix_plugin_observer_; ///< Observes plugin status for install prompt

    /// Observer for PrintHistoryManager - updates file status when history changes
    helix::HistoryChangedCallback history_observer_;

    /// Guards async API callbacks from accessing a destroyed instance
    helix::AsyncLifetimeGuard lifetime_;

    /// Compatibility alive flag for ThumbnailLoadContext (which uses shared_ptr<atomic<bool>> API)
    std::shared_ptr<std::atomic<bool>> thumbnail_alive_ = std::make_shared<std::atomic<bool>>(true);

    /// Navigation generation counter: incremented on each directory change.
    /// Metadata callbacks capture the current value and discard results
    /// if the generation has changed (user navigated away).
    std::atomic<uint32_t> nav_generation_{0};

    // File list change notification handler name (for unregistering)
    std::string filelist_handler_name_;

    // Plugin installer for helix_print Moonraker plugin
    helix::HelixPluginInstaller plugin_installer_;
    PluginInstallModal plugin_install_modal_;

    //
    // === Internal Methods ===
    //

    /**
     * @brief Calculate optimal card dimensions for current container size
     */
    CardDimensions calculate_card_dimensions();

    /**
     * @brief Populate card view with file list (delegates to card_view_)
     * @param preserve_scroll If true, preserve scroll position; otherwise reset to top
     */
    void populate_card_view(bool preserve_scroll = false);

    /**
     * @brief Populate list view with file list (delegates to list_view_)
     * @param preserve_scroll If true, preserve scroll position; otherwise reset to top
     */
    void populate_list_view(bool preserve_scroll = false);

    /**
     * @brief Animate view container entrance with fade-in
     * @param container The view container to animate (card or list)
     */
    void animate_view_entrance(lv_obj_t* container);

    /**
     * @brief Handle scroll event for virtualization
     * @param container The scrolled container (card or list view)
     */
    void handle_scroll(lv_obj_t* container);

    /**
     * @brief Refresh content of currently visible cards/rows
     */
    void refresh_visible_content();

    /**
     * @brief Check if Moonraker has symlink access to USB files
     *
     * Queries Moonraker for files in the "usb/" path. If files exist,
     * it means there's a symlink (gcodes/usb -> /media/sda1) and the
     * USB tab should be hidden since files are accessible via Printer source.
     */
    void check_moonraker_usb_symlink();

    /**
     * @brief Schedule a debounced view refresh
     *
     * Instead of rebuilding views immediately, this schedules a single refresh
     * after REFRESH_DEBOUNCE_MS. Multiple calls within the debounce window
     * only trigger one actual refresh. This prevents O(n²) widget rebuilds
     * when metadata arrives for each file.
     */
    void schedule_view_refresh();

    /**
     * @brief Apply current sort settings to file_list_
     */
    void apply_sort();

    /**
     * @brief Hide the "Recently Printed" context banner if visible
     */
    void hide_context_banner();

    /**
     * @brief Update empty state visibility based on file_list_ size
     */
    void update_empty_state();

    /**
     * @brief Update print button enabled/disabled state based on print job state
     *
     * Disables the print button when a print is in progress to prevent concurrent prints.
     */
    void update_print_button_state();

    /**
     * @brief Update sort indicator icons on column headers
     */
    void update_sort_indicators();

    /**
     * @brief Create detail view overlay (called once during setup)
     */
    void create_detail_view();

    /**
     * @brief Handle resize event for responsive card layout
     */
    void handle_resize();

    /**
     * @brief Attach click handler to a file card
     *
     * @param card Card widget
     * @param file_index Index into file_list_
     */
    void attach_card_click_handler(lv_obj_t* card, size_t file_index);

    /**
     * @brief Attach click handler to a list row
     *
     * @param row Row widget
     * @param file_index Index into file_list_
     */
    void attach_row_click_handler(lv_obj_t* row, size_t file_index);

    /**
     * @brief Copy a file's fields into the panel's "selected file" state.
     *
     * Shared by handle_file_click() (which then opens the detail view) and
     * on_file_long_pressed() (which then opens the delete confirmation modal).
     * Only valid when file.is_dir == false.
     */
    void apply_file_selection(const PrintFileData& file);

    /**
     * @brief Handle file card/row click
     *
     * @param file_index Index of clicked file in file_list_
     */
    void handle_file_click(size_t file_index);

    /**
     * @brief Handle long-press on a file card: select the file then show the delete modal.
     * @param file_index Index into file_list_
     *
     * Bounds-checks the index, ignores directories (belt-and-suspenders — the card view
     * also filters), applies the selection, then delegates to show_delete_confirmation().
     */
    void on_file_long_pressed(size_t file_index);

    /**
     * @brief Merge print history status into file list
     *
     * Uses PrintHistoryManager to populate FileHistoryStatus and success_count
     * for each file. Also detects "currently printing" by comparing with
     * PrinterState's print_filename.
     */
    void merge_history_into_file_list();

    //
    // === Static Callbacks (trampolines) ===
    //

    static void on_resize_static(void* user_data);
    static void on_scroll_static(lv_event_t* e);
    static void on_file_clicked_static(lv_event_t* e);
};

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================
//
// Single global instance for compatibility with main.cpp panel creation.
// Created lazily on first use.
// ============================================================================

/**
 * @brief Get or create the global PrintSelectPanel instance
 *
 * @param printer_state Reference to helix::PrinterState
 * @param api Pointer to IMoonrakerAPI (may be nullptr)
 * @return Pointer to the global instance
 */
PrintSelectPanel* get_print_select_panel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

/**
 * @brief Get reference to the global PrintSelectPanel instance
 *
 * @note Must be called after get_print_select_panel() has been called at least once
 * @return Reference to the global instance
 */
PrintSelectPanel& get_global_print_select_panel();
