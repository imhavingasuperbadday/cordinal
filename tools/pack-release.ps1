# Pack the linked binaries without changing their machine code, resources or command surface.
param([string]$BuildDir, [switch]$Experimental, [switch]$ExperimentalAddresses,
      [switch]$Cdpack, [switch]$StockNrv)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'packing-mode.ps1')
$mode = Resolve-PackingMode -Experimental:$Experimental `
    -ExperimentalAddresses:$ExperimentalAddresses -Cdpack:$Cdpack -StockNrv:$StockNrv
$Experimental = $mode -eq 'experimental'
$ExperimentalAddresses = $mode -eq 'experimental-addresses'
if ($Cdpack) {
    & (Join-Path $PSScriptRoot 'pack-cdpack.ps1') -BuildDir $BuildDir
    return
}
$useResearch = $Experimental -or $ExperimentalAddresses
Import-Module Microsoft.PowerShell.Utility
Import-Module Microsoft.PowerShell.Archive
if (!$BuildDir) { $BuildDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build' }
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path

# Pin both the official archive and the executable extracted from it.
$version = '5.2.1'
$archiveHash = 'EABC6792A347D45E945BE7748423E7868FD01B0D2BCAA2F4B1031FD71FF69BDA'
$toolHash = 'D20EBE0B7B22B6BE968C8C34BE61F94DDEA12CB11462E2CEC27F548EF9574DF8'
$cache = Join-Path $BuildDir ('tools\upx-' + $version)
$upx = Join-Path $cache ('upx-' + $version + '-win64\upx.exe')
if (!(Test-Path -LiteralPath $upx) -or (Get-FileHash -LiteralPath $upx -Algorithm SHA256).Hash -ne $toolHash) {
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $archive = Join-Path $cache 'upx.zip'
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $archiveHash) {
        $url = "https://github.com/upx/upx/releases/download/v$version/upx-$version-win64.zip"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $archiveHash) { throw 'UPX archive checksum mismatch.' }
    Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
}
if ((Get-FileHash -LiteralPath $upx -Algorithm SHA256).Hash -ne $toolHash) { throw 'UPX executable checksum mismatch.' }
$verifier = $upx
$stockUpx = $upx
$packingMode = 'stock-nrv'
$codecOptions = @('--best')
if ($useResearch) {
    # Personal development only: the modified GPL stub does not have stock UPX's release exception.
    $repo = Split-Path -Parent $PSScriptRoot
    $upx = Join-Path (Split-Path -Parent $repo) 'upx\build\upx.exe'
    if (!(Test-Path -LiteralPath $upx)) { throw 'Build the sibling upx research checkout first.' }
    $toolHash = (Get-FileHash -LiteralPath $upx -Algorithm SHA256).Hash
    $versionLine = @(& $upx --version)[0]
    if ($LASTEXITCODE -ne 0 -or $versionLine -notmatch '^upx (\S+)') { throw 'Cannot identify research UPX.' }
    $version = $Matches[1]
    $packingMode = 'experimental'
    $researchOptions = Join-Path $PSScriptRoot 'research-packing.ps1'
    if (!(Test-Path -LiteralPath $researchOptions)) { throw 'The experimental modes need toolsesearch-packing.ps1.' }
    . $researchOptions
    if ($ExperimentalAddresses) {
        $packingMode = 'experimental-addresses'
        $verifier = $upx
    }
    Write-Host 'Experimental packer: local development only. The modified stub is GPL-2.0-or-later.'
}

function Read-Pe([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw "Invalid executable: $Path" }
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ($pe -lt 0 -or $pe + 264 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20B) {
        throw "Expected an x64 PE32+ image: $Path"
    }
    $sectionTable = $pe + 24 + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $sections = @()
    for ($i = 0; $i -lt [BitConverter]::ToUInt16($bytes, $pe + 6); ++$i) {
        $offset = $sectionTable + $i * 40
        $sections += [pscustomobject]@{
            Name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).TrimEnd([char]0)
            Size = [BitConverter]::ToUInt32($bytes, $offset + 8)
            Rva = [BitConverter]::ToUInt32($bytes, $offset + 12)
            RawSize = [BitConverter]::ToUInt32($bytes, $offset + 16)
            Raw = [BitConverter]::ToUInt32($bytes, $offset + 20)
            Flags = [BitConverter]::ToUInt32($bytes, $offset + 36)
        }
    }
    [pscustomobject]@{
        Bytes = $bytes; Header = $pe; Sections = $sections
        CodeBytes = [BitConverter]::ToUInt32($bytes, $pe + 28)
        DllCharacteristics = [BitConverter]::ToUInt16($bytes, $pe + 94)
        Subsystem = [BitConverter]::ToUInt16($bytes, $pe + 92)
    }
}

function Hash-Range([byte[]]$Bytes, [int]$Offset, [int]$Count) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($Bytes, $Offset, $Count)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Rva-Offset($Pe, [uint32]$Rva) {
    foreach ($section in $Pe.Sections) {
        if ($Rva -ge $section.Rva -and $Rva -lt $section.Rva + $section.RawSize) {
            return [int]($section.Raw + $Rva - $section.Rva)
        }
    }
    throw "RVA is outside file-backed sections: $Rva"
}

# Compare resource payloads by type/name/language: UPX relocates the directory entries. With
# --compress-resources=1 the packed payloads are compressed, so only a decompressed image can be
# compared byte for byte; the packed image is held to the same resource names instead.
function Resource-Hashes($Pe) {
    $resourceRva = [BitConverter]::ToUInt32($Pe.Bytes, $Pe.Header + 24 + 112 + 2 * 8)
    $hashes = @{}
    if ($resourceRva) {
        $root = Rva-Offset $Pe $resourceRva
        function Walk-Resources([int]$Offset, [string]$Prefix) {
            $at = $root + $Offset
            $count = [int][BitConverter]::ToUInt16($Pe.Bytes, $at + 12) + [BitConverter]::ToUInt16($Pe.Bytes, $at + 14)
            for ($i = 0; $i -lt $count; ++$i) {
                $entry = $at + 16 + $i * 8
                $name = [BitConverter]::ToUInt32($Pe.Bytes, $entry)
                if ($name -band 2147483648L) {
                    $text = $root + ($name -band 2147483647L)
                    $label = '@' + [Text.Encoding]::Unicode.GetString($Pe.Bytes, $text + 2, 2 * [BitConverter]::ToUInt16($Pe.Bytes, $text))
                } else { $label = '#' + $name }
                $path = $Prefix + '/' + $label
                $target = [BitConverter]::ToUInt32($Pe.Bytes, $entry + 4)
                if ($target -band 2147483648L) { Walk-Resources ($target -band 2147483647L) $path }
                else {
                    $data = $root + $target
                    $raw = Rva-Offset $Pe ([BitConverter]::ToUInt32($Pe.Bytes, $data))
                    $size = [BitConverter]::ToUInt32($Pe.Bytes, $data + 4)
                    $hashes[$path] = Hash-Range $Pe.Bytes $raw $size
                }
            }
        }
        Walk-Resources 0 ''
    }
    return $hashes
}

# The resource keys Explorer reads straight from the packed file: the first icon group (type 14)
# and the icon images it lists (type 3). UPX's --compress-icons=2 leaves exactly these raw.
function Shell-Icon-Keys($Pe) {
    $keys = @()
    $resourceRva = [BitConverter]::ToUInt32($Pe.Bytes, $Pe.Header + 24 + 112 + 2 * 8)
    if (!$resourceRva) { return $keys }
    $root = Rva-Offset $Pe $resourceRva
    function First-Child([int]$Offset) {  # the first entry of a directory: its label and target
        $at = $root + $Offset
        $name = [BitConverter]::ToUInt32($Pe.Bytes, $at + 16)
        [pscustomobject]@{ Label = '#' + $name; Target = [BitConverter]::ToUInt32($Pe.Bytes, $at + 20) }
    }
    $at = $root
    $count = [int][BitConverter]::ToUInt16($Pe.Bytes, $at + 12) + [BitConverter]::ToUInt16($Pe.Bytes, $at + 14)
    for ($i = 0; $i -lt $count; ++$i) {
        $entry = $at + 16 + $i * 8
        if ([BitConverter]::ToUInt32($Pe.Bytes, $entry) -ne 14) { continue }
        $group = First-Child ([BitConverter]::ToUInt32($Pe.Bytes, $entry + 4) -band 2147483647L)
        $language = First-Child ($group.Target -band 2147483647L)
        $data = $root + $language.Target
        $dir = Rva-Offset $Pe ([BitConverter]::ToUInt32($Pe.Bytes, $data))
        $keys += '/#14/' + $group.Label + '/' + $language.Label
        for ($k = 0; $k -lt [BitConverter]::ToUInt16($Pe.Bytes, $dir + 4); ++$k) {
            $keys += '/#3/#' + [BitConverter]::ToUInt16($Pe.Bytes, $dir + 6 + $k * 14 + 12) + '/'
        }
    }
    return $keys
}

function Fill-ProtectionTable($Pe) {
    if ([BitConverter]::ToUInt32($Pe.Bytes, $Pe.Header + 56) -ne 4096) { throw 'Expected 4096-byte section alignment.' }
    $regions = @($Pe.Sections | Where-Object { $_.Name -notin @('.rsrc', '.reloc') } | Sort-Object Rva)
    if (!$regions.Count -or $regions.Count -gt 16) { throw 'Image protection table capacity exceeded.' }
    $encoding = [Text.Encoding]::GetEncoding(28591)
    $marker = [byte[]](0x8C,0x53,0x21,0x97,0x6A,0xC4,0x48,0x5B,0xB0,0x1F,0x43,0x4F,0x52,0x44,0x01,0x00)
    $pattern = $encoding.GetString($marker)
    $image = $encoding.GetString($Pe.Bytes)
    $at = $image.IndexOf($pattern, [StringComparison]::Ordinal)
    if ($at -lt 0 -or $image.IndexOf($pattern, $at + 1, [StringComparison]::Ordinal) -ge 0) { throw 'Missing or duplicate image protection table.' }
    $at += 16
    if ([BitConverter]::ToUInt32($Pe.Bytes, $at) -ne 0) { throw 'Expected a freshly linked native executable.' }
    [BitConverter]::GetBytes([uint32]$regions.Count).CopyTo($Pe.Bytes, $at)
    $cursor = 0
    foreach ($region in $regions) {
        $execute = ($region.Flags -band 0x20000000) -ne 0
        $write = ($region.Flags -band 2147483648L) -ne 0
        if ($region.Rva -lt $cursor -or $region.Rva % 4096 -or !($region.Flags -band 0x40000000) -or ($execute -and $write)) {
            throw 'Unexpected native section layout or permissions.'
        }
        $protection = if ($execute) { 0x20 } elseif ($write) { 4 } else { 2 }
        $size = [uint32]([Math]::Ceiling([Math]::Max($region.Size, $region.RawSize) / 4096.0) * 4096)
        foreach ($value in @($region.Rva, $size, $protection)) {
            $at += 4
            [BitConverter]::GetBytes([uint32]$value).CopyTo($Pe.Bytes, $at)
        }
        $cursor = $region.Rva + $size
    }
    return $cursor
}

$stage = (New-Item -ItemType Directory -Path (Join-Path $BuildDir ('pack-' + [Guid]::NewGuid().ToString('N')))).FullName
$reports = @()
foreach ($name in @('cordinal.exe', 'cordinal-test.exe')) {
    if ($useResearch) {
        $codecOptions = Get-ResearchCodecOptions $name ([bool]$ExperimentalAddresses)
    }
    $nativePath = Join-Path $BuildDir ('unpacked\' + $name)
    $native = Read-Pe $nativePath
    $endOfNative = Fill-ProtectionTable $native
    $inputPath = Join-Path $stage ($name + '.native')
    $packedPath = Join-Path $stage $name
    $restoredPath = Join-Path $stage ($name + '.restored')
    [IO.File]::WriteAllBytes($inputPath, $native.Bytes)
    & $upx @codecOptions --compress-resources=1 --compress-icons=2 --strip-relocs=0 --overlay=copy -o $packedPath $inputPath
    if ($LASTEXITCODE -ne 0) { throw "UPX compression failed: $name" }
    & $upx -t $packedPath
    if ($LASTEXITCODE -ne 0) { throw "UPX integrity check failed: $name" }
    if ($ExperimentalAddresses) {
        # The new format must reject in stock before decoding; the research host restores it below.
        $rejectedPath = Join-Path $stage ($name + '.stock-rejected')
        $stdout = Join-Path $stage ($name + '.stock.stdout')
        $stderr = Join-Path $stage ($name + '.stock.stderr')
        $rejectArgs = @('-d', '-o', ('"{0}"' -f $rejectedPath), ('"{0}"' -f $packedPath))
        $rejected = Start-Process -FilePath $stockUpx -ArgumentList $rejectArgs -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru
        $message = (Get-Content -Raw -LiteralPath $stdout) + (Get-Content -Raw -LiteralPath $stderr)
        if ($rejected.ExitCode -eq 0 -or (Test-Path -LiteralPath $rejectedPath) -or
            $message -notmatch 'need a newer version of UPX') { throw "Stock UPX did not reject version 15 cleanly: $name" }
        Remove-Item -LiteralPath $stdout, $stderr
    } elseif ($Experimental) {
        & $verifier -t $packedPath
        if ($LASTEXITCODE -ne 0) { throw "Stock UPX interop check failed: $name" }
    }
    & $verifier -d -o $restoredPath $packedPath
    if ($LASTEXITCODE -ne 0) { throw "UPX round-trip failed: $name" }
    $packed = Read-Pe $packedPath
    $restored = Read-Pe $restoredPath
    if (($restored.Sections.Name -join ',') -ne ($native.Sections.Name -join ',') -or
        $restored.DllCharacteristics -ne $native.DllCharacteristics -or $restored.Subsystem -ne $native.Subsystem) {
        throw "Round-trip changed section order or security flags: $name"
    }
    $nativeEnd = [int](($native.Sections | ForEach-Object { $_.Raw + $_.RawSize } | Measure-Object -Maximum).Maximum)
    $restoredEnd = [int](($restored.Sections | ForEach-Object { $_.Raw + $_.RawSize } | Measure-Object -Maximum).Maximum)
    $overlaySize = $native.Bytes.Length - $nativeEnd
    if ($overlaySize -ne $restored.Bytes.Length - $restoredEnd -or
        (Hash-Range $native.Bytes $nativeEnd $overlaySize) -ne
        (Hash-Range $restored.Bytes $restoredEnd $overlaySize)) { throw "Round-trip changed overlay: $name" }
    if (($packed.Sections.Name -join ',') -ne 'UPX0,UPX1,.rsrc' -or
        ($packed.Sections | Where-Object Name -eq '.rsrc').Rva -lt $endOfNative -or
        $packed.DllCharacteristics -ne $native.DllCharacteristics -or $packed.Subsystem -ne $native.Subsystem) {
        throw "Packed image layout or security flags changed unexpectedly: $name"
    }
    foreach ($section in $native.Sections | Where-Object { $_.Name -in @('.text', '.data', '.pdata', '.reloc') }) {
        $copy = $restored.Sections | Where-Object Name -eq $section.Name
        if (!$copy -or $section.Rva -ne $copy.Rva -or $section.RawSize -ne $copy.RawSize -or
            (Hash-Range $native.Bytes $section.Raw $section.RawSize) -ne (Hash-Range $restored.Bytes $copy.Raw $copy.RawSize)) {
            throw "Round-trip changed $($section.Name): $name"
        }
    }
    # The packed copy keeps compressed payloads, so its resource names are checked here and the
    # restored copy proves that compression round-trips every payload back to the linked bytes.
    $resources = Resource-Hashes $native
    $restoredResources = Resource-Hashes $restored
    if ($restoredResources.Count -ne $resources.Count) { throw "Resource count changed: $name" }
    foreach ($key in $resources.Keys) {
        if ($restoredResources[$key] -ne $resources[$key]) { throw "Resource payload changed ($key): $name" }
    }
    $packedResources = Resource-Hashes $packed
    $packedNames = @($packedResources.Keys | Sort-Object)
    $nativeNames = @($resources.Keys | Sort-Object)
    if (Compare-Object $nativeNames $packedNames) { throw "Packed resource set changed: $name" }
    # RCDATA (type 10) and the icons only the running player loads are compressed; the shell's
    # icon, version info and the manifest must stay readable straight from the packed file.
    $shellIcons = Shell-Icon-Keys $native
    foreach ($key in $resources.Keys) {
        $raw = $key -like '/#16/*' -or $key -like '/#24/*' -or
               @($shellIcons | Where-Object { $key -eq $_ -or $key.StartsWith($_) }).Count -gt 0
        if ($raw -and $packedResources[$key] -ne $resources[$key]) {
            throw "Packed resource payload changed ($key): $name"
        }
    }
    if ($shellIcons.Count -lt 2) { throw "The shell icon group was not found: $name" }
    $reports += [pscustomobject][ordered]@{
        Executable = $name; NativeExeBytes = $native.Bytes.LongLength; NativeCodeBytes = $native.CodeBytes
        PackedExeBytes = $packed.Bytes.LongLength; UpxVersion = $version; ResourcePayloads = $resources.Count
        PackingMode = $packingMode; PackerSha256 = $toolHash; PackerPath = $upx
        FastBytes = if ($useResearch) { $fastBytes } else { $null }
        MatchFinderCycles = if ($useResearch) { $matchFinderCycles } else { $null }
        CodeDataUnwindRelocationsMatch = $true; ResourcePayloadsMatch = $true; SecurityFlagsMatch = $true
        Sha256 = (Get-FileHash -LiteralPath $packedPath -Algorithm SHA256).Hash
    }
}
# Publish the pair only after every integrity check succeeds.
foreach ($report in $reports) {
    Copy-Item -LiteralPath (Join-Path $stage $report.Executable) -Destination (Join-Path $BuildDir $report.Executable) -Force
}
$reports | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $BuildDir 'packing.json') -Encoding UTF8
foreach ($name in @('cordinal.exe', 'cordinal-test.exe')) {
    foreach ($suffix in @('', '.native', '.restored')) { Remove-Item -LiteralPath (Join-Path $stage ($name + $suffix)) }
}
Remove-Item -LiteralPath $stage
& (Join-Path $PSScriptRoot 'check-size.ps1') -Exe (Join-Path $BuildDir 'cordinal.exe') -UnpackedExe (Join-Path $BuildDir 'unpacked\cordinal.exe')
