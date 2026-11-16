// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace helix {
namespace detail {
inline std::atomic<std::size_t>& type_tag_counter() {
    static std::atomic<std::size_t> counter{0};
    return counter;
}
} // namespace detail

/// A value of type_tag<T>().
using TypeTag = std::size_t;

/// RTTI-free per-type identity: a process-local monotonic ID, assigned on
/// first use. Replacement for std::type_index map keys under -fno-rtti.
/// Tags are NOT stable across runs - never persist or log them as identity.
/// The one future hazard: if a dlopen'd plugin ever instantiates this
/// template across the .so boundary (today's plugin/BT boundaries are C ABI),
/// duplicate statics could yield different tags per module.
template <typename T> std::size_t type_tag() {
    static_assert(std::is_same_v<T, std::remove_cv_t<std::remove_reference_t<T>>>,
                  "type_tag<T>: pass the plain type, not a cv/ref-qualified one");
    static const std::size_t tag =
        detail::type_tag_counter().fetch_add(1, std::memory_order_relaxed);
    return tag;
}

/// The kind_tag() of a class that declares none. No type_tag<T>() ever equals
/// it, so a typed lookup over such a class answers nullptr for every T.
inline constexpr std::size_t kUndeclaredKind = static_cast<std::size_t>(-1);

} // namespace helix

/**
 * @brief Declare a class's concrete kind for its base's typed lookup
 *
 * One line in the body of every class a `from_event<T>()` (Modal, OverlayBase)
 * must resolve: `HELIX_KIND_TAG(MyModal)`, naming the class being declared.
 * Emits no access specifier, so it can sit anywhere in the body; the override
 * may be private, since every call goes through the base's public declaration.
 */
#define HELIX_KIND_TAG(T)                                                                          \
    std::size_t kind_tag() const override {                                                        \
        return helix::type_tag<T>();                                                               \
    }
