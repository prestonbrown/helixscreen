#!/usr/bin/env python3
"""Find widgets carrying two or more bindings that drive the same state or flag.

Bindings on one property of one widget compose: the bits are applied while any of
them holds. Each widget listed here is a place that relies on that.

A binding is a `bind_state_*`/`bind_flag_*` child element, or the
`moves_machine="true"` attribute, which the engine expands into a
`machine_motion_blocked` -> disabled binding on the element itself. Missing the
attribute form drops every control that carries the toolhead guard plus one
hand-written state binding, and the generated table's own count assertion cannot
see that the population shrank.

    scripts/scan_double_binds.py                  # human-readable inventory
    scripts/scan_double_binds.py --emit-cpp       # the table the test iterates
    scripts/scan_double_binds.py --check          # is the committed table current?

`--emit-cpp` writes tests/unit/double_bind_widgets.inc, which
tests/unit/test_xml_bind_compose.cpp includes. Regenerate it whenever ui_xml
grows or loses one of these widgets. `--check` renders the table in memory and
fails, with a diff and the regenerate command, when the committed file differs;
`--check --staged-only` reads both ui_xml/ and the table from the index. It is
gated as qc_double_binds.
"""
import argparse
import difflib
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from staged_content import catfile_batch, tree_files  # noqa: E402

INC_PATH = "tests/unit/double_bind_widgets.inc"
REGEN_CMD = "python3 scripts/scan_double_binds.py --emit-cpp"

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
# machine_motion_blocked eq 1 -> state disabled. It composes with the element's other
# state bindings exactly as a hand-written one would, so a control carrying both
# is a double-bind group even though only one of the two is spelled as a child
# element. Attributes are applied before children, so it goes at the head of the
# bucket - the order the runtime installs them in.
GUARD_ATTRIBUTE = "moves_machine"
GUARD_BINDING = {"subject": "machine_motion_blocked", "state": "disabled", "ref_value": "1"}


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


def is_widget_element(el):
    """Whether `el` builds an object: <style> and the bind_* and event_cb
    children carry a name too, but none of them is a widget."""
    return not el.tag.startswith("bind_") and el.tag not in ("style", "event_cb")


# `<if cond="subject OP n">`: the structural conditional, decided once at build
# time. Only this simple form is drivable; anything richer marks its subtree.
IF_COND_RE = re.compile(r"^\s*(\w+)\s+(eq|not_eq|gt|ge|lt|le)\s+(-?\d+)\s*$")


def branch_value(cond, want):
    """A value of the cond's subject that makes `cond` evaluate to `want`, nearest
    zero, or None when the cond is not a simple comparison."""
    m = IF_COND_RE.match(cond or "")
    if m is None:
        return None
    subject, op, ref = m.group(1), "if_" + m.group(2), int(m.group(3))
    for value in sorted(set(range(-2, 5)) | {ref - 1, ref, ref + 1}, key=lambda v: (abs(v), v)):
        if evaluate(op, ref, False, value) == want:
            return subject, value
    return None


def branches(view):
    """Map every element under `view` to the <if> branches it sits in, as a tuple
    of (if element, taken branch, (subject, value) or None) from the outside in.

    Children of an <if> before its <else/> are the true branch, the rest the
    false one; the engine builds only the branch the condition picks.
    """
    out = {}

    def walk(el, path):
        out[el] = path
        if el.tag == "if":
            taken = True
            for child in el:
                if child.tag == "else":
                    taken = False
                    continue
                walk(child, path + ((el, taken, branch_value(el.attrib.get("cond"), taken)),))
        else:
            for child in el:
                walk(child, path)

    walk(view, ())
    return out


def compatible(a, b):
    """Whether two branch paths can be built together: every <if> both sit in
    takes the same branch for both."""
    taken = {id(el): t for el, t, _ in a}
    return all(taken.get(id(el), t) == t for el, t, _ in b)


def widget_ordinal(el, paths, view):
    """`el`'s index among the same-named widgets built beside it.

    A component can repeat a name, one copy per <if> branch or per layout, with
    the bindings on only one of them. The engine builds widgets in document
    order, so the index is also the match a depth-first walk of the built tree
    reaches once the row's branch is selected.
    """
    name = el.attrib.get("name")
    nth = 0
    for other in view.iter():
        if other is el:
            return nth
        if (other.attrib.get("name") == name and is_widget_element(other)
                and compatible(paths.get(other, ()), paths.get(el, ()))):
            nth += 1
    return 0


def scan(path, text):
    """Yield (widget_name, nth, build, kind, bits, [Binding]) for every composed
    property. `nth` is the widget's index among the same-named widgets built
    with it, `build` the (subject, value) pairs that select its <if> branches,
    or None when a branch condition is not a simple comparison."""
    text = STATE_ATTR_RE.sub(r"\1\2__\3=\4", text)
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        print("  ! %s: %s" % (path, exc), file=sys.stderr)
        return

    # Only the <view> builds anything; <api> props and <styles> are named too.
    view = root.find("view")
    if view is None:
        view = root
    paths = branches(view)
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
                branch = paths.get(parent, ())
                build = (None if any(sv is None for _, _, sv in branch)
                         else [sv for _, _, sv in branch])
                yield (parent.attrib.get("name") or "<%s>" % parent.tag,
                       widget_ordinal(parent, paths, view), build, kind, bits, binds)


