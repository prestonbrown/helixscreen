# Translation System

HelixScreen's translation system is built on LVGL's native XML translations. Source strings live in per-locale YAML files; a generator turns them into runtime XML translation packs that the app loads at startup, and C++ resolves strings at runtime through LVGL's `lv_translation_*` API (`lv_tr()`). This document explains how translations flow from source YAML files through generation to the running application.

## Supported Languages

| Code | Language   |
|------|------------|
| de   | German     |
| en   | English    |
| en-GB | British English (a regional variant of `en`) |
| es   | Spanish    |
| fr   | French     |
| it   | Italian    |
| ja   | Japanese   |
| pt   | Portuguese |
| ru   | Russian    |
| zh   | Chinese    |

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           translations/*.yml                            │
│                    (Master source files - one per locale)               │
└────────────────────────────────┬────────────────────────────────────────┘
                                 │
                                 ▼
                   ┌─────────────────────────────┐
                   │  generate_translations.py   │
                   │  (Python pack generator)    │
                   └─────────────┬───────────────┘
                                 │
                ┌────────────────┴────────────────┐
                ▼                                  ▼
┌───────────────────────────┐      ┌──────────────────────────────┐
│  translations.xml         │      │  <lang>.xml (per-locale)     │
│  (combined LVGL XML pack) │      │  en.xml, de.xml, zh.xml, ... │
└─────────────┬─────────────┘      └───────────────┬──────────────┘
              │                                     │
              ▼                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          Runtime (Application)                          │
│  • App loads the active locale's pack via the locale loader             │
│    (lv_translation_set_language + lv_xml_register_translation_from_file) │
│  • XML parser resolves translation_tag / placeholder_tag via lv_tr()    │
│  • C++ resolves strings with lv_tr("English text") (English fallback)   │
│  • Plural rules applied per-language                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

> The generator emits only the runtime XML packs. The app reads them through
> LVGL's native `lv_translation_*` API.

## Source Files (YAML)

Translation source files live in `translations/` with one file per locale.

### File Format

```yaml
locale: en
translations:
  # Simple singular strings
  "Cancel": "Cancel"
  "Save": "Save"
  "Temperature": "Temperature"

  # Strings with special characters need quoting
  "Nozzle: {temp}°C": "Nozzle: {temp}°C"

  # Count strings: one key per English form, both in the C++ call
  "1 print": "1 print"
  "{} prints": "{} prints"
```

### Key Rules

- **Keys are English text** - The translation key is the English version of the string
- **Singular translations** - Direct string-to-string mapping
- **Plural translations** - A locale whose count words inflect beyond one/other turns the
  plural key's value into a dictionary of CLDR categories (see [Plural Rules](#plural-rules))

### Example: Russian with Complex Plurals

Russian uses one/few/many categories for whole numbers:

```yaml
locale: ru
translations:
  "1 print": "1 печать"            # the singular key, used for exactly 1
  "{} prints":
    one: "{} печать"                # 21, 31, 101...
    few: "{} печати"                # 2-4, 22-24...
    many: "{} печатей"              # 0, 5-20, 25-30...
    other: "{} печати"              # required: the fallback for any missing category
```

## Regional Variants

A locale code is a language (`en`, `pt`), optionally followed by a hyphen and a region
(`en-GB`, `pt-BR`). A code with a region is a **variant** of its base language. The hyphen
form is the one canonical spelling: the YAML file name and its `locale:` line, the
generated pack (`ui_xml/translations/en-GB.xml`), the LVGL language name and the
`/language` setting all use it. `helix::locale_base_language()` (`include/core/locale_code.h`)
and `base_locale()` (`scripts/translations/locale_variants.py`) also read an underscore, so a
POSIX name (`pt_BR`) resolves the same way.

**The file.** `translations/en-GB.yml` carries every key `en.yml` has, like any locale, but
only the strings the region writes differently get a value. An empty value means "inherit
from the base language", not "untranslated". `make translation-sync` adds new keys to a
variant with an empty value, as it does for every other locale, so a variant needs no extra
upkeep: a new string inherits until someone gives it a regional spelling.

**The fallback chain.** For a key, the UI shows the first of:

1. the variant's own value;
2. the base language's value;
3. the tag, which is the English source text.

The generator resolves steps 1 and 2 when it writes the variant's pack
(`scripts/translations/locale_variants.py#merge_variant`): the pack holds the base's entries
overlaid with the variant's. Base entries equal to their own key are left out, since the tag
already says the same thing, so `en-GB.xml` holds only the strings British English respells.
A variant value replaces the base entry whole: a flat string in `pt-BR` drops `pt`'s plural
forms for that key. Step 3 is LVGL's own fallback for a key no pack holds. The runtime
therefore loads one pack per language, as for any other locale. A variant whose base has no
YAML file fails generation.

**Coverage.** The coverage report and the two coverage gates
(`tests/python/test_cpp_translation_coverage.py`, `tests/python/test_explicit_tag_coverage.py`)
count a variant's empty value as translated when its base has a value
(`scripts/translations/locale_variants.py#effective_value`). The key must still exist in the variant's file. A plain
language's empty value stays a missing translation, so the gate is as strict for `de` as it
was. `make translation-coverage` lists a variant with the number of strings it defines itself:
`en-GB ... variant: N own, rest inherited`.

**Code that branches on a language.** Plural rules, day and month names, the CJK font
switch, the screensaver's English state words and the long-date wording all read the base
language, so a variant behaves like its base without a row of its own. The region defaults
(`src/system/region_defaults.cpp#find_language_defaults`) look up the full code first and
then the base, so a variant gets its own date order or clock only when it has its own row:
`en-GB` writes day-month-year and reads a 24-hour clock, while `en` stays month first and
12-hour.

**Adding one.** Copy `en.yml` with every value emptied (or `make translation-sync` after
creating a file holding `locale: <code>` and `translations: {}`), fill in the strings that
differ, and register the code in `SystemSettingsManager`'s language list right after its base,
named in its own words ("English (UK)"). Add the name to `kLanguageNames`
(`src/system/translation_loader.cpp`), `LANGUAGE_NAMES` (`scripts/translations/extractor.py`)
and `tests/fixtures/translation_key_cases.json` so it is never extracted as a key. If the
region writes dates or reads the clock differently from the base, give it a row in
`kLanguageDefaults`. The first-run picker (`ui_xml/wizard_language_chooser.xml`) takes a button
whose `user_data` is the Settings index, plus a flag image registered in
`src/application/asset_manager.cpp`. Packaging needs nothing: every packager ships every
`<lang>.xml` but the merged catalogue and `en.xml`.

## Build Pipeline

### Generator Script

`scripts/generate_translations.py` is the main pack generator. It reads all YAML files and produces the runtime XML translation packs:

| Output | Path | Purpose |
|--------|------|---------|
| `translations.xml` | `ui_xml/translations/translations.xml` | Combined LVGL XML pack (all languages) |
| `<lang>.xml` | `ui_xml/translations/<lang>.xml` | Per-locale packs (`en.xml`, `de.xml`, `zh.xml`, …) loaded individually at runtime |

These are the generator's only outputs.

### Makefile Integration

Translation generation is integrated into the build via `mk/translations.mk`:

```makefile
# Regenerate the XML packs if the YAML, the script or HELIX_LANG changes
$(TRANS_XML): $(TRANS_YAML) $(TRANS_SCRIPT) $(TRANS_LANG_STAMP)
    $(VENV_PYTHON_TRANS) $(TRANS_SCRIPT) $(TRANS_LANG_FLAG)
```

`HELIX_LANG` limits which locales a build packs; the stamp file forces a regeneration when it
changes.

Run manually with:

```bash
make translations
```

## Generated Files

### translations.xml

LVGL's native XML translation format. Each `<translation>` element maps a tag to translations in each language:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<translations languages="de en es fr it ja pt ru zh">
  <translation tag="Cancel" de="Abbrechen" en="Cancel" es="Cancelar" fr="Annuler" .../>
  <translation tag="Save" de="Speichern" en="Save" es="Guardar" fr="Enregistrer" .../>
  <translation tag="Temperature" de="Temperatur" en="Temperature" es="Temperatura" .../>
</translations>
```

### Per-locale packs (`<lang>.xml`)

Alongside the combined `translations.xml`, the generator emits one pack per
locale (`en.xml`, `de.xml`, `zh.xml`, …). At runtime the app loads only the
active locale's pack via the locale loader (`src/system/translation_loader.cpp`,
`helix::ui::ensure_translation_loaded()` → `lv_xml_register_translation_from_file()`),
so it never has to parse every language up front.

**English never loads a pack.** English tags ARE the English UI text, and
`lv_translation_get()` returns the tag unchanged when no pack matches, so
`ensure_translation_loaded()` skips the identity locale entirely (heap + lookup
scan savings - see `kIdentityLocale`). The hard consequence: **every key in
`translations/en.yml` must be its own English text.** A "semantic" key whose
value differs from the key (`pre_print_option.timelapse.label: Timelapse`)
renders as the raw dotted identifier in the English UI. If the English text already
exists as a tag, point the reference (C++ / XML / `printer_database.json`) at
the existing tag instead of adding a near-duplicate key.
`scripts/check_translation_identity.py` (wired into `quality-checks.sh`) fails
the build if `en.yml` ever grows a non-identity entry.

## XML Usage

### Static Text: the literal is the tag

A literal `text=` is its own translation key. The engine reads it as if
`translation_tag=` repeated it, so there is nothing to add:

```xml
<text_heading text="Temperature"/>
<ui_button text="Cancel" variant="secondary"/>
```

The same holds for a component's `*_tag` props: `label="Fans"` on a component
that declares `label_tag` implies `label_tag="Fans"`. `X_tag` pairs with `X`,
else with `X_text` (`primary_tag` with `primary_text`).

- **Write a tag only when it differs from the text**, e.g. a shorter key for a
  long label. A tag equal to its text fails
  `tests/python/test_explicit_tag_coverage.py`.
- **`translation_tag=""` opts out.** An explicit empty tag means "do not
  translate". Untranslated strings rarely need it: a key absent from the
  catalog renders as itself.
- **Nothing is implied** from a `$prop`, `#const` or `${}` value, beside
  `bind_text`, for a literal that cannot be a key (numbers, short punctuation,
  icon glyphs, identifiers: `should_skip_text()`), or for `options_tag` and
  `placeholder_tag` anywhere. Code replaces dropdown options and input
  placeholders at runtime, and a tagged widget restores its XML value on every
  language change, so write those two tags out where the value should translate.

The rule lives in `lib/helix-xml/src/xml/lv_xml.c#implied_tag_value` and its
Python twin `scripts/translations/extractor.py#implied_tag_value`, which the
extractor and the coverage gate both use. The key verdict is
`src/system/translation_loader.cpp#is_translation_key`, the engine's key
callback; `tests/python/test_implied_tags.py` and
`tests/unit/test_translation_key_filter.cpp` hold the two sides to the same
answers.

### Semantic Text Widgets

Use semantic text widgets (`text_heading`, `text_body`, `text_small`, `text_muted`) rather than raw `lv_label`:

```xml
<!-- ✅ Correct -->
<text_body text="Material"/>

<!-- ❌ Avoid -->
<lv_label text="Material" style_text_font="..."/>
```

### Dynamic Text with bind_text

For text that changes at runtime based on data, use `bind_text` with subjects instead of `translation_tag`:

```xml
<!-- Subject-bound text (NOT translated) -->
<text_body bind_text="printer_status_subject"/>

<!-- Temperature display from subject -->
<text_heading bind_text="nozzle_temp_display"/>
```

**Important:** `bind_text` and `translation_tag` serve different purposes:
- `translation_tag` - Static text that varies by locale
- `bind_text` - Dynamic text that varies by application state

### Text-input Placeholders with placeholder_tag

Text inputs translate their placeholder text via `placeholder_tag`, which
mirrors `translation_tag` on labels and is resolved through `lv_tr()` at parse
time:

```xml
<text_input placeholder_text="Search vendors..." placeholder_tag="Search vendors..."/>
```

The `form_field` component forwards the same attribute, so wrap a placeholder
the same way there:

```xml
<form_field placeholder="Enter network name" placeholder_tag="Enter network name"/>
```

As with `translation_tag`, the literal `placeholder_text` / `placeholder`
attribute is the English fallback, and the `*_tag` value is the YAML key to
look up.

## C++ Runtime API

Runtime translation goes through LVGL's native `lv_translation_*` API.

### Translating a string

Wrap a user-facing string literal in `lv_tr()`. It returns the active language's
string for that key, falling back to the English literal if there's no
translation:

```cpp
// Returns the localized string for the active language ("Cancel" in English)
const char* label = lv_tr("Cancel");

// Format strings: translate the format, then fmt::format with the args
std::string msg = fmt::format(lv_tr("Nozzle: {}°C"), temp);
```

Wrapping a C++ string literal in `lv_tr(...)` (or `fmt::format(lv_tr("..{}.."), arg)`
for format strings) is exactly how a C++ string becomes translatable - the
wrapped literal is also picked up by `make translation-sync`.

### Loading a locale and changing language

The active locale's pack is loaded on demand and made active. The locale loader
registers the per-locale XML pack, and `lv_translation_set_language()` switches
the active language for all `lv_tr()` lookups:

```cpp
// In system_settings_manager.cpp / application.cpp
helix::ui::ensure_translation_loaded(lang);   // registers ui_xml/translations/<lang>.xml
lv_translation_set_language(lang.c_str());     // switch active language
lv_obj_invalidate(lv_screen_active());         // refresh visible UI
```

See `src/system/translation_loader.cpp` for the loader, which tracks which
locales have already been registered so a pack is parsed at most once.

## Plural Rules

Plural forms follow the [CLDR standard](https://unicode-org.github.io/cldr-staging/charts/latest/supplemental/language_plural_rules.html).

### Categories

| Category | Typical Usage |
|----------|---------------|
| `zero`   | Zero items (some languages only) |
| `one`    | Singular (1 item, or special cases) |
| `two`    | Dual (exactly 2, some languages) |
| `few`    | Paucal (2-4 in Slavic languages) |
| `many`   | Large numbers (5+ in Slavic languages) |
| `other`  | General plural / fallback |

### Language Examples

**English** (simple - one/other):
- 1 → one ("1 item")
- 0, 2, 3... → other ("0 items", "2 items")

**French** (0/1 singular):
- 0, 1 → one ("0 élément", "1 élément")
- 2, 3... → other ("2 éléments")

**Russian** (complex - one/few/many/other):
- 1, 21, 31... → one ("1 элемент")
- 2-4, 22-24... → few ("2 элемента")
- 5-20, 25-30... → many ("5 элементов")
- Decimals → other

### How plurals resolve at runtime

A count string in C++ names both English forms and the count, and formats the result with
the same arguments whichever form comes back:

```cpp
#include "i18n_plural.h"

snprintf(buf, sizeof(buf), helix::ui::tr_plural("1 day ago", "%lu days ago", days), days);
fmt::format(fmt::runtime(helix::ui::tr_plural(nullptr, "{} critical issues", n)), n);
```

`helix::ui::tr_plural(one_key, other_key, n)` (`include/i18n_plural.h`) returns:

1. `lv_tr(one_key)` when `n == 1` and there is a singular key;
2. otherwise the `<other_key>_<category>` tag for `n`'s CLDR category in the active
   language, when that language's pack carries one;
3. otherwise `lv_tr(other_key)`.

The generator flattens a YAML plural dictionary into those tags: `other` becomes the plain
`other_key` tag, and every other category becomes `<other_key>_<category>` (`"{} prints_few"`).
A dictionary without `other`, or with a category outside `zero one two few many other`, fails
generation. Locales with flat strings get no variant tags and keep their one/other behaviour.

The category comes from `helix::ui::plural_category(lang, n)`, CLDR's integer rules for
every shipped language: `ja`/`zh` are always `other`; `en`/`de` are `one` for 1; `es`/`it`
are `one` for 1 and `many` for exact millions; `fr`/`pt` are `one` for 0 and 1 and `many` for
exact millions; `ru` is `one` for 1, 21, 101..., `few` for 2-4, 22-24..., `many` otherwise. A
new language needs its rule there.

Both keys must be string literals: `make translation-sync` extracts them from the
`tr_plural(` call, and `scripts/check_translation_format_specifiers.py` checks every plural
form against the format it replaces.

## Developer Workflow

### Adding New Translatable Strings

1. **Add the literal to XML** (the literal is its own tag; see "Static Text" above):
   ```xml
   <text_body text="New Feature"/>
   ```

2. **Extract to YAML files:**
   ```bash
   make translation-sync
   ```
   This scans XML and C++ and adds new keys to every YAML file: the English text in `en.yml`, an empty value (needs translation) in the others.

3. **Translate in YAML files:**
   Edit each `translations/*.yml` file to add proper translations:
   ```yaml
   # translations/de.yml
   "New Feature": "Neue Funktion"

   # translations/fr.yml
   "New Feature": "Nouvelle fonctionnalité"
   ```

   When translating a recurring domain term, reuse the rendering in
   `translations/GLOSSARY.md` for that language. For terms not in the glossary,
   grep the target `translations/<lang>.yml` and reuse the dominant existing
   rendering - never coin a new word for a concept that's already translated.

4. **Regenerate the XML translation packs:**
   ```bash
   make translations
   ```

5. **Rebuild and test:**
   ```bash
   make && ./build/bin/helix-screen --test -vv
   ```

### Preview Changes Before Sync

```bash
make translation-sync-dry-run
```

This shows what keys would be added without modifying files.

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make translations` | Regenerate the XML translation packs from YAML |
| `make translation-sync` | Extract strings from XML, merge new keys to YAML |
| `make translation-sync-dry-run` | Preview sync without modifying files |
| `make translation-coverage` | Show translation coverage statistics |
| `make translation-glossary` | Regenerate `translations/GLOSSARY.md` from the YAML |
| `make translation-obsolete` | Find translation keys not referenced anywhere |

### How `obsolete` decides a key is unused

Extraction and obsolete-detection ask opposite questions, so they use different
oracles:

- **`sync`** asks *"should this string be translated?"* It is tuned for
  **precision** - the `lv_tr()` / `text="…"` patterns in `scripts/translations/extractor.py`
  keep junk (subject names, hex colors, numbers) out of the YAML.
- **`obsolete`** asks *"is this key mentioned at all?"* It is tuned for
  **recall** - `collect_referenced_strings()` in `scripts/translations/obsolete.py`
  scans `src/`, `include/`, `ui_xml/`, `assets/`, `config/`, and `tests/` for the
  key as a delimited literal (a complete C/JSON string, an XML attribute value,
  or an XML text node).

Using the precision-tuned extractor as the usage oracle reports live keys as
dead, because keys reached *indirectly* are invisible to it:

| Indirect pattern | Example |
|------------------|---------|
| Tag in a struct initializer, resolved via `lv_tr(var)` | tour titles/bodies in `src/ui/tour/tour_steps.cpp` |
| Tag stored as JSON data | pre-print option labels in `assets/config/printer_database.json` |
| Label text declared in a header | `include/ui_panel_controls.h` |

Matching is deliberately **delimited, not substring** - otherwise the word
`Unlink` inside a code comment would count as a use of the `Unlink` key and hide
a genuinely dead entry. The scan errs toward reporting a reference: a key that
also appears in an unrelated log message just leaves a stale entry behind,
whereas a missed reference deletes a string users see.

Two categories are reported obsolete **by design**:

- Strings that live only in a dev/test panel carrying an `<!-- i18n: skip-file -->`
  marker: the marker opts the file out, so its strings should not be keys.
- Strings containing a literal `\n` (multi-line dropdown option labels), which
  `should_skip_text()` does not extract.

The generated packs under `ui_xml/translations/` are excluded from the scan;
they list every key by definition and so can never prove usage.

If a key is being wrongly reported, prefer making the reference visible (or add
an `// i18n:` note explaining the indirection, as in `tour_steps.cpp`) over
re-adding it after a delete.

## Temperature Units, Date Order and Region Defaults

Two display settings sit beside the language: the temperature unit and the date order. Neither changes what the printer or the config stores.

### Temperature unit

Every temperature in state, settings and G-code stays in Celsius. Only what is shown and what is typed crosses `include/core/temperature_units.h`: `helix::units::to_display_temp()` and `format_display_temp()` for readings, `display_temp_to_celsius()` for a typed target, `to_display_temp_delta()` for a band or a rate (a difference scales and has no offset), and `temp_axis_ticks()` for a graph axis, whose Fahrenheit ticks fall on round values. Both directions round with `std::lround`. New temperature UI calls these and never converts by hand.

The setting is `/display/temp_unit` (0 Celsius, 1 Fahrenheit), owned by `DisplaySettingsManager` (`src/system/display_settings_manager.cpp#set_temp_unit`) and shown as a row in Settings > Appearance. A change sets the process-wide display unit, updates the `settings_temp_unit_symbol` subject that XML labels bind for their "(°C)" suffix, refreshes the preset labels, and calls `units::publish_temp_unit()`.

A string formatted in C++ from a cached Celsius value keeps its unit until its owner formats it again. `include/core/temp_unit_subject.h` is the trigger for that: `helix::ui::observe_temp_unit_change(owner, on_change)` runs `on_change` only when the unit really changes, never on registration, and queues it so it runs on the main thread. A panel, widget or state object that caches formatted temperature text observes it and reformats (examples: `src/ui/controls/temperature_service.cpp`, `src/printer/printer_temperature_state.cpp`, `src/ui/panel_widgets/nozzle_temps_widget.cpp`). The printer's own temperature subjects are not touched by a unit switch, so their observers do not fire.

### Date order

`/display/date_format` holds 0 for day-month-year, 1 for month-day-year and 2 for year-month-day. `src/ui/locale_formats.cpp#get_date_pattern` takes the arrangement from the saved order and falls back to the language's default while none is saved; the language still supplies day and month names and the punctuation German writes. The order is kept in an atomic (`saved_date_format()` in `include/region_defaults.h`) because dates are also formatted off the main thread. The row lives in Settings > Language & Time, and `DisplaySettingsManager::on_language_changed` updates it while it still follows the language.

### Region defaults and the wizard step

`src/system/region_defaults.cpp#region_defaults` turns a language and an IANA timezone into the defaults: a United States zone reads Fahrenheit and month-first, every other zone reads Celsius, an English speaker in a known non-US zone writes the day first, and with no zone the language alone decides the date order (Chinese and Japanese year first, US English month first, British English and the other languages day first). The same table answers the clock: `default_24h_clock_for_language()` is 12-hour for US English and 24-hour for every other offered language, British English included; the wizard's clock row and `helix::ui::locale_default_24h()` both read it.

The setup wizard's Appearance & Region step (`src/ui/wizard/ui_wizard_appearance.cpp`, `ui_xml/wizard_appearance.xml`) follows the Language step. It offers timezone, 12 or 24-hour clock, theme (applied live), dark mode, speed and flow units, temperature unit and date order. Each row reuses its Settings twin's subject and callback, so there is one copy of the state. Picking a timezone applies the region defaults to the unit rows that still follow the region, meaning rows nobody had saved when the page opened and nobody has picked on it since. A pick on a row stops that row following the region. The step is skipped when a printer is added after the first run, because every row is global.

## Key Files Reference

| File | Purpose |
|------|---------|
| `translations/*.yml` | Source translation files (one per language or regional variant) |
| `scripts/translations/locale_variants.py`, `include/core/locale_code.h` | Regional variants: base language, pack merge, coverage inheritance |
| `translations/GLOSSARY.md` | Canonical per-language renderings of recurring terms (generated) |
| `scripts/generate_translations.py` | Main pack generator |
| `scripts/translation_sync.py` | String extraction, sync, and glossary tool |
| `scripts/translations/` | Support modules for sync tool |
| `mk/translations.mk` | Makefile integration |
| `ui_xml/translations/translations.xml` | Generated combined LVGL XML pack |
| `ui_xml/translations/<lang>.xml` | Generated per-locale packs (loaded at runtime) |
| `src/system/translation_loader.cpp` | Per-locale pack loader (`ensure_translation_loaded()`) |
| `include/core/temperature_units.h`, `include/core/temp_unit_subject.h` | Celsius/Fahrenheit conversion and the unit-change observer |
| `include/region_defaults.h`, `src/system/region_defaults.cpp` | Region defaults for temperature unit and date order |
| `src/application/application.cpp` | Translation startup / language switching |
| `src/system/system_settings_manager.cpp` | Locale switching (`ensure_translation_loaded()` + `lv_translation_set_language()`) |

## Troubleshooting

### Translation not appearing

1. Check the key exists in `translations/en.yml`
2. Verify `translation_tag` matches the key exactly
3. Run `make translations` to regenerate
4. Rebuild the application

### Missing translation warning

The generator warns about keys missing in non-English locales:
```
WARNING: Missing 'New Feature' in de
WARNING: Missing 'New Feature' in fr
```

Fill in missing translations in the appropriate YAML files.

### CJK text renders as tofu

The runtime CJK fonts (`assets/fonts/cjk/*.bin`) are subset to the characters `scripts/translations/cjk_charset.py` finds in the translations, C++ sources, XML layouts and `assets/config/printer_database.json`; `scripts/check_cjk_font_staleness.sh` (run by the commit hook) fails when that set holds a character the baked manifest lacks, and `make regen-text-fonts` fixes it. The first-run language chooser renders before any `.bin` loads, from the compiled `.c` fonts, so its characters must also be in `WIZARD_CJK` in `scripts/regen_text_fonts.sh`; the same gate enforces that (#1622).

Print filenames are user text, so they get a second, larger set whatever the UI language (#2093). `scripts/translations/cjk_filename_fonts.py` defines it from `include/core/cjk_ranges.inc`, the ranges `contains_cjk()` detects: every codepoint of an `ALL` range (CJK punctuation, kana, fullwidth ASCII, halfwidth katakana), and from a `LISTED` range only level 1 of the Table of General Standard Chinese Characters (`scripts/translations/hanzi_level1.txt`, 3,500 hanzi) and the Joyo kanji (`joyo_kanji.txt`, 2,136). Traditional-only hanzi and rarer kanji render as boxes. The bins under `assets/fonts/cjk/filename/` hold only the part of that set the translation bins lack, one per pixel size that `font_body`, `font_small` or `font_xs` (the `text_body`, `text_small` and `text_xs` widgets every filename label is) takes at any breakpoint and any UI scale. The scale steps a face up through `DisplayMetrics::scaled_font_name()`; `scripts/translations/font_scale_reach.txt` lists what each face can become, and `[display_metrics]` fails when it disagrees with that function. A scaled face is adopted only when AssetManager registers it at the breakpoint: `scripts/translations/registered_text_fonts.txt` lists those names per breakpoint, written from `AssetManager::text_font_names_for_breakpoint()` (the registration table in `src/application/asset_manager.cpp`) and pinned by `[font_tier]`. `HELIX_UPDATE_GOLDEN=1` rewrites either file. Nothing loads until a filename containing CJK is formatted for display: `strip_gcode_extension()` and `get_display_filename()` in `include/core/gcode_filename.h` call `note_displayed_filename()`, as do the sites that show a raw name (a folder, a history row), and the hook `init_cjk_fonts()` (`src/application/cjk_filename_glyphs.cpp`) registers makes `CjkFontManager::ensure_filename_glyphs()` chain each size's filename bin behind a translation bin of the same size. A load then refreshes every object's style, so a label laid out before it, measured with missing glyphs, measures and wraps its text again. `check_cjk_font_coverage.py` fails when a size has no bin, a character of the set is in neither bin, or a listed character lies outside every `LISTED` range, and `platform_manifest.py prune-assets` ships a fixed panel only the sizes its breakpoint reaches at any scale in a build of its font tier. The ESP32 build (`HELIX_HAS_CJK=0`) loads none of it.

### Plural form not working

1. Verify the call site uses `helix::ui::tr_plural()` with the plural key as a literal
2. Verify the dictionary sits on the plural (`other_key`) entry and has an `other` form
3. Check the language has a rule in `helix::ui::plural_category()` (`src/core/i18n_plural.cpp`)
4. Check the regenerated `ui_xml/translations/<lang>.xml` carries the `<key>_<category>` tags
