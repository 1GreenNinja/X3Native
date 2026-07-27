CLUB 1127 JUKEBOX  —  your own music (personal use)
===================================================

Drop your OWN music files into THIS folder and Club 1127 will play them, GTA
"Self Radio" style. The club's whole beat grid (subwoofers, dance-floor tiles,
the dancers, and the moving-head lights) rides the tempo of whatever is playing.

  Supported:   .mp3  and  .wav
  Personal use only. Nothing you drop here is committed or shared — the .gitignore
  keeps this folder's audio local to your machine.

HOW TO USE
----------
  1. Copy some song.mp3 (or song.wav) files into this folder.
  2. Launch the club (--world club) and walk in.
  3. In the club:  N = next track,  Shift+N = previous track.
     A "Now Playing: <name>" toast shows the current track + its BPM.
  4. Tracks play in alphabetical order (or shuffle — see cvars below), and
     auto-advance when each one ends. A single track loops.

  Empty folder?  The club just plays its built-in house track, exactly as before.

WHERE YOUR MUSIC CAN LIVE  (highest priority first)
----------------------------------------------------
  1. the snd_clubmusic_dir cvar folder            — explicit override
  2. %USERPROFILE%\Documents\X3Native\club_music  — YOUR library (recommended)
  3. THIS folder, inside the repo                 — README + one test tone only

  All three are scanned and the results merged. The same FILENAME in more than
  one place resolves to the HIGHEST-priority copy, so your real track always
  beats the repo's test fixture.

  Subfolders are NOT scanned — that is why samples/ never auto-plays.

TELLING THE CLUB YOUR TRACK'S BPM AND DOWNBEAT (optional)
----------------------------------------------------------
  The beat grid rides each track's BPM. There is NO auto-detect — tag it yourself
  with a tiny sidecar JSON next to the track:

      my_song.mp3
      my_song.mp3.json      <-  { "bpm": 128, "offset_s": 0.35 }
        (or my_song.json)

  "bpm"       required. Musical range 20-400.

  "offset_s"  OPTIONAL. Seconds from the START of the file to its FIRST DOWNBEAT.
              Range -10 to 10. This is the difference between the room being on
              the beat and merely at the right speed: without it the subs, tiles,
              dancers and moving heads run at your track's tempo but punch
              BETWEEN your beats. Set it to where beat 1 actually lands and the
              whole room locks to the music.

              Most MP3s start with a short lead-in, so a small positive value
              (0.05-0.5) is typical. A track with no offset_s resets the phase to
              zero — it never inherits the previous track's.

  No sidecar?  The club uses the snd_clubmusic_bpm cvar (default 120), phase 0.

  See samples/test_tone_120.wav (+ .json) for a working example.

CVARS  (in %LOCALAPPDATA%\x3native_settings.cfg, one KEY=VALUE per line;
        each is also overridable by the matching X3_SND_CLUBMUSIC_* env var)
-----------------------------------------------------------------------------
  snd_clubmusic_dir       EXTRA folder to scan, on top of the two below. Empty
                          by default. (It used to replace the Documents folder;
                          now both are scanned.)
  snd_clubmusic_shuffle   1 = shuffle the playlist, 0 = alphabetical (default 0)
  snd_clubmusic_bpm       fallback BPM when a track has no sidecar (default 120)

  Volume rides your normal MUSIC volume setting.