def disk_files(roots):
    """(path, text) for every *.xml under `roots` in the working tree."""
    for root in roots:
        for path in Path(root).rglob("*.xml"):
            yield path, path.read_text(encoding="utf-8")


def staged_xml(roots):
    """(path, text) for every *.xml under `roots` as the index holds it."""
    for path, text in tree_files(None, tuple(roots)):
        if path.endswith(".xml"):
            yield Path(path), text


def collect(files):
    for path, text in sorted(files, key=lambda f: Path(f[0])):
        for row in scan(path, text):
            yield (path,) + row


def report(rows):
    current = None
    for path, name, _nth, _build, kind, bits, binds in rows:
        if path != current:
            print(path)
            current = path
        print("  %-28s %s=%s" % (name, kind, bits))
        for b in binds:
            print("      %s" % (b.describe() if b else "<not drivable from a test>"))
    print()
    print("widgets carrying 2+ bindings on one property: %d" % len(rows))


def render_cpp(rows):
    """The DRIVABLE subset, as a table of C++ initialisers: (text, drivable, skipped)."""
    lines = [
        "// Generated by scripts/scan_double_binds.py --emit-cpp. Do not edit.",
        "//",
        "// Every widget in ui_xml/ carrying two or more bindings on one state or flag.",
        "// A moves_machine=\"true\" attribute is one of them: the engine installs the",
        "// machine_motion_blocked -> disabled binding for the element itself, so a control",
        "// carrying the attribute and one hand-written state binding is a group of two.",
        "// Each row names the component to build, the widget in it, which of the",
        "// component's widgets of that name it is (from 0, in document order, among",
        "// those built with it), the subject values that select its <if> branches at",
        "// build time, the property, the subjects involved with a value that releases",
        "// every binding on them, and one value per binding that makes exactly that",
        "// binding ask for the property.",
        "//",
        "// `orders_testable` is false when two bindings share a subject: they cannot be",
        "// held one at a time, so only the each-alone phase applies to that row.",
        "//",
        "// Rows left out are counted at the bottom - a cond= expression, a subject",
        "// the instantiating parent supplies as $param, a ref_value that is a",
        "// per-iteration ${...} substitution, or an <if> branch whose cond is more than",
        "// one comparison.",
        "",
        "// clang-format off",
        "static const BindGroup kDoubleBindWidgets[] = {",
    ]
    drivable = skipped = 0
    for path, name, nth, build, kind, bits, binds in rows:
        component = str(path).removeprefix("ui_xml/").removesuffix(".xml")
        if build is None or len(build) > 2 or any(b is None for b in binds):
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
        build_src = ", ".join('{"%s", %d}' % sv for sv in build)
        lines.append('    {"%s", "%s", %d, {%s}, %d, %s, "%s", {%s}, %d, {%s}, %d, %s},'
                     % (component, name, nth, build_src, len(build),
                        "BindKind::State" if kind == "state" else "BindKind::Flag", bits,
                        subj_src, len(order), hold_src, len(binds),
                        "true" if len(order) == len(binds) else "false"))
    lines += [
        "};",
        "// clang-format on",
        "",
        "// Groups in ui_xml/: %d drivable, %d that no test can drive (see the header"
        % (drivable, skipped),
        "// comment), %d total. qc_double_binds fails a commit that leaves this file"
        % (drivable + skipped),
        "// behind ui_xml/.",
        "",
    ]
    return "\n".join(lines), drivable, skipped


def emit_cpp(rows, out):
    text, drivable, skipped = render_cpp(rows)
    Path(out).write_text(text)
    print("wrote %s: %d drivable, %d undrivable, %d total"
          % (out, drivable, skipped, drivable + skipped))


def check_cpp(rows, committed):
    """0 when `committed` (the table's text, or None) is what ui_xml/ generates."""
    want, drivable, skipped = render_cpp(rows)
    if committed == want:
        print("✅ %s matches ui_xml/: %d double-bind groups" % (INC_PATH, drivable + skipped))
        return 0
    print("❌ %s does not match ui_xml/" % INC_PATH)
    diff = list(difflib.unified_diff((committed or "").splitlines(), want.splitlines(),
                                     "committed", "generated", lineterm="", n=0))
    for line in diff[:40]:
        print("   " + line)
    if len(diff) > 40:
        print("   ... %d more diff lines" % (len(diff) - 40))
    print("   Regenerate and commit it: %s && git add %s" % (REGEN_CMD, INC_PATH))
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="*", default=["ui_xml"])
    ap.add_argument("--emit-cpp", nargs="?", const=INC_PATH,
                    metavar="PATH", help="write the C++ table instead of a report")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when %s differs from what ui_xml/ generates" % INC_PATH)
    ap.add_argument("--staged-only", action="store_true",
                    help="with --check, read ui_xml/ and the table from the index")
    args = ap.parse_args()
    roots = args.roots or ["ui_xml"]

    if args.check:
        if args.staged_only:
            rows = list(collect(staged_xml(roots)))
            committed = dict(catfile_batch([(INC_PATH, ":" + INC_PATH)])).get(INC_PATH)
        else:
            rows = list(collect(disk_files(roots)))
            inc = Path(INC_PATH)
            committed = inc.read_text(encoding="utf-8") if inc.exists() else None
        return check_cpp(rows, committed)

    rows = list(collect(disk_files(roots)))
    if args.emit_cpp:
        emit_cpp(rows, args.emit_cpp)
    else:
        report(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
