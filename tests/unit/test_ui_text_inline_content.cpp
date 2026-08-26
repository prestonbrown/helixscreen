// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: drives a HelixScreen typography widget through ui_xml at runtime
//
// Inline element text through a HelixScreen SEMANTIC TYPOGRAPHY widget.
//
// The engine-level inline-text behaviour -- PCDATA accumulation, HTML-style
// whitespace collapse, entity decoding, $prop/#const substitution, attribute
// precedence, mixed content, the root-<view> drop -- now lives in the standalone
// helix-xml suite, in lib/helix-xml/tests/cases/test_inline_text.c. It drives
// lv_label there, because helix-xml has no idea `text_muted` exists.
//
// What CANNOT move is the half that is HelixScreen's own contract: `text_muted`
// (and its siblings text_heading / text_body / text_small / text_xs) is a widget
// registered by src/ui/ui_text.cpp with its own apply callback, ui_text_apply().
// The engine synthesizes {text, translation_tag} from the inline content and
// hands that pair to whatever apply_cb the element's processor registered -- so
// if ui_text_apply() ever stopped delegating to lv_xml_label_apply() (say, a
// refactor that only kept the stroke_* / text_transform handling), every
// `<text_muted>Some string</text_muted>` in ui_xml/ would silently render blank
// and nothing in the engine suite would notice.
//
// These two tests pin both halves of that forwarding contract:
//   - text=            : the literal reaches lv_label_set_text()
//   - translation_tag= : the SYNTHESIZED TAG is stored on the label, not merely
//                        applied as a literal. With no pack registered lv_tr()
//                        falls back to the tag string itself, so the two are
//                        indistinguishable by reading the text -- proving it
//                        takes a real pack and a language switch under a built
//                        tree, which is what the second test does.

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include "../catch_amalgamated.hpp"

namespace {

// Registers a one-off component from an XML string and instantiates it.
// Returns the named child, or nullptr.
lv_obj_t* create_and_find(XMLTestFixture& fx, const char* comp_name, const char* xml,
                          const char* child_name) {
    if (lv_xml_register_component_from_data(comp_name, xml) != LV_RESULT_OK)
        return nullptr;
    lv_obj_t* root = fx.create_component(comp_name);
    if (!root)
        return nullptr;
    return lv_obj_find_by_name(root, child_name);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "text_muted forwards inline text down to its label",
                 "[xml][inline_text][ui_text]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <text_muted name="msg">Hello world</text_muted>
  </view>
</component>)";
    lv_obj_t* msg = create_and_find(*this, "it_semantic", xml, "msg");
    REQUIRE(msg != nullptr);
    CHECK(lv_streq(lv_label_get_text(msg), "Hello world"));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "text_muted forwards the synthesized translation tag, not just the literal",
                 "[xml][inline_text][ui_text][translation]") {
    // Pack lifetime note: LVGLTestFixture calls lv_init_safe() once via
    // std::call_once (tests/lvgl_test_fixture.cpp) and is never torn down with
    // lv_deinit() between test cases -- LVGL, and any dynamic translation pack
    // registered into it, persists for the lifetime of the whole test binary.
    // LVGL's translation module has no "remove one pack" API (see
    // include/translation_loader.h) and lv_translation_deinit() nukes every
    // registered pack process-wide, which would be unsafe to call here since
    // other tests may load the real app translation catalog. So this pack is
    // deliberately left registered rather than partially/unsafely torn down.
    // That's safe: lv_translation_get() walks packs most-recently-added
    // first, so it only intercepts lookups for the exact tag below, and the
    // tag is a deliberately mangled nonsense string, not a near-miss of a real
    // catalog entry -- a real key differing only by case (e.g. "Print speed"
    // vs. the catalog's "Print Speed") would still pass today because
    // lv_streq()/expat comparisons are case-sensitive, but that's isolation by
    // accident: a future catalog addition matching this test's exact case
    // would silently shadow real UI strings for the rest of the run.
    lv_translation_pack_t* pack = lv_translation_add_dynamic();
    REQUIRE(pack != nullptr);
    lv_translation_add_language(pack, "en");
    lv_translation_add_language(pack, "de");
    lv_translation_tag_dsc_t* tag = lv_translation_add_tag(pack, "Zz inline i18n probe zZ");
    REQUIRE(tag != nullptr);
    lv_translation_set_tag_translation(pack, tag, 0, "Zz inline i18n probe zZ");
    lv_translation_set_tag_translation(pack, tag, 1, "Zz fake Deutsch probe zZ");
    lv_translation_set_language("en");

    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <text_muted name="msg">Zz inline i18n probe zZ</text_muted>
  </view>
</component>)";
    lv_obj_t* msg = create_and_find(*this, "it_semantic_i18n", xml, "msg");
    REQUIRE(msg != nullptr);
    CHECK(lv_streq(lv_label_get_text(msg), "Zz inline i18n probe zZ"));

    // If ui_text_apply() had dropped translation_tag= and only forwarded text=,
    // the label would keep the English literal across this switch.
    lv_translation_set_language("de");
    process_lvgl(50);
    CHECK(lv_streq(lv_label_get_text(msg), "Zz fake Deutsch probe zZ"));

    // Restore the global language selection so it doesn't bleed into
    // whatever test runs next in this process (packs themselves are left
    // registered -- see note above).
    lv_translation_set_language("en");
}
