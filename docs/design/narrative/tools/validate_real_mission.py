#!/usr/bin/env python3
"""Validate mission docs against the REAL parser contract.

Mirrors app/mission.cpp (loadMissionFromJson/parseStage/validateMission) and
app/story_ops.cpp (parseStoryCond/parseStoryFx). Closed contract: any op key not
listed here is a loader error in the engine.
"""
import json, sys, os

COND_AXIS = {"karma_gte","karma_lte","humanity_gte","trust_gte","mercy_gte",
             "love_gte","redemption_gte"}
FX_AXIS   = {"karma","humanity","trust","mercy","love","redemption"}

def err(errors, msg): errors.append(msg)

def check_cond(c, errors, where):
    if not isinstance(c, dict) or not c:
        err(errors, f"{where}: condition is not an object"); return
    key = next(iter(c)); val = c[key]
    if key == "flag":
        if not (isinstance(val,str) and val): err(errors, f"{where}: empty flag")
    elif key in COND_AXIS:
        if not isinstance(val,(int,float)): err(errors, f"{where}: {key} wants number")
    elif key == "timeline":
        if not isinstance(val,list): err(errors, f"{where}: timeline wants array")
    elif key in ("girl_saved","girl_lost","item","lua"):
        if not (isinstance(val,str) and val): err(errors, f"{where}: {key} empty")
    elif key == "rel_gte":
        if not (isinstance(val,list) and len(val)==2 and isinstance(val[0],str)
                and isinstance(val[1],(int,float))):
            err(errors, f"{where}: rel_gte wants [npc,n]")
    elif key == "chance":
        if not isinstance(val,(int,float)): err(errors, f"{where}: chance wants number")
    elif key == "any":
        if not isinstance(val,list): err(errors, f"{where}: any wants array")
        else:
            for i,s in enumerate(val): check_cond(s, errors, f"{where}.any[{i}]")
    elif key == "not":
        check_cond(val, errors, f"{where}.not")
    else:
        err(errors, f"{where}: unrecognized condition kind `{key}`")

def check_fx(f, errors, where):
    if not isinstance(f, dict) or not f:
        err(errors, f"{where}: effect is not an object"); return
    key = next(iter(f)); val = f[key]
    if key in FX_AXIS:
        if not isinstance(val,(int,float)): err(errors, f"{where}: {key} wants number")
    elif key in ("set","clear","give","take"):
        if not (isinstance(val,str) and val): err(errors, f"{where}: {key} empty")
    elif key == "fire":
        if not (isinstance(val,str) and val): err(errors, f"{where}: fire wants event")
        if "args" in f and not isinstance(f["args"],dict):
            err(errors, f"{where}: args not an object")
    elif key == "follow" or key == "ally":
        pass
    elif key == "rel":
        if not (isinstance(val,list) and len(val)==2 and isinstance(val[0],str)
                and isinstance(val[1],(int,float))):
            err(errors, f"{where}: rel wants [npc,stage]")
    elif key == "end":
        pass
    elif key == "args":
        if "fire" not in f: err(errors, f"{where}: dangling args with no fire")
    else:
        err(errors, f"{where}: unrecognized effect kind `{key}`")

def check_list(lst, errors, where, is_cond):
    if lst is None: return
    if not isinstance(lst, list):
        err(errors, f"{where}: not an array"); return
    for i,e in enumerate(lst):
        (check_cond if is_cond else check_fx)(e, errors, f"{where}[{i}]")

def load_and_validate(path, errors):
    with open(path, encoding="utf-8") as fh:
        try: root = json.load(fh)
        except Exception as e:
            err(errors, f"{path}: JSON parse failed: {e}"); return None
    if root.get("format") != "x3.mission/1":
        err(errors, f"format is `{root.get('format')}` (want x3.mission/1)"); return None
    mid = root.get("id","")
    if not mid: err(errors, "missing mission id"); return None
    stages = root.get("stages")
    if not isinstance(stages, list) or not stages:
        err(errors, "missing/empty stages array"); return None
    ids = []
    for si,s in enumerate(stages):
        sw = f"stage{si}"
        if not isinstance(s, dict): err(errors, f"{sw}: not an object"); continue
        sid = s.get("id","")
        if not sid: err(errors, f"{sw}: missing id")
        sw = f"{mid}.{sid}"
        ids.append(sid)
        check_list(s.get("on_enter"), errors, sw+".on_enter", False)
        check_list(s.get("on_complete"), errors, sw+".on_complete", False)
        check_list(s.get("on_fail"), errors, sw+".on_fail", False)
        check_list(s.get("advance_when"), errors, sw+".advance_when", True)
        check_list(s.get("fail_when"), errors, sw+".fail_when", True)
        if "branch" in s:
            br = s["branch"]
            if not isinstance(br, dict): err(errors, f"{sw}: branch not object")
            else:
                check_list(br.get("if"), errors, sw+".branch.if", True)
                if not br.get("then") or not br.get("else"):
                    err(errors, f"{sw}: branch wants both then and else")
    # validateMission
    start = root.get("start") or (ids[0] if ids else "")
    seen_dupe = set()
    for sid in ids:
        if sid == "end": err(errors, f"{mid}: stage id `end` reserved")
        if sid in seen_dupe: err(errors, f"{mid}: duplicate stage id `{sid}`")
        seen_dupe.add(sid)
    idset = set(ids)
    if start not in idset: err(errors, f"{mid}: start `{start}` not a stage")
    def checkref(ref, at):
        if not ref or ref == "end": return
        if ref not in idset: err(errors, f"{mid}.{at}: dangling ref `{ref}`")
    bystage = {s.get("id"): s for s in stages if isinstance(s,dict)}
    for s in stages:
        if not isinstance(s,dict): continue
        sid = s.get("id")
        checkref(s.get("next"), sid+".next")
        checkref(s.get("fail_to"), sid+".fail_to")
        if "branch" in s and isinstance(s["branch"],dict):
            checkref(s["branch"].get("then"), sid+".branch.then")
            checkref(s["branch"].get("else"), sid+".branch.else")
    # reachability
    if start in idset:
        seen=set(); stack=[start]; seen.add(start)
        while stack:
            s = bystage.get(stack.pop())
            if not s: continue
            for ref in (s.get("next"), s.get("fail_to")):
                if ref and ref!="end" and ref in idset and ref not in seen:
                    seen.add(ref); stack.append(ref)
            if "branch" in s and isinstance(s["branch"],dict):
                for ref in (s["branch"].get("then"), s["branch"].get("else")):
                    if ref and ref!="end" and ref in idset and ref not in seen:
                        seen.add(ref); stack.append(ref)
        for sid in ids:
            if sid not in seen: err(errors, f"{mid}: stage `{sid}` unreachable from start")
    return mid

def main():
    d = sys.argv[1] if len(sys.argv)>1 else os.path.join(
        os.path.dirname(__file__), "..","..","..","..","missions")
    files = sorted(f for f in os.listdir(d) if f.endswith(".mission.json"))
    total=0; ok=0
    for fn in files:
        total+=1
        errs=[]
        mid = load_and_validate(os.path.join(d,fn), errs)
        if not errs:
            ok+=1; print(f"  PASS {fn}")
        else:
            print(f"  FAIL {fn}")
            for e in errs: print(f"      {e}")
    print(f"\nmissions: {ok}/{total} conform to the real parser contract")
    return 0 if ok==total else 1

if __name__ == "__main__":
    sys.exit(main())
