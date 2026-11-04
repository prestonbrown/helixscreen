// SPDX-License-Identifier: GPL-3.0-or-later
//
// include/json_fwd.h declares nlohmann::basic_json by hand. This unit includes it ahead of
// the full library, so the library's definition has to accept that declaration, and it pins
// the defaults the declaration supplies. It stops compiling when a libhv bump or the
// firmware's vendored copy moves away from them.

// clang-format off
#include "json_fwd.h"
#include "hv/json.hpp"
// clang-format on

#include <type_traits>

static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR == 12,
              "json_fwd.h copies the 3.12 forward declarations: re-copy them");
static_assert(std::is_same<::json, nlohmann::basic_json<std::map, std::vector, std::string, bool,
                                                        std::int64_t, std::uint64_t, double,
                                                        std::allocator, nlohmann::adl_serializer,
                                                        std::vector<std::uint8_t>, void>>::value,
              "json_fwd.h declares different basic_json defaults from the library");
