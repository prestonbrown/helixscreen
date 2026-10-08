// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_client_mock.h"

#include <string>
#include <utility>

namespace helix {

// Grants tests access to MoonrakerClientMock internals that have no public
// equivalent. Declared a friend of MoonrakerClientMock.
class MoonrakerClientMockTestAccess {
  public:
    // Replace one JSON-RPC method's handler, to script an answer the mock's
    // stock handler never gives.
    static void set_method_handler(MoonrakerClientMock& c, const std::string& method,
                                   mock_internal::MethodHandler handler) {
        c.method_handlers_[method] = std::move(handler);
    }
};

} // namespace helix
