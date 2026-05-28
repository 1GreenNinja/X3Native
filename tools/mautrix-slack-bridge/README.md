# mautrix-slack-bridge — Phase 3 setup notes

Bridges the fleet's Slack workspace into the Matrix homeserver, two-way. After Phase 2 is stable, the bridge lets Tim see Slack messages inside Element and vice versa — making the 30-day Slack soak a true coexistence instead of two parallel feeds.

**Status:** Phase 3, not implemented yet. This is sketch / pre-deploy notes.

## What mautrix-slack does

[mautrix-slack](https://github.com/mautrix/slack) is a maintained, mature bridge by the [mautrix](https://github.com/mautrix) project (same family as mautrix-telegram, mautrix-whatsapp, etc.). It connects to Slack as a custom Slack app + presents Slack channels as Matrix rooms (and vice versa). Both DMs and channels work bidirectionally.

| Slack side | ↔ | Matrix side |
|---|---|---|
| `#fleet-ops` | ↔ | `#fleet-ops-slack:tims-fleet.xyz` (bridged ghost room) |
| Tim's `@djbooth` DM | ↔ | `@djbooth-slack:tims-fleet.xyz` ghost user DM |
| Slack user posts | → | Mirrored as ghost-user posts on Matrix side |
| Matrix user posts in bridged room | → | Mirrored to Slack |
| Reactions / threads / edits | ↔ | Most are bridged faithfully |

The bridge runs as a **separate process** alongside Conduit. It's a Go binary that talks Matrix Application Service API to Conduit and Slack Web/Events API to Slack.

## Why we want it (Phase 3 only)

Phase 1 + 2 has us running:
- The Slack daemon writing messages to `~/.claude/.slack_inbox.jsonl` + the matrix mirror block writing same messages to `~/.claude/.matrix_inbox.jsonl`
- The matrix-daemon writing real Matrix events to `~/.claude/.matrix_inbox.jsonl`
- The /loop draining the unified inbox

That's a **passive** coexistence — messages flow into Matrix from both sides, but Matrix → Slack is one-way (Matrix-side replies don't appear on Slack).

The bridge upgrades this to **active two-way**: a reply Tim sends from Element on his phone appears in Slack in real time, so other humans using Slack (if any) see it without having to migrate to Matrix.

This matters most if:
- Tim has other humans on Slack who can't move to Matrix
- Tim is on a trip and his phone only has the Slack app installed
- The mautrix bridge is more reliable than the Anthropic-hosted MCP path (which failed our auth earlier)

For a small private fleet where Tim is the only human user, the bridge is **optional** — Phase 1+2 covers the use case. The bridge is the answer for "what if I want to keep Slack accessible long-term?"

## Architecture

```
                   ┌─────────────────────────────────────┐
                   │  13700K Command Center (Windows)    │
                   │                                     │
                   │  ┌─────────────┐  ┌──────────────┐  │
                   │  │   Conduit   │←─┤ mautrix-slack│  │
                   │  │   :6167     │  │  bridge      │  │
                   │  │             │  │  :29335      │  │
                   │  └─────────────┘  └──────┬───────┘  │
                   │         ▲                │          │
                   │         │                │          │
                   │  ┌──────┴────────────────▼───────┐  │
                   │  │     cloudflared tunnel       │  │
                   │  └──────┬───────────────────────┘  │
                   │         │                          │
                   └─────────┼──────────────────────────┘
                             │
                             ▼ https
                        ┌──────────────────────────┐
                        │ chat.<CHOSEN_DOMAIN>     │
                        │   ↑                       │
                        │   Element (web/iOS)       │
                        └──────────────────────────┘
                             │
                             ▼ outbound API calls
                        ┌──────────────────────────┐
                        │ slack.com                 │
                        └──────────────────────────┘
```

## Setup outline (estimated 1-2 hour effort)

### Prerequisites

- Conduit deployed and stable (Phase 1 complete)
- Tim's admin Matrix account exists
- Slack app already created (DJBOOTH-Bot we built in 2026-05-26 session)
- Bot token, app token, signing secret, client ID, client secret available

### Step 1: Install mautrix-slack

