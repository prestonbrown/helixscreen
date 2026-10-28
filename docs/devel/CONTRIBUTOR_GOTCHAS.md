# Contributor Gotchas

A symptom-indexed reference for "my thing isn't working — what did I miss?"

HelixScreen's declarative-UI stack is designed so XML contributors don't have to know C++ internals. The tradeoff is that when something goes wrong, the failure mode is often **silent** — no crash, no stack trace, just a thing that doesn't render or doesn't react. This doc is the "if you see X, you forgot Y" lookup table.

Flip here when stuck. Search by symptom.

---

## XML & Layout

### My new XML component doesn't render — the `<my_component/>` tag is just nothing.

**Cause:** The engine found no XML file for the tag. Components register on first use, by file name: the tag `<my_component/>` loads `ui_xml/<my_component>.xml` or `ui_xml/components/<my_component>.xml` (resolved through the active layout variant).

**Fix:** Name the file exactly `<component>.xml` and put it in `ui_xml/` or `ui_xml/components/`. There is no registration list to edit, and no ordering between components. A C++ widget, callback or subject the XML uses still has to be registered before the XML is created.

**Check the log:** A name with no file makes `lv_xml_create()` return NULL and log an error. A tag nested in a view whose file is missing leaves its container empty.

---

### My `bind_style` doesn't change the color / background / border.

**Cause:** Inline style attributes (`style_bg_color="..."`, `style_text_color="..."`, etc.) on the same widget win over `bind_style` in LVGL's style cascade. Inline is always higher priority.

**Fix:** For any property you want to change reactively, remove the inline attribute. Then either:

1. Use **two `<bind_style>` entries** — one per state — so both states have explicit styling, or
2. Put the default in a non-inline style sheet referenced by `bind_style`.

```xml
<!-- ✗ Broken: inline style_bg_color wins, bind_style does nothing -->
<lv_obj style_bg_color="#card_bg" bind_style="active_style" subject="is_active"/>

<!-- ✓ Works: two bind_styles, no inline conflict -->
<lv_obj>
    <bind_style name="inactive_style" subject="is_active" ref_value="0"/>
    <bind_style name="active_style" subject="is_active" ref_value="1"/>
</lv_obj>
```

Reference: lesson **L040**.

---

### My `<style name="...">` or `bind_style` has no effect inside a component, but works in the parent file.

**Cause:** A `<styles>` block is file-local. A bare style name resolves in the file that declares it, then in `globals`, so a component in its own XML file cannot see the `<styles>` of the panel that embeds it. The log carries a `No style found with <name> name` warning.

**Fix:** When more than one file needs the style, move it into `ui_xml/styles.xml` and reference it by dotted name everywhere, including the file that used to own it:

```xml
<!-- ui_xml/styles.xml -->
<style name="press_wash" bg_color="#primary" bg_opa="30%" radius="#border_radius"/>

<!-- ✓ any file -->
<style name="styles.press_wash" selector="pressed"/>
<bind_style name="styles.invisible" subject="preparing_visible" ref_value="1"/>
```

Do not move it into `globals.xml` instead: that file parses before theme init, so a style there that uses a `#token` registers empty. Details: `UI_CONTRIBUTOR_GUIDE.md` § "Shared styles".

---

### My `style_*:checked` (colon) state style does nothing.

**Cause:** The engine reads state and part selectors only in dash form. `lv_xml_style_string_process` (`lib/helix-xml/src/xml/lv_xml_style.c`) splits every style attribute name on `-`, so `style_opa-checked` is property `style_opa` with selector `checked`. The colon form never splits: `style_opa:checked` stays one property name, matches nothing in the property table, and the attribute is dropped. The engine logs `` `style_opa:checked` is ignored: style selectors follow '-' `` and `make lint-xml` fails on it.

**Fix:** Write the dash form:

```xml
<!-- ✗ Dropped: the colon form never splits -->
<lv_obj style_opa:checked="255"/>

<!-- ✓ Parsed as style_opa with a checked selector -->
<lv_obj style_opa-checked="255"/>
```

---

### My subject binding is stuck at the default value. Updates in C++ don't show up.

