// SPDX-License-Identifier: GPL-3.0-or-later
//
// nlohmann::json by name only, for a header that takes, stores or returns one by
// reference or pointer. A source that builds, reads or copies a json includes
// "hv/json.hpp" itself.
//
// These are the declarations the library's own forward section holds, and its guard
// is defined here, so the full header then skips that section: a default template
// argument may be given only once. Both the libhv copy and the firmware's vendored
// copy are 3.12.0; src/core/json_fwd_check.cpp fails to compile if either stops matching.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef INCLUDE_NLOHMANN_JSON_FWD_HPP_
#define INCLUDE_NLOHMANN_JSON_FWD_HPP_

namespace nlohmann {
inline namespace json_abi_v3_12_0 {

template <typename T = void, typename SFINAE = void> struct adl_serializer;

template <template <typename U, typename V, typename... Args> class ObjectType = std::map,
          template <typename U, typename... Args> class ArrayType = std::vector,
          class StringType = std::string, class BooleanType = bool,
          class NumberIntegerType = std::int64_t, class NumberUnsignedType = std::uint64_t,
          class NumberFloatType = double,
          template <typename U> class AllocatorType = std::allocator,
          template <typename T, typename SFINAE = void> class JSONSerializer = adl_serializer,
          class BinaryType = std::vector<std::uint8_t>, class CustomBaseClass = void>
class basic_json;

template <typename RefStringType> class json_pointer;

using json = basic_json<>;

template <class Key, class T, class IgnoredLess, class Allocator> struct ordered_map;

using ordered_json = basic_json<nlohmann::ordered_map>;

} // namespace json_abi_v3_12_0
} // namespace nlohmann

#endif // INCLUDE_NLOHMANN_JSON_FWD_HPP_

using json = nlohmann::json;
