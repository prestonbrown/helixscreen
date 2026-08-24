#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that layout-variant XML overrides stay wired to the same things as their base.

A file under ui_xml/<variant>/ replaces ui_xml/<file> wholesale (see
LayoutManager::resolve_xml_path). Nothing else diffs the two, so a variant can
quietly fall behind the base: a subject gets renamed, a button gains a callback,
a widget the C++ looks up by name disappears — and the only symptom is a warning
at parse time and a control that silently does nothing.

That is not hypothetical. prestonbrown/helixscreen#1203 proposed a portrait
print_status carrying bind_text="preparing_operation", a subject deleted in
c2bd749c7 a week earlier. lv_xml_label_parser.c logs LV_LOG_WARN and continues,
so the heading would have read a frozen "Preparing..." for the whole pre-print
phase with nothing failing.

What this gate compares, per (base, variant) pair:

  - widget names            name="..."
  - subject bindings        bind_text=, bind_value=, bind_icon=, bind_*=,
                            and subject="..." on <bind_flag_*>/<bind_state_*>/
                            <bind_style*> children
  - event callbacks         <event_cb callback="...">, *_callback="..."

Deliberately NOT compared: attributes, element structure, ordering, consts,
styles. Reflowing a panel is the entire point of a variant — only the wiring has
to match.

Exit 0 when every pair agrees, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from xml.etree import ElementTree

# Mirrors VARIANT_DIRS in src/layout_manager.cpp. Content subdirectories
# (components/, translations/) are not layout variants and are skipped.
VARIANT_DIRS = ("micro_portrait", "tiny_portrait", "portrait", "ultrawide", "micro", "tiny")

# Attributes naming a subject. bind_* covers bind_text/bind_value/bind_icon and
# any future sibling; subject= is how the <bind_flag_if_*> children name theirs.
SUBJECT_ATTR_RE = re.compile(r"^bind_[a-z_]+$")

# Attributes naming an event callback. <event_cb callback="..."/> plus the
# *_callback="..." form components use (action_button_callback, primary_callback).
CALLBACK_ATTR_RE = re.compile(r"^([a-z_]+_)?callback$")

# Attributes that name a *style*, not a subject — bind_style takes name= for the
# style and subject= for the trigger, so the subject is picked up via subject=.
NON_SUBJECT_BIND_ATTRS = {"bind_style"}

# Wiring a variant may legitimately drop, keyed by variant path -> kind -> items.
#
# Every entry needs a reason, and the reason has to be about the LAYOUT, not
# about the effort of fixing it. A variant that omits a row omits that row's
# bindings; that is the mechanism working. A variant that drops a binding whose
# widget it still renders is a bug and does not belong here.
ALLOWED_OMISSIONS: dict[str, dict[str, dict[str, str]]] = {
    "ui_xml/micro/controls_panel.xml": {
        "widget name": {
            "nozzle_status": "480x272 omits the Heating/Idle/Off word; temp_display still binds "
            "extruder_temp. Base hides these at breakpoint 1 anyway.",
            "bed_status": "as nozzle_status",
            "chamber_status": "as nozzle_status",
        },
        "subject": {
            "controls_nozzle_status": "consumed only by the omitted nozzle_status label",
            "controls_bed_status": "consumed only by the omitted bed_status label",
            "controls_chamber_status": "consumed only by the omitted chamber_status label",
            "controls_nozzle_label": "ToolState::nozzle_label() tool disambiguator; base hides it "
            "at breakpoint 1. Multi-tool micro loses the T0 hint — cosmetic.",
            "macro_header_visible": "collapses the Quick Actions header to reclaim vertical "
            "space; micro's macro section is laid out to not need it.",
        },
    },
    "ui_xml/portrait/print_status_panel.xml": {
        "subject": {
            "ui_breakpoint": "the base hides speed_flow_row at breakpoints 1-2 because the "
            "landscape controls column has no room for it. Portrait merged Speed/Flow and the "
            "filament+AMS cluster into ONE row that fits at every portrait width, so there is "
            "nothing left to hide — the row is rendered unconditionally, not dropped.",
        },
    },
}


# name= on these elements declares something (a style, a const, an API prop) —
# it is not a widget name that lv_obj_find_by_name will ever resolve.
DECLARATION_TAGS = {"style", "px", "percentage", "const", "color", "string", "prop", "subject"}

# LVGL's XML uses a state-qualified attribute form, style_bg_opa:disabled="100".
# The colon reads as an XML namespace prefix and makes a conforming parser reject
# the file outright, so flatten it before parsing.
STATE_QUALIFIED_ATTR_RE = re.compile(r"\s([a-zA-Z_][\w-]*):([a-zA-Z_][\w-]*)=")


class Collected:
    """Wiring a file references, split by what a mismatch would break."""

    def __init__(self) -> None:
        self.names: set[str] = set()
        self.subjects: set[str] = set()
        self.callbacks: set[str] = set()
        self.props: set[str] = set()


