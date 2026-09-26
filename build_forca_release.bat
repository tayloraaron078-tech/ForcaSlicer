@REM [regional-supports fork] Forca Slicer - local release packager (double-click friendly wrapper).
@REM Bundles the built app into a dated, shareable portable .zip in dist\ - no cloud CI needed.
@REM
@REM Usage:
@REM   build_forca_release.bat                 package the existing RelWithDebInfo build (build-dbginfo)
@REM   build_forca_release.bat release         package a Release build instead (build\)
@REM   build_forca_release.bat build           do a full build first, then package
@REM   build_forca_release.bat installer       also emit an NSIS installer (needs NSIS)
@REM   flags combine, e.g.:  build_forca_release.bat release build installer
@echo off
setlocal
set "PS_ARGS="
:parse
if "%~1"=="" goto run
if /I "%~1"=="release"    set "PS_ARGS=%PS_ARGS% -Config Release"
if /I "%~1"=="build"      set "PS_ARGS=%PS_ARGS% -Build"
if /I "%~1"=="installer"  set "PS_ARGS=%PS_ARGS% -Installer"
shift
goto parse
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_forca_release.ps1"%PS_ARGS%
endlocal
