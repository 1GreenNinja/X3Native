# fleet/ — Claude Code ↔ Matrix bridge helpers

Two tiny Python scripts that connect each fleet box's Claude Code session to
the self-hosted Matrix homeserver at `fleetcommand.slopclaude.com`. Used in
conjunction with `tools/matrix-daemon/` (DJBOOTH's matrix-bot-sdk daemon).

## Components

- **`fleet_inbox.py`** — reads `~/.claude/.matrix_inbox.jsonl`, prints unread
  fleet messages as concise markdown, advances a per-Claude "seen" marker so
  the next hook fire only shows what's new. Wired into Claude Code hooks
  (`SessionStart` and `UserPromptSubmit`) so every Claude session on this box
  auto-surfaces fleet activity.

- **`fleet_send.py`** — posts a message to a Matrix room via this machine's
  bot token (`~/.claude/.matrix_token`). Accepts either a raw `!room_id`
  or a `#alias:server`. Side-effect: caches the room's friendly name in
  `~/.claude/.matrix_room_names.json` so `fleet_inbox.py` can show "Fleet Ops"
  instead of a bare hash.

- **`fleet_image.py`** — posts an **image** (render, screenshot, build
  artifact) to a room: uploads to the media repo, then sends an `m.image`
  event with an optional text caption. Stdlib-only (no Pillow — image
  dimensions are parsed from PNG/JPEG/GIF/BMP headers). Usage:
  `python fleet_image.py <room_id_or_#alias> <image_path> ["caption"]`.

  **Multi-bot per host:** `fleet_image.py` honours the env override
  `FLEET_TOKEN_FILE` (absolute path, or a bare name under `~/.claude`) so
  two Claude sessions sharing one Windows user can post under different
  identities — e.g. `FLEET_TOKEN_FILE=.matrix_token_snake` posts as `@snake`
  instead of the host's default bot. (`fleet_send.py` can adopt the same
  pattern for text.)

## Per-machine setup

Already done on `13700K` as part of FleetCommand Phase 1. For any other
fleet box (DJBOOTH, 14900K, i5000, i4400/Predator, Snake) the install is:

1. Bot registered (`@<machine>:fleetcommand.slopclaude.com`) and access token
   at `~/.claude/.matrix_token` (mode 600).
2. `matrix-daemon` running as a Scheduled Task (see `tools/matrix-daemon/`).
3. Hooks wired into `~/.claude/settings.json` like:

   ```json
   {
     "hooks": {
       "SessionStart": [
         {
           "matcher": "*",
           "hooks": [
             { "type": "command", "command": "python G:/X3Native/tools/fleet/fleet_inbox.py" }
           ]
         }
       ],
       "UserPromptSubmit": [
         {
           "matcher": "*",
           "hooks": [
             { "type": "command", "command": "python G:/X3Native/tools/fleet/fleet_inbox.py" }
           ]
         }
       ]
     }
   }
   ```

   (Path is `G:\X3Native\...` on this 13700K; replace with the local repo
   path on other fleet machines.)

## How a "live" exchange looks

1. Tim types a message in Element Web (`@tim:fleetcommand.slopclaude.com`).
2. The matrix-daemon on this box sees the room event via its sync, appends to
   `~/.claude/.matrix_inbox.jsonl`.
3. Next time Tim submits a prompt to Claude Code, the `UserPromptSubmit` hook
   fires `fleet_inbox.py`, which surfaces the message as session context.
4. Claude reads it, decides whether/how to respond.
5. Claude runs `fleet_send.py "<room>" "<reply>"` from a Bash tool call; the
   reply lands in Matrix; Tim sees it in Element.

Two-way "live chat whenever" — without needing the daemon's named-pipe
outbox path. The pipe stays available for future structured RPC.

## Why two seen-markers?

The daemon writes its own `~/.claude/.matrix_seen.json` to track which Matrix
events it has already delivered to the inbox (so it doesn't double-append on
reconnect). This script keeps a separate `~/.claude/.matrix_inbox_claude_seen.txt`
that tracks what Claude has surfaced to the user. They serve different layers:
daemon dedupe vs UX dedupe.
