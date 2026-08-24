# Theme Contributor Guide

Contributing a theme is the lowest-barrier way to contribute to HelixScreen. No C++. No build. No framework knowledge. Just a JSON file with colors and a few style properties, and an eye for what looks right.

If you've ever wanted HelixScreen to match your setup — a Catppuccin workstation, a retro amber phosphor vibe, your shop's brand colors — you can ship that in an afternoon.

This doc is for people creating or contributing themes. If you want to understand how the theme system is *implemented* (C++ internals, token pipeline, reactive application), read [THEME_SYSTEM.md](THEME_SYSTEM.md) instead.

---

## What a theme is

A HelixScreen theme is a single JSON file. It defines:

- A **dark** palette (16 colors)
- A **light** palette (same 16 colors)
- A handful of **style properties** (border radius, border width, shadow intensity)

That's it. Users pick your theme in Settings → Display & Sound, and the UI re-renders using your colors instantly — no restart needed.

The file lives in `assets/config/themes/defaults/` in the repo. Every theme in that directory is auto-discovered and shown in the theme picker. Adding a new one is literally dropping a JSON file.

---

## Anatomy of a theme

Here's the full `ayu.json` — use this as a reference while reading:

```json
{
  "name": "Ayu",
  "dark": {
    "screen_bg": "#0D1017",
    "overlay_bg": "#1F2430",
    "card_bg": "#272D38",
    "elevated_bg": "#3D4455",
    "border": "#4D5566",
    "text": "#BFBDB6",
    "text_muted": "#9DA2AC",
    "text_subtle": "#5C6773",
    "primary": "#FFCC66",
    "secondary": "#F28779",
    "tertiary": "#95E6CB",
    "info": "#73D0FF",
    "success": "#87D96C",
    "warning": "#FFCC66",
    "danger": "#F28779",
    "focus": "#FFCC66"
  },
  "light": {
    "screen_bg": "#FAFAFA",
    "overlay_bg": "#F3F4F5",
    "card_bg": "#FFFFFF",
    "elevated_bg": "#E7E8E9",
    "border": "#D1D2D3",
    "text": "#5C6166",
    "text_muted": "#787B80",
    "text_subtle": "#8A9199",
    "primary": "#FFAA33",
    "secondary": "#F07171",
    "tertiary": "#4CBF99",
    "info": "#399EE6",
    "success": "#6CBF43",
    "warning": "#FFAA33",
    "danger": "#E65050",
    "focus": "#FFAA33"
  },
  "border_radius_size": 3,
  "border_width": 1,
  "border_opacity": 40,
  "shadow_intensity": 0,
  "shadow_opa": 0,
  "shadow_offset_y": 2
}
```

---

## The 16 color tokens

Each palette (`dark` and `light`) has the same 16 tokens. What each one is used for:

### Backgrounds (4)

| Token | What it is | Example use |
|---|---|---|
| `screen_bg` | The deepest background — the screen itself | Home panel, print select, app-level background |
| `overlay_bg` | Background for overlays that sit above the main panel | Settings subpages, filament overlay |
| `card_bg` | Cards and primary content containers | Print job cards, widget tiles, list rows |
| `elevated_bg` | Content raised above cards — drops, pressed states, modals | Dropdowns, pressed buttons, modal dialogs |

Ordering matters. `screen_bg` should be the darkest (in a dark theme) or lightest (in light), and each subsequent layer steps *toward* the foreground. Users' eyes read depth from these. Flat palettes that use the same value for all four look amateurish.

### Borders (1)

| Token | What it is |
|---|---|
| `border` | Dividers, outlines, card edges |

Usually subtle. The `border_opacity` property (see below) controls how strongly it shows.

### Text (3)

| Token | What it is | Example use |
|---|---|---|
| `text` | Primary readable text | Labels, values, paragraphs |
| `text_muted` | Secondary text, de-emphasized | Captions, descriptions under a heading |
| `text_subtle` | Tertiary text, nearly background | Placeholders, disabled state, long-dead info |

