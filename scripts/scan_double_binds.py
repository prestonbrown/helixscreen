#!/usr/bin/env python3
"""Find widgets carrying two or more bindings that drive the same state or flag.

Bindings on one property of one widget compose: the bits are applied while any of
them holds. Each widget listed here is a place that relies on that.

A binding is a `bind_state_*`/`bind_flag_*` child element, or the
`moves_machine="true"` attribute, which the engine expands into a
`job_holds_machine` -> disabled binding on the element itself. Missing the
attribute form drops every control that carries the toolhead guard plus one
hand-written state binding, and the generated table's own count assertion cannot
see that the population shrank.

    scripts/scan_double_binds.py                  # human-readable inventory
    scripts/scan_double_binds.py --emit-cpp       # the table the test iterates

`--emit-cpp` writes tests/unit/double_bind_widgets.inc, which
tests/unit/test_xml_bind_compose.cpp includes. Regenerate it whenever ui_xml
grows or loses one of these widgets; the test's own count assertion is what
notices that you did not.
"""
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# The elements that write a state or a flag, mapped to the comparison they make.
# The value is (operator, negated) where the operator is applied as `subject OP ref`.
COMPARISONS = {
    "if_eq": ("eq", False),
    "if_not_eq": ("eq", True),
    "if_gt": ("gt", False),
    "if_ge": ("ge", False),
    "if_lt": ("ge", True),
    "if_le": ("gt", True),
}

# `style_text_color:checked="..."` is the engine's per-state attribute syntax, and
# a colon there is a namespace prefix to any XML parser. Flatten it before parsing.
STATE_ATTR_RE = re.compile(r'(\s)([A-Za-z_][\w]*):([A-Za-z_][\w]*)=(["\'])')

INT_RE = re.compile(r"-?\d+")

# moves_machine="true" is a binding the engine installs on the element itself:
# job_holds_machine eq 1 -> state disabled. It composes with the element's other
# state bindings exactly as a hand-written one would, so a control carrying both
# is a double-bind group even though only one of the two is spelled as a child
# element. Attributes are applied before children, so it goes at the head of the
# bucket - the order the runtime installs them in.
GUARD_ATTRIBUTE = "moves_machine"
GUARD_BINDING = {"subject": "job_holds_machine", "state": "disabled", "ref_value": "1"}


def lv_xml_to_bool(value):
    """The engine's truthiness: present and not the literal "false"."""
    return value is not None and value != "false"


def classify(tag):
    """(kind, comparison_suffix) for a binding element, or None if it is not one."""
    for kind, prefix in (("state", "bind_state_"), ("flag", "bind_flag_")):
        if not tag.startswith(prefix):
            continue
        suffix = tag[len(prefix):]
        if suffix == "if":
            return kind, None  # the cond= form
        if suffix in COMPARISONS:
            return kind, suffix
    return None


def evaluate(suffix, ref, invert, value):
    """Whether the binding asks for its bits when its subject reads `value`."""
    op, negated = COMPARISONS[suffix]
    if op == "eq":
        res = value == ref
    elif op == "gt":
        res = value > ref
    else:  # ge
        res = value >= ref
    return res != (negated != invert)


def hold_release(suffix, ref, invert):
    """Two subject values: one that makes this binding hold, one that releases it.

    Derived from the comparison rather than written down, so a binding whose
    ref_value changes cannot leave the test driving a stale number.
    """
    op, negated = COMPARISONS[suffix]
    if op == "eq":
        holds, releases = ref, ref + 1
    elif op == "gt":
        holds, releases = ref + 1, ref
    else:  # ge
        holds, releases = ref, ref - 1
    if negated != invert:
        holds, releases = releases, holds
    return holds, releases


def quiet_value(binds):
    """One subject value that releases EVERY binding in `binds`, or None.

    Two bindings can share a subject - `ui_breakpoint eq 1` and `ui_breakpoint
    eq 2` on one widget - and then no single value can hold them independently.
    The baseline still has to release both at once, so it is searched for rather
    than taken from any one binding.
    """
    # Nearest zero first, so the value a test drives stays in the range the
    # subject really uses instead of some far-off integer that also happens to
    # release everything.
    candidates = sorted({v for b in binds for v in (b.ref - 1, b.ref, b.ref + 1)}
                        | set(range(-2, 5)), key=lambda v: (abs(v), v))
    for value in candidates:
        if not any(evaluate(b.suffix, b.ref, b.invert, value) for b in binds):
            return value
    return None


class Binding:
    def __init__(self, el, suffix):
        self.subject = el.attrib.get("subject", "")
        self.invert = el.attrib.get("invert") in ("true", "1")
        self.ref = int(el.attrib.get("ref_value", "0"))
        self.suffix = suffix
        self.holds, self.releases = hold_release(suffix, self.ref, self.invert)

    def describe(self):
        inv = ' invert="true"' if self.invert else ""
        return '<bind_*_%s subject="%s" ref_value="%d"%s>' % (self.suffix, self.subject,
                                                              self.ref, inv)


