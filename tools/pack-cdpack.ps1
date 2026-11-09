# Release packing with cordinal's own packer (cdpack/). Python runs the build-time encoder and
# verifier: CDPACK_PYTHON names one (release CI sets it), else the py launcher's 3.14.
param([string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$BuildDir) { $BuildDir = Join-Path $root 'build' }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$source = Join-Path $root 'cdpack'
$tools = Join-Path $BuildDir 'tools\cdpack'
if (!(Test-Path -LiteralPath (Join-Path $source 'tools\build.ps1'))) { throw 'Bundled cdpack sources are missing.' }
& (Join-Path $source 'tools\build.ps1') -Out $tools
$stage = (New-Item -ItemType Directory -Path (Join-Path $BuildDir ('cdpack-' + [Guid]::NewGuid().ToString('N')))).FullName
$reports = @()
$python = @(if ($env:CDPACK_PYTHON) { $env:CDPACK_PYTHON } else { 'py.exe'; '-3.14' })
# The profile lists the functions the player runs as hashes of their bytes, so a relink does not
# age it; the map turns those hashes into the addresses of this build. Only the player has a map,
# so only it is packed with code decoded on demand. Without both files the packager makes an
# ordinary, all-eager image.
$profile = Join-Path $root 'tools\code-profile.txt'
$map = Join-Path $BuildDir 'cordinal.map'
$hot = @()
if ((Test-Path -LiteralPath $profile) -and (Test-Path -LiteralPath $map)) {
    $hot = @('--profile', $profile, '--map', $map)
}
foreach ($name in @('cordinal.exe', 'cordinal-test.exe')) {
    $native = Join-Path $BuildDir ('unpacked\' + $name)
    $packed = Join-Path $stage $name
    $report = Join-Path $stage ($name + '.json')
    $extra = if ($name -eq 'cordinal.exe') { $hot } else { @() }
    & $python[0] @($python | Select-Object -Skip 1) (Join-Path $source 'tools\pack.py') pack $native $packed `
        --tools $tools --report $report @extra
    if ($LASTEXITCODE) { throw "Independent packing/verification failed: $name" }
    $reports += Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
}
# The standalone verifier has already compared every original file byte and visible resource.
# Publish neither executable until both checks pass, matching the stock pipeline's pair contract.
foreach ($name in @('cordinal.exe', 'cordinal-test.exe')) {
    Copy-Item -LiteralPath (Join-Path $stage $name) -Destination (Join-Path $BuildDir $name) -Force
    & icacls.exe (Join-Path $BuildDir $name) /setintegritylevel M | Out-Null
    if ($LASTEXITCODE) { throw "Cannot set integrity label: $name" }
}
$reports | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $BuildDir 'packing.json') -Encoding UTF8
foreach ($name in @('cordinal.exe', 'cordinal-test.exe')) {
    Remove-Item -LiteralPath (Join-Path $stage $name), (Join-Path $stage ($name + '.json'))
}
Remove-Item -LiteralPath $stage
& (Join-Path $PSScriptRoot 'check-size.ps1') -Exe (Join-Path $BuildDir 'cordinal.exe') `
    -UnpackedExe (Join-Path $BuildDir 'unpacked\cordinal.exe')