Contrast matters most for `text` on `card_bg`. If you can't read it easily in both light and dark modes, the theme is broken regardless of how cool the palette feels.

### Accent colors (3)

| Token | What it is | Example use |
|---|---|---|
| `primary` | The theme's signature accent | Primary buttons, focus rings, brand-feel elements |
| `secondary` | A supporting accent, often complementary | Secondary buttons, alternate highlights |
| `tertiary` | A third accent for variety | Chart series, multi-color indicators |

These should be harmonious — not three saturated hues fighting each other. Pick a palette strategy (analogous, complementary, triadic) and stick to it.

### Status colors (4)

| Token | What it is | Example use |
|---|---|---|
| `info` | Informational messaging | Tooltips, notices |
| `success` | Things went right | "Print complete", green checkmarks |
| `warning` | Caution required | Temperature warnings, "are you sure?" prompts |
| `danger` | Errors and destructive actions | Crash reports, delete buttons, emergency stop |

**Don't get clever with status colors.** Green-success, red-danger, yellow-warning is universally understood. A theme that uses purple for success because it fits the aesthetic makes the UI harder to scan. There's room for stylization, but the semantic signal has to survive.

### Focus (1)

| Token | What it is |
|---|---|
| `focus` | Keyboard/navigation focus indicator |

Often the same as `primary`. Must contrast clearly against `card_bg` and `screen_bg`.

---

## Style properties

Below the two palettes, a theme can also set:

| Property | Range | Default | What it does |
|---|---|---|---|
| `border_radius_size` | 0–7 (index) | 3 | Corner roundness preset. 0 = sharp, 7 = fully rounded. Try 2 for classic, 4 for modern-soft, 6 for bubbly. |
| `border_width` | 0–4 (px) | 1 | Width of borders in pixels. 0 disables borders entirely. |
| `border_opacity` | 0–255 | 40 | How opaque borders are (0 = transparent, 255 = fully opaque). The default 40 is ≈16%, i.e. subtle; low values (20–60) keep borders faint. |
| `shadow_intensity` | px | 0 | Blur radius of drop shadows, in pixels (not a 0–100 scale). 0 disables. |
| `shadow_opa` | 0–100 | 0 | Opacity of drop shadows. 0 disables. Shadows need both intensity AND opa to show. |
| `shadow_offset_y` | int | 2 | Vertical shadow offset in pixels. |

These are global to the theme — they apply to both dark and light palettes.

---

## Creating a theme, step by step

### Step 1: Start from an existing theme

Never start from scratch. Copy a theme whose aesthetic is closest to what you want:

```bash
cp assets/config/themes/defaults/nord.json assets/config/themes/defaults/my-theme.json
```

Edit `name` to your theme's display name ("My Cool Theme"). Change the palette values one token at a time, testing as you go.

### Step 2: Test locally without rebuilding

You don't need to rebuild HelixScreen to test a theme. Put your theme in the **user themes directory**:

