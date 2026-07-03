# Public FleetCommand — going from a private fleet chat server to a public Matrix service

**Status:** Decision-grade scoping doc — read in 10 minutes, act on it.
**Date:** 2026-07-03
**Author:** Integrator (13700K)
**Audience:** Tim (solo-ish founder, cost-conscious self-hoster)

---

## TL;DR — the headline

- **Conduit cannot go Postgres.** Conduit (and its whole family) only does SQLite/RocksDB. So the question you actually asked ("move the DB to Postgres so we don't convert later") has no answer inside Conduit — it forces a **homeserver** decision, not a DB-config change.
- **"conduwuit" is dead as a name.** girlbossceo archived conduwuit on **2025-04-11**. The living successor is **continuwuity** (community fork, ~v0.5.x, v0.6.0 in dev, RocksDB, actively releasing security fixes in 2026). When this doc says "the light Rust option," it means **continuwuity**, not conduwuit.
- **Recommendation:** **Do NOT migrate now. Do NOT switch to Postgres/Synapse yet.** Your instinct to "convert before we have users" is half-right (empty is the cheap time to move) but points at the wrong target. The right move is a **staged path**: harden the current Conduit box first, migrate **Conduit → continuwuity** (trivial, same DB family, keeps your single-binary simplicity) for the limited-invite beta, and only reach for **Synapse + PostgreSQL** *if and when* you actually open federated public registration at real scale. Most likely you never need Synapse.
- **The one decision you must make first (everything hangs on it):** **Federated, or a closed island?** If FleetCommand federates into the global Matrix network, you inherit spam, moderation duty, and legal exposure from day one, and Synapse's ecosystem starts to matter. If it stays a **closed island** (your own users, your own Slick client, no federation), continuwuity comfortably runs it for years and the whole heavy path evaporates. **Pick this before anything else.**

---

## 1. Define "public" — scale + threat model (pick a tier)

"Public" is doing a lot of work in this conversation. It spans three very different systems. Force the choice:

