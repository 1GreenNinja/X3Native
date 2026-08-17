# The garage chooser, the tuning panel, the thermometer — 2026-08-16

Branch `inspx/road-network`. Four shots, all captured headlessly so they can be
reviewed from any machine without building.

| shot | what to look at |
|---|---|
| `09_garage_lnss.png` | **The chooser (G).** The card over the LATE NIGHT SPEED bay: name, drivetrain, the real build figures in ft-lb / rpm / lb / inches, five bars scaled across your own fleet, and the car's character line. Behind it: the parked fleet, which until this commit was loaded, logged, and never drawn. |
| `01_approach.png` | The same card with the world behind it, and the snow/thermometer running. |
| `../tunepanel/01_approach.png` | **The tuning panel (F7).** Twelve sliders on the driven car's own spec — drag one and the car retunes as you drive. SAVE writes the whole roster back to `assets/vehicles/cars.json`, curves included. |
| `../thermo/03_far_mouth.png` | **The thermometer.** Round bulb, domed meniscus, bezel, glass specular, minor ticks — the HUD had no way to draw anything that was not an axis-aligned rectangle until `hudDisc`/`hudRoundRect`. |

## Reproducing them

Headless, no window, no GPU interaction needed beyond a device:

```
X3Engine.exe --screenshot-tunnel <dir>          # add env vars below
```

| env | effect |
|---|---|
| `X3_GARAGE=1` | opens the chooser at boot, so it lands in the capture |
| `X3_TUNE_PANEL=1` | opens the F7 tuning panel at boot |
| `X3_WEATHER=snow` `X3_SNOW_IN=3` | weather + lying snow, which is what makes the thermometer draw |

**Captures write relative to the working directory**, not the repo root — they
land in `<builddir>/bin/docs/screenshots/<dir>/`. That has cost real time
before; these were copied up by hand.

Both `X3_GARAGE` and `X3_TUNE_PANEL` exist for one reason: a UI whose only
witness is the person holding the key ships with its text overlapping. Every
layout fault in this set was caught by screenshotting it, not by a test.

## Known, and deliberately not fixed here

The **parking layout wants a pass**. The cars overlap the lifts and one sits
right on the capture vantage. That geometry was authored blind — against a
renderer that was never drawing it — so nobody could see it until now.

The **turntable model is unverified by eye**: the garage vantage is filled by a
parked car in the foreground. The logic is tested (`--test-garage`, 9/9:
deterministic spin, wrap after ten simulated minutes, an empty roster that
refuses to open), but the placement wants a human look.