- **On your dev machine:** `~/helixscreen/config/themes/my-theme.json`
- **On a device:** `~/helixscreen/config/themes/my-theme.json` (SSH'd to the printer)

User themes take precedence over defaults and are auto-discovered at launch. Restart the app, go to Settings → Display & Sound → Theme, and pick your theme.

Edit the JSON, save, restart. Iterate fast.

### Step 3: Test both modes

Every theme must provide both `dark` and `light`. Users switch modes in settings — your theme must look intentional in both.

Common mistake: designing in dark mode and copy-pasting the dark palette into `light` with minor tweaks. The result is always wrong because the *relationships* between layers invert. In dark mode, deeper = darker; in light mode, deeper = lighter. Design each mode from its own starting point.

### Step 4: Test on multiple panels

Themes look different on sparse panels (home screen) vs dense ones (AMS, settings). Exercise:

- **Home** — lots of widgets, multiple accent colors visible simultaneously
- **Print select** — file cards, thumbnails, metadata
- **AMS** — status colors on slots, multi-unit layouts
- **Settings** — lots of rows with dividers, lots of text
- **Print status** (during a simulated print) — primary accent used heavily, progress states

If your theme looks great on one panel and terrible on another, it's not done.

### Step 5: Test contrast

Text on backgrounds must meet readable contrast. HelixScreen runs on 3D printers — often in a workshop, often viewed from an angle, often through safety glasses or at a distance. Low-contrast aesthetics that look refined on your monitor become unreadable in practice.

Rough targets:
- `text` on `card_bg`: aim for ≥ 7:1 contrast (WCAG AAA)
- `text_muted` on `card_bg`: ≥ 4.5:1 (WCAG AA)
- `text` on `screen_bg`: ≥ 7:1

Use any contrast checker. If your contrast is marginal, bump `text` darker (light mode) or lighter (dark mode) — don't try to get away with it.

---

## Using the in-app theme editor

HelixScreen has a built-in live theme editor (Settings → Display & Sound → Theme → Edit). It lets you tweak colors with live preview and export the result as JSON.

Workflow: create a minimal JSON stub, load it as your active theme, tune it in the editor, export the result, copy back into your `.json` file. Faster than editing hex codes in a text editor when you're still searching for the palette.

---

## What makes a good contribution

Themes get accepted when they:

- **Have a clear identity.** Someone should be able to describe your theme in one short phrase — "solarized-light port", "warm coffee-shop browns", "synthwave neon on near-black". If you can't articulate it, neither can the user.
- **Both modes are intentional.** Not just "I made dark, and light is a hasty inversion."
- **Accent colors are harmonious.** Three accents that fight each other on the home panel are a no.
- **Status colors stay legible semantically.** Stylize, but keep green≈success, red≈danger, yellow≈warning recognizable.
- **Work on the devices HelixScreen runs on.** Some printers have 480x320 displays with 6-bit color depth; extreme gradients or very close color values flatten out. If possible, test on real hardware or at least at smaller sizes.
- **Aren't a near-duplicate** of an existing default theme. If your "custom dark" looks like Nord with three tokens nudged, ship it as a user theme, not a default.

Themes get bounced when they:

- Only provide one mode (`dark` or `light` missing).
- Have unreadable text/background combinations in normal use.
- Break status color conventions (red-success, green-danger).
- Copy a copyrighted brand identity without permission.

---

## Contributing your theme

1. **Put the final JSON in `assets/config/themes/defaults/`** in the repo.
2. **Filename is the theme's kebab-cased name** — my-cool-theme.json, not MyCoolTheme.json.
3. **Take screenshots** of your theme on at least three panels — home, settings, print status. These go in the PR description.
4. **Open a PR** on `prestonbrown/helixscreen` with `feat(theme): add <theme name>` as the commit subject.
5. **Mention the inspiration** if it's a port of an existing palette (Ayu, Catppuccin, Gruvbox, etc.) so the PR review can verify any attribution requirements.

That's it. No C++. No build. No rebuild after merge — the packaging picks up every JSON in the defaults directory automatically.

---

## When you're stuck

- **Theme picker doesn't show my theme** — the JSON is probably malformed. Check the logs (`./build/bin/helix-screen --test -vv` and grep for `[ThemeLoader]`). A missing `"dark"` or `"light"` key or a JSON syntax error will log a warning and fall back to Nord.
- **Colors look wrong in one mode** — confirm you're editing the right palette (`dark` vs `light`). Easy to edit one and forget the other.
- **Borders don't show** — check `border_width` (default 1) and `border_opacity` (default 40). Setting either to 0 disables borders entirely.
- **A specific widget looks off** — it may be hardcoded against a token you haven't considered. File an issue with a screenshot and the widget name.

---

## Related docs

- [THEME_SYSTEM.md](THEME_SYSTEM.md) — how the theme system is implemented (for developers adding new tokens or widgets)
- [UI Contributor Guide § Color System](UI_CONTRIBUTOR_GUIDE.md) — how tokens are referenced from XML and C++
- [Your First Contribution](YOUR_FIRST_CONTRIBUTION.md) — if you want to graduate from theming to feature work
