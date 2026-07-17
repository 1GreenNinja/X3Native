@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "EXE=build\bin\Release\X3Engine.exe"

if not exist "%EXE%" (
  echo.
  echo   X3Engine.exe not found at:
  echo       %CD%\%EXE%
  echo.
  echo   Build it first:   cmake --build build --config Release
  echo.
  pause
  exit /b 1
)

:menu
cls
echo ==================================================
echo                X3 ENGINE  --  LAUNCHER
echo ==================================================
echo.
echo     [1]  Echo Harbor       island city, day/night
echo     [2]  Escape Lab Zero   the canon lab
echo     [3]  EoS Grey-Box      Empires of Shadow spike orbit
echo.
echo     [Q]  Quit
echo.
echo ==================================================
choice /c 123Q /n /m "  Pick a world: "
if errorlevel 4 exit /b 0
if errorlevel 3 goto eos
if errorlevel 2 goto lab
if errorlevel 1 goto harbor

:harbor
set ECHO_TOD=golden
title Echo Harbor  (log - closing this window closes the game)
echo.
echo   Launching Echo Harbor...
"%EXE%" --world echotropolis
goto done

:eos
title EoS Grey-Box  (log - closing this window closes the game)
echo.
echo   Launching the EoS grey-box orbit...
"%EXE%" --world eos-scene
goto done

:lab
title Escape Lab Zero  (log - closing this window closes the game)
echo.
echo   Launching Escape Lab Zero...
"%EXE%" --world canonlevel
goto done

:done
echo.
echo   (game exited - back to menu in 3s, or close this window)
timeout /t 3 >nul
goto menu
