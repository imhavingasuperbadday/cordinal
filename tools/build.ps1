# Builds cordinal on Windows. Invoke it through build.bat (the supported entry point):
#
#   build.bat                 local experimental packed build (with the address filter)
#   build.bat stock-nrv       stock UPX release build
#   build.bat unpacked        development build: plain executables, no UPX download
#   build.bat full            ignore the incremental record and rebuild everything
#   build.bat experimental  use the sibling UPX research decoder for a local development build
#   build.bat experimental-addresses  also use its smaller x64 address-filter format
#   build.bat cdpack          independent decoder, packer and loader; local development
#   build.bat themes          also include the themes beyond Light and Dark (combines with the others)
#
# What it produces:
#   build\unpacked\cordinal.exe        the player (src\*.cpp + src\chiptune\*.cpp, no tests)
#   build\unpacked\cordinal-test.exe   console diagnostics with src\tests\*.cpp (no player window)
#   build\cordinal.exe                 the published pair: packed, or copied for "unpacked"
#
# Incremental model: build\obj\build.json records the SHA-256 of every source, this script and the
# MSVC toolset. A .cpp recompiles when its hash changes; any header change, a new build script version
# or a new toolset rebuilds everything. There is no timestamp or shadow-directory state to go stale.
# The record is written only after a successful build, so a failed build repeats its work.
#
# To add a source file: drop a .cpp in src\ (or src\chiptune, src\tests). It is picked up by the globs
# below and compiled with the default app flags. Only the codecs in $SlowCodecs / $HotCodecs and the
# two files with their own SIMD/MDCT flags need an entry here.

[CmdletBinding()]
param(
    [switch]$Unpacked,
    [switch]$Full,
    [switch]$Experimental,
    [switch]$ExperimentalAddresses,
    [switch]$Cdpack,
    [switch]$StockNrv,
    [switch]$Themes
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'packing-mode.ps1')
$mode = Resolve-PackingMode -Unpacked:$Unpacked -Experimental:$Experimental `
    -ExperimentalAddresses:$ExperimentalAddresses -Cdpack:$Cdpack -StockNrv:$StockNrv
$Experimental = $mode -eq 'experimental'
$ExperimentalAddresses = $mode -eq 'experimental-addresses'
$StockNrv = $mode -eq 'pack'
$useResearch = $Experimental -or $ExperimentalAddresses
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$timer = [Diagnostics.Stopwatch]::StartNew()

$objDir = 'build\obj'
$manifestPath = Join-Path $objDir 'build.json'
# The packed player's size budget in decimal bytes (PRINCIPLES.md: fast and small are features). A
# release build over it fails: a change that needs more raises this number and says why in docs\size.md.
$SizeBudget = 1611000

function Write-Step([string]$Text) { Write-Host "==> $Text" }
function Fail([string]$Message) { throw $Message }

# Repo-relative paths are used for every tool argument, so a checkout below a path with spaces never
# needs shell quoting. Forward slashes keep the manifest stable; Windows tools accept backslashes.
function Get-Relative([string]$FullName) {
    return $FullName.Substring($root.Length + 1).Replace('\', '/')
}
function Get-NativeRelative([string]$FullName) {
    return (Get-Relative $FullName).Replace('/', '\')
}

# One SHA-256 instance reused for every file: hashing the whole tree takes a few milliseconds.
$sha = [Security.Cryptography.SHA256]::Create()
function Get-ContentHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose() }
}
function Get-SetHash([string[]]$Lines) {
    # Ordinal sort: Sort-Object's culture-aware order differs between Windows PowerShell and pwsh,
    # which would give the same tree two hashes and force a full rebuild when the host changes.
    [string[]]$sorted = @($Lines).Clone()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    $bytes = [Text.Encoding]::UTF8.GetBytes($sorted -join "`n")
    return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
}