| | **Tier A — Open Island** | **Tier B — Public Federated Small** | **Tier C — Public Federated At Scale** |
|---|---|---|---|
| Who signs up | Anyone, but **only your server**, no federation | Anyone; federates with matrix.org et al. | Anyone; a "real" public homeserver |
| Users | 50–500 | 500–5k | 5k–50k+ |
| Federation | **OFF** | ON | ON |
| Threat model | Bot signups, local trolls | + Federated spam waves, abuse reports about *your* users, DMCA | + Sustained attack surface, T&S caseload, CSAM reporting duty |
| Client | Slick (your face) + optional Element | Slick + Element | Slick + Element |
| Realistic stack | **continuwuity** | **continuwuity** (viable) or Synapse | **Synapse + Postgres + workers** |
| Ops burden | Low | Medium | High (you're now a service operator) |

**Reality check for your constraints:** you are one person with two collaborators, a home power bill that already stung you $317, and a stated love of the single-binary stack. **Tier A (Open Island) is the honest fit.** It gives you a "public" product — strangers can make accounts and use Slick — without signing you up to police the global Matrix network. Tier B is a deliberate, later escalation. Tier C is a different company; don't sleepwalk into it.

**Recommendation:** Target **Tier A first.** Treat federation as a switch you flip *later, on purpose*, not a default.

---

## 2. Homeserver decision: Synapse+Postgres vs continuwuity+RocksDB vs stay-Conduit

The three real options, honestly:

| | **Stay Conduit 0.10.x** | **continuwuity + RocksDB** | **Synapse + PostgreSQL** |
|---|---|---|---|
| Language / shape | Rust, single binary | Rust, single binary | Python + Postgres (+ Redis + workers at scale) |
| Maintenance status | **Slow/stalled upstream** | **Active** (2026 releases, security fixes) | Reference impl, Element-backed, very active |
| DB | SQLite / RocksDB | **RocksDB** (SQLite legacy, don't use for public) | **PostgreSQL required** for any serious load |
| Ops burden | Very low | **Very low** | **High** — Python deps, Postgres tuning, workers, MAS |
| Idle power draw | Tiny | Tiny | Meaningful (Python process + Postgres always resident) |
| Scale ceiling (single node) | Small | **Small→mid** (hundreds, low thousands of local users on a closed island) | **Effectively unbounded** with workers + Postgres |
| Federation maturity | Basic | Good, improving; presence/typing sane defaults | Gold standard |
| Moderation tooling | Minimal | Improving (policy servers, works with Draupnir/Meowlnir bots) | **Best** (native admin API, Draupnir, Synapse admin, room/user controls) |
| Admin/registration controls | Basic | Registration tokens, admin room | reCAPTCHA, email verify, registration tokens, rich rate limits, MAS |
| Migrate cost from today | n/a | **Low** (same Conduit DB lineage) | **High** (full export/import, new auth stack) |
| Fit to *your* values | You've outgrown it (stalled) | **Best fit** | Overkill unless Tier C |

**The honest read on continuwuity for public use:**
- **What it's genuinely good for:** a Tier A open island. Single binary, RocksDB, low power, active security maintenance, real public servers exist (e.g., transfem.dev, listed on servers.joinmatrix.org). It is a credible daily-driver homeserver for small-to-mid deployments. This is exactly your shape.
- **Where the real gaps are — do not cheerlead past these:**
  1. **Moderation depth.** It has no rich native admin/T&S console like Synapse. For a *federated* public server fighting spam waves, you'll lean on external bots (Draupnir/Meowlnir) and policy servers. Workable, but more assembly and less mature than Synapse's ecosystem. On a **closed island** this barely matters — you control who's on it.
  2. **Horizontal scale.** RocksDB is a single-node embedded store. No workers, no read replicas, no sharding. Backup = stop-and-copy or snapshot the data dir (no `pg_dump` streaming, no PITR). Fine to a point; a hard ceiling if you ever hit Tier C.
  3. **Beta-ish provenance.** It's a 2nd-degree community fork on a 0.x version line. Active and stable in practice, but you are trusting a community project's release cadence and security response. (They *do* ship coordinated security releases — subscribe to their feed and patch promptly.)
- **Where Synapse wins and it's real:** federated public scale, T&S caseload, native moderation/admin, the MAS auth future (see §5). If you ever go Tier C, Synapse is the answer and it's not close. But it costs you Python + Postgres + likely Redis/workers + MAS — the exact heavy, always-on, power-drawing ops stack you dislike.

**Recommendation:** **continuwuity + RocksDB.** It is the correct successor to your current Conduit box, keeps the single-binary simplicity and low power you value, and covers Tier A and even Tier B. Reserve **Synapse + Postgres** as the *escalation* you adopt only if you commit to Tier C federated-at-scale. **Do not adopt Synapse pre-emptively "to avoid converting later"** — you'd be paying the full heavy-ops tax now for a scale you may never reach.

---

## 3. Migrate now (while empty) vs later

Your instinct — "convert before we have users so we don't convert later" — is a **good instinct pointed at the wrong conversion.**

- **The Postgres conversion you asked about doesn't exist** inside Conduit. There's nothing to do now on that front except *change homeserver*, which is the real decision.
- **The conversion that IS cheap-while-empty: Conduit → continuwuity.** Same DB lineage (RocksDB), same config shape, same single binary, same Cloudflare Tunnel in front. Doing it now (a dozen accounts, no strangers) is near-zero risk. Doing it after you have hundreds of strangers' message histories is a real migration with downtime and data-integrity stakes.
- **The conversion that is NOT cheap and you should NOT do speculatively: → Synapse.** It's a one-way, full export/import, new auth stack (MAS), new database engine. You only pay that cost when Tier C forces it — and by then you'll have concrete load numbers to justify it.

**Timing recommendation:**
- **Now (empty):** migrate **Conduit → continuwuity**. Cheap, reversible-ish, buys active maintenance + better admin controls immediately.
- **Later (only if Tier C):** continuwuity → Synapse. Accept it as a real project when the numbers demand it, not before.

---

## 4. Federation — on or off (this is THE decision)

Federation is the single switch that changes the size of the problem.

**Federation OFF (closed island, your current posture):**
- Your users talk only to each other, through your server, via Slick.
- **Spam surface = local signups only** (bots making accounts). Bounded, CAPTCHA/registration-tokens handle it.
- **No inbound abuse** from the global network. **No "your user harassed someone on matrix.org" reports.** **No federated CSAM/DMCA flowing through your box.**
- Trade-off: your users can't reach the wider Matrix world. For a **product with its own identity (FleetCommand/Slick)**, that's often *fine* — arguably desirable (it's your walled garden, your brand, your moderation).

**Federation ON:**
- You join a network of thousands of servers. You inherit **federated spam waves**, must run moderation bots (Draupnir/Meowlnir) and/or subscribe to policy/blocklist servers, and you become a **data controller for content flowing through your server from strangers on other servers**.
- Moderation stops being optional. This is where Synapse's ecosystem earns its keep — and where continuwuity is workable-but-more-assembly.

**Recommendation:** **Federation OFF for launch.** It slashes your threat model, moderation duty, and legal exposure to the manageable "local signups" case, and it's the only posture consistent with your solo/low-ops/low-power constraints. Flip it on later only as an explicit Tier B decision, with moderation tooling stood up *first*.

---

## 5. Registration & abuse control

What each stack gives you for keeping bots and abusers out:

| Control | continuwuity | Synapse |
|---|---|---|
| Closed / invite-only | Yes | Yes |
| **Registration tokens** (invite codes) | **Yes** — your best Tier-A tool | Yes |
| CAPTCHA | Limited/none native | **reCAPTCHA v2** (needs Google keys) |
| Email verification | Limited | Yes (via SMTP; note: Synapse ≥1.56 refuses open registration with *no* verification) |
| Rate limits | Basic | Rich, per-endpoint |
| Modern auth (OIDC) | No MAS | **MAS** (Matrix Authentication Service) — the Matrix-wide direction; matrix.org migrated 110M users to it in 2025. Token rotation, SMTP-gated registration. Adds a *second service* to run. |
| Moderation bots | Draupnir/Meowlnir + policy servers | Draupnir/Mjolnir/Meowlnir + policy servers + native admin API |

**Practical Tier-A recipe (continuwuity, federation off):**
1. **Registration by token/invite** (you mint codes; Slick can gate on them). This alone stops ~all drive-by bot signups.
2. If you want truly open signup later: put **Cloudflare Turnstile in front of Slick's signup form** (you already terminate at a Cloudflare Tunnel — Turnstile is the natural CAPTCHA here, and cleaner than wiring Synapse's Google reCAPTCHA). This solves continuwuity's weak native CAPTCHA story at the edge.
3. **Cloudflare rate-limiting / WAF rules** on the signup + login endpoints at the tunnel.
4. Keep **one more human admin** than you have today (see §7 — one admin is a bus-factor and lockout risk).

**Note on MAS:** it's the strategic future of Matrix auth, but it's a Synapse-side, extra-service concern. For Tier A on continuwuity you don't need it. Don't let "MAS is the future" pull you into Synapse prematurely.

---

## 6. Media at scale

Local disk on the 13700K does not survive public use — it's unbounded growth, no CDN, and it ties media to one box's uptime and your power bill.

**The move: S3-compatible object storage, and specifically Cloudflare R2.**
- You already front everything with Cloudflare Tunnel. **R2 has $0 egress** — decisive for a media-serving chat app, where users repeatedly re-download images/video.
- **R2 pricing (2026):** **$0.015/GB-month** storage, Class A ops $4.50/M, Class B ops $0.36/M, **egress free**. So 100 GB of media ≈ **$1.50/month** + trivial op costs. 1 TB ≈ **$15/month**. That's noise compared to your power bill.
- Contrast AWS S3: same storage-ish but **$0.09/GB egress** — a media chat app bleeds money on S3 egress. R2 is the right call.
- Alternatives: **Backblaze B2** (cheap, egress free via Cloudflare Bandwidth Alliance), or **self-hosted MinIO** on a fleet box (no monthly bill, but back on your power/uptime — defeats the purpose).

**Homeserver support:** Synapse has mature S3 media support (media-repo/S3 storage provider). continuwuity/conduwuit media-to-S3 is **less turnkey** — verify the current continuwuity release's S3/media backend support before committing; you may need a media proxy or a specific config. **Flag: confirm continuwuity's 2026 S3 media story hands-on before you rely on it.** If it's not ready, a pragmatic bridge is: keep media on a dedicated data disk with a **retention policy** (auto-expire remote/old media after N days) to cap growth until you move to R2.

**Retention:** set a media retention policy regardless (e.g., purge unused/remote media after 30–90 days). It caps storage cost *and* limits how much strangers' data you're holding (see §8).

**Recommendation:** **Cloudflare R2** as the media target, with a retention policy. Verify continuwuity's S3 support level first; if immature, run local-disk-plus-retention as the bridge and revisit.

---

## 7. The single-box → real-infra gap

Today: one box (the 13700K, also your dev/Integrator machine), one admin, one SQLite file, tunnel-only ingress, no HA, backups unclear. That's perfect for a private fleet. It has **four specific failure modes** under public load:

1. **The box is also your dev machine.** Public users on your primary workstation means every reboot, GPU experiment, or crash is an outage — and the $317 power reality means you *sleep boxes*. **A public service cannot live on a machine you turn off.** → Move the homeserver to a **dedicated always-on target** (a low-power fleet box kept up, or a cheap VPS — see §9).
2. **One admin = lockout + bus factor.** Recovery is a single emergency_password. Add a **second admin account** and store recovery creds somewhere durable (not just in your head/one box).
3. **No real backups.** RocksDB backup = consistent snapshot of the data dir (stop-and-copy or filesystem snapshot) on a schedule, shipped **off the box** (to R2, another fleet box, anywhere). Today a disk failure = total loss of strangers' accounts and history. That's both a product and a **legal** problem (§8).
4. **No monitoring.** You won't notice the box is down/full/under attack until users complain. Add minimal uptime + disk + signup-rate alerting (even a cheap uptime pinger + a disk-space alert).

**HA (multi-node)?** **No — explicitly out of scope for Tier A/B.** RocksDB/continuwuity is single-node; real HA means Synapse+Postgres+workers. Don't chase it. "Good backups + fast restore + dedicated always-on host" is the right reliability posture for your scale, not clustering.

**Minimum viable change to go public:** (a) dedicated always-on host, (b) second admin, (c) off-box automated backups, (d) basic monitoring. Everything else is optional polish.

---

## 8. Legal / privacy (obligations, not legal advice)

The moment strangers' messages live on your box, you're a **data controller/processor**, not a hobbyist. This is not optional and it's not just an EU thing. Real obligations to plan for (get actual legal review before Tier B/C):

- **Terms of Service + Privacy Policy** on the Slick signup page. What you collect, why, how long, who can see it, how to delete an account. Use a generator as a *draft*, have it reviewed.
- **Data handling / retention:** don't keep more than you need. Your §6 media retention + a message/account deletion path directly serve this. "Right to deletion" requests are real if any EU users sign up (and the 2026 Digital Omnibus reform pushes privacy-by-design as a hard requirement).
- **Abuse / reporting path:** a way for people to report content and for you to act — plus a way to be contacted (abuse@ address). If federated, you *will* get reports.
- **DMCA-style takedowns:** hosting user uploads means takedown requests. Have a process and a contact.
- **The hard one — CSAM:** any open-registration, media-accepting service can be targeted for illegal-content upload. You need to be able to detect, remove, and (jurisdiction-dependent) report it. **This alone is a strong argument for Tier A closed-island + invite tokens + media retention**, which shrinks the exposure surface dramatically vs. open federated signup.
- **Jurisdiction:** you're hosting on home hardware in the US; know that where the box (or VPS) physically sits affects obligations, and hosting EU users pulls in GDPR regardless.

**Honest framing:** going federated-public is as much a **legal/moderation commitment** as a technical one. The staged plan (§10) is deliberately structured so you don't take that on until you've decided you want it. **Closed island + invite-only is not just easier ops — it's dramatically lower legal exposure.**

---

## 9. Cost model (rough monthly, realistic tiers)

Two axes: **where it runs** (home hardware vs VPS) and **scale tier**. Power numbers assume US ~$0.15–0.20/kWh (your bill suggests you're at the high end).

| Option | Hardware/host | Media | Rough monthly | Notes |
|---|---|---|---|---|
| **Home box, always-on, Tier A** | One low-power fleet box up 24/7 (say 40–70 W idle-ish) | R2 (~100 GB) | **~$5–12 power + $1.50 R2 ≈ $7–14** | *If* it's a low-power box, not the 13700K. Your dev rig 24/7 is far more. |
| **Cheap VPS, Tier A (continuwuity)** | Hetzner CX22/CX23 (2 vCPU / 4 GB / NVMe) | R2 | **~€4–6 (~$5–7) + $1.50 R2 ≈ $7–9** | **Cheaper than running a home box 24/7, and it's off your power bill and off your dev machine.** 20 TB traffic included. |
| **VPS, Tier B federated (continuwuity)** | Hetzner CX32/CX42 (4 vCPU / 8–16 GB) | R2 (~500 GB) | **~$12–25 + ~$8 R2 ≈ $20–33** | Headroom for federation traffic + moderation bots. |
| **VPS, Tier C (Synapse+Postgres+workers)** | 4+ vCPU / 8–16+ GB, possibly split DB | R2 (1 TB+) | **~$25–60+ + $15+ R2 ≈ $40–75+** | Now you're paying for the heavy stack *and* your time as an operator. |

**The standout finding:** for Tier A/B, a **Hetzner VPS is cheaper than keeping a home box powered 24/7** — and it removes the service from your dev machine, removes it from your power bill, and gives you a proper always-on host with included bandwidth and DDoS protection. Given the $317-bill context and the WoL "sleep idle boxes" project, **moving FleetCommand off home hardware to a ~$6/mo Hetzner box is the single highest-leverage cost+ops move in this whole doc.** Your fleet stays for dev/rendering/GPU work (sleepable); the public service lives somewhere that's supposed to be always-on and cheap to keep that way.

(Caveat: Hetzner requires ID/business verification at signup — budget a day. And check current continuwuity ARM builds if you pick a cheaper CAX ARM instance.)

---

## 10. Staged recommendation

A concrete phased path that respects your constraints and never pays a cost before it's forced.

### Phase 0 — Harden private (do now, days, ~free)
Stay Conduit for the moment; fix the bus-factor and data-loss risks that are dangerous *even privately*:
- Add a **second admin** account; store recovery creds durably off-box.
- Stand up **automated off-box backups** of the DB (snapshot → another box or R2).
- Add **basic monitoring** (uptime + disk).
- **Decide the ONE thing:** *Federated, or closed island?* (§4). Recommendation: **closed island.**

### Phase 1 — Limited-invite beta (weeks, ~$7/mo)
- **Migrate Conduit → continuwuity** (cheap while empty; §3).
- **Move the homeserver to a dedicated always-on host — recommend a ~$6/mo Hetzner VPS** (off your dev box, off your power bill; §9). Keep the Cloudflare Tunnel + Slick as the public face.
- **Registration = invite tokens only.** Federation **OFF**.
- **Media → Cloudflare R2** with a retention policy *(verify continuwuity S3 support first; §6 bridge if not ready)*.
- Onboard a handful of real outside users behind invite codes. This IS "public" in the way that matters (strangers using your product) without any of Tier B/C's weight.
- Draft a **ToS + Privacy Policy** and an abuse-contact address (§8).

### Phase 2 — Open public island (when Phase 1 is stable)
- Open signup **with Cloudflare Turnstile + edge rate-limiting** on Slick's signup (§5). Still **federation OFF** (Tier A). Still continuwuity.
- Firm up moderation: an admin bot (Draupnir/Meowlnir), content reporting flow, retention enforcement.
- Legal review of ToS/Privacy; abuse process live.

### Phase 3 — Federate / scale (ONLY if you deliberately choose it)
- Turn federation **ON** *(Tier B)* only after moderation tooling + policy-server blocklists are in place.
- Reach for **Synapse + PostgreSQL (+ workers + MAS)** *(Tier C)* only when real load or a real product reason demands it — accepting the heavy always-on, higher-power, higher-ops stack as a conscious business decision, not a default.

### The single most important decision, restated
**Closed island or federated?** Everything downstream — stack, moderation burden, legal exposure, cost, whether you ever touch Synapse — is set by that one choice. This doc's recommendation: **closed island + continuwuity + a cheap always-on VPS + R2 media**, staged as above. Revisit federation as a deliberate future step, never as a launch default.

---

## Open questions flagged for Tim's judgment

1. **Federation — the load-bearing call.** I recommend OFF/closed-island hard, but if the *point* of FleetCommand is to reach the broader Matrix network (not just be your own branded chat), that changes everything and pulls Synapse forward. Only you know the product intent. **Decide this first.**
2. **continuwuity S3/media maturity in mid-2026.** I could not fully confirm hands-on how turnkey continuwuity's S3 media backend is right now. **Verify before relying on R2 media** on continuwuity; §6 gives a local-disk-plus-retention bridge if it's not ready.
3. **Home-box vs VPS for the always-on host.** I recommend a ~$6/mo Hetzner VPS on cost+power+isolation grounds. If you have a genuinely low-power fleet box you're happy to keep up 24/7 anyway (and you value data staying physically on your hardware for privacy reasons — a legit §8 consideration), a home box is defensible. It's a values call between "cheapest/simplest" and "my hardware, my data."
4. **Trusting a 0.x community fork for strangers' data.** continuwuity is active and credible, but it's a community project on a 0.x line. Comfortable for Tier A; if you're risk-averse about others' data on a fork, that's a (weak) point toward Synapse. Subscribe to its security feed and patch promptly regardless.
