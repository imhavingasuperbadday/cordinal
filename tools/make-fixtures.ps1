# Generates the self-test fixtures (needs ffmpeg on PATH):
#   pwsh -File tools\make-fixtures.ps1 -Out C:\path\to\fixtures
#   build\cordinal-test.exe --selftest C:\path\to\fixtures C:\path\to\selftest.log
param([Parameter(Mandatory = $true)][string]$Out)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function FFmpeg([string[]]$a) {
    & ffmpeg.exe -hide_banner -loglevel error -y @a
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE): $a" }
}
function Subdir([string]$name) { (New-Item -ItemType Directory -Force (Join-Path $Out $name)).FullName }
function Sine([double]$seconds) { 'aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=44100:d=' + $seconds }

$codecs = @{ flac = 'flac'; mp3 = 'libmp3lame'; m4a = 'aac'; ogg = 'libvorbis'; opus = 'libopus'; wav = 'pcm_s16le'; wma = 'wmav2' }

# media\: 8 s sines in every format; all but WAV and Opus carry the test tags.
$media = Subdir 'media'
$tags = @('-metadata', 'title=Sine Ünïcode', '-metadata', 'artist=Test Artist', '-metadata', 'album=Test Album', '-metadata', 'track=7')
foreach ($ext in $codecs.Keys) {
    $t = if ($ext -in 'wav', 'opus') { @() } else { $tags }
    $id3 = if ($ext -eq 'mp3') { @('-id3v2_version', '3') } else { @() }
    FFmpeg (@('-f', 'lavfi', '-i', (Sine 8), '-c:a', $codecs[$ext]) + $id3 + $t + "$media\sine.$ext")
}
FFmpeg (@('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'alac') + $tags + "$media\alac.m4a")
$native = Subdir 'native'
FFmpeg @('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'wavpack', "$native\sine.wv")
FFmpeg @('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'ac3', "$native\sine.ac3")
FFmpeg @('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'eac3', '-b:a', '192k', "$native\sine.eac3")
FFmpeg @('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'tta', "$native\sine.tta")
FFmpeg @('-f', 'lavfi', '-i', (Sine 8), '-c:a', 'flac', "$native\sine.mka")
# A longer native FLAC without a seek table, with a rising envelope for whole-track previews.
FFmpeg @('-f', 'lavfi', '-i', 'aevalsrc=(0.1+0.02*t)*sin(2*PI*440*t)|(0.1+0.02*t)*sin(2*PI*440*t):s=44100:d=20',
         '-c:a', 'flac', "$native\preview.flac")
# A video track before the AAC track exercises MP4 audio selection and edit-list trimming.
FFmpeg @('-f', 'lavfi', '-i', 'color=c=black:s=16x16:r=1:d=8', '-f', 'lavfi', '-i', (Sine 8),
         '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'mpeg4', '-c:a', 'aac', '-shortest', "$native\video-aac.mp4")

# gap2\: a 4 s chirp (never periodic within the lag search) split exactly at 1.5 s into a + b.
$gap = Subdir 'gap2'
FFmpeg @('-f', 'lavfi', '-i', 'aevalsrc=0.5*sin(2*PI*(200*t+150*t*t))|0.5*sin(2*PI*(200*t+150*t*t)):s=44100:d=4',
         '-c:a', 'pcm_s16le', "$gap\ref.wav")
foreach ($ext in 'flac', 'mp3', 'm4a', 'ogg') {
    foreach ($part in @(@('a', 0, 66150), @('b', 66150, 176400))) {
        FFmpeg @('-i', "$gap\ref.wav", '-af', "atrim=start_sample=$($part[1]):end_sample=$($part[2]),asetpts=PTS-STARTPTS",
                 '-c:a', $codecs[$ext], "$gap\$($part[0]).$ext")
    }
}

# rg\: ReplayGain tags (ID3 TXXX for MP3), and an Opus R128 gain on the stream's comment header.
$rg = Subdir 'rg'
$rgTags = @('-metadata', 'REPLAYGAIN_TRACK_GAIN=-6.50 dB', '-metadata', 'REPLAYGAIN_TRACK_PEAK=0.988000',
            '-metadata', 'REPLAYGAIN_ALBUM_GAIN=-7.25 dB', '-metadata', 'REPLAYGAIN_ALBUM_PEAK=0.995000')
foreach ($ext in 'flac', 'mp3', 'ogg') {
    $id3 = if ($ext -eq 'mp3') { @('-id3v2_version', '3') } else { @() }
    FFmpeg (@('-f', 'lavfi', '-i', (Sine 2), '-c:a', $codecs[$ext]) + $id3 + $rgTags + "$rg\rg.$ext")
}
FFmpeg @('-f', 'lavfi', '-i', (Sine 2), '-c:a', 'libopus', '-metadata:s:a:0', 'R128_TRACK_GAIN=-1408', "$rg\rg.opus")

# tags2\: Extended metadata fields (composer, comment, grouping, label, disc, bpm, compilation, lyrics)
$tags2 = Subdir 'tags2'
# Common metadata for Vorbis-based formats (FLAC, OGG use Vorbis comments directly)
$vorbisExt = @('-metadata', 'composer=Test Composer', '-metadata', 'comment=Test Comment',
               '-metadata', 'grouping=Test Group', '-metadata', 'label=Test Label',
               '-metadata', 'track=3/12', '-metadata', 'disc=2/3', '-metadata', 'BPM=128',
               '-metadata', 'compilation=1', '-metadata', 'lyrics=La la la')
# M4A uses different metadata keys
$m4aExt = @('-metadata', 'composer=Test Composer', '-metadata', 'comment=Test Comment',
            '-metadata', 'grouping=Test Group',
            '-metadata', 'track=3/12', '-metadata', 'disc=2/3', '-metadata', 'tmpo=128',
            '-metadata', 'compilation=1', '-metadata', 'lyrics=La la la')
# MP3/ID3 uses different keys
$mp3Ext = @('-metadata', 'composer=Test Composer', '-metadata', 'comment=Test Comment',
            '-metadata', 'grouping=Test Group', '-metadata', 'publisher=Test Label',
            '-metadata', 'track=3/12', '-metadata', 'disc=2/3', '-metadata', 'BPM=128',
            '-metadata', 'compilation=1', '-metadata', 'lyrics=La la la')
# FLAC
FFmpeg (@('-f', 'lavfi', '-i', (Sine 2), '-c:a', 'flac') + $vorbisExt + "$tags2\full.flac")
# MP3 with ID3v2.3
FFmpeg (@('-f', 'lavfi', '-i', (Sine 2), '-c:a', 'libmp3lame', '-id3v2_version', '3') + $mp3Ext + "$tags2\full.mp3")
# M4A (label is omitted; ffmpeg does not have a standard mapping for label/publisher in M4A)
FFmpeg (@('-f', 'lavfi', '-i', (Sine 2), '-c:a', 'aac') + $m4aExt + "$tags2\full.m4a")
# OGG
FFmpeg (@('-f', 'lavfi', '-i', (Sine 2), '-c:a', 'libvorbis') + $vorbisExt + "$tags2\full.ogg")

# native\: the AU, CAF, VOC, Ogg FLAC and 8SVX decoders. Each lossless file is made from
# media\sine.wav, so it must decode to exactly those samples. For the lossy or narrower ones, FFmpeg's
# own decode is written beside them as a 16-bit reference ("-ref.wav"), except where noted.
$sineWav = "$media\sine.wav"
FFmpeg @('-i', $sineWav, '-c:a', 'pcm_s16be', "$native\sine.au")
FFmpeg @('-i', $sineWav, '-c:a', 'pcm_s24le', "$native\sine.caf")
FFmpeg @('-i', $sineWav, '-c:a', 'alac', "$native\alac.caf")
FFmpeg @('-i', $sineWav, '-c:a', 'wavpack', "$native\pcm16.wv")  # integer WavPack; sine.wv above is float
FFmpeg @('-i', $sineWav, '-c:a', 'flac', "$native\sine.oga")
FFmpeg @('-i', $sineWav, '-c:a', 'pcm_s16le', "$native\sine.voc")
FFmpeg @('-i', $sineWav, '-c:a', 'pcm_mulaw', "$native\ulaw.au")
FFmpeg @('-i', $sineWav, '-c:a', 'adpcm_ima_qt', "$native\ima4.caf")
FFmpeg @('-i', $sineWav, '-c:a', 'adpcm_ima_qt', "$native\ima4.aifc")
FFmpeg @('-i', $sineWav, '-c:a', 'pcm_u8', "$native\u8.voc")

# 8SVX, which FFmpeg reads but cannot write: a mono Fibonacci-delta sample and an uncompressed
# stereo one, 1 s at 22050 Hz, with NAME and AUTH text. Big-endian IFF chunks, padded to even sizes.
function BE32([int64]$v) { [byte[]]@((($v -shr 24) -band 255), (($v -shr 16) -band 255), (($v -shr 8) -band 255), ($v -band 255)) }
function Chunk([string]$id, [byte[]]$data) {
    $pad = if ($data.Length % 2) { [byte[]]@(0) } else { [byte[]]@() }
    [byte[]]([Text.Encoding]::ASCII.GetBytes($id) + (BE32 $data.Length) + $data + $pad)
}
function Write8svx([string]$path, [int]$rate, [int[][]]$channels, [int]$compression) {
    $fib = @(-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21)
    $bodies = foreach ($samples in $channels) {
        if (!$compression) { , [byte[]]($samples | ForEach-Object { $_ -band 255 }) ; continue }
        # Greedy Fibonacci-delta coding that never leaves the signed 8-bit range.
        $out = [Collections.Generic.List[byte]]::new(); $out.Add(0); $out.Add($samples[0] -band 255)
        $value = $samples[0]; $codes = @()
        foreach ($target in $samples) {
            $best = 8
            for ($code = 0; $code -lt 16; ++$code) {
                $next = $value + $fib[$code]
                if ($next -ge -128 -and $next -le 127 -and [Math]::Abs($target - $next) -lt [Math]::Abs($target - ($value + $fib[$best]))) { $best = $code }
            }
            $value += $fib[$best]; $codes += $best; $script:decoded += $value
        }
        for ($i = 0; $i -lt $codes.Count; $i += 2) { $out.Add(($codes[$i] -shl 4) -bor $codes[$i + 1]) }
        , $out.ToArray()
    }
    $frames = $channels[0].Length
    $vhdr = [byte[]]((BE32 $frames) + (BE32 0) + (BE32 0) + [byte[]]@((($rate -shr 8) -band 255), ($rate -band 255), 1, $compression) + (BE32 65536))
    $body = [byte[]]@(); foreach ($b in $bodies) { $body += $b }
    $form = [byte[]]([Text.Encoding]::ASCII.GetBytes('8SVX') + (Chunk 'VHDR' $vhdr) + (Chunk 'NAME' ([Text.Encoding]::ASCII.GetBytes('Test Sample'))) +
                     (Chunk 'AUTH' ([Text.Encoding]::ASCII.GetBytes('Test Artist'))))
    if ($channels.Count -eq 2) { $form += Chunk 'CHAN' (BE32 6) }
    $form += Chunk 'BODY' $body
    [IO.File]::WriteAllBytes($path, [byte[]]([Text.Encoding]::ASCII.GetBytes('FORM') + (BE32 $form.Length) + $form))
}
$left = [int[]](0..22049 | ForEach-Object { [int][Math]::Round(100 * [Math]::Sin(2 * [Math]::PI * 440 * $_ / 22050)) })
$right = [int[]](0..22049 | ForEach-Object { [int][Math]::Round(60 * [Math]::Sin(2 * [Math]::PI * 660 * $_ / 22050)) })
$script:decoded = @()
Write8svx "$native\fib.8svx" 22050 @(, $left) 1
Write8svx "$native\stereo.8svx" 22050 @($left, $right) 0
# The Fibonacci reference is the encoder's own reconstruction, which follows the standard's
# unpacking (high nibble first). FFmpeg reads the low nibble first, so its decode cannot serve.
$pcm = [byte[]]@(); foreach ($v in $script:decoded) { $pcm += [byte]0; $pcm += [byte]($v -band 255) }  # 16-bit LE: v * 256
function LE32([int64]$v) { [byte[]]@(($v -band 255), (($v -shr 8) -band 255), (($v -shr 16) -band 255), (($v -shr 24) -band 255)) }
$fmt = [byte[]](@(1, 0, 1, 0) + (LE32 22050) + (LE32 44100) + @(2, 0, 16, 0))
[IO.File]::WriteAllBytes("$native\fib-8svx-ref.wav", [byte[]]([Text.Encoding]::ASCII.GetBytes('RIFF') + (LE32 (36 + $pcm.Length)) +
    [Text.Encoding]::ASCII.GetBytes('WAVEfmt ') + (LE32 16) + $fmt + [Text.Encoding]::ASCII.GetBytes('data') + (LE32 $pcm.Length) + $pcm))
foreach ($name in 'ulaw.au', 'ima4.caf', 'ima4.aifc', 'u8.voc', 'stereo.8svx') {
    FFmpeg @('-i', "$native\$name", '-c:a', 'pcm_s16le', "$native\$($name -replace '\.', '-')-ref.wav")
}

# native\: the speech and game codecs (G.722, GSM 06.10, DFPWM, CRI ADX), each
# from a 3 s chirp at its own rate, with FFmpeg's decode as the 16-bit reference beside it.
function Chirp([int]$rate, [string]$second = '') { "aevalsrc=0.4*sin(2*PI*(200*t+300*t*t))$second`:s=$rate`:d=3" }
FFmpeg @('-f', 'lavfi', '-i', (Chirp 16000), '-c:a', 'g722', "$native\g722.wav")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 16000), '-c:a', 'g722', "$native\g722.au")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 16000), '-c:a', 'g722', '-f', 'g722', "$native\raw.g722")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 8000), '-c:a', 'libgsm', "$native\raw.gsm")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 8000), '-c:a', 'libgsm_ms', "$native\gsm.wav")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 48000), '-c:a', 'dfpwm', '-f', 'dfpwm', "$native\raw.dfpwm")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 48000), '-c:a', 'dfpwm', "$native\dfpwm.wav")
FFmpeg @('-f', 'lavfi', '-i', (Chirp 44100 '|0.3*sin(2*PI*660*t)'), '-c:a', 'adpcm_adx', "$native\stereo.adx")
foreach ($name in 'g722.wav', 'g722.au', 'raw.gsm', 'gsm.wav', 'dfpwm.wav', 'stereo.adx') {
    FFmpeg @('-i', "$native\$name", '-c:a', 'pcm_s16le', "$native\$($name -replace '\.', '-')-ref.wav")
}
# Raw streams name their format to FFmpeg.
FFmpeg @('-f', 'g722', '-i', "$native\raw.g722", '-c:a', 'pcm_s16le', "$native\raw-g722-ref.wav")
FFmpeg @('-f', 'dfpwm', '-sample_rate', '48000', '-ch_layout', 'mono', '-i', "$native\raw.dfpwm", '-c:a', 'pcm_s16le', "$native\raw-dfpwm-ref.wav")

Write-Host "Fixtures written to $Out"
