# tools/eflz — EFLZ design-docs reader baker

Bundles every `.md` under `docs/` into a single self-contained `_reader.html`
served at `https://docs.slopclaude.com/_reader.html` (Cloudflare Access
gated) and `http://192.168.1.206:7777/_reader.html` on the LAN.

## Run

```powershell
python -X utf8 G:\X3Native\tools\eflz\build_reader.py
```

Writes the reader to `docs/design/_reader.html`. The `DocsReader-Static`
Scheduled Task already serves that directory, so the live URL refreshes on
the next browser load. No server restart needed.

## How it picks docs

Two-tier selection:

1. **Curated list** (`DOCS` in the script) — order matters. "Master Task List"
   pinned first, then "New" specs, "Existing" lore, "Fleet" docs, "Plan
   Reviews". This is what controls the sidebar grouping you see.

2. **Auto-discovery** — anything else under `docs/` not in the curated list
   gets bundled under a group derived from its parent directory (e.g.,
   `docs/superpowers/plans/` → group **"Superpowers / Plans"**). Means new
   fleet docs just appear next rebuild without touching the script.

To promote an auto-discovered doc into a custom group with a pretty title,
add a line to the `DOCS` list above the closing `]`.

## Rebuild cadence

Manual right now. If we end up rebuilding often enough to want a
Scheduled Task, the candidate is `EflzReader-Rebuild` (daily 03:00, runs
`build_reader.py`). Not installed yet — keeping it manual until the doc
churn justifies it.