# --- toolchain ---------------------------------------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    Fail 'vswhere.exe not found. Install Visual Studio Build Tools with the C++ x64 workload.'
}
$vsDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsDir) { Fail 'MSVC x64 tools not found. Install the Visual Studio C++ x64 workload.' }
$toolsetDir = Get-ChildItem -LiteralPath (Join-Path $vsDir 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$toolset = if ($toolsetDir) { $toolsetDir.Name } else { 'unknown' }

# vcvars64.bat writes the complete MSVC environment to a temporary file; importing it once is much
# faster than running cmd for every compiler invocation.
$script:msvcReady = $false
function Initialize-Msvc {
    if ($script:msvcReady) { return }
    $vcvars = Join-Path $vsDir 'VC\Auxiliary\Build\vcvars64.bat'
    $envFile = Join-Path $objDir '.msvc-env.cmd'
    Set-Content -LiteralPath $envFile -Encoding Default -Value @(
        "@call `"$vcvars`" >nul 2>nul",
        '@set'
    )
    try {
        $lines = & cmd.exe /d /c $envFile
        if ($LASTEXITCODE -ne 0) { Fail 'vcvars64.bat failed.' }
    } finally {
        Remove-Item -LiteralPath $envFile -Force -ErrorAction SilentlyContinue
    }
    foreach ($line in $lines) {
        $line = $line.TrimEnd("`r")
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    $script:msvcReady = $true
}

function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { Fail "$Exe failed with exit code $LASTEXITCODE." }
}

# --- what changed ------------------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $objDir, 'build\unpacked' | Out-Null

$scriptHash = Get-ContentHash (Join-Path $PSScriptRoot 'build.ps1')
# Repack when either the packing policy or the research executable changes, even with no relink.
$packingHash = Get-ContentHash (Join-Path $PSScriptRoot 'pack-release.ps1')
$packingHash += ':' + (Get-ContentHash (Join-Path $PSScriptRoot 'packing-mode.ps1'))
if ($Cdpack) {
    $cdpackRoot = Join-Path $root 'cdpack'
    if (!(Test-Path -LiteralPath $cdpackRoot)) { Fail 'The bundled cdpack source directory is missing.' }
    $packingHash += ':' + (Get-ContentHash (Join-Path $PSScriptRoot 'pack-cdpack.ps1'))
    $packingHash += ':' + (Get-SetHash @(Get-ChildItem -LiteralPath $cdpackRoot -Recurse -File |
        Where-Object { $_.Extension -in @('.h', '.cpp', '.ps1', '.py') } |
        ForEach-Object { $_.FullName + '=' + (Get-ContentHash $_.FullName) }))
}
if ($useResearch) {
    $researchUpx = Join-Path (Split-Path -Parent $root) 'upx\build\upx.exe'
    if (!(Test-Path -LiteralPath $researchUpx)) {
        Fail 'Build the sibling upx research checkout first: cmake --build ..\upx\build --config Release'
    }
    $packingHash += ':' + (Get-ContentHash $researchUpx)
    $packingHash += ':' + (Get-ContentHash (Join-Path $PSScriptRoot 'research-packing.ps1'))
}
$files = @(Get-ChildItem -LiteralPath 'src' -Recurse -File)
$hashes = @{}
foreach ($file in $files) { $hashes[(Get-Relative $file.FullName)] = Get-ContentHash $file.FullName }

# A header, toolset or build-script change can reach every translation unit, so it rebuilds them all.
$headerHash = Get-SetHash @($hashes.Keys | Where-Object { $_ -like '*.h' } |
    ForEach-Object { "$_=$($hashes[$_])" })
$resourceExtensions = @('.rc', '.ico', '.manifest', '.png', '.ttf', '.wav', '.flac', '.txt')
$resourceHash = Get-SetHash @($hashes.Keys |
    Where-Object { $resourceExtensions -contains [IO.Path]::GetExtension($_) } |
    ForEach-Object { "$_=$($hashes[$_])" })

$old = $null
$oldFiles = @{}
if (Test-Path -LiteralPath $manifestPath) {
    $old = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($old.files) {
        foreach ($property in $old.files.PSObject.Properties) { $oldFiles[$property.Name] = $property.Value }
    }
}

$rebuildAll = [bool]$Full
$reason = 'requested on the command line'
if (!$rebuildAll -and !$old) { $rebuildAll = $true; $reason = 'no record of a previous build' }
if (!$rebuildAll -and $old.script -ne $scriptHash) { $rebuildAll = $true; $reason = 'build.ps1 changed' }
if (!$rebuildAll -and $old.toolset -ne $toolset) { $rebuildAll = $true; $reason = 'MSVC toolset changed' }
if (!$rebuildAll -and $old.headers -ne $headerHash) { $rebuildAll = $true; $reason = 'a header changed' }
if (!$rebuildAll -and [bool]$old.themes -ne [bool]$Themes) { $rebuildAll = $true; $reason = 'the theme set changed' }
if ($rebuildAll) { Write-Step "Rebuilding everything: $reason." }

$resPath = Join-Path $objDir 'cordinal.res'
$resourceStale = $rebuildAll -or $old.resources -ne $resourceHash -or !(Test-Path -LiteralPath $resPath)
$oldMode = if ($old) { $old.mode } else { '' }

# --- compile plan ------------------------------------------------------------------------------

# Flags shared by every translation unit. The groups below add only what differs, so a new .cpp needs
# no flag copy: put it in src\ and it builds with the app set.
$common = @('/nologo', '/std:c++20', '/Zc:throwingNew', '/W4', '/EHsc', '/GR-', '/MT', '/utf-8', '/DNOMINMAX')
# Releases ship Light and Dark only; `themes` compiles in the others and embeds their art (theme_palette.h).
$themeDefine = if ($Themes) { @('/DCORDINAL_EXTRA_THEMES') } else { @() }
$common += $themeDefine
$size = @('/O1', '/Oi', '/Gy', '/Gw', '/D_USE_STD_VECTOR_ALGORITHMS=0')
$hot = @('/O2', '/Ob3', '/Oi', '/Gy', '/Gw', '/D_USE_STD_VECTOR_ALGORITHMS=0')
$unicode = @('/DUNICODE', '/D_UNICODE')
$slowCodecs = @('audio_pcm', 'audio_opus_silk', 'audio_ape')
$hotCodecs = @('audio_flac', 'audio_mpeg', 'audio_alac', 'audio_aac', 'audio_resample', 'engine', 'audio_cord', 'cord_encode',
    'audio_vorbis', 'audio_wma', 'audio_wavpack', 'audio_ac3', 'audio_opus_celt')

$groups = [ordered]@{}
function Add-Group([string]$Name, [string]$ObjSubdir, [string[]]$Flags, $Sources) {
    $out = if ($ObjSubdir) { Join-Path $objDir $ObjSubdir } else { $objDir }
    $groups[$Name] = @{ ObjDir = $out; Flags = $Flags; Sources = @($Sources) }
}

$appSources = @(Get-ChildItem -LiteralPath 'src' -Filter *.cpp -File)
$plainApp = @($appSources | Where-Object {
    $name = $_.BaseName
    $name -ne 'audio_avx' -and $name -ne 'audio_mdct' -and
    $slowCodecs -notcontains $name -and $hotCodecs -notcontains $name
})

# Game-music emulators: size-optimised and whole-program optimised like the app.
Add-Group 'chiptune' 'chiptune' ($common + $size + @('/Ob0', '/GL', '/MP')) `
    (Get-ChildItem -LiteralPath 'src\chiptune' -Filter *.cpp -File)
# Most app modules favour size; the app set also gets Unicode and whole-program optimisation.
Add-Group 'app' '' ($common + $size + $unicode + @('/Ob0', '/GL', '/MP')) $plainApp
# Built-in lossy encoders: source-only, baseline SSE2, one worker supplied by the caller.
Add-Group 'encode' 'encode' ($common + $size + @('/Ob2', '/GL', '/MP', '/D_USE_MATH_DEFINES', '/DGLINT_NO_THREADS')) `
    (Get-ChildItem -LiteralPath 'src\encode' -Filter *.cpp -File)
# Hot codec cores and the playback engine keep /O2 /Ob3; whole-program decisions would undo their
# hot-loop inlining. The slow group favours size but keeps bit-reader inlining (/Ob2).
Add-Group 'slow' '' ($common + $size + $unicode + @('/Ob2', '/GL', '/MP')) `
    @($appSources | Where-Object { $slowCodecs -contains $_.BaseName })
Add-Group 'hot' '' ($common + $hot + $unicode + @('/MP')) `
    @($appSources | Where-Object { $hotCodecs -contains $_.BaseName })
# Shared MDCT/FFT code keeps speed optimization without whole-program optimisation.
Add-Group 'mdct' '' ($common + $hot) `
    @($appSources | Where-Object { $_.BaseName -eq 'audio_mdct' })
# Only this translation unit contains AVX2 instructions; SSE2 remains the baseline.
Add-Group 'avx' '' (@('/nologo', '/std:c++20', '/O2', '/Ob2', '/arch:AVX2', '/Gy', '/Gw', '/W4',
        '/EHsc', '/GR-', '/MT', '/utf-8', '/DNOMINMAX')) `
    @($appSources | Where-Object { $_.BaseName -eq 'audio_avx' })
# Test code and the shipping forwarder get their own object folders; /I src resolves "app.h".
Add-Group 'tests' 'tests' ($common + $size + $unicode + @('/Ob0', '/GL', '/MP', '/I', 'src')) `
    (Get-ChildItem -LiteralPath 'src\tests' -Filter *.cpp -File)
Add-Group 'stub' 'stub' ($common + $size + $unicode + @('/Ob0', '/GL', '/MP', '/I', 'src')) `
    (Get-ChildItem -LiteralPath 'src\stub' -Filter *.cpp -File)

# A deleted or renamed source leaves the old object out of the link, but nothing else would ask for a
# relink. Fingerprint the set of compiled sources so removals relink too.
$sourceSetHash = Get-SetHash @($groups.Keys | ForEach-Object {
    $groups[$_].Sources | ForEach-Object { Get-Relative $_.FullName }
})
$sourceSetChanged = $sourceSetHash -ne $(if ($old) { $old.sources } else { '' })

function Test-Stale([string]$SourceRel, [string]$Object) {
    if ($rebuildAll) { return $true }
    if (!(Test-Path -LiteralPath $Object)) { return $true }
    if (!$oldFiles.ContainsKey($SourceRel)) { return $true }
    return $oldFiles[$SourceRel] -ne $hashes[$SourceRel]
}
function Get-Objects([string]$ObjDir, $Sources) {
    return @($Sources | ForEach-Object { Join-Path $ObjDir ($_.BaseName + '.obj') })
}

$appLinked = $false
$testLinked = $false
foreach ($name in $groups.Keys) {
    $group = $groups[$name]
    New-Item -ItemType Directory -Force -Path $group.ObjDir | Out-Null
    $stale = @($group.Sources | Where-Object {
        Test-Stale (Get-Relative $_.FullName) (Join-Path $group.ObjDir ($_.BaseName + '.obj'))
    })
    if (!$stale) { continue }

    Initialize-Msvc
    Write-Host ("Compiling {0} file(s) [{1}]" -f $stale.Count, $name)
    Invoke-Tool 'cl.exe' (@($group.Flags) + @('/c', "/Fo$($group.ObjDir)\") +
        @($stale | ForEach-Object { Get-NativeRelative $_.FullName }))
    if ($name -eq 'tests') { $testLinked = $true } else { $appLinked = $true }
}

if ($resourceStale) {
    Initialize-Msvc
    Write-Host 'Compiling resources [cordinal.rc]'
    Invoke-Tool 'rc.exe' (@('/nologo', "/fo$resPath") + $themeDefine + @('src\cordinal.rc'))
    $appLinked = $true
}

# --- link --------------------------------------------------------------------------------------

if ($sourceSetChanged) { $appLinked = $true }

$appObjects = @()
foreach ($name in @('app', 'encode', 'slow', 'hot', 'mdct', 'avx')) {
    $appObjects += Get-Objects $groups[$name].ObjDir $groups[$name].Sources
}
$chipObjects = Get-Objects $groups['chiptune'].ObjDir $groups['chiptune'].Sources
$testObjects = Get-Objects $groups['tests'].ObjDir $groups['tests'].Sources
$stubObjects = Get-Objects $groups['stub'].ObjDir $groups['stub'].Sources

$libraries = @('ucrt.lib', 'user32.lib', 'gdi32.lib', 'comctl32.lib', 'comdlg32.lib', 'shell32.lib',
    'shlwapi.lib', 'ole32.lib', 'oleaut32.lib', 'advapi32.lib', 'uxtheme.lib', 'dwmapi.lib',
    'windowscodecs.lib', 'd2d1.lib', 'dwrite.lib', 'runtimeobject.lib', 'shcore.lib', 'winhttp.lib',
    'crypt32.lib', 'hid.lib', 'setupapi.lib', 'ws2_32.lib', 'bcrypt.lib', 'iphlpapi.lib')
$appExe = 'build\unpacked\cordinal.exe'
$testExe = 'build\unpacked\cordinal-test.exe'

if ($appLinked -or !(Test-Path -LiteralPath $appExe)) {
    Initialize-Msvc
    Write-Host 'Linking cordinal.exe'
    Invoke-Tool 'link.exe' (@('/nologo', '/LTCG', '/SUBSYSTEM:WINDOWS', '/MANIFEST:NO', '/OPT:REF',
            '/OPT:ICF', '/DEBUG:NONE', '/NODEFAULTLIB:libucrt.lib', '/MAP:build\cordinal.map',
            "/OUT:$appExe") + $appObjects + @($resPath) + $stubObjects + $chipObjects + $libraries)
    $testLinked = $true
}
if ($testLinked -or !(Test-Path -LiteralPath $testExe)) {
    Initialize-Msvc
    Write-Host 'Linking cordinal-test.exe'
    Invoke-Tool 'link.exe' (@('/nologo', '/LTCG', '/SUBSYSTEM:CONSOLE', '/MANIFEST:NO', '/OPT:REF',
            '/OPT:ICF', '/DEBUG:NONE', '/NODEFAULTLIB:libucrt.lib', "/OUT:$testExe") +
        $appObjects + @($resPath) + $testObjects + $chipObjects + $libraries)
    $testLinked = $true
}

# --- publish -----------------------------------------------------------------------------------

$modeChanged = $oldMode -ne $mode
$packingChanged = !$old -or $old.packing -ne $packingHash
if ($Unpacked) {
    if ($appLinked -or $modeChanged -or !(Test-Path -LiteralPath 'build\cordinal.exe')) {
        Copy-Item -LiteralPath $appExe -Destination 'build\cordinal.exe' -Force
    }
    if ($testLinked -or $modeChanged -or !(Test-Path -LiteralPath 'build\cordinal-test.exe')) {
        Copy-Item -LiteralPath $testExe -Destination 'build\cordinal-test.exe' -Force
    }
} elseif ($appLinked -or $testLinked -or $modeChanged -or $packingChanged -or
    !(Test-Path -LiteralPath 'build\cordinal.exe') -or
    !(Test-Path -LiteralPath 'build\cordinal-test.exe')) {
    Write-Host 'Packing the published pair'
    & (Join-Path $PSScriptRoot 'pack-release.ps1') -Experimental:$Experimental `
        -ExperimentalAddresses:$ExperimentalAddresses -Cdpack:$Cdpack -StockNrv:$StockNrv
}

# Sandbox workspaces can label new files Low integrity. Windows then launches even a double-clicked
# player at Low integrity, which cannot save its normal AppData profile. Publish at Medium instead.
foreach ($exe in @('build\cordinal.exe', 'build\cordinal-test.exe', 'build\unpacked\cordinal.exe',
        'build\unpacked\cordinal-test.exe')) {
    if (!(Test-Path -LiteralPath $exe)) { continue }
    & icacls.exe $exe /setintegritylevel M | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "icacls failed for $exe." }
}

# Record only what a complete build produced. A failure above leaves the previous record in place.
[ordered]@{
    version = 2
    script = $scriptHash
    toolset = $toolset
    mode = $mode
    themes = [bool]$Themes
    packing = $packingHash
    headers = $headerHash
    resources = $resourceHash
    sources = $sourceSetHash
    files = $hashes
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if (Test-Path -LiteralPath 'build\cordinal.exe') {
    $size = (Get-Item -LiteralPath 'build\cordinal.exe').Length
    Write-Host ("Built executable ({0}): {1:N0} bytes" -f $mode, $size)
    # Checked on every build, rebuilt or not, so an over-budget player keeps failing until it shrinks
    # or the budget is raised. A development build is not packed, so it has no budget.
    if (!$Unpacked -and $size -gt $SizeBudget) {
        Fail (('build\cordinal.exe is {0:N0} bytes, {1:N0} over its {2:N0}-byte budget. Make it smaller, or ' +
               'raise $SizeBudget in tools\build.ps1 and say why in docs\size.md.') -f $size, ($size - $SizeBudget), $SizeBudget)
    }
}
# The improvement loop's build observation (tools\optimization-checkpoint.ps1) is paused with the loop
# for the finishing release (AGENTS.md); run that script by hand to record one.
Write-Host ("Build finished in {0:N1}s." -f $timer.Elapsed.TotalSeconds)
