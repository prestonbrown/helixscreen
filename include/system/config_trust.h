// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix::config_trust {

/// Update URL overrides from the root-owned state file
/// (app_constants::update::state_dir()/update_urls.json). Empty string means
/// "use the built-in default".
struct UpdateUrls {
    std::string r2_url;
    std::string dev_url;
    /// A minisign public key (base64 key line) release manifests must be signed with in place
    /// of the compiled-in release key; for a test server signing with its own key.
    std::string release_pubkey;
};

/// Read update_urls.json. The file only counts when it and its directory are
/// owned by root or by the app's own user and carry no group or world write
/// bit: the settings the updater acts on must not be writable by the web
/// UI's user. Absent, unparseable or untrusted file yields empty strings
/// (the compiled-in defaults), never a partial result: one bad field refuses
/// the whole file.
UpdateUrls read_update_urls();

/// Whether /log_path from settings.json may aim the app's log file at PATH.
/// The C++ twin of the launcher's HELIX_LOG_FILE rule
/// (scripts/helix-launcher.sh#helix_env_log_file_ok): an absolute *.log path
/// under /tmp, /var/log or the install dir whose subdirectory is owned by
/// root or this user with no group/world write bit, with no dot segments and
/// no symlink at the file itself; an existing file must be a regular file
/// with a single link (st_nlink == 1) owned by root or this user.
bool log_path_allowed(const std::string& path);

namespace detail {
/// Install root used by log_path_allowed. Empty (the default) means the real
/// app_get_install_root(). Tests redirect it because the test binary's
/// app_get_install_root() stub always returns "", which would leave the
/// install-dir branch of the rule permanently dark.
inline std::string& install_root_override_ref() {
    static std::string root;
    return root;
}
} // namespace detail

} // namespace helix::config_trust