**Cause:** The XML `<subjects>` block declared a subject with the same name as a C++-registered subject. XML component-scoped subjects shadow global subjects — your bindings resolve to the local XML subject (default-initialized), not the C++ one that's actually getting updates.

**Fix:** If a value is owned by C++ (`UI_SUBJECT_INIT_AND_REGISTER_*` or explicit `lv_xml_register_subject`), **do not** also declare it in the XML `<subjects>` block. Let the C++ registration be the only source.

Reference: lesson **L046**.

---

### My button click does nothing. `<event_cb callback="foo">` looks right but doesn't fire.

**Cause:** The C++ callback isn't registered. XML names are symbolic — they resolve to C++ function pointers at parse time via `lv_xml_register_event_cb()`.

**Fix:** Register in one of two places:

- **Global / reusable callbacks:** `register_xml_components()` in `src/xml_registration.cpp`.
- **Panel / modal-specific callbacks:** In the class's `register_callbacks()` method or constructor. Example: `src/ui/ui_panel_macros.cpp#register_callbacks`.

```cpp
lv_xml_register_event_cb(nullptr, "on_my_button_clicked", on_my_button_clicked);
```

**Double-check:** The string name in XML (`callback="on_my_button_clicked"`) must match the first argument here exactly. Typos are silent.

---

### A hold also fires the tap: the hold opens its action, then the lift runs the tap behind it.

**Cause:** The hold handler acted but left the press running. LVGL sends `clicked` on every release, including the one that ends a long press, so the tap fires behind the hold. The same happens across elements when the hold sits on a child and the tap on an ancestor its events bubble to, which every home-panel widget does.

**Fix:** Keep the tap on `clicked`, and have the hold handler call `helix::ui::end_press(e)` when it acts. The release then sends `press_lost` instead of `clicked`. A hold the handler declines must not call it, so a slow tap still works. Do not move the tap to `short_clicked` (a slow tap then does nothing), and do not swallow it with a flag. The rule and its exceptions: `docs/devel/LVGL9_XML_GUIDE.md` § "Tap and hold".

**Check:** `scripts/check_hold_ends_press.py` fails a `long_pressed` handler that neither calls `end_press()` nor carries `HOLD_DECLINES_OK: <reason>`.

---

### My icon renders as tofu (□) or as a blank placeholder.

**Cause:** You added a codepoint to `include/ui_icon_codepoints.h` but didn't regenerate the icon fonts.

**Fix:** Run `make regen-fonts`, then rebuild. The fonts' glyph list is computed from
`include/ui_icon_codepoints.h` and the `ICON_*` macros in `include/ui_fonts.h`
(`scripts/mdi_icon_codepoints.py`), so adding the entry there is the only edit; the
regeneration is the step that gets skipped.

Reference: lesson **L009**.

---

### The inspector says my widget has no text, but I set `text="Hello"`.

**Cause:** You're setting `text` on a widget that doesn't have a text role — e.g., a raw `<lv_obj>` or a container. Only widgets that inherit from `lv_label` or have explicit text support (`text_body`, `text_heading`, `text_small`, `ui_button`) render text from `text="..."`.

**Fix:** Put a `<text_body>` or `<text_heading>` child inside, or use a widget that already includes text.

---

### My long label or description is cut off instead of wrapping onto a second line.

**Cause:** The label has no width. A `text_body`/`text_small` left at its default is `LV_SIZE_CONTENT` wide, so it lays out on one line at whatever length the string needs, and the parent clips the rest. `long_mode="wrap"` does not fix this on its own — `wrap` is already LVGL's default long mode, and a content-width label has nothing to wrap against.

**Fix:** Give the label `width="100%"` *and* `long_mode="wrap"`. If its parent is a `flex_grow` column, that column also needs `width="0"`, because a percentage-width child is excluded from a parent's content-width calculation (`w_ignore_size`, `lv_obj_pos.c`) and the column would otherwise collapse to its widest non-percentage child.

