@echo off
REM ============================================================================
REM  Launch Echo Harbor — double-click this from Explorer.
REM  Running from Explorer means the game is owned by Windows, NOT by the Claude
REM  Code session, so nothing reaps it — it stays open until YOU close it (menu
REM  Quit, or the window X). The window opens PAUSED at golden hour; press T to
REM  start the day/night clock.
REM ============================================================================
cd /d "%~dp0"
set ECHO_TOD=golden
title Echo Harbor (log — closing this closes the game)
"build\bin\Release\EchoHarbor.exe" --world echotropolis
