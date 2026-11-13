# Build only our own sources with the installed compiler; no vendor or UPX stub toolchain.
param([string]$Out)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$Out) { $Out = Join-Path $repo 'build\cdpack' }
$Out = (New-Item -ItemType Directory -Force -Path $Out).FullName
$compiler = (Get-Command clang++.exe -ErrorAction Stop).Source
# CDPACK_PYTHON names the interpreter (release CI sets it); otherwise the py launcher's 3.14.
$python = if ($env:CDPACK_PYTHON) { $env:CDPACK_PYTHON } else { (Get-Command py.exe -ErrorAction Stop).Source }
# @() outside the if: PowerShell unrolls a one-element array an if returns, and splatting the bare
# string '-3.14' passes it one character at a time, so Python read a script from stdin and waited.
$pythonArgs = @(if (!$env:CDPACK_PYTHON) { '-3.14' })
$flags = @('-std=c++20', '-O2', '-Wall', '-Wextra')
& $compiler @flags (Join-Path $repo 'cdpack/tests/decode.cpp') '-o' (Join-Path $Out 'decode.exe')
if ($LASTEXITCODE) { throw 'Host decoder build failed.' }
$stubFlags = $flags + @('-ffreestanding', '-fno-builtin', '-fno-exceptions', '-fno-rtti',
    '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-fno-jump-tables',
    '-mno-red-zone', '-mgeneral-regs-only', '-fstack-usage', '-nostdlib',
    '-Wl,--entry,cdp_entry,--image-base,0,--section-alignment,16,--file-alignment,16,--no-seh,--disable-auto-import,--no-insert-timestamp,--build-id=none')
& $compiler @stubFlags (Join-Path $repo 'cdpack/src/stub/loader.cpp') '-o' (Join-Path $Out 'loader.exe')
if ($LASTEXITCODE) { throw 'Freestanding loader build failed.' }
& $python @pythonArgs (Join-Path $repo 'cdpack/tools/pack.py') check-loader (Join-Path $Out 'loader.exe')
if ($LASTEXITCODE) { throw 'Loader is not self-contained.' }
foreach ($kind in @('exe', 'dll')) {
    $fixtureFlags = @('-std=c++20', '-O2', '-fno-stack-protector', '-nostdlib',
        '-Wl,--entry,fixture_entry,--no-insert-timestamp,--delayload=version.dll')
    if ($kind -eq 'dll') { $fixtureFlags += @('-DCDP_TEST_DLL', '-shared') }
    & $compiler @fixtureFlags (Join-Path $repo 'cdpack/tests/pe_fixture.cpp') '-o' (Join-Path $Out "fixture.$kind") '-lkernel32' '-lversion'
    if ($LASTEXITCODE) { throw "PE $kind fixture build failed." }
}
& $compiler @flags (Join-Path $repo 'cdpack/tests/dll_host.cpp') '-o' (Join-Path $Out 'dll-host.exe')
if ($LASTEXITCODE) { throw 'DLL host build failed.' }
