# Report shipping/native sizes. Optional decimal-byte budgets use 0 for no limit.
param(
    [string]$Exe,
    [string]$UnpackedExe,
    [ValidateRange(0, [long]::MaxValue)][long]$Budget = 0,
    [ValidateRange(0, [long]::MaxValue)][long]$CodeBudget = 0,
    [switch]$Json
)
$ErrorActionPreference = 'Stop'
if (!$Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\cordinal.exe' }
function Read-Size([string]$File) {
    $path = (Resolve-Path -LiteralPath $File).Path
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw 'Not a PE executable.' }
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ($pe -lt 0 -or $pe + 32 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw 'Invalid PE header.' }
    $sections = $pe + 24 + [BitConverter]::ToUInt16($bytes, $pe + 20)
    if ($sections + 8 -gt $bytes.Length) { throw 'Invalid PE section table.' }
    [pscustomobject]@{
        Path = $path; Bytes = $bytes.LongLength; Code = [BitConverter]::ToUInt32($bytes, $pe + 28)
        Packed = [Text.Encoding]::ASCII.GetString($bytes, $sections, 4) -in @('UPX0', 'CDP0')
    }
}
$file = Read-Size $Exe
if (!$UnpackedExe -and $file.Packed) {
    $candidate = Join-Path (Split-Path -Parent $file.Path) ('unpacked\' + (Split-Path -Leaf $file.Path))
    if (Test-Path -LiteralPath $candidate) { $UnpackedExe = $candidate }
}
$native = if ($UnpackedExe) { Read-Size $UnpackedExe } elseif (!$file.Packed) { $file }
if ($native -and $native.Packed) { throw 'UnpackedExe must be a native, unpacked build.' }
$report = [pscustomobject][ordered]@{
    Executable = $file.Path
    ExeBytes = $file.Bytes
    Packed = $file.Packed
    NativeExeBytes = if ($native) { $native.Bytes } else { $null }
    CodeSectionBytes = if ($native) { $native.Code } else { $null }
    FileCodeFieldBytes = $file.Code
    BudgetBytes = $Budget
    CodeBudgetBytes = $CodeBudget
    WithinBudget = !$Budget -or $file.Bytes -le $Budget
    WithinCodeBudget = if ($native) { !$CodeBudget -or $native.Code -le $CodeBudget } else { $null }
}
if ($Json) { $report | ConvertTo-Json }
else { $report | Format-List }