def collect(path: Path) -> Collected:
    """Return the wiring referenced anywhere in an XML file."""
    out = Collected()

    source = STATE_QUALIFIED_ATTR_RE.sub(r" \1__\2=", path.read_text(encoding="utf-8"))
    try:
        root = ElementTree.fromstring(source)
    except ElementTree.ParseError as exc:
        raise SystemExit(f"{path}: unparseable XML: {exc}")

    for el in root.iter():
        for attr, value in el.attrib.items():
            if not value:
                continue
            # "$foo" is a component-parameter reference, resolved from the <api>
            # <prop> of the same name at instantiation. Parity for those is
            # covered by comparing the prop declarations themselves.
            is_param_ref = value.startswith("$")

            if attr == "name":
                if el.tag == "prop":
                    out.props.add(value)
                elif el.tag.startswith("bind_"):
                    # <bind_style name="foo" subject="bar"/> — name= references a
                    # <style>, not a widget. The subject is what matters and is
                    # picked up from subject= below.
                    pass
                elif el.tag not in DECLARATION_TAGS:
                    out.names.add(value)
            elif attr == "subject":
                if not is_param_ref:
                    out.subjects.add(value)
            elif SUBJECT_ATTR_RE.match(attr) and attr not in NON_SUBJECT_BIND_ATTRS:
                if not is_param_ref:
                    out.subjects.add(value)
            elif CALLBACK_ATTR_RE.match(attr):
                if not is_param_ref:
                    out.callbacks.add(value)

    return out


def report(kind: str, base_rel: str, variant_rel: str, base: set[str], variant: set[str]) -> bool:
    """Print any divergence. Returns True only for divergence that breaks things.

    The two directions are not symmetric. Wiring the base has and the variant
    lacks is a silent runtime failure: a subject that never binds, a name the
    C++ resolves to nullptr, a button whose callback is gone. Wiring the variant
    adds is how a variant earns its keep — ui_xml/micro/theme_preview_overlay.xml
    observes ui_breakpoint that the base has no need for. Only the first fails.
    """
    allowed = ALLOWED_OMISSIONS.get(variant_rel, {}).get(kind, {})
    missing = {item for item in (base - variant) if item not in allowed}
    extra = variant - base
    if not missing and not extra:
        return False

    print(f"\n  {variant_rel}  vs  {base_rel}")
    for item in sorted(missing):
        print(f"    - missing {kind}: {item}")
    for item in sorted(extra):
        print(f"      (variant-only {kind}: {item})")
    return bool(missing)


def iter_variant_pairs(ui_xml: Path):
    """Yield (base_file, variant_file) for every ui_xml/<variant>/ override that
    has a same-relative-path base file.

    Shared with check_variant_content_drift.py so the two gates cannot disagree
    about what counts as a variant — VARIANT_DIRS above is the single list of
    directories either gate treats as a layout variant (as opposed to a content
    subdirectory like components/ or translations/).
    """
    for variant in VARIANT_DIRS:
        variant_dir = ui_xml / variant
        if not variant_dir.is_dir():
            continue

        for variant_file in sorted(variant_dir.rglob("*.xml")):
            rel = variant_file.relative_to(variant_dir)
            base_file = ui_xml / rel
            if not base_file.is_file():
                # A variant-only component is legitimate: nothing to compare
                # against, and resolve_xml_path falls back to base only when a
                # base exists.
                continue
            yield base_file, variant_file


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    ui_xml = repo_root / "ui_xml"
    if not ui_xml.is_dir():
        print(f"error: {ui_xml} not found", file=sys.stderr)
        return 1

    failures = 0
    pairs = 0

    for base_file, variant_file in iter_variant_pairs(ui_xml):
        pairs += 1
        base = collect(base_file)
        var = collect(variant_file)

        base_rel = str(base_file.relative_to(repo_root))
        variant_rel = str(variant_file.relative_to(repo_root))

        bad = False
        bad |= report("widget name", base_rel, variant_rel, base.names, var.names)
        bad |= report("subject", base_rel, variant_rel, base.subjects, var.subjects)
        bad |= report("callback", base_rel, variant_rel, base.callbacks, var.callbacks)
        bad |= report("api prop", base_rel, variant_rel, base.props, var.props)
        if bad:
            failures += 1

    if failures:
        print(
            f"\n✗ {failures} of {pairs} layout-variant override(s) diverge from their base.\n"
            "\n"
            "  A variant may reflow anything — attributes, nesting, order. What it may not do\n"
            "  is drop or invent wiring: a name the C++ resolves with lv_obj_find_by_name, a\n"
            "  subject binding, or an event callback. Those failures are silent at runtime.\n"
            "\n"
            "  If the base legitimately changed, mirror the change into the variant.\n"
        )
        return 1

    print(f"✓ variant parity: {pairs} override(s) match their base wiring")
    return 0


if __name__ == "__main__":
    sys.exit(main())