```xml
<!-- ✗ one clipped line -->
<lv_obj height="content" flex_flow="column" flex_grow="1">
  <text_small name="description" text="$description"/>
</lv_obj>

<!-- ✓ wraps -->
<lv_obj height="content" width="0" flex_flow="column" flex_grow="1">
  <text_small name="description" width="100%" text="$description" long_mode="wrap"/>
</lv_obj>
```

**Why silent?** LVGL is behaving correctly at every step — the label is exactly as wide as its text, and the parent clips its children as it should. Nothing warns. It reads fine in English on an 800x480 panel and truncates at the Small breakpoint or in de/fr/es, which run 15-29% longer on average and up to +84% on individual strings.

Full mechanism and the one-line `dots` alternative: `LVGL9_XML_GUIDE.md`, "Text never wraps inside a `flex_grow` column".

---

### My `long_mode="dots"` label wraps onto two lines, or runs off the edge with no ellipsis.

**Cause:** LVGL ellipsizes only when the wrapped text is taller than the label. A bare `<lv_label>` at content height grows to fit and wraps instead. A label with no width bound (no `width`, or `width="content"` without `style_max_width`) never wraps at all, so it runs past its parent and is clipped.

**Fix:** Use a `text_*` widget (or `ui_button`), which holds a dotted content-height label to one line of its font, and give it a width bound: `width="100%"`, or `width="1" flex_grow="1"` in a row. In C++, call `helix::ui::label_hold_one_line(label)` after setting `LV_LABEL_LONG_MODE_DOTS`. An explicit px/% `height` or `style_max_height` opts out, for a deliberately multi-line dotted label. Text that should wrap must not use `dots`. Details: `LVGL9_XML_GUIDE.md`, "One-line labels: `long_mode="dots"`".

---

### Chevron scroll buttons are drawn on top of my widget's content.

**Cause:** A container in your XML is scrollable when you did not intend it to be, so `PageScrollAutoInject` treated it as a scroll region and attached the page-scroll gutter over it. In XML an `<lv_obj>` keeps LVGL's `LV_OBJ_FLAG_SCROLLABLE` default, which is **on**, unless you write `scrollable="false"`. Our theme overrides width, height, border, background, and padding on `lv_obj` - it does **not** override scrollable, so "our theme makes `lv_obj` a pure layout container" is only true of appearance.

**Fix:** Add `scrollable="false"` to every container that is not a real scroll region. Confirm with `helix-screen ctl geom <name>`, which prints the `scrollable` flag and the scroll extents (`docs/devel/HELIXCTL.md#geom--why-a-widget-is-the-size-it-is`).

```xml
<!-- ✗ Scrollable, and therefore gutter-eligible -->
<lv_obj name="print_card_idle" flex_flow="column">...</lv_obj>

<!-- ✓ -->
<lv_obj name="print_card_idle" flex_flow="column" scrollable="false">...</lv_obj>
```

That is exactly what happened in `ui_xml/components/panel_widget_print_status.xml` - `print_card_idle` and `print_card_idle_compact` were the only two containers in the file missing the attribute (the other nine had it), and the chevrons landed on top of the thumbnail on an 800x480 panel.

Page scroll is a *page-level* affordance, so home panel widget tiles are now excluded outright: the auto-inject walk stops at any tile (`src/ui/page_scroll_auto_inject.cpp#walk_and_attach`, flag set in `src/ui/panel_widget_manager.cpp#populate_widgets`). A tile is sized by the home grid and scrolled by dragging it, and the gutter is 172px tall at the medium tier - most of a tile at 800x480. If you are seeing chevrons *inside* a home widget, that is a bug in the walk, not something to work around in XML. Full picture: `PAGE_SCROLL_BUTTONS.md`.

**Why silent?** Nothing is wrong from LVGL's point of view. The container is scrollable, the gutter is doing its job, and neither logs anything. You only find out by looking at the screen.

---

### My XML edit doesn't show up. I rebuilt and restarted and nothing changed.

**Cause:** You **don't need to rebuild** for XML changes. But you also need to make sure you're editing the right file and that the binary is reading from the right location.

**Fix:**

