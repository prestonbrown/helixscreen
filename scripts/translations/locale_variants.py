# SPDX-License-Identifier: GPL-3.0-or-later
"""Regional variants of a translated language (``en-GB``, ``pt-BR``).

A locale code is a language subtag, optionally followed by ``-`` and a region
subtag. A code with a region is a *variant* of its base language: its YAML file
(``translations/en-GB.yml``) carries every key, but only the strings the region
writes differently have a value. An empty value means "inherit from the base",
not "untranslated".

The generator resolves the inheritance when it writes the variant's runtime pack
(``merge_variant``), so the app loads one pack per language and needs no
fallback chain of its own: a key the variant and its base both leave empty is
absent from the pack, and LVGL falls back to the tag, the English source text.

The C++ twin of ``base_locale`` is ``helix::locale_base_language()`` in
``include/core/locale_code.h``.
"""

from typing import Any, Optional


def base_locale(locale: str) -> Optional[str]:
    """The language a variant inherits from (``en`` for ``en-GB``), or None for a
    plain language code."""
    for sep in ("-", "_"):
        if sep in locale:
            return locale.split(sep, 1)[0]
    return None


def is_variant(locale: str) -> bool:
    return base_locale(locale) is not None


def has_value(value: Any) -> bool:
    """Whether a YAML value carries text: a non-blank string or a plural mapping
    with at least one non-blank form."""
    if isinstance(value, dict):
        return any(isinstance(v, str) and v.strip() for v in value.values())
    return isinstance(value, str) and bool(value.strip())


def merge_variant(base: dict[str, Any], variant: dict[str, Any]) -> dict[str, Any]:
    """The translations a variant's pack holds: the base's entries overlaid with
    every entry the variant gives a value.

    A base entry equal to its own key is left out. It would only repeat the tag,
    which LVGL returns for a missing key anyway, and it keeps a variant of the
    identity locale (``en``) down to the strings it really changes. A variant
    value replaces the base entry whole, so a flat variant string drops the
    base's plural forms for that key.
    """
    merged = {k: v for k, v in base.items() if has_value(v) and v != k}
    for key, value in variant.items():
        if has_value(value):
            merged[key] = value
    return merged


def resolve_variants(translations: dict[str, dict[str, Any]]) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Replace each variant's entries with its merged entries.

    Returns the resolved {locale: entries} map and an error for each variant whose
    base language has no file.
    """
    resolved: dict[str, dict[str, Any]] = {}
    errors: list[str] = []
    for locale, entries in translations.items():
        base = base_locale(locale)
        if base is None:
            resolved[locale] = entries
        elif base not in translations:
            errors.append(f"{locale}: variant of '{base}', but there is no {base}.yml")
        else:
            resolved[locale] = merge_variant(translations[base], entries)
    return resolved, errors


def effective_value(locale: str, key: str, catalogs: dict[str, dict[str, Any]]) -> Any:
    """The value a key renders with in ``locale``: its own, else (for a variant)
    the base language's. ``catalogs`` maps locale to its raw YAML entries."""
    own = catalogs.get(locale, {}).get(key)
    if has_value(own):
        return own
    base = base_locale(locale)
    if base is not None:
        return catalogs.get(base, {}).get(key)
    return own
