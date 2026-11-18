#!/usr/bin/env bash
# Runs the libFuzzer targets of tools/fuzz-media.cpp, each for SECONDS, under the AddressSanitizer
# and UndefinedBehaviorSanitizer they are built with (CMake -DCORDINAL_FUZZ=ON). CI runs it; on
# Windows, `.\linux.bat test -Suite sanitize` does too.
#
#   tools/fuzz-media.sh BUILD_DIR OUT_DIR [SECONDS] [TARGET...]
#
# Seeds are short files FFmpeg makes in OUT_DIR/seeds (tagged, with chapters and pictures), plus the
# self-test fixtures of build/fixtures when that folder exists (tools/make-fixtures.ps1 makes it on
# Windows). Targets that take several containers read the first byte as the choice, so their seeds
# carry that byte. New inputs go to OUT_DIR/corpus/<target>; a crash, leak, timeout or finding goes
# to OUT_DIR/crashes/<target>/ with its report in OUT_DIR/<target>.log, and the script fails.
# Reproduce one with: BUILD_DIR/fuzz-<target> OUT_DIR/crashes/<target>/<file>
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${1:?usage: fuzz-media.sh BUILD_DIR OUT_DIR [SECONDS] [TARGET...]}
OUT=${2:?usage: fuzz-media.sh BUILD_DIR OUT_DIR [SECONDS] [TARGET...]}
SECONDS_EACH=${3:-60}
shift $(($# < 3 ? $# : 3))
targets=("$@")
((${#targets[@]})) || targets=(flac mp3 opus vorbis aac tags art decoders)
for t in "${targets[@]}"; do [[ -x $BUILD/fuzz-$t ]] || { echo "ERROR: $BUILD/fuzz-$t is missing; build with -DCORDINAL_FUZZ=ON" >&2; exit 2; }; done
command -v ffmpeg >/dev/null || { echo 'ERROR: ffmpeg makes the seeds; install it' >&2; exit 2; }
mkdir -p -- "$OUT"
OUT=$(cd -- "$OUT" && pwd)

# ---- Seeds ----
S=$OUT/seeds/files
mkdir -p "$S"
ff() { ffmpeg -nostdin -hide_banner -loglevel error -y "$@" || echo "note: ffmpeg could not make ${*: -1}"; }
sine='aevalsrc=0.5*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=44100:d=0.3'
speech='aevalsrc=0.4*sin(2*PI*(200*t+300*t*t)):s=16000:d=0.5'
tags=(-metadata title='Seed title' -metadata artist='Seed artist' -metadata album='Seed album'
      -metadata track=3/9 -metadata date=2001 -metadata genre=Test -metadata lyrics='la la la')
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=100\ntitle=One\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=100\nEND=300\ntitle=Two\n' >"$S/chapters.txt"
for image in png jpg bmp gif; do ff -f lavfi -i 'testsrc=s=16x12:d=1' -frames:v 1 "$S/cover.$image"; done
ff -f lavfi -i 'testsrc=s=24x24:d=1' -frames:v 1 -q:v 2 -huffman 0 "$S/progressive.jpg"
picture() {  # picture CODEC OUTPUT: audio with a front cover and tags
    ff -f lavfi -i "$sine" -i "$S/cover.png" -map 0:a -map 1:v -c:a "$1" -c:v copy -disposition:v attached_pic \
        "${tags[@]}" "$2"
}
ff -f lavfi -i "$sine" -c:a flac "${tags[@]}" "$S/tags.flac"
ff -f lavfi -i "$sine" -c:a flac -sample_fmt s32 -ar 96000 "$S/hires.flac"
ff -f lavfi -i "$sine" -i "$S/chapters.txt" -map_metadata 1 -map_chapters 1 -c:a flac "$S/chapters.flac"
picture flac "$S/art.flac"
ff -f lavfi -i "$sine" -c:a libmp3lame -b:a 128k -id3v2_version 3 "${tags[@]}" "$S/tags.mp3"
ff -f lavfi -i "$sine" -c:a libmp3lame -q:a 4 -id3v2_version 4 "$S/vbr.mp3"
ff -f lavfi -i "$sine" -i "$S/chapters.txt" -map_metadata 1 -map_chapters 1 -c:a libmp3lame -id3v2_version 3 "$S/chapters.mp3"
picture libmp3lame "$S/art.mp3"
ff -f lavfi -i "$sine" -c:a libopus -b:a 96k "${tags[@]}" "$S/music.opus"
ff -f lavfi -i "$speech" -c:a libopus -b:a 16k -application voip "$S/speech.opus"
ff -f lavfi -i "$sine" -c:a libopus -b:a 32k -frame_duration 60 "$S/hybrid.opus"
ff -f lavfi -i "$sine" -c:a libvorbis -metadata LOOPSTART=1000 -metadata LOOPLENGTH=2000 "${tags[@]}" "$S/loop.ogg"
ff -f lavfi -i "$speech" -c:a libvorbis -q:a 0 "$S/mono.ogg"
ff -f lavfi -i "$sine" -c:a aac -f adts "$S/short.aac"
ff -f lavfi -i "$sine" -c:a aac "${tags[@]}" "$S/tags.m4a"
ff -f lavfi -i "$sine" -c:a alac "$S/alac.m4a"
picture aac "$S/art.m4a"
ff -f lavfi -i "$sine" -c:a pcm_s24le "${tags[@]}" "$S/tags.wav"
ff -f lavfi -i "$sine" -c:a pcm_s16be -write_id3v2 1 "${tags[@]}" "$S/tags.aiff"
ff -f lavfi -i "$sine" -c:a wmav2 "${tags[@]}" "$S/tags.wma"
ff -f lavfi -i "$sine" -c:a flac "${tags[@]}" "$S/tags.mka"
ff -f lavfi -i "$sine" -c:a wavpack "${tags[@]}" "$S/tags.wv"
ff -f lavfi -i "$sine" -c:a pcm_s16be "$S/short.au"
ff -f lavfi -i "$sine" -c:a pcm_s16le "$S/short.caf"
ff -f lavfi -i "$sine" -c:a alac "$S/alac.caf"
ff -f lavfi -i "$sine" -c:a pcm_u8 -ac 1 "$S/short.voc"
ff -f lavfi -i "$sine" -c:a adpcm_adx "$S/short.adx"
ff -f lavfi -i "$speech" -c:a g722 -f g722 "$S/speech.g722"
ff -f lavfi -i "$speech" -ar 8000 -c:a libgsm -f gsm "$S/speech.gsm"
ff -f lavfi -i "$speech" -ar 48000 -c:a dfpwm -f dfpwm "$S/speech.dfpwm"
ff -f lavfi -i "$sine" -c:a mp2 "$S/short.mp2"
ff -f lavfi -i "$sine" -c:a flac -f ogg "$S/flac.oga"
ff -f lavfi -i "$sine" -c:a tta "$S/short.tta"
ff -f lavfi -i "$sine" -c:a ac3 "$S/short.ac3"
ff -f lavfi -i "$sine" -c:a eac3 "$S/short.eac3"
ff -f lavfi -i "$sine" -c:a libvorbis "$S/vorbis.mka"
rm -f "$S/chapters.txt"

# The fixtures the Windows self-test uses, where there are some, kept to the small ones.
fixtures=$ROOT/build/fixtures
extra=()
if [[ -d $fixtures ]]; then mapfile -t extra < <(find "$fixtures" -type f -size -1024k ! -name '*-ref.wav' 2>/dev/null); fi

# seed TARGET CHOICE FILE: copy FILE into TARGET's seeds, after the choice byte when CHOICE >= 0.
seed() {
    local dir=$OUT/seeds/$1 name
    mkdir -p "$dir"
    name=$(basename -- "$3")
    if (($2 >= 0)); then { printf "\\$(printf '%03o' "$2")"; cat -- "$3"; } >"$dir/$name"
    else cp -- "$3" "$dir/$name"; fi
}
# The extension lists of tools/fuzz-media.cpp's kTargets, in order.
tag_exts=(mp3 flac ogg opus m4a wav aiff wma mka wv ape mpc dsf cord)
art_exts=(flac mp3 m4a ogg opus wma mka img)
decoder_exts=(wav aiff au caf voc 8svx adx g722 gsm dfpwm mp2 oga wv ape mpc tta ac3 eac3 mka wma dsf dff cord
              mod s3m xm it mid spc nsf gbs vgm)
index_of() { local want=$1 i=0; shift; for e in "$@"; do [[ $e == "$want" ]] && { echo $i; return; }; i=$((i + 1)); done; echo -1; }
for f in "$S"/* "${extra[@]}"; do
    [[ -s $f ]] || continue
    ext=${f##*.}; ext=${ext,,}
    case $ext in
        flac) seed flac -1 "$f" ;; mp3) seed mp3 -1 "$f" ;; opus) seed opus -1 "$f" ;; ogg) seed vorbis -1 "$f" ;;
        aac) seed aac 0 "$f" ;; m4a) seed aac 1 "$f" ;;
    esac
    i=$(index_of "$ext" "${decoder_exts[@]}"); ((i < 0)) || seed decoders "$i" "$f"
    i=$(index_of "$ext" "${tag_exts[@]}"); ((i < 0)) || seed tags "$i" "$f"
    case $ext in png|jpg|bmp|gif) i=$(index_of img "${art_exts[@]}") ;; *) i=$(index_of "$ext" "${art_exts[@]}") ;; esac
    ((i < 0)) || seed art "$i" "$f"
done

# ---- Run ----
# Findings that libFuzzer can't see itself abort (tools/fuzz-media.cpp). A case may take a while
# under the sanitizers, so the timeout is generous; a hang still fails the run.
export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:symbolize=1:detect_leaks=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
jobs=${FUZZ_JOBS:-$(nproc)}
run_one() {
    local t=$1
    mkdir -p "$OUT/corpus/$t" "$OUT/crashes/$t" "$OUT/seeds/$t"
    if "$BUILD/fuzz-$t" -max_total_time="$SECONDS_EACH" -timeout=30 -rss_limit_mb=2560 -max_len=1048576 \
        -print_final_stats=1 -artifact_prefix="$OUT/crashes/$t/" "$OUT/corpus/$t" "$OUT/seeds/$t" >"$OUT/$t.log" 2>&1; then
        echo "ok   fuzz-$t: $(grep -m1 -oE 'stat::number_of_executed_units: *[0-9]+' "$OUT/$t.log" | grep -oE '[0-9]+$') cases from $(ls "$OUT/seeds/$t" | wc -l) seeds"
    else
        echo "FAIL fuzz-$t; see $OUT/$t.log"
        grep -E '^(==[0-9]+==ERROR|==FINDING==|SUMMARY|.*runtime error:)' "$OUT/$t.log" | head -5 || true
        ls "$OUT/crashes/$t"
        return 1
    fi
}
status=0
running=0
for t in "${targets[@]}"; do
    run_one "$t" &
    running=$((running + 1))
    if ((running >= jobs)); then wait -n || status=1; running=$((running - 1)); fi
done
while ((running > 0)); do wait -n || status=1; running=$((running - 1)); done
exit "$status"
