# Builds Verso with the Visual Studio C++ toolchain (MSVC + CMake + Ninja).
#   .\build.ps1                 Release build
#   .\build.ps1 -Config Debug   Debug build
#   .\build.ps1 -Test           Build and run the core tests
param(
  [string]$Config = "Release",
  [switch]$Test,
  [switch]$CoreOnly
)
$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio Build Tools with the C++ workload." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the C++ x64 toolset was found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$build = Join-Path $PSScriptRoot ("build\" + $Config.ToLower())
$app = if ($CoreOnly) { "OFF" } else { "ON" }
$cmd = "`"$vcvars`" >nul && cmake -S `"$PSScriptRoot`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DVERSO_BUILD_APP=$app && cmake --build `"$build`""
if ($Test) { $cmd += " && ctest --test-dir `"$build`" --output-on-failure" }
cmd /c $cmd
exit $LASTEXITCODE