- XML loads at runtime from `ui_xml/`. Hot reload is ON by default for native builds — just edit + save; the active panel/overlay/modal rebuilds in place within ~500ms. No relaunch needed.
- If hot reload isn't picking up the change, you can force a clean restart: `./build/bin/helix-screen --test -vv`.
- If you truly see no change, confirm: (1) you saved the file, (2) it's in `ui_xml/` not a copy elsewhere, (3) the component is actually instantiated on the panel you're viewing.

Reference: lesson **L031**.

---

### I wrote `cond="a && b"` in an XML expression and it's rejected / broken.

**Cause:** `&&` and `<` are XML metacharacters. Written literally inside an attribute value, they either fail to parse as valid XML or get mangled before reaching the expression compiler.

**Fix:** Use the word-form operators instead — they need no escaping and tokenize identically to the symbolic forms: `and`/`or`/`not` for `&&`/`||`/`!`, and `eq`/`ne`/`lt`/`le`/`gt`/`ge` for `==`/`!=`/`<`/`<=`/`>`/`>=`. This is house style for `cond=` and `<subject_expr expr=>`:

```xml
<!-- ✗ Needs escaping, easy to get wrong -->
<bind_flag_if cond="demo_error &amp;&amp; demo_temp &gt; demo_threshold" flag="hidden"/>

<!-- ✓ House style: word forms, no escaping -->
<bind_flag_if cond="demo_error or demo_temp gt demo_threshold" flag="hidden" invert="true"/>
```

If you do need the symbolic form for some reason, escape it as XML entities (`&amp;&amp;`, `&lt;`, `&gt;`) — both forms compile to the same expression.

See `LVGL9_XML_GUIDE.md` § "Expression Conditionals".

---

### My new `cond=`/`<subject_expr>` XML fails CI with `UNKNOWN_WIDGET` or `UNKNOWN_ATTRIBUTE`.

**Cause:** The XML linter validates against a committed schema snapshot (`tools/xml-linter/schema/schema.json`), not against the C++ source directly. Adding a new tag or attribute (like `subject_expr`, `cond`, or any new custom widget) doesn't update that snapshot automatically.

**Fix:** Regenerate and commit the schema:

```bash
make regen-xml-schema
git add tools/xml-linter/schema/schema.json
```

`.github/workflows/lint-xml.yml` runs against the committed schema, so a stale one fails the first XML fixture that uses the new syntax — even though it works fine locally.

Reference: lesson **L089**.

---

### My `<repeat>` label binds to nothing — `bind_text="slot_${i}_label"` shows the default text, or the linter warns `UNKNOWN_SUBJECT_REF`.

**Cause:** Two independent things go wrong with embedded `${name}` composition:

1. **Runtime shows the default.** `bind_text="slot_${i}_label"` composes a subject name per iteration (`slot_0_label`, `slot_1_label`, …). If the C++ side never registered those exact indexed subjects, each bind resolves against a non-existent subject and the widget keeps its default value. The XML is fine — the subjects are missing. Register `slot_0_label`…`slot_N_label` (matching the composed names exactly) in your panel's subject init.

2. **CI warns `UNKNOWN_SUBJECT_REF`.** The linter cannot statically resolve a composed name (the loop index / prop is only known at runtime), so it must skip it. The `${...}` skip lives in `tools/xml-linter/src/helix_xml_linter/crossref.py` (`_check_subject_reference`). A brace-free name like `bind_text="typo_subject"` still warns — that skip only applies when the value contains `${`.

