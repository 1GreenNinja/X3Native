@echo off
REM Double-clickable launcher for the FleetCommand user-creation GUI.
REM Drop a shortcut to this on your desktop and you have one-click new-user provisioning.
cd /d "G:\X3Native"
python -X utf8 "G:\X3Native\tools\fleet\create_user.py" --gui
