// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

// display_backend_drm.cpp / display_backend_fbdev.cpp are compiled by
// mk/display-lib.mk WITHOUT the precompiled header, so they never see
// lv_xml_component_scope_t (which ordinary TUs get transitively from
// lvgl_pch.h -> helix-xml/helix_xml.h). This header only references the type by
// pointer, so a forward-declaring typedef keeps it self-contained without
// pulling the whole XML engine into every includer. Identical to the typedef in
// lib/helix-xml/src/xml/lv_xml_types.h, which C++ permits to repeat.
typedef struct _lv_xml_component_scope_t lv_xml_component_scope_t;

namespace helix::xml {

// RAII override for the current XML subject-registration scope.
// When active, register_subject_in_current_scope() registers into the
// overridden scope; otherwise registration goes to the global scope.
//
// Thread-local: each thread has its own active scope. Push/pop must happen
// on the same thread; do not share the RAII guard across threads.
class ScopedSubjectRegistryOverride {
  public:
    explicit ScopedSubjectRegistryOverride(lv_xml_component_scope_t* scope);
    ~ScopedSubjectRegistryOverride();

    ScopedSubjectRegistryOverride(const ScopedSubjectRegistryOverride&) = delete;
    ScopedSubjectRegistryOverride& operator=(const ScopedSubjectRegistryOverride&) = delete;

  private:
    lv_xml_component_scope_t* previous_;
};

// Register a subject in the currently-active scope. Returns LV_RESULT_OK on success.
// When no ScopedSubjectRegistryOverride is active, registers into the global scope
// (equivalent to lv_xml_register_subject(nullptr, name, subject)).
lv_result_t register_subject_in_current_scope(const char* name, lv_subject_t* subject);

/// Drop `name` from the active scope so it stops resolving. The counterpart to
/// register_subject_in_current_scope(): the registry keeps resolving a name after
/// its lv_subject_t is freed, so any owner that does not live for the whole
/// process must unregister or leave the name pointing at dead memory.
lv_result_t unregister_subject_in_current_scope(const char* name);

/// As above, but only while `name` still resolves to `subject`. A newer owner that
/// re-published the name keeps its record when the previous owner tears down.
lv_result_t unregister_subject_in_current_scope(const char* name, const lv_subject_t* subject);

// Access the active scope (nullptr if none). For debug assertions only.
lv_xml_component_scope_t* current_scope();

} // namespace helix::xml
