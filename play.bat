@echo off
setlocal
rem Plays the Source Port built by build.bat: drop your Melee NTSC 1.02 ISO onto this file, or put it
rem beside it named melee.iso. Builds first if there is no build yet. Runs from this folder, so the
rem game finds port\slippi_sys and writes its log (melee_port.log) here.
cd /d "%~dp0"
set "ISO=%~1"
if "%ISO%"=="" set "ISO=%~dp0melee.iso"
if not exist "%ISO%" (
  echo Drop your Melee NTSC 1.02 ISO onto play.bat, or put it next to it named melee.iso
  pause
  exit /b 1
)
set "EXE=build-host\port\Release\melee_source.exe"
if not exist "%EXE%" (
  echo No build yet; building from source first.
  call build.bat || exit /b 1
)
if not exist "build-host\port\Release\melee_game.dll" (
  echo build-host\port\Release has no melee_game.dll: run build.bat.
  pause
  exit /b 1
)
"%EXE%" --iso "%ISO%" --threaded-renderer
if errorlevel 1 pause