**Fix:** Make sure the composed names and the registered subject names line up character-for-character, and keep the `${...}` skip in `crossref.py`. Remember `$i` is a whole-value substitution — `slot_$i` does **not** splice; you must write `slot_${i}` (see the [XML guide](LVGL9_XML_GUIDE.md#self-wiring-indexed-subjects-with-name)).

---

### My label shows `foo__bar` (a doubled underscore / missing word), or the log warns `xml_compose_indexed ... could not be resolved`.

**Cause:** A literal `${...}` in visible text or an attribute value is not just decoration — it's a live composition sigil, and `xml_compose_indexed` tries to resolve it wherever it appears, not just inside a `<repeat>` body. If you write descriptive text like `text="each card binds to demo_slot_${i}_label"` outside a `<repeat>` (or referencing a name that isn't `i`/a component prop), there's no loop index or prop to resolve against, so it splices empty and logs a warning — the rendered text comes out mangled (`demo_slot__label`).

**Fix:** There is no escape sequence for `${...}`. If you need to show the literal characters `${i}` or `${name}` in a label (e.g. explaining the syntax in a demo panel), don't put it in `text=`/`translation_tag=` — describe it in prose instead (e.g. "self-wires to its indexed subject (demo_slot_N_label)"). Only use `${...}` where you actually want composition to fire: inside a `<repeat>` body (`${i}`), or against a component prop that's genuinely in scope.

---

## Design Tokens & Theming

### Review feedback: "please use design tokens instead of hardcoded colors."

**Cause:** You used `lv_color_hex(0xE0E0E0)` in C++ or `style_bg_color="#E0E0E0"` in XML.

**Fix:**

- **XML:** Use token names prefixed with `#` — e.g., `style_bg_color="#card_bg"`, `style_pad_all="#space_md"`.
- **C++:** Use `theme_manager_get_color("card_bg")` for semantic tokens. Use `theme_manager_parse_hex_color("#RRGGBB")` only when you're given a literal hex string that can't be tokenized (e.g., user-picked colors).

Reference: lesson **L008**.

---

### I'm redundantly specifying properties that semantic widgets already have.

**Cause:** Semantic widgets like `ui_card`, `ui_button`, `divider_light` already apply their tokenized defaults. Re-specifying `style_radius` on `ui_card` or `button_height` on `ui_button` just duplicates the default and makes the XML noisier.

**Fix:** Only override what you actually need to change. See `docs/devel/LVGL9_XML_GUIDE.md` "Custom Semantic Widgets" for each widget's built-in defaults.

---

### My `#token` resolves to nothing / the widget comes out 0px.

**Cause:** The `_small`/`_medium`/`_large` declarations sit in a `ui_xml/` subdirectory — `ui_xml/components/`, `ui_xml/portrait/`, `ui_xml/micro/`. Token discovery is top-level-only: `theme_manager_find_xml_files()` skips directory entries, so only `ui_xml/*.xml` is ever scanned for `<px>` and `<string>` tokens. A suffixed token declared below the top level is never registered, and the `#reference` to it silently resolves to nothing. Neither side warns.

**Fix:** Move the declarations to `ui_xml/globals.xml` (or another top-level token file) and keep the `#reference` where it was — referencing a global token from a variant file is exactly what variants are for. Recursing into subdirectories is not the fix: discovery is alphabetical last-wins, so a portrait-only `nav_width_small` would shadow the base token globally instead of only while the portrait variant is active.

Gate: `scripts/check_responsive_token_scope.py` (prestonbrown/helixscreen#1211).

---

### An attribute like `y="-#space_md"` does nothing.

**Cause:** The const resolver substitutes only values that start with `#`, so there is no way to negate a token in place. `resolve_consts` (`lib/helix-xml/src/xml/lv_xml.c`) drops a `-#` value with a warning (`` `-#space_md` ... negates a const, which is not supported ``) and the widget keeps its default; a style property takes the same path in `lv_xml_style.c`. `make lint-xml` fails on it.

**Fix:** Give the token the negative value and reference it bare. `ui_xml/globals.xml` carries negated spacing ladders (`space_md_neg`, `space_xxs_neg`, `space_2xl_neg`), so an inward offset reads `y="#space_md_neg"`. Add a ladder there when a tier you need has none:

```xml
<px name="space_md_neg_large" value="-12"/>
```

### With Glass Effects on, my panel stays flat, or my color chip gets shaded.

Glass picks a widget's look from the shared theme style it carries: a `ui_card` or dialog is
lit as a surface, a solid `ui_button` is raised. A container that paints its own background
(`style_bg_color="#card_bg"` on a plain `lv_obj`) carries neither, so it stays flat. The
opposite case is a widget whose color *means* something, such as a filament or LED color:
glass would shade it like any button. Mark both from the owning C++ with
`helix::ui::glass_mark(obj, GlassFamily::Surface)` or `glass_mark(obj, GlassFamily::None)`.
Swatches drawn through `ui_swatch.h` are already exempt. To check a screen, run with
`HELIX_GLASS=1` (forces the hardware check to pass) and turn the setting on. `THEME_SYSTEM.md` § "Glass appearance" has the rules.

---

## Translations

### `lv_tr()` on a product name is generating a translation key that shouldn't exist.

**Cause:** Product names, URLs, technical abbreviations used as standalone labels, and universal terms must **not** be wrapped in `lv_tr()`.

**Fix:** Leave these untranslated:

- **Product names:** HelixScreen, Klipper, Moonraker, Spoolman, Mainsail, Fluidd, OrcaSlicer.
- **URLs / domains:** `https://helixscreen.org`, `github.com/prestonbrown/helixscreen`.
- **Technical abbreviations as standalone labels:** AMS, QGL, ADXL, PID, IFS, CFS.
- **Material codes:** PLA, PETG, ABS, TPU, PA. (In XML a literal is looked up anyway and renders as itself when the catalog has no key for it.)
- **Universal terms:** OK, WiFi.

Add a comment when you skip translation: `// i18n: do not translate (product name)`.

**But:** Sentences *containing* product names *are* translatable. "Restarting HelixScreen..." is fine because "Restarting" needs to translate.

Reference: lesson **L070**.

---

### All my translation strings compile but don't show in other languages.

**Cause:** Translation artifacts weren't regenerated or weren't committed.

**Fix:** After editing YAML translation files, rebuild — the build regenerates `ui_xml/translations/translations.xml`. It is **tracked in git** (not gitignored), so you must `git add` it explicitly before committing.

Reference: lesson **L064**.

---

## Subjects & Lifecycle

### The app crashes on reconnect, or on panel rebuild, in an observer callback.

**Cause:** You're observing a **dynamic** subject (per-fan, per-sensor, per-extruder) and its `SubjectLifetime` token never reached the `observe_*` factory. The factory's `lifetime` parameter is required, but nothing stops you passing `{}` (or `subject_never_freed()`) instead of the token you fetched, and then the guard has no way to learn the subject was freed, so `reset()` calls `lv_observer_remove()` on freed memory.

**Fix:**

- Pass the token as the factory's last argument. That is the part that makes it safe.
- Whether the token lives in a local or a member does *not* decide correctness: the accessors hand you a **copy** of a `shared_ptr` the owner keeps, and the owner signals death by writing `*token = false`, not by dropping the refcount. Members are still the recommended shape — self-documenting, and correct under either ownership model.
- For per-item collections (carousels, slot lists), use parallel vectors: `std::vector<ObserverGuard>` and `std::vector<SubjectLifetime>`, kept index-aligned.

```cpp
// Header
SubjectLifetime temp_lifetime_;
ObserverGuard   temp_observer_;

// Rebind
auto* s = tsm.get_temp_subject(name, temp_lifetime_);
temp_observer_ = observe<int>(s, this, handler, temp_lifetime_);  // <- token, not omitted
```

Reference: lessons **L077**, **L084**, `include/ui_observer_guard.h`, and `docs/devel/THREADING.md` § 5 (which explains why the older "local lifetime = UAF" phrasing was wrong).

---

### Two instances of my panel widget exist, but only one gets updates.

**Cause:** You declared a per-instance subject (one per widget) but registered it globally, or declared a single subject and expected both instances to share it but the XML subjects scope is per-component-instantiation.

**Fix:** Decide which you want:

- **Shared subject across all instances:** Declare `static inline` at class scope, register into the component's scope using `lv_xml_register_subject(lv_xml_component_get_scope("my_component"), "subject_name", &subject_)`.
- **Per-instance subject:** Register per-instance and ensure the XML scope resolves to the right one (rarely what you want — usually you'd just use a shared subject filtered by an instance ID).

---

### I'm using `lifetime_.defer()` from a background thread and crashing.

**Cause:** `lifetime_.defer()` reads `this->lifetime_`, which is a TOCTOU race from a background thread — `this` can be destroyed between the check and the deref.

**Fix:** Use `tok.defer()` instead. The token holds its own `shared_ptr`, so it's safe from background threads. Only use `lifetime_.defer()` on the main thread.

```cpp
auto tok = lifetime_.token();
api->fetch([this, tok]() {
    if (tok.expired()) return;
    tok.defer([this]() { update_ui(); });   // Safe
    // NOT: lifetime_.defer([this]() {...}); // TOCTOU race from kBg thread
});
```

Reference: `THREADING.md` § "2. Async callback safety", issue #707.

---

## C++ Threading & Deletion

### My fire-and-forget `std::thread([...]{}).detach()` crashes on the K1/AD5M/CC1.

**Cause:** `pthread_create` returns `EAGAIN` under thread exhaustion on small-memory ARM devices. The `std::thread` constructor then throws, and the throw propagating through an LVGL C event-dispatch frame aborts the process.

**Fix:** Use a managed pool. For HTTP work, use `helix::http::HttpExecutor::fast()` or `::slow()`. For sd-bus/BlueZ, use `helix::bluetooth::BusThread::run_sync()`. For the narrow cases where you genuinely need a one-shot thread (device discovery, QR decode, USB print), wrap in `try { std::thread([...]{}).detach(); } catch (const std::system_error&) { /* toast + error callback */ }`.

Reference: `THREADING.md` § "No `std::thread(...).detach()` for fire-and-forget work", lesson **L083**.

---

### I called `lv_obj_delete()` inside a queued callback and got a SIGSEGV.

**Cause:** Multiple synchronous deletions inside the same `UpdateQueue::process_pending()` batch corrupt LVGL's global event linked list (#776, #190, #80). `lifetime_.defer()` and `tok.defer()` don't escape the batch — they fire in the next tick of the same queue.

**Fix:** Use the deferred variants that route through LVGL's own async list:

| In queued / async callback, instead of: | Use: |
|---|---|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

Reference: `THREADING.md` § "3. No synchronous widget deletion inside queued callbacks", **L081**.

---

## Before You Submit

Run through this before opening a PR:

- [ ] **XML-only change?** Confirm hot reload or plain relaunch shows your changes. No rebuild needed.
- [ ] **Added an icon?** Ran `make regen-fonts` and rebuilt.
- [ ] **Added a new XML component?** File is `ui_xml/<name>.xml` or `ui_xml/components/<name>.xml`, named for the component.
- [ ] **Added an event callback?** Registered with `lv_xml_register_event_cb()`.
- [ ] **Any hardcoded colors or pixel values?** Swap for design tokens.
- [ ] **Any new user-visible strings?** Wrapped for translation — `lv_tr()` in C++, or a literal `text=` in XML, which is its own translation key (the path most first contributions use) — *except* product names, URLs, material codes.
- [ ] **Modified translation YAML?** Rebuild, then `git add` the regenerated `ui_xml/translations/translations.xml`.
- [ ] **Added an observer on a dynamic subject?** The `SubjectLifetime` you fetched is passed to the `observe<V>` factory, not swapped for `{}` or `subject_never_freed()`.
- [ ] **Tested at multiple sizes?** At minimum: `480x320`, `800x480`, `1024x600`. See `docs/devel/UI_CONTRIBUTOR_GUIDE.md` § Screen Breakpoints.
- [ ] **`make test-run` passes.**

---

## When This Doc Doesn't Help

If your symptom isn't here, the next places to look:

- `docs/devel/UI_CONTRIBUTOR_GUIDE.md` — layout, breakpoints, theming
- `docs/devel/LVGL9_XML_GUIDE.md` — XML syntax reference, widget attributes
- `docs/devel/DEVELOPER_QUICK_REFERENCE.md` — code patterns
- `docs/devel/MODAL_SYSTEM.md` — modal-specific patterns
- `docs/devel/TRANSLATION_SYSTEM.md` — i18n architecture
- `CLAUDE.md` (repo root) — the full rules reference; `CTRL-F` for your symptom

And if you've hit something that took you more than an hour to diagnose, **it probably belongs in this doc** — send a PR adding it. Future contributors will thank you.
