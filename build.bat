@echo off
setlocal
rem Builds the Source Port: melee_game.dll (the decompiled game, MinGW-w64 GCC 14 or newer with Ninja)
rem and melee_source.exe (the host, Visual Studio 2022 with the C++ workload), then puts the game
rem library beside the program. docs\build-source-port.md
rem
rem   build.bat [MinGW folder]     default C:\toolchains\mingw64 (the folder holding bin\gcc.exe and bin\ninja.exe)
rem
rem Output: build-host\port\Release\melee_source.exe. Play with play.bat.
cd /d "%~dp0"
set "ROOT=%CD:\=/%"
set "MINGW=%~1"
if "%MINGW%"=="" set "MINGW=C:\toolchains\mingw64"
set "MINGW=%MINGW:\=/%"

if not exist "%MINGW%/bin/gcc.exe" (
  echo No MinGW-w64 GCC at %MINGW%: install GCC 14 or newer with Ninja and pass its folder,
  echo e.g. build.bat D:\mingw64  ^(docs\build-source-port.md^)
  goto :fail
)
where python >nul 2>nul || (echo Python 3 is needed on PATH. & goto :fail)
set "CMAKE=cmake"
where cmake >nul 2>nul && goto :have_cmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio 2022 with the C++ workload is needed. & goto :fail)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" (echo No CMake on PATH or in Visual Studio. & goto :fail)
:have_cmake

echo [1/4] The decompiled game sources
if not exist sourceport\extern\melee\src (
  git submodule update --init sourceport/extern/melee || goto :fail
)
python tools\prepare_native_sources.py || goto :fail
echo [2/4] melee_game.dll ^(GCC^)
"%CMAKE%" -S sourceport/game -B build-gcc -G Ninja "-DCMAKE_TOOLCHAIN_FILE=%ROOT%/sourceport/cmake/mingw-w64-x86_64.cmake" "-DMELEE_MINGW_ROOT=%MINGW%" || goto :fail
"%CMAKE%" --build build-gcc --target melee_game --parallel || goto :fail
echo [3/4] melee_source.exe ^(MSVC; a first build takes a while^)
"%CMAKE%" -S . -B build-host -G "Visual Studio 17 2022" -A x64 || goto :fail
"%CMAKE%" --build build-host --config Release --target melee_source --parallel || goto :fail
echo [4/4] The game library beside the program
for %%f in (melee_game.dll melee_game.snapexcl melee_game.syms) do (
  copy /y "build-gcc\%%f" "build-host\port\Release\%%f" >nul || goto :fail
)
echo.
echo Done: build-host\port\Release\melee_source.exe
echo Play with:  play.bat "C:\path\to\Melee NTSC 1.02.iso"
exit /b 0

:fail
echo Build failed; see the messages above.
pause
exit /b 1
