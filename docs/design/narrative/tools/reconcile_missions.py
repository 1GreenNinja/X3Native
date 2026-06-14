#!/usr/bin/env python3
"""Reconcile invented-format Act-2 missions to the REAL x3.mission/1 schema.

The REAL schema (app/mission.cpp + app/story_ops.cpp) is the source of truth:
  doc:   format,id,title,start,stages[]   (other top-level keys are ignored by loader)
  stage: id,objective,on_enter,advance_when,fail_when,on_complete,on_fail,
         next,fail_to,branch{if,then,else},no_fasttravel
  ops:   ONLY the story_ops vocabulary. No counter/counter_lt/counter_set/counter_add.

Transforms applied (see ACT2_CONTENT_INDEX reconciliation note):
  objectives[]            -> stages[]
  obj.text                -> stage.objective
  obj.complete_when       -> stage.advance_when
  obj.fx                  -> stage.on_complete
  obj.on_enter            -> stage.on_enter   (unchanged)
  obj.fail{when,fx,next}  -> stage.fail_when/on_fail/fail_to
  obj.branch[ ... ]       -> real single branch{if,then,else} (+ synth router stages
                             for multi-entry; per-edge fx pushed to target on_enter)
  obj.if/else (skip-gate) -> synth router stage that branches into the gated stage
  counter ops             -> milestone flags (kill.<t>.<n> for kills.*, <id>.<n> else)
  top-level act/level/location/summary/give/_hooks/_voice -> preserved (loader ignores)
"""
import json, sys, os, collections

MISS_DIR = os.path.join(os.path.dirname(__file__), "..", "missions")
OUT_DIR  = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "missions")

remap_log = []

def remap_counter(cond, mid):
    """counter ops -> real flag milestones. Returns a real cond dict."""
    if "counter" in cond:
        cid, n = cond["counter"]
        if cid.startswith("kills."):
            t = cid[len("kills."):]
            flag = f"kill.{t}.{int(n)}"      # bridge emits kill.<type>.<n>
        else:
            flag = f"{cid}.{int(n)}"          # host-set milestone flag (same convention)
        remap_log.append(f"{mid}: counter {cid}>={n} -> flag {flag}")
        return {"flag": flag}
    if "counter_lt" in cond:
        cid, n = cond["counter_lt"]
        flag = (f"kill.{cid[len('kills.'):]}.{int(n)}" if cid.startswith("kills.")
                else f"{cid}.{int(n)}")
        remap_log.append(f"{mid}: counter_lt {cid}<{n} -> not flag {flag}")
        return {"not": {"flag": flag}}
    return cond

def remap_cond(cond, mid):
    if any(k in cond for k in ("counter", "counter_lt")):
        return remap_counter(cond, mid)
    if "any" in cond:
        return {"any": [remap_cond(c, mid) for c in cond["any"]]}
    if "not" in cond:
        return {"not": remap_cond(cond["not"], mid)}
    return cond

def remap_fx(fx, mid):
    # counter_set/counter_add are not real ops; none present in data, but guard.
    if "counter_set" in fx or "counter_add" in fx:
        remap_log.append(f"{mid}: DROPPED unsupported fx {list(fx)[0]}")
        return None
    return fx

def remap_list(lst, mid, is_cond):
    out = []
    for e in (lst or []):
        r = remap_cond(e, mid) if is_cond else remap_fx(e, mid)
        if r is not None:
            out.append(r)
    return out

