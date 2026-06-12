# Slick — design spec

| Field | Value |
|---|---|
| **Date** | 2026-06-12 |
| **Status** | APPROVED by Tim in Fleet Ops ("REbuild it from source, but make it OURS.. identical to SLACK") + kickoff directive ("Write that spec and then commit and start bootstrap") |
| **Author** | Integrator (13700K), running Fable 5 |
| **Builds on** | FleetCommand (Conduit + cloudflared + per-box daemons), already live |

---

## 1. Problem

Element Web is the fleet's only human chat UI and Tim finds it genuinely hostile: rooms are hard to find, members are hard to see, settings are buried, and the identity/verification nags add friction on a private single-tenant homeserver. Tim's words: *"it is currently very CONFUSING and HARD TO FIND ROOMS, MEMBERS, etc. This should be Easy, and Simple, Like SLACK is."*

Additional asks from the same Fleet Ops thread:
- **Inline images at high quality** — render `m.image` in the stream at full resolution (the `mxc://` download endpoint, not the 800px thumbnail), click-to-expand (DJBOOTH's framing).
- **ComfyUI on the 5090, remotely** — a `/gen <prompt>` slash command that drops a generation job into the channel as a structured event; StarForge's on-LAN box catches it, runs FLUX→Hunyuan on the 5090, posts the result image back into the room.

## 2. Goals

1. **A Slack-shaped client.** Purple sidebar, `#channel` list, DM section, white message pane, messages grouped by sender, composer at the bottom. Anyone who has used Slack should orient in five seconds.
2. **Rooms + members always visible.** No hunting. Sidebar lists every joined room; member panel toggles on the right.
3. **Full-quality inline images.** `m.image` events render inline from the full-res media endpoint with click-to-expand lightbox. Uploads via drag-drop or paste go up at original quality.
4. **`/gen` built in.** Composer recognizes `/gen <prompt>`, emits a structured `com.fleet.gen.request` event; results arrive as ordinary `m.image` posts from StarForge's bridge.
5. **OURS.** Our repo, our pixels, our deploy. No Element code.
6. **Boring to operate.** Static files behind the existing tunnel; no new server process beyond a static file server.

## 3. Non-goals (v1)

- **E2EE.** Fleet rooms are unencrypted on a private, federation-disabled homeserver behind a tunnel. Encryption is the single biggest source of Matrix-client complexity; skipping it is what makes a 1-week MVP honest. (If ever needed, swap the transport layer for matrix-js-sdk.)
- **Federation, spaces, voice/video, threads, reactions, read receipts** — v2+ candidates, not v1.
- **Mobile-native apps.** The web app should be usable on a phone browser, but no native builds.
- **Replacing the bot daemons.** Bots keep using the Node daemon + named pipes. Slick is the human window.

## 4. Architecture

```
┌─ Slick: static SPA (Preact + TypeScript, built by Vite) ──────────────┐
│                                                                       │
│  ┌──────────────┐  ┌───────────────────────────┐  ┌───────────────┐   │
│  │ Sidebar      │  │ Message stream            │  │ Member panel  │   │
│  │  workspace   │  │  grouped by sender        │  │  (toggle)     │   │
│  │  # channels  │  │  inline full-res images   │  │  presence     │   │
│  │  DMs         │  │  click-to-expand lightbox │  │  display name │   │
│  │  unread dots │  │  /gen results             │  └───────────────┘   │
│  └──────────────┘  │  composer + slash cmds    │                      │
│                    └───────────────────────────┘                      │
└──────────────┬────────────────────────────────────────────────────────┘
               │ fetch() — Matrix client-server REST (CORS: Matrix spec
               │ requires permissive CORS; Conduit complies)
               ▼
        Conduit @ fleetcommand.slopclaude.com (existing)
               ▲
               │ com.fleet.gen.request events
        ┌──────┴──────────┐
        │ StarForge bridge │  on-LAN watcher → ComfyUI API on the 5090
        │ (his lane)       │  FLUX→Hunyuan → upload mxc:// → m.image back
        └─────────────────┘
```

### 4.1 Transport: thin hand-rolled client, not matrix-js-sdk

v1 uses a ~250-line typed `client.ts` (fetch-based) instead of matrix-js-sdk. Reasons:

- The SDK's main value is E2EE, federation edge cases, and IndexedDB state reconciliation — all out of scope on our unencrypted single-server fleet.
- A thin client keeps the bundle tiny, the behavior fully inspectable, and the code OURS — which is the brief.
- The endpoints we need are few and stable: `/login`, `/sync` (long-poll), `/rooms/{id}/send`, `/rooms/{id}/messages`, `/media/v3/upload`, `/media/v3/download`, `/profile`, `/rooms/{id}/joined_members`.

If requirements grow into E2EE or federation, the component layer stays and `client.ts` gets swapped for the SDK behind the same interface.

### 4.2 Serving + auth

- **Dev:** `vite dev` on the 13700K, reachable over LAN at `http://192.168.1.206:5173`.
- **Prod:** `vite build` → static `dist/` served at **`slick.slopclaude.com`** via a new cloudflared ingress rule (`slick.slopclaude.com → 127.0.0.1:8090`, a `python -m http.server` Scheduled Task `Slick-Static`, same pattern as DocsReader). DNS route added with `cloudflared tunnel route dns`.
- **Login** = Matrix password login against Conduit (the accounts `create_user.py` mints). Access token cached in `localStorage`; logout clears it. No Cloudflare Access on this hostname — Conduit's own auth is the gate (same trust model as fleetcommand.slopclaude.com today).

### 4.3 The `/gen` contract (StarForge's bridge consumes this)

Composer input `/gen a chrome saucer over a stormy sea` sends:

```json
{
  "type": "com.fleet.gen.request",
  "content": {
    "prompt": "a chrome saucer over a stormy sea",
    "requested_by": "@tim:fleetcommand.slopclaude.com",
    "params": { "pipeline": "flux-hunyuan", "count": 1 }
  }
}
```

The bridge (StarForge's watcher, on the home LAN with the 5090) long-polls `/sync`, catches the event, runs the job, uploads the result via `/media/v3/upload`, and posts an `m.image` with `m.relates_to` referencing the request event. Slick renders a pending chip for outstanding requests and replaces it when the related image lands. Failures post `com.fleet.gen.error` with a reason string.

### 4.4 Inline images, full quality

- Render path: `mxc://server/id` → `/_matrix/client/v1/media/download/server/id` (authenticated full-res download endpoint, never `/thumbnail`).
- `<img>` constrained to max 480px tall in-stream via CSS only — the fetched asset is the original; click opens a lightbox at natural size with a download link.
- Upload path: drag-drop / paste / file-picker → `POST /_matrix/media/v3/upload` (no client-side recompression — original bytes) → `m.image` with width/height/mimetype/size metadata.
- Server side (DJBOOTH's lane): confirm Conduit's `max_request_size` accommodates big PNGs (bump to ≥ 50 MB if clamping).

## 5. Components (v1 file map)

```
tools/slick/
  package.json            preact + vite + typescript only
  vite.config.ts
  tsconfig.json
  index.html
  README.md               run/build/deploy instructions
  src/
    main.tsx              mount point
    app.tsx               session state: Login vs Workspace
    client.ts             thin Matrix client (login/sync/send/media/members)
    store.ts              rooms + timelines + members; sync-loop reducer
    components/
      Login.tsx           server-fixed login card
      Sidebar.tsx         workspace header, channel + DM sections, unread dots
      RoomView.tsx        header, message stream, day dividers, member toggle
      Message.tsx         sender-grouped rows, m.text / m.image / m.notice
      ImageEvent.tsx      full-res inline render + lightbox
      Composer.tsx        textarea, enter-to-send, /gen + /me parsing, upload
      MemberPanel.tsx     joined members + presence
    slick.css             the Slack-shaped skin (sidebar #3F0E40 family)
```

State model: a single `store.ts` holding `Map<roomId, Room>` where `Room = { name, alias, timeline: Event[], members, unread }`, fed by one long-poll `/sync` loop with `since` token persisted to `localStorage`. No external state library.

## 6. Slack-likeness checklist (the "five-second orientation" bar)

- [x] Left sidebar in Slack aubergine; workspace name "FleetCommand" top-left
- [x] `# channel-name` rows, bold + white-dot when unread, alphabetical
- [x] DMs section beneath channels with presence dots
- [x] Main pane white; messages grouped by sender with avatar + name + time
- [x] Hover timestamp on grouped messages
- [x] Composer pinned bottom with placeholder "Message #channel"
- [x] Enter sends; Shift+Enter newlines
- [x] Day divider lines ("Today", "Yesterday", dates)
- [ ] v2: reactions, threads, search, jump-to-unread, notification sounds

## 7. Delivery plan + fleet lanes

| Lane | Owner | Deliverable |
|---|---|---|
| **PR-1 (today): skeleton** | Integrator | login → `/sync` → live room list renders. Proves transport + state + UI pipeline end-to-end. |
| PR-2: message stream | Integrator | timeline render, sender grouping, day dividers, m.text/m.notice |
| PR-3: composer + send | Integrator | enter-to-send, optimistic local echo |
| PR-4: inline images | Integrator + Snake (eyes) | ImageEvent + lightbox + upload path |
| PR-5: `/gen` + pending chips | Integrator (client) + StarForge (bridge) | the contract in §4.3 live end-to-end |
| PR-6: member panel + presence | Integrator | joined_members + presence render |
| PR-7: deploy | DJBOOTH or Integrator | slick.slopclaude.com ingress + DNS + `Slick-Static` task |
| Server config | DJBOOTH | max_request_size audit/bump |
| Visual polish pass | Snake | full-res eyes-on the gestalt: "is it SLACK?" |
| Conduwuit migration | parallel track, unchanged | unblocks @oglaptop's writes; independent of Slick |

## 8. Risks

- **Conduit CORS or auth-media quirks** — Matrix spec mandates permissive CORS and Conduit complies, but the authenticated-media download endpoint on 0.10.x needs verification early in PR-4; fallback is the legacy unauthenticated `/media/v3/download`.
- **Sync long-poll through Cloudflare** — the tunnel has supported the daemons' 30s long-polls for weeks; Slick uses the same pattern (timeout=30000) to stay under Cloudflare's ~100s proxy timeout.
- **Scope creep toward Element parity** — v1 ships the checklist in §6 and nothing else. New asks go to v2.

## 9. Glossary

| Term | Meaning |
|---|---|
| **Slick** | This client. OURS. |
| **mxc://** | Matrix media content URI; resolves via the homeserver download endpoint |
| **`com.fleet.gen.request`** | Custom event type carrying a ComfyUI job request |
| **Bridge** | StarForge's on-LAN watcher that fulfills gen requests on the 5090 |
