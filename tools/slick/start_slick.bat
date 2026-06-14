@echo off
REM Slick static server — serves the built dist/ for the cloudflared tunnel.
REM Scheduled Task Slick-Static runs this; tunnel routes slick.x3designs.net -> :8090
cd /d "G:\X3Native\tools\slick\dist"
python -X utf8 -m http.server 8090 --bind 127.0.0.1
