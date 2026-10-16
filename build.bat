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
if %errorlevel%==0 goto gcc

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
goto msvc

:gcc
echo Building with gcc ...
rem the icon is compiled in when windres is available (the exe still builds without it)
set "RES="
where windres >nul 2>nul && windres s3d.rc -O coff -o s3d_res.o && set "RES=s3d_res.o"
gcc -std=gnu11 -O2 -w -Isrc -Igenerated -o "Sonic 3D Blast - Directors Cut Recompiled.exe" src\cpu.c src\hw.c src\render.c src\widescreen.c src\crt.c src\z80.c src\audio.c src\win32_main.c generated\*.c %RES% -lgdi32 -lwinmm -lcomdlg32 -luser32 -mwindows
goto done

:msvc
echo Building with MSVC ...
rem the icon is compiled in when rc.exe is available (the exe still builds without it)
set "RES="
where rc >nul 2>nul && rc /nologo /fo s3d.res s3d.rc >nul && set "RES=s3d.res"
cl /nologo /O2 /std:c11 /w /Isrc /Igenerated "/Fe:Sonic 3D Blast - Directors Cut Recompiled.exe" src\cpu.c src\hw.c src\render.c src\widescreen.c src\crt.c src\z80.c src\audio.c src\win32_main.c generated\*.c %RES% /link user32.lib gdi32.lib winmm.lib comdlg32.lib /SUBSYSTEM:WINDOWS
goto done

:nocompiler
echo No C compiler found.
echo Install "Desktop development with C++" in Visual Studio (or Build Tools), or w64devkit/MSYS2 mingw-w64, then run this again.
exit /b 1

:done
if exist "Sonic 3D Blast - Directors Cut Recompiled.exe" (echo. & echo Built "Sonic 3D Blast - Directors Cut Recompiled.exe". Run it and pick your ROM.) else (echo Build failed.)
