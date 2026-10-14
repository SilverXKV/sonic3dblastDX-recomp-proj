@echo off
setlocal
cd /d "%~dp0"
rem Builds "Sonic 3D Blast - Directors Cut Recompiled.exe" from src\ and generated\.

if not exist generated\dispatch.c (
  echo generated\ is missing. Make it from your own ROM first:
  echo   set S3D_ROM=path\to\your\rom.md
  echo   python tools\gen.py generated
  exit /b 1
)
rem Uses MinGW-w64 gcc if it is on PATH, otherwise finds Visual Studio (2017+) automatically.

where gcc >nul 2>nul
if %errorlevel%==0 (
  echo Building with gcc ...
  gcc -std=gnu11 -O2 -w -Isrc -Igenerated -o "Sonic 3D Blast - Directors Cut Recompiled.exe" src\cpu.c src\hw.c src\render.c src\widescreen.c src\z80.c src\audio.c src\win32_main.c generated\*.c -lgdi32 -lwinmm -lcomdlg32 -luser32 -mwindows
  goto done
)

where cl >nul 2>nul
if %errorlevel%==0 goto msvc

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nocompiler
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" goto nocompiler
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto nocompiler
echo Found Visual Studio at %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:msvc
echo Building with MSVC ...
cl /nologo /O2 /std:c11 /w /Isrc /Igenerated "/Fe:Sonic 3D Blast - Directors Cut Recompiled.exe" src\cpu.c src\hw.c src\render.c src\widescreen.c src\z80.c src\audio.c src\win32_main.c generated\*.c /link user32.lib gdi32.lib winmm.lib comdlg32.lib /SUBSYSTEM:WINDOWS
goto done

:nocompiler
echo No C compiler found.
echo Install "Desktop development with C++" in Visual Studio (or Build Tools), or w64devkit/MSYS2 mingw-w64, then run this again.
exit /b 1

:done
if exist "Sonic 3D Blast - Directors Cut Recompiled.exe" (echo. & echo Built "Sonic 3D Blast - Directors Cut Recompiled.exe". Run it and pick your ROM.) else (echo Build failed.)
