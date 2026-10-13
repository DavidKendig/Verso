@echo off
rem Starts Verso, building the Release binary first if it does not exist yet.
rem Usage: launch.bat [files or folders...] [-n]
setlocal
set "ROOT=%~dp0"
set "EXE=%ROOT%build\release\Verso.exe"

if not exist "%EXE%" (
  echo Verso has not been built yet. Building the Release configuration...
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%build.ps1" -Config Release
  if errorlevel 1 (
    echo.
    echo Build failed. Install Visual Studio Build Tools with the C++ workload and try again.
    pause
    exit /b 1
  )
)

start "" "%EXE%" %*
endlocal