The bridge is a Go binary. Two paths:
- **Docker:** `docker run --restart unless-stopped dock.mau.dev/mautrix/slack:latest` (recommended)
- **Direct binary:** download from [GitLab releases](https://mau.dev/mautrix/slack/-/releases)

For DJBOOTH/13700K's Windows stack, Docker is simpler since we already need it for Conduit. Add another service to `tools/conduit-prep/docker-compose.yml`:

```yaml
  mautrix-slack:
    image: dock.mau.dev/mautrix/slack:latest
    container_name: mautrix-slack
    restart: unless-stopped
    depends_on:
      - conduit
    volumes:
      - C:\opt\mautrix-slack:/data
    ports:
      - "127.0.0.1:29335:29335"
```

### Step 2: Generate the bridge config

```bash
docker exec mautrix-slack /usr/bin/mautrix-slack -e
# Creates /data/config.yaml with defaults; edit:
```

Critical fields:
```yaml
homeserver:
  address: http://conduit:6167
  domain: <CHOSEN_DOMAIN>

appservice:
  address: http://mautrix-slack:29335
  hostname: 0.0.0.0
  port: 29335
  database:
    type: sqlite3-fk-wal
    uri: file:/data/mautrix-slack.db
  as_token: <generated>     # generate with `openssl rand -hex 32`
  hs_token: <generated>     # ditto

bridge:
  username_template: "slack_{userid}"
  command_prefix: "!slack"
  permissions:
    "@tim:<CHOSEN_DOMAIN>": admin
```

### Step 3: Register the appservice with Conduit

The bridge generates a registration YAML; Conduit needs to load it. Copy it into Conduit's appservices dir or register via Conduit's admin command:

```
!admin register-appservice <paste registration yaml>
```

(Conduit's appservice support varies by version — verify in v0.10.12 docs.)

### Step 4: Connect Slack workspace

In Element, DM `@slackbot:<CHOSEN_DOMAIN>` (the bridge's control bot):

```
!slack login-token <slack-bot-token-xoxb-...> <slack-app-token-xapp-...>
```

The bridge logs into Slack as the bot, starts mirroring.

### Step 5: Map channels

```
!slack bridge channel <slack-channel-id> --room <matrix-room-id>
```

Map `#fleet-ops` (`C0B76SQ0XQ8`) ↔ Matrix `#fleet-ops` room. Repeat for other channels Tim wants bridged.

### Step 6: Test

- Send message from Element in the bridged room → appears in Slack `#fleet-ops` within ~1-2 seconds
- Send message from Slack → appears in Matrix room within ~1-2 seconds
- Reactions and edits should round-trip
- Threads work but Slack's threading model maps imperfectly to Matrix's — minor cosmetic quirks expected

## Bridging limitations to expect

| Feature | Bridge behavior |
|---|---|
| Text messages | ✓ Bidirectional |
| Reactions | ✓ Bidirectional |
| Edits | ✓ Bidirectional |
| Threads | ⚠ Slack threads work; Matrix threads bridge inconsistently to Slack |
| File uploads | ✓ Files copy through (eats Slack's upload quota) |
| Slack mentions `<@U…>` | ✓ Bridged as Matrix mentions |
| Slack emoji | ⚠ Custom Slack emoji may not have Matrix equivalents |
| Slack channels with > 1000 members | ⚠ Bridge backfills slowly; not relevant for our fleet (<10 users) |
| DMs from Tim to `@djbooth` | ✓ Bridged ↔ Element DM to `@djbooth:<CHOSEN_DOMAIN>` |
| Slack workspace deletion | ↔ Bridge drops; restart with new token |

## Cutover plan

Once the bridge is running stable for 7 days:
- Tim disables Slack push notifications on his phone (Element push handles it)
- Tim only opens Slack to verify the bridge mirrors — actual work happens in Element
- After 30 days (per Phase 3 §8 cutover), bridge can be retained for archive access OR shut down + Slack workspace archived

If anyone else later joins the fleet via Slack: bridge keeps them visible to Matrix users transparently. No mandatory migration.

## Files in this directory (future)

When this phase ships:

| File | Purpose |
|---|---|
| `docker-compose.fragment.yml` | Adds `mautrix-slack` to the existing compose |
| `config.yaml.template` | Bridge config with placeholders for tokens + domain |
| `setup.md` | Step-by-step deploy notes (this README will expand) |
| `cutover-checklist.md` | The 7-day-soak + 30-day-cutover checklist with verification steps |

## Reference

- Project: [github.com/mautrix/slack](https://github.com/mautrix/slack) + GitLab mirror at [mau.dev/mautrix/slack](https://mau.dev/mautrix/slack)
- Docs: [docs.mau.fi/bridges/go/slack/](https://docs.mau.fi/bridges/go/slack/)
- Maintainer: tulir (very responsive on the #slack:maunium.net Matrix room for support questions)
