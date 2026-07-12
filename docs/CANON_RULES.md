
## SHIPS ARE SELF-LIT (Tim, 2026-07-12) — NEW CANON RULE
> "All spaceships shall have built in lighting like in StarTrek. New canon rule."

EVERY spaceship carries its OWN lighting. A ship must NEVER render as a black silhouette,
regardless of where the star is or whether anything external lights it. Star Trek convention:
hulls read even on the dark side because the ship is lit from within/by itself.
Implement per-ship:
- Emissive hull accents / running lights / nav lights / window rows (per-texel emissive, NOT a
  flat pane flood — use GlassMaterial::emissiveMap / Entity::emissiveTex).
- Engine glow (the brightest thing, but not the ONLY thing).
- A small self-illumination / rim term on the hull so form reads on the unlit side. This is a
  DELIBERATE stylistic self-light, not a crutch: the hull must still shade honestly from the
  directional star — the self-light lifts the dark side so the silhouette never dies.
- Applies to: Jake's fighter, the Overlord, the enemy/capital ships, and any future ship.
Symptom this fixes: the Overlord reads great on its first appearance and goes BLACK on the
second, because it was relying on external light that isn't there at that camera/angle.