def reconcile(doc):
    mid = doc["id"]
    objs = doc["objectives"]
    by_id = {o["id"]: o for o in objs}
    synth = []          # newly created router stages
    redirect = {}       # old target id -> router id (for if/else skip gates)
    counter = [0]

    def new_id(base):
        counter[0] += 1
        return f"{base}__r{counter[0]}"

    # ---- Pass 1: if/else skip-gates become router stages -------------------
    # An objective with `if`/`else` is entered conditionally: if `if` passes ->
    # run it, else jump to `else`. Real format: a router stage that branches
    # {if: C, then: <obj>, else: <else>}. Everything that pointed at <obj> is
    # redirected to the router so the gate is always honored.
    for o in list(objs):
        if "if" in o:
            cond = remap_list(o.get("if"), mid, True)
            elseto = o.get("else") or o.get("next") or "end"
            rid = new_id(o["id"] + "_gate")
            synth.append({
                "id": rid,
                "advance_when": [],                 # decide immediately
                "branch": {"if": cond, "then": o["id"], "else": elseto},
            })
            redirect[o["id"]] = rid
            o.pop("if", None); o.pop("else", None)

    def resolve(target):
        if not target or target == "end":
            return "end"
        return redirect.get(target, target)

    def edge_then(o, b):
        """Resolve a branch edge's `then` target. Real branch{if,then,else}
        carries NO per-edge fx, so an edge that has fx gets a thin synthesized
        fx-only stage (on_enter == the edge fx, then -> the real target). The
        fx therefore fires ONLY when that edge is taken, never on other paths
        into a shared target — faithful to the invented per-edge semantics."""
        target = resolve(b.get("next", "end"))
        if not b.get("fx"):
            return target
        edge_fx = remap_list(b["fx"], mid, False)
        rid = new_id(o["id"] + "_bfx")
        synth.append({"id": rid, "on_enter": edge_fx,
                      "advance_when": [], "next": target})
        remap_log.append(
            f"{mid}: branch edge fx on {o['id']}->{b.get('next')} "
            f"-> synth fx stage {rid} (fires only on that edge)")
        return rid

    # ---- Pass 2: per-objective transform ----------------------------------
    stages = []
    for o in objs:
        st = {"id": o["id"]}
        if o.get("text"):
            st["objective"] = o["text"]
        on_enter = remap_list(o.get("on_enter"), mid, False)
        adv      = remap_list(o.get("complete_when"), mid, True)
        on_comp  = remap_list(o.get("fx"), mid, False)
        default_next = resolve(o.get("next", "end"))

        # branch array -> real single branch (+ router chain for multi-entry)
        branches = o.get("branch") or []
        if len(branches) == 0:
            final_next = default_next
            final_branch = None
        elif len(branches) == 1:
            # single branch: real branch{if,then,else}; edge fx (if any) routed
            # through a synth fx stage by edge_then().
            b = branches[0]
            final_branch = {
                "if":   remap_list(b.get("if"), mid, True),
                "then": edge_then(o, b),
                "else": default_next,
            }
            final_next = None
        else:
            # Build a router chain: stage -> r1 -> r2 ... each tests one branch
            # entry; on match jumps to that target (after firing edge fx via the
            # target's on_enter), else falls to the next router / default.
            chain_head = None
            prev_else_setter = None
            for idx, b in enumerate(branches):
                rid = new_id(o["id"] + "_b")
                btarget = edge_then(o, b)     # synth fx stage if the edge has fx
                rstage = {
                    "id": rid,
                    "advance_when": [],
                    "branch": {"if": remap_list(b.get("if"), mid, True),
                               "then": btarget, "else": None},  # else filled below
                }
                synth.append(rstage)
                if chain_head is None:
                    chain_head = rid
                if prev_else_setter is not None:
                    prev_else_setter["branch"]["else"] = rid
                prev_else_setter = rstage
            prev_else_setter["branch"]["else"] = default_next
            final_next = chain_head
            final_branch = None

        if on_enter:  st["on_enter"] = on_enter
        if adv:       st["advance_when"] = adv
        # fail block
        if o.get("fail"):
            fb = o["fail"]
            fw = remap_list(fb.get("when"), mid, True)
            if fw:
                st["fail_when"] = fw
            ff = remap_list(fb.get("fx"), mid, False)
            if ff:
                st["on_fail"] = ff
            # invented `"next":"retry"` == re-arm THIS objective. Real runner
            # re-enters a stage when fail_to == its own id (re-fires on_enter,
            # re-emits objective) — the faithful "retry" semantics.
            raw_ft = fb.get("next", "end")
            ft = o["id"] if raw_ft == "retry" else resolve(raw_ft)
            if raw_ft == "retry":
                remap_log.append(f"{mid}: fail next `retry` on {o['id']} -> fail_to self")
            if ft:
                st["fail_to"] = ft
        if on_comp:   st["on_complete"] = on_comp
        if final_branch is not None:
            st["branch"] = final_branch
        else:
            st["next"] = final_next
        stages.append(st)

    out = {"format": "x3.mission/1", "id": mid, "title": doc.get("title", "")}
    if doc.get("start"):
        out["start"] = doc["start"]
    # preserve design metadata (loader ignores unknown top-level keys)
    for k in ("act", "level", "location", "summary", "give", "_hooks", "_voice"):
        if k in doc:
            out[k] = doc[k]
    out["stages"] = stages + synth
    return out

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    files = sorted(f for f in os.listdir(MISS_DIR) if f.endswith(".json"))
    for fn in files:
        with open(os.path.join(MISS_DIR, fn), encoding="utf-8") as fh:
            doc = json.load(fh)
        out = reconcile(doc)
        opath = os.path.join(OUT_DIR, f"{out['id']}.mission.json")
        with open(opath, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2, ensure_ascii=False)
            fh.write("\n")
        print(f"wrote {os.path.basename(opath)}  ({len(out['stages'])} stages)")
    print("\n--- remaps ---")
    for r in remap_log:
        print(" ", r)

if __name__ == "__main__":
    main()
