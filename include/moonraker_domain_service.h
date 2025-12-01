// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MOONRAKER_DOMAIN_SERVICE_H
#define MOONRAKER_DOMAIN_SERVICE_H

#include <set>
#include <string>
#include <vector>

/**
 * @brief Bed mesh profile data from Klipper
 *
 * Contains the probed Z-height matrix and associated metadata for bed mesh
 * visualization and compensation. This is a forward-declared structure
 * used by the domain service interface.
 */
struct BedMeshProfile {
    std::string name;                              ///< Profile name (e.g., "default", "adaptive")
    std::vector<std::vector<float>> probed_matrix; ///< Z height grid (row-major order)
    float mesh_min[2];                             ///< Min X,Y coordinates
    float mesh_max[2];                             ///< Max X,Y coordinates
    int x_count;                                   ///< Probes per row
    int y_count;                                   ///< Number of rows
    std::string algo;                              ///< Interpolation algorithm

    BedMeshProfile() : mesh_min{0, 0}, mesh_max{0, 0}, x_count(0), y_count(0) {}
};

/**
 * @brief Interface for Moonraker domain operations
 *
 * Abstracts domain logic (hardware discovery, bed mesh, object exclusion)
 * from the transport layer. This interface enables:
 *
 * 1. **Separation of concerns**: Transport layer (MoonrakerClient) handles
 *    WebSocket communication; domain layer handles printer-specific logic.
 *
 * 2. **Testability**: Domain logic can be tested independently of network I/O.
 *
 * 3. **Flexibility**: Different implementations can provide hardware guessing
 *    for different printer types or configurations.
 *
 * ## Hardware Discovery Methods
 *
 * The `guess_*` methods implement heuristic-based hardware identification:
 * - Search discovered hardware lists for matching patterns
 * - Prioritize exact matches over substring matches
 * - Fall back to common naming conventions
 *
 * ## Bed Mesh Operations
 *
 * Bed mesh data is received via Moonraker subscriptions and cached locally.
 * The interface provides read-only access to the cached data.
 *
 * ## Object Exclusion
 *
 * Object exclusion state is queried from Klipper's exclude_object module.
 * Available objects are defined in G-code; excluded objects are tracked
 * during the print.
 */
class IMoonrakerDomainService {
  public:
    virtual ~IMoonrakerDomainService() = default;

    // ========================================================================
    // Hardware Discovery
    // ========================================================================

    /**
     * @brief Guess the most likely bed heater from discovered hardware
     *
     * Searches heaters for names containing "bed", "heated_bed", "heater_bed".
     * Returns the first match found using priority-based search:
     * 1. Exact match: "heater_bed"
     * 2. Exact match: "heated_bed"
     * 3. Substring match: any heater containing "bed"
     *
     * @return Bed heater name or empty string if none found
     */
    virtual std::string guess_bed_heater() const = 0;

    /**
     * @brief Guess the most likely hotend heater from discovered hardware
     *
     * Searches heaters for names containing "extruder", "hotend", "e0".
     * Prioritizes "extruder" (base extruder) over numbered variants.
     * Priority order:
     * 1. Exact match: "extruder"
     * 2. Exact match: "extruder0"
     * 3. Substring match: any heater containing "extruder"
     * 4. Substring match: any heater containing "hotend"
     * 5. Substring match: any heater containing "e0"
     *
     * @return Hotend heater name or empty string if none found
     */
    virtual std::string guess_hotend_heater() const = 0;

    /**
     * @brief Guess the most likely bed temperature sensor from discovered hardware
     *
     * First checks heaters for bed heater (heaters have built-in sensors).
     * If no bed heater found, searches sensors for names containing "bed".
     *
     * @return Bed sensor name or empty string if none found
     */
    virtual std::string guess_bed_sensor() const = 0;

    /**
     * @brief Guess the most likely hotend temperature sensor from discovered hardware
     *
     * First checks heaters for extruder heater (heaters have built-in sensors).
     * If no extruder heater found, searches sensors for names containing
     * "extruder", "hotend", "e0".
     *
     * @return Hotend sensor name or empty string if none found
     */
    virtual std::string guess_hotend_sensor() const = 0;

    // ========================================================================
    // Bed Mesh Operations
    // ========================================================================

    /**
     * @brief Get currently active bed mesh profile
     *
     * Returns pointer to the active mesh profile loaded from Moonraker's
     * bed_mesh object. The probed_matrix field contains the 2D Z-height
     * array ready for rendering.
     *
     * @return Pointer to active mesh profile, or nullptr if none loaded
     */
    virtual const BedMeshProfile* get_active_bed_mesh() const = 0;

    /**
     * @brief Get list of available mesh profile names
     *
     * Returns profile names from bed_mesh.profiles (e.g., "default",
     * "adaptive", "calibration"). Empty vector if no profiles available
     * or discovery hasn't completed.
     *
     * @return Vector of profile names
     */
    virtual std::vector<std::string> get_bed_mesh_profiles() const = 0;

    /**
     * @brief Check if bed mesh data is available
     *
     * @return true if a mesh profile with valid probed_matrix is loaded
     */
    virtual bool has_bed_mesh() const = 0;

    // ========================================================================
    // Object Exclusion Operations
    // ========================================================================

    /**
     * @brief Get set of currently excluded object names
     *
     * Returns the names of objects that have been excluded from the current
     * print via EXCLUDE_OBJECT command. Empty set if no objects excluded
     * or exclude_object module not enabled.
     *
     * @return Set of excluded object names
     */
    virtual std::set<std::string> get_excluded_objects() const = 0;

    /**
     * @brief Get list of available objects in current print
     *
     * Returns names of all objects defined in the current G-code file
     * (from EXCLUDE_OBJECT_DEFINE commands). Empty vector if no objects
     * defined or not currently printing.
     *
     * @return Vector of available object names
     */
    virtual std::vector<std::string> get_available_objects() const = 0;
};

#endif // MOONRAKER_DOMAIN_SERVICE_H
