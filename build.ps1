# Builds FF7RebirthVegetationFix.addon64 with the newest installed Visual Studio (x64, Release).
param([string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 toolset was not found.' }

$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Push-Location $PSScriptRoot
try {
  cmake -S . -B build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
  if ($LASTEXITCODE) { throw 'CMake configure failed.' }
  cmake --build build
  if ($LASTEXITCODE) { throw 'Build failed.' }
  Get-Item build\FF7RebirthVegetationFix.addon64
} finally {
  Pop-Location
}
