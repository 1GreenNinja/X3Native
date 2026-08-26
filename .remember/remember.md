# Handoff

## State
PHASE 0 COMPLETE (4/4 + Tim's r_fog ask), folded to `origin/integration/complete`
(87d6d07a) from D:\GameDev\X3N-lanerecover branch fold/phase0-integrity:
- 0.1 mr-factors sweep (11 sites + SceneEntity fields; 115/152 GLBs affected)
- 0.2 filmic-post verified default-on
- 0.3 aerial perspective (fog.frag height integral + sky melt; outdoor world
  had NO fog before); receipts in shots_phase0/, vista sent to Tim
- 0.4 normal-map audit: nrm=X/Y bind receipt per load; 85/152 GLBs author ZERO
  normal maps -> docs/plans/NORMAL_MAP_AUDIT_0825.md (content list + recipe)
- r_fog dials LIVE: r_fogdensity/start/height/skyblend/max from console AND
  --set on outdoor hosts (audit-claimed; first try read a null console on the
  settle path — the --set audit caught it; hc.cliCVars is the settle-path truth)
ArmoryTaskMaster owns app/space/*, space_pilot, host_introcockpit — hands off.

## Next
1. Phase 1 (PLAN_GTA6_CAMPAIGN.md): foliage translucency per-material (engine
   term exists), cloud light transport, far-terrain texture + water tone (the
   vista's exposed slops), normal-map content burn-down (NORMAL_MAP_AUDIT list).
2. Suggest Revision 7 X3Play build off 87d6d07a (overlord + canon spine +
   authored materials + aerial air + fog dials since Rev 6).

## Context
- Aerial base values: tunnelAerialFog() host_tunnel.cpp ~185; dials override
  live (change-gated every frame, -1 = keep).
- Capture A/Bs: `--world tunnel --screenshot t.png --shot-cam "..." --set
  r_fogdensity X` — run's own log proves application.
- Naming law: game = ECHO HARBOR, Echotropolis = district.
