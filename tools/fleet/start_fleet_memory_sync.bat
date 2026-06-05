@echo off
REM Scheduled Task launcher for FleetMemory-Sync. Runs every 3 hours, pushes
REM any new ~/.claude/ changes to https://github.com/1GreenNinja/fleet-memory
REM Log line per run lands in ~/.claude/.fleet-memory-sync.log.
python -X utf8 G:\X3Native\tools\fleet\sync_memory.py
