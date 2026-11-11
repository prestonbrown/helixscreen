// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix::ui {

/**
 * @brief Manages directory path navigation for the print file selector.
 *
 * Tracks the current path relative to the gcodes root directory.
 * An empty path represents the root gcodes directory.
 */
class PrintSelectPathNavigator {
  public:
    PrintSelectPathNavigator() = default;

    /**
     * @brief Navigate into a subdirectory.
     * @param dirname Name of directory to enter
     */
    void navigate_to(const std::string& dirname);

    /**
     * @brief Navigate to parent directory. No-op if already at root.
     */
    void navigate_up();

    /**
     * @brief Check if at root gcodes directory.
     * @return true if current path is empty (at root)
     */
    bool is_at_root() const {
        return current_path_.empty();
    }

    /**
     * @brief Get current path relative to gcodes root.
     * @return Current path (empty string = root)
     */
    const std::string& current_path() const {
        return current_path_;
    }

    void reset() {
        current_path_.clear();
    }

  private:
    std::string current_path_;
};

} // namespace helix::ui
