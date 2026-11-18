#!/usr/bin/env bash
# Runs the official Opus conformance vectors (RFC 6716 section 6, as updated by RFC 8251) through
# cordinal's own Opus decoder, the way libopus's tests/run_vectors.sh runs them through opus_demo:
# each of the twelve vectors must decode with every packet's final range-coder state equal to the
# encoder's (bit-exact entropy decoding), and the official opus_compare must accept the 48 kHz stereo
# output against the vector's .dec or its RFC 8251 m.dec. cordinal always decodes to stereo, so
# run_vectors.sh's mono half (opus_demo decoding to one channel) does not apply.
#
#   tools/opus-conformance.sh BUILD_DIR OUT_DIR [DOWNLOADS_DIR]
#
# BUILD_DIR needs cordinal-opus-conformance (cmake --build BUILD_DIR --target cordinal-opus-conformance).
# The vectors and libopus's source (for opus_compare.c alone; nothing of libopus is linked into
# cordinal) are downloaded once into DOWNLOADS_DIR, by default OUT_DIR, and checked against these
# SHA-256 sums.
set -euo pipefail
BUILD=${1:?usage: opus-conformance.sh BUILD_DIR OUT_DIR [DOWNLOADS_DIR]}
OUT=${2:?usage: opus-conformance.sh BUILD_DIR OUT_DIR [DOWNLOADS_DIR]}
DOWNLOADS=${3:-$OUT}
VECTORS_URL=https://opus-codec.org/static/testvectors/opus_testvectors-rfc8251.tar.gz
VECTORS_SHA=6b26a22f9ba87b2b836906a9bb7afec5f8e54d49553b1200382520ee6fedfa55
OPUS_URL=https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
OPUS_SHA=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
decoder=$BUILD/cordinal-opus-conformance
[[ -x $decoder ]] || { echo "ERROR: $decoder is missing; build the cordinal-opus-conformance target" >&2; exit 2; }
mkdir -p -- "$OUT" "$DOWNLOADS"
fetch() {  # fetch URL SHA256: into DOWNLOADS, once, and only if its sum is right
    local file=$DOWNLOADS/${1##*/}
    if [[ ! -f $file ]] || ! echo "$2  $file" | sha256sum -c --status; then
        curl -fsSL --retry 3 -o "$file.part" "$1"
        echo "$2  $file.part" | sha256sum -c --status || { echo "ERROR: $1 does not have SHA-256 $2" >&2; rm -f "$file.part"; exit 2; }
        mv -- "$file.part" "$file"
    fi
    echo "$file"
}
vectors=$(fetch "$VECTORS_URL" "$VECTORS_SHA")
opus=$(fetch "$OPUS_URL" "$OPUS_SHA")
tar -xzf "$vectors" -C "$OUT"
tar -xzf "$opus" -C "$OUT" opus-1.5.2/src/opus_compare.c
compare=$OUT/opus_compare
cc -O2 -o "$compare" "$OUT/opus-1.5.2/src/opus_compare.c" -lm
dir=$OUT/opus_newvectors
failures=0
for n in 01 02 03 04 05 06 07 08 09 10 11 12; do
    out=$OUT/testvector$n.pcm
    if ! "$decoder" "$dir/testvector$n.bit" "$out" >"$OUT/testvector$n.log" 2>&1; then
        echo "FAIL testvector$n: $(tail -n 1 "$OUT/testvector$n.log")"
        failures=$((failures + 1)); continue
    fi
    if "$compare" -s -r 48000 "$dir/testvector$n.dec" "$out" >>"$OUT/testvector$n.log" 2>&1 ||
       "$compare" -s -r 48000 "$dir/testvector${n}m.dec" "$out" >>"$OUT/testvector$n.log" 2>&1; then
        echo "ok   testvector$n: $(grep -m1 -oE '[0-9]+ packets' "$OUT/testvector$n.log"), range states exact, $(grep -oE 'quality metric: *[-0-9.]+ *%' "$OUT/testvector$n.log" | sort -t: -k2 -gr | head -n 1)"
        rm -f -- "$out"
    else
        echo "FAIL testvector$n: opus_compare rejects the output against both references"
        grep -E 'quality|error|Error' "$OUT/testvector$n.log" | tail -n 2
        failures=$((failures + 1))
    fi
done
((failures == 0)) || { echo "$failures of 12 Opus conformance vectors failed; logs in $OUT"; exit 1; }
echo "All 12 Opus conformance vectors pass (stereo, 48 kHz)"
