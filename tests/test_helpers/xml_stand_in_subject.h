// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"

#include <array>
#include <map>
#include <memory>
#include <string>

namespace helix::test {

/// Process-lifetime storage for one stand-in subject per XML name. The XML registry keeps
/// the pointer past every fixture, so a stand-in can never be a fixture member.
inline helix::Subject& stand_in_slot(const char* name) {
    static std::map<std::string, std::unique_ptr<helix::Subject>> slots;
    auto& slot = slots[name];
    if (!slot) {
        slot = std::make_unique<helix::Subject>();
    }
    return *slot;
}

/// The subject XML resolves as @p name: the owner's when one publishes it, else a stand-in
/// seeded to @p value. A stand-in stays live after an owner takes the name and withdraws
/// it, so it is re-seeded and re-registered, never initialized a second time.
inline lv_subject_t* xml_stand_in_int(const char* name, int32_t value) {
    if (lv_subject_t* owned = lv_xml_get_subject(nullptr, name)) {
        return owned;
    }
    helix::Subject& subject = stand_in_slot(name);
    subject.publish_int(value);
    lv_xml_register_subject(nullptr, name, &subject);
    return &subject;
}

/// xml_stand_in_int() for a string subject of up to 63 characters.
inline lv_subject_t* xml_stand_in_string(const char* name, const char* value) {
    if (lv_subject_t* owned = lv_xml_get_subject(nullptr, name)) {
        return owned;
    }
    static std::map<std::string, std::array<char, 64>> buffers;
    helix::Subject& subject = stand_in_slot(name);
    if (subject.is_live()) {
        lv_subject_set_string(&subject, value);
    } else {
        auto& buf = buffers[name];
        subject.init_string(buf.data(), nullptr, buf.size(), value);
    }
    lv_xml_register_subject(nullptr, name, &subject);
    return &subject;
}

} // namespace helix::test
