# Slick — the fleet's Slack-shaped Matrix client

OURS. Spec: `docs/superpowers/specs/2026-06-12-slick-design.md`.

## Dev

```powershell
cd G:\X3Native\tools\slick
npm install
npm run dev          # vite on http://localhost:5173, --host exposes LAN
```

Log in with any Conduit account (the ones `tools/fleet/create_user.py` mints).

## Build

```powershell
npm run build        # typecheck + bundle to dist/
```

## Deploy (PR-7, not yet wired)

`dist/` is served at `slick.slopclaude.com` via a cloudflared ingress rule →
`127.0.0.1:8090` (a `Slick-Static` Scheduled Task, same pattern as
DocsReader-Static). See spec §4.2.

## Architecture notes

- **No matrix-js-sdk.** `src/client.ts` is a ~250-line typed fetch client.
  The fleet homeserver has no E2EE and no federation, so the SDK's weight
  buys nothing (spec §4.1). If that ever changes, swap the transport behind
  the same interface.
- **One sync loop** (`src/store.ts`) feeds a room map; components subscribe
  via a version counter. No state library.
- **PR ladder:** PR-1 skeleton (login → sync → room list) ✅ · PR-2 message
  stream · PR-3 composer · PR-4 inline images · PR-5 `/gen` · PR-6 members ·
  PR-7 deploy.
