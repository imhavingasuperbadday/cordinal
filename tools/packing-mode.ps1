# Share the default and conflict checks between the build and direct packing entry points.
function Resolve-PackingMode {
    param([switch]$Unpacked, [switch]$Experimental, [switch]$ExperimentalAddresses,
          [switch]$Cdpack, [switch]$StockNrv)
    $selected = @()
    if ($Unpacked) { $selected += 'unpacked' }
    if ($Experimental) { $selected += 'experimental' }
    if ($ExperimentalAddresses) { $selected += 'experimental-addresses' }
    if ($Cdpack) { $selected += 'cdpack' }
    if ($StockNrv) { $selected += 'pack' }
    if ($selected.Count -gt 1) { throw 'Choose one build mode.' }
    if ($selected.Count) { return $selected[0] }
    # The experimental packer needs the sibling research checkout; a checkout without one (any other
    # machine) builds the public release instead.
    if (Test-Path -LiteralPath (Get-ResearchPacker)) { return 'experimental-addresses' }
    return 'pack'
}
function Get-ResearchPacker {
    return Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'upx\build\upx.exe'
}
