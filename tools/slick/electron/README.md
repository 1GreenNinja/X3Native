# Slick Desktop (Electron)

The Windows .exe — same architecture Slack/Discord/VSCode ship (packaged web
app + its own Chromium, no system-WebView dependency, which Tim dislikes).
Wraps the same `tools/slick/dist/` the web app serves.

## Dev run
```powershell
cd G:\X3Native\tools\slick\electron
npm install
npm start            # opens the Slick window against slick.slopclaude.com
```
Set `SLICK_LOCAL=1` to load the bundled dist/ offline, or `SLICK_URL=...` to
point at another deployment (e.g. slick.x3designs.net once its DNS is live).

## Build the installer + portable exe
```powershell
cd G:\X3Native\tools\slick
npm run build        # refresh dist/ first
cd electron
npm run dist         # electron-builder --win -> release\
```
Outputs to `electron\release\`:
- `Slick Setup <ver>.exe` — NSIS installer (desktop shortcut, choose-dir)
- `Slick <ver>.exe` — portable single-file, no install

By default the app loads the LIVE deployment so it always tracks the latest
build with no re-package. Re-package only when you change main.js / icon / the
local-bundled fallback.
