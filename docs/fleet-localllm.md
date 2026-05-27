# Fleet — Local LLM via LM Studio (optional cost lever)

**Purpose:** route Claude Code at a *local* coding model (e.g. **Qwen3.6-Coder-27B** on **LM Studio**)
for cheap, high-volume grunt-work — keeping the Anthropic subscription for interactive +
gnarly agentic work. This is an **optional cost optimization, NOT a required migration.**

> **Billing context (June 15, 2026):** only **programmatic** usage (`claude -p`, the Agent SDK,
> Claude Code GitHub Actions) moves off the subscription to a separate, *claimable* monthly
> Agent SDK credit. **Interactive Claude Code stays on the subscription.** So a local model is
> for trimming *bulk/automation* spend (or just experimenting) — your day-to-day interactive
> sessions don't need it. Sources: support.claude.com articles 15036540 + 11145838.

---

## TL;DR architecture — one host, served fleet-wide

A 27B model is too big for most of the fleet's GPUs. So **run ONE LM Studio server on the
14900K (RTX 5090, 32 GB)** — the only box that comfortably fits a 27B — and **serve it to the
whole fleet over the LAN** (the new Wyyerd fiber makes this snappy). Workers point Claude Code
at it; no per-box model download.

| Box | Qwen-Coder-27B (~16–18 GB @ Q4 + ctx)? |
|---|---|
| **14900K / RTX 5090 32 GB** | 🟢 ideal (Q5/Q6, big context) — **host here** |
| i5000 / 2× 980 Ti (12 GB split) | 🟡 marginal — won't fully fit, CPU offload → slow |
| DJBOOTH, 13700K / 1080 Ti 11 GB | 🔴 too small for 27B |

---

## 1. LM Studio (host = 14900K)

1. Load `Qwen3.6-Coder-27B` (`Q4_K_M` or `Q5_K_M`), **max GPU offload**, context ≥ 32k.
2. **Developer → Start Server**; toggle **"Serve on Local Network"** (binds `0.0.0.0`), port `1234`. Note the host's LAN IP.
3. Confirm the model's chat template supports **tool / function calling** — Claude Code's Edit/Bash tools depend on it; without it the agent loop stalls.
4. Endpoint (OpenAI-compatible): `http://<14900k-lan-ip>:1234/v1`

---

## 2. Router — translate Anthropic ↔ OpenAI

Claude Code speaks the Anthropic Messages API; LM Studio speaks OpenAI. A proxy bridges them.

### Option A — `claude-code-router` (purpose-built)
```bash
npm i -g @musistudio/claude-code-router      # verify current pkg name in its README
```
`~/.claude-code-router/config.json` *(field names track the tool's README — they shift between versions):*
```json
{
  "Providers": [
    {
      "name": "lmstudio",
      "api_base_url": "http://<14900k-lan-ip>:1234/v1/chat/completions",
      "api_key": "lm-studio",
      "models": ["qwen3.6-coder-27b"]
    }
  ],
  "Router": { "default": "lmstudio,qwen3.6-coder-27b" }
}
```
Launch (the wrapper sets the env for you): `ccr code`

### Option B — LiteLLM (more battle-tested)
```yaml
# config.yaml
model_list:
  - model_name: qwen-coder
    litellm_params:
      model: openai/qwen3.6-coder-27b
      api_base: http://<14900k-lan-ip>:1234/v1
      api_key: lm-studio
```
```bash
litellm --config config.yaml --port 4000     # exposes an Anthropic-format /v1/messages
```

---

## 3. Point Claude Code at it (any worker)

```powershell
$env:ANTHROPIC_BASE_URL   = "http://localhost:3456"   # ccr's local proxy (or your LiteLLM port)
$env:ANTHROPIC_AUTH_TOKEN = "local"                    # dummy — the local server ignores it
claude
```
(With `ccr code` the wrapper exports these automatically.)

---

## Caveats — read before trusting it on hard work

- **Agentic reliability < Claude.** A 27B handles single edits / bulk refactors well, but gets
  flakier on long multi-step tool chains (the `act2_desert`-build / engine-debug / full-gate
  kind of work). Recommended split: **local model for grunt-work, Claude for the gnarly agentic
  loops + gate runs.**
- **Tool-calling is make-or-break.** If the model/template doesn't emit clean function calls,
  Claude Code's tools simply don't fire.
- **Version-specific bits** — exact `claude-code-router` keys and Qwen3.6-Coder's specs are
  fast-moving; trust the tool README + the model card over this note.

---

*Drafted by i5000 (desert lane). Workers contribute fleet docs via `docs/fleet-<topic>` branches
per `FLEET.md`; a primary merges. 14900K — you're the natural host (the 5090); ping if you want
me to help wire/test a worker against it.*
