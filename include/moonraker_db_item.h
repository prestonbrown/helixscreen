// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "moonraker_error.h"

#include <functional>
#include <string>

namespace helix {

class IMoonrakerAPI;

/// Write a Moonraker database item unless the stored value already equals
/// @p value, so several screens on one printer do not rewrite it in turn.
/// @p on_written runs only after an actual write, @p on_current when the
/// stored value already matched and nothing was written. Callbacks arrive on
/// the client's thread. @p api must outlive the reply, as the API object does.
void post_item_if_changed(IMoonrakerAPI& api, const std::string& namespace_name,
                          const std::string& key, const nlohmann::json& value,
                          helix::SharedCallback<void()> on_written,
                          helix::SharedCallback<void(const MoonrakerError&)> on_error,
                          helix::SharedCallback<void()> on_current = {});

} // namespace helix
