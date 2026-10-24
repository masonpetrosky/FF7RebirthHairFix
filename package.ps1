# Builds the add-on and assembles the release zip in dist\.
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Pinned DirectX Shader Compiler release; only dxcompiler.dll is shipped.
$dxcUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip'
$dxcSha256 = 'AD31B1FC8443175D204F77A611FDB3EF2EC42759BDC2F1167368DE24A4A7E7F1'

if (-not $Version) {
  $Version = (Select-String -Path CMakeLists.txt -Pattern 'project\(FF7RebirthVegetationAndHairFix VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}

& "$PSScriptRoot\build.ps1" | Out-Null

$dxcZip = Join-Path $PSScriptRoot 'build\dxc.zip'
if (-not (Test-Path $dxcZip) -or (Get-FileHash $dxcZip -Algorithm SHA256).Hash -ne $dxcSha256) {
  Invoke-WebRequest $dxcUrl -OutFile $dxcZip
  if ((Get-FileHash $dxcZip -Algorithm SHA256).Hash -ne $dxcSha256) { throw 'DXC download does not match the pinned SHA-256.' }
}
$dxcDir = Join-Path $PSScriptRoot 'build\dxc'
if (Test-Path $dxcDir) { Remove-Item $dxcDir -Recurse -Force }
Expand-Archive $dxcZip $dxcDir

$stage = Join-Path $PSScriptRoot "build\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\FF7RebirthVegetationAndHairFix\licenses" | Out-Null
Copy-Item build\FF7RebirthVegetationAndHairFix.addon64 $stage
Copy-Item config\FF7RebirthVegetationAndHairFix.ini $stage
Copy-Item "$dxcDir\bin\x64\dxcompiler.dll" "$stage\FF7RebirthVegetationAndHairFix\"
Copy-Item "$dxcDir\LICENSE-LLVM.txt" "$stage\FF7RebirthVegetationAndHairFix\licenses\dxcompiler-LICENSE-LLVM.txt"
Copy-Item "$dxcDir\LICENCE-MIT.txt" "$stage\FF7RebirthVegetationAndHairFix\licenses\dxcompiler-LICENSE-MIT.txt"
Copy-Item LICENSE "$stage\FF7RebirthVegetationAndHairFix\licenses\LICENSE.txt"
Copy-Item THIRD_PARTY_NOTICES.md "$stage\FF7RebirthVegetationAndHairFix\licenses\"
Copy-Item third_party\reshade\LICENSE.md "$stage\FF7RebirthVegetationAndHairFix\licenses\reshade-LICENSE.md"
Copy-Item third_party\imgui\LICENSE.txt "$stage\FF7RebirthVegetationAndHairFix\licenses\imgui-LICENSE.txt"
Copy-Item third_party\dxc\LICENSE.TXT "$stage\FF7RebirthVegetationAndHairFix\licenses\dxc-LICENSE.txt"

New-Item -ItemType Directory -Force dist | Out-Null
$zip = Join-Path $PSScriptRoot "dist\FF7RebirthVegetationAndHairFix-v$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-Item $zip
