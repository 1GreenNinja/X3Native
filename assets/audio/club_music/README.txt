CLUB 1127 JUKEBOX — drop your own music here (personal use)
===========================================================

Put your MP3s (or WAVs) in this folder and Club 1127 ("The Deep") plays THEM
instead of the built-in track — GTA "Self Radio" style. The entire light show
(sub thumps, dance tiles, blacklights, dancers) locks to each track's BPM.

Folders scanned (all three are merged; duplicates by filename keep the first):
  1. the folder the cvar `snd_clubmusic_dir` points at (console: `snd_clubmusic_dir D:/Music/club`)
  2. <your Documents>/X3Native/club_music     <- put your real library HERE so it
                                                 never has to live in the repo
  3. this folder (assets/audio/club_music)    <- only this README + a tiny test
                                                 tone ship with the repo; other
                                                 audio here is .gitignore'd

Optional BPM sidecar (recommended!): next to "MyTrack.mp3" add "MyTrack.json":

    { "bpm": 128.0, "offset_s": 0.35 }

  * bpm       — the track's tempo. The club's beat grid (subs/tiles/dancers/
                lights) retunes to it the moment the track starts.
  * offset_s  — optional: seconds until the FIRST beat, so the light show hits
                on the downbeat.
No sidecar -> the default BPM cvar `snd_clubmusic_bpm` (120) is used.
No auto BPM detection in v1 — sidecar or default only.

In the club:
  * N          next track          * Shift+N    previous track
  * Tracks play alphabetically (cvar `snd_clubmusic_shuffle 1` to shuffle),
    auto-advance at track end, and a "Now Playing" readout shows on change.
  * Empty folders -> the club's built-in Descent loop plays exactly as always.

jukebox_test_tone.wav (+ .json) is a generated 130 BPM click used as the wiring
proof — delete it or drown it out with real music. Personal use only: keep your
music files out of commits (the .gitignore here already does).
