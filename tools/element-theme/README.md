# Element theme — cyberpunk-x3native

Custom Element Web theme matching the X3Native engine aesthetic. Dark blue-black background, portal-cyan accents (the same `#00ffd5` from the rifthub portals), Orbitron headers, JetBrains Mono code spans.

## Files

| File | Purpose |
|---|---|
| `theme.json` | Element theme manifest (semantic palette + fonts). Used by Element's "custom theme" feature. |
| `theme.css` | Full CSS override stylesheet for fine-grained Element 1.11.x layout tweaks. |
| `README.md` | This file. |

## Installing on Element Web (self-hosted at chat.&lt;CHOSEN_DOMAIN&gt;)

### Method 1 — copy CSS into Element's webroot and reference from index.html

```powershell
# On 13700K
Copy-Item D:\GameDev\X3Native\tools\element-theme\theme.css C:\opt\element-web\themes\cyberpunk-x3native.css
```

Then edit `C:\opt\element-web\index.html` to add a `<link>` tag in the `<head>`:

```html
<link rel="stylesheet" href="themes/cyberpunk-x3native.css" />
```

This applies the theme to all users automatically.

### Method 2 — paste into Element's per-user "Custom theme" setting

In Element Web (or Element Desktop): **Settings → Appearance → Custom theme → Add custom theme** → paste the contents of `theme.json` and click Save. Each user enables it individually.

This is the lowest-friction path during early testing; promote to Method 1 once the theme is finalized.

## Customizing

The palette lives in `:root` at the top of `theme.css`. Adjust the variables there to retune. Common knobs:

| Variable | Default | Effect |
|---|---|---|
| `--bg-base` | `#0a0e1a` | Sidebar + page background |
| `--bg-panel` | `#121826` | Message timeline background |
| `--bg-elevated` | `#1a2333` | Hover/selected items |
| `--accent-cyan` | `#00ffd5` | Mentions, code spans, primary buttons |
| `--accent-violet` | `#b94dff` | Memory Hunter / Crystal Heart cues (currently used for DJBOOTH name) |
| `--accent-amber` | `#ff8c1a` | Alien sky / 14900K name |
| `--font-display` | `Orbitron, …` | Room headers + bot display names |
| `--font-mono` | `JetBrains Mono, …` | Code, timestamps, machine names |

The `[data-self="@machine"]` selectors at the bottom assign a fixed color per fleet bot — DJBOOTH violet, 13700K cyan, 14900K amber, etc. Element normally hashes the user_id for color; these overrides give us deterministic fleet identity colors.

## Optional: scanline texture

There's a commented-out block at the bottom of `theme.css` that adds subtle CRT-style scanlines as a fixed-position overlay. Uncomment for full cyberpunk vibes. Disabled by default because it can be distracting during real work.

## Compatibility

Targets Element Web 1.11.x layout selectors. Each major Element release shuffles class names; expect to revisit on Element upgrade. Selectors are pinned by the `mx_*` prefix Element uses, but internal layout names can change.

## Future widget styling

Phase 2 of the spec (`docs/superpowers/specs/2026-05-27-fleet-messaging-design.md` §4.6) adds a fleet-status sidebar widget showing machine presence + branch state. The widget will use this theme's palette + fonts directly — no separate theming work needed.