def scan(path):
    """Yield (widget_name, kind, bits, [Binding]) for every composed property."""
    text = STATE_ATTR_RE.sub(r"\1\2__\3=\4", path.read_text(encoding="utf-8"))
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        print("  ! %s: %s" % (path, exc), file=sys.stderr)
        return

    for parent in root.iter():
        buckets = {}
        for child in parent:
            hit = classify(child.tag)
            if hit is None:
                continue
            kind, suffix = hit
            bits = child.attrib.get(kind)
            if not bits:
                continue
            # Three things a test cannot drive, all recorded rather than skipped so
            # the group still counts: a `cond=` expression, a subject the
            # instantiating parent supplies as `$param` (an empty one is the
            # engine's documented "no binding" escape), and a `ref_value` that is a
            # `${...}` substitution resolved per <repeat> iteration.
            subject = child.attrib.get("subject", "")
            unbindable = (suffix is None or not subject or subject.startswith("$")
                          or not INT_RE.fullmatch(child.attrib.get("ref_value", "")))
            buckets.setdefault((kind, bits), []).append(
                None if unbindable else Binding(child, suffix))

        if lv_xml_to_bool(parent.attrib.get(GUARD_ATTRIBUTE)):
            buckets.setdefault(("state", "disabled"), []).insert(
                0, Binding(ET.Element("bind_state_if_eq", GUARD_BINDING), "if_eq"))

        for (kind, bits), binds in sorted(buckets.items()):
            if len(binds) > 1:
                yield parent.attrib.get("name") or "<%s>" % parent.tag, kind, bits, binds


def collect(roots):
    for root in roots:
        for path in sorted(Path(root).rglob("*.xml")):
            for row in scan(path):
                yield (path,) + row


def report(rows):
    current = None
    for path, name, kind, bits, binds in rows:
        if path != current:
            print(path)
            current = path
        print("  %-28s %s=%s" % (name, kind, bits))
        for b in binds:
            print("      %s" % (b.describe() if b else "<not drivable from a test>"))
    print()
    print("widgets carrying 2+ bindings on one property: %d" % len(rows))


def emit_cpp(rows, out):
    """The DRIVABLE subset, as a table of C++ initialisers."""
    lines = [
        "// Generated by scripts/scan_double_binds.py --emit-cpp. Do not edit.",
        "//",
        "// Every widget in ui_xml/ carrying two or more bindings on one state or flag.",
        "// A moves_machine=\"true\" attribute is one of them: the engine installs the",
        "// job_holds_machine -> disabled binding for the element itself, so a control",
        "// carrying the attribute and one hand-written state binding is a group of two.",
        "// Each row names the component to build, the widget in it, the property, the",
        "// subjects involved with a value that releases every binding on them, and one",
        "// value per binding that makes exactly that binding ask for the property.",
        "//",
        "// `orders_testable` is false when two bindings share a subject: they cannot be",
        "// held one at a time, so only the each-alone phase applies to that row.",
        "//",
        "// Rows left out are counted in kUndrivableGroups - a cond= expression, a subject",
        "// the instantiating parent supplies as $param, or a ref_value that is a",
        "// per-iteration ${...} substitution.",
        "",
        "// clang-format off",
        "static const BindGroup kDoubleBindWidgets[] = {",
    ]
    drivable = skipped = 0
    for path, name, kind, bits, binds in rows:
        component = str(path).removeprefix("ui_xml/").removesuffix(".xml")
        if any(b is None for b in binds):
            skipped += 1
            continue

        # One entry per distinct subject, so a shared subject is quieted once.
        order = []
        for b in binds:
            if b.subject not in order:
                order.append(b.subject)
        quiets = {s: quiet_value([b for b in binds if b.subject == s]) for s in order}
        if any(q is None for q in quiets.values()):
            skipped += 1
            continue

        drivable += 1
        subj_src = ", ".join('{"%s", %d}' % (s, quiets[s]) for s in order)
        hold_src = ", ".join("{%d, %d}" % (order.index(b.subject), b.holds) for b in binds)
        lines.append('    {"%s", "%s", %s, "%s", {%s}, %d, {%s}, %d, %s},'
                     % (component, name,
                        "BindKind::State" if kind == "state" else "BindKind::Flag", bits,
                        subj_src, len(order), hold_src, len(binds),
                        "true" if len(order) == len(binds) else "false"))
    lines += [
        "};",
        "// clang-format on",
        "",
        "// Groups the scan found but that no test can drive; see the header comment.",
        "static constexpr int kUndrivableGroups = %d;" % skipped,
        "",
        "// Total groups in ui_xml/. A new double-bind widget moves this, which is the",
        "// point: it has to be regenerated and re-run rather than silently uncovered.",
        "static constexpr int kTotalGroups = %d;" % (drivable + skipped),
        "",
    ]
    Path(out).write_text("\n".join(lines))
    print("wrote %s: %d drivable, %d undrivable, %d total"
          % (out, drivable, skipped, drivable + skipped))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="*", default=["ui_xml"])
    ap.add_argument("--emit-cpp", nargs="?", const="tests/unit/double_bind_widgets.inc",
                    metavar="PATH", help="write the C++ table instead of a report")
    args = ap.parse_args()

    rows = list(collect(args.roots or ["ui_xml"]))
    if args.emit_cpp:
        emit_cpp(rows, args.emit_cpp)
    else:
        report(rows)


if __name__ == "__main__":
    main()
