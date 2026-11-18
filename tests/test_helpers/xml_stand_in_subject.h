// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <array>
#include <map>
#include <string>
#include <type_traits>

namespace helix::test {

/// The subject XML resolves as @p name: the owner's when one publishes it, else a stand-in
/// seeded to @p value. Stand-ins live for the process, since the registry keeps the pointer
/// past every fixture, and one an owner took over and withdrew is still live, so it is
/// re-seeded and re-registered, never initialized twice. Strings hold up to 63 characters.
template <typename T> lv_subject_t* xml_stand_in(const char* name, T value) {
    if (lv_subject_t* owned = lv_xml_get_subject(nullptr, name)) {
        return owned;
    }
    static std::map<std::string, std::pair<helix::Subject, std::array<char, 64>>> stand_ins;
    auto& [subject, buf] = stand_ins[name];
    if constexpr (std::is_same_v<T, const char*>) {
        subject.is_live() ? lv_subject_set_string(&subject, value)
                          : subject.init_string(buf.data(), nullptr, buf.size(), value);
    } else {
        subject.publish_int(value);
    }
    lv_xml_register_subject(nullptr, name, &subject);
    return &subject;
}

} // namespace helix::test
