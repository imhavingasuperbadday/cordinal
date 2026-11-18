#!/usr/bin/env bash
# Fast offline coverage of the real portable core, without a display, audio server or network.
# Fixtures and every tool's output remain in the run folder for diagnosis.
set -euo pipefail
BUILD=${1:?usage: smoke-test.sh BUILD_DIR OUT_DIR}
OUT=${2:?usage: smoke-test.sh BUILD_DIR OUT_DIR}
mkdir -p -- "$OUT"
failures=0
check() {
    local name=$1 code=0
    shift
    if "$@" >"$OUT/$name.log" 2>&1; then echo "ok   $name"
    else code=$?; echo "FAIL $name (exit $code)"; cat "$OUT/$name.log"; failures=$((failures + 1)); fi
}
# The waveform check compares an exact seek at 12 seconds, so this fixture must extend beyond it.
ffmpeg -nostdin -loglevel error -y -f lavfi -i 'sine=frequency=440:duration=14' -ac 2 \
    -metadata title='Smoke track' -metadata artist='Test artist' -metadata album_artist='Test artist' \
    -metadata album='Smoke album' -metadata track=1 "$OUT/source.flac"
for extension in wav mp3 ogg opus m4a aiff; do
    ffmpeg -nostdin -loglevel error -y -i "$OUT/source.flac" "$OUT/source.$extension"
done
# The locales the libc check compares with glibc's own data. localedef writes them unpacked into the
# run folder, which CORDINAL_LOCALE_DIR points both libraries at, and a locale the image has no
# definition for is left out: Debian's glibc ships every definition, while the Arch image has only
# the English ones (so en_GB.UTF-8 is always there).
mkdir -p "$OUT/locales"
for locale in de_DE ru_RU el_GR tr_TR cs_CZ ja_JP en_GB; do
    [ -e "$OUT/locales/$locale.UTF-8/LC_CTYPE" ] && continue
    localedef --no-archive -i "$locale" -f UTF-8 "$OUT/locales/$locale.UTF-8" 2>/dev/null ||
        { rm -rf "$OUT/locales/$locale.UTF-8"; echo "note: localedef has no $locale definition, so the libc check skips it"; }
done
# One 8-bit locale as well: its narrow table is the only place the byte folds (strcasecmp, the class
# table) differ from ASCII, so the check compares them under it.
if [ ! -e "$OUT/locales/de_DE.ISO-8859-1/LC_CTYPE" ]; then
    localedef --no-archive -i de_DE -f ISO-8859-1 "$OUT/locales/de_DE.ISO-8859-1" 2>/dev/null ||
        { rm -rf "$OUT/locales/de_DE.ISO-8859-1"; echo "note: localedef cannot make de_DE.ISO-8859-1 (it needs the ISO-8859-1 charmap), so the 8-bit comparisons skip it"; }
fi
for extension in flac wav mp3 ogg opus m4a aiff; do
    check "decode-$extension" "$BUILD/cordinal-core-test" decode "$OUT/source.$extension"
done
check empty-audio python3 - "$BUILD/cordinal-core-test" "$OUT" <<'PY'
from pathlib import Path
import subprocess
import sys

player, output = sys.argv[1], Path(sys.argv[2])
directory = output / 'empty-audio'
directory.mkdir()
placeholder = directory / 'placeholder.ogg'
placeholder.write_bytes(b'')
def scan(count):
    result = subprocess.run([player, 'scan', str(directory)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0 and result.stdout.endswith(f'{count} files\n'), result
scan(0)
result = subprocess.run([player, 'decode', str(placeholder)], capture_output=True, text=True, timeout=20)
assert result.returncode == 1 and 'the file is empty' in result.stderr, result
assert placeholder.read_bytes() == b''
placeholder.write_bytes((output / 'source.ogg').read_bytes())
scan(1)
placeholder.write_bytes(b'')
scan(0)
print('ok empty placeholders stay out until populated, and direct playback explains why')
PY
# The static build's libc check loads a shared object with the program's own loader, so it wants one:
# a self-contained object that imports only names the program exports (linux/cordial-export.map) and
# carries no C library of its own, which is why it also runs on a musl distribution. Without a C
# compiler the check says so and skips those, and nothing else changes.
mkdir -p "$OUT/libc"
if command -v cc >/dev/null 2>&1; then
    cat >"$OUT/plug.c" <<'PLUG'
/* The loader check's fixture: a shared object with no C library of its own. Every name it imports is
   one the program exports: strlen, snprintf, malloc and free, plus the compiler's stack protector. */
unsigned long strlen(const char*);
int snprintf(char*, unsigned long, const char*, ...);
void* malloc(unsigned long);
void free(void*);

__thread int plug_tls_counter;

int plug_sum(int a, int b) { return a + b; }

int plug_tls_bump(void) {
    plug_tls_counter += 1;
    return plug_tls_counter;
}

char* plug_greet(const char* name) {
    unsigned long length = strlen(name);
    char* text = (char*)malloc(length + 32);
    if (!text) return 0;
    snprintf(text, length + 32, "hello %s (%lu)", name, length);
    return text;
}
PLUG
    cat >"$OUT/plugref.c" <<'PLUGREF'
/* A fixture that needs plug.so's plug_sum, for the RTLD_LOCAL against RTLD_GLOBAL check. */
int plug_sum(int, int);
int plug_ref(int a) { return plug_sum(a, 10); }
PLUGREF
    if ! cc -nostdlib -shared -fPIC -O2 -o "$OUT/libc/plug.so" "$OUT/plug.c" 2>"$OUT/plug-build.log" ||
        ! cc -nostdlib -shared -fPIC -O2 -o "$OUT/libc/plugref.so" "$OUT/plugref.c" 2>>"$OUT/plug-build.log"; then
        echo "note: the loader fixture did not build (see $OUT/plug-build.log); the loaded-object checks skip"
        rm -f "$OUT/libc/plug.so" "$OUT/libc/plugref.so"
    fi
else
    echo "note: no C compiler on PATH, so the loader fixture is skipped"
fi
check libc env CORDINAL_LOCALE_DIR="$OUT/locales" "$BUILD/cordinal-core-test" libc "$OUT/libc"
for mode in eq resampler weighted-shuffle; do check "$mode" "$BUILD/cordinal-core-test" "$mode"; done
check tagsuggest "$BUILD/cordinal-core-test" tagsuggest
check waybar "$BUILD/cordinal-core-test" waybar
check keymap "$BUILD/cordinal-core-test" keymap
check waveform "$BUILD/cordinal-core-test" waveform "$OUT/source.flac" "$OUT/source.ogg" "$OUT/source.opus"
check replaygain "$BUILD/cordinal-core-test" replaygain "$OUT/source.flac" "$OUT/source.mp3"
check replaygain-scan python3 - "$BUILD/cordinal-core-test" "$OUT" <<'PY'
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import wave

player, directory = sys.argv[1], Path(sys.argv[2])
samples = []
for i in range(48000):
    value = round(512 * math.sin(2 * math.pi * 1000 * i / 48000))
    if i >= 48000 - 8:
        value = 23170 if i % 4 < 2 else -23170
    samples.append(struct.pack('<hh', value, -value))
audio = b''.join(samples)
peaks = []
for name, payload in [('terminal', audio), ('padded', audio + bytes(4096 * 4))]:
    path = directory / f'rg-{name}.wav'
    with wave.open(str(path), 'wb') as file:
        file.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        file.writeframes(payload)
    before = path.read_bytes()
    result = subprocess.run([player, 'replaygain', str(path)], capture_output=True, text=True, timeout=20)
    print(result.stdout, end='')
    assert result.returncode == 0, result.stderr
    assert path.read_bytes() == before
    peaks.append(float(re.search(r'peak ([\d.]+)', result.stdout).group(1)))
assert peaks[0] > 0.95 and abs(peaks[0] - peaks[1]) < 0.0001, peaks
damaged = bytearray((directory / 'source.flac').read_bytes())
assert len(damaged) > 4096
damaged[len(damaged) // 2:len(damaged) // 2 + 32] = bytes([255]) * 32
path = directory / 'rg-damaged.flac'
path.write_bytes(damaged)
result = subprocess.run([player, 'replaygain', str(path)], capture_output=True, text=True, timeout=20)
assert result.returncode == 1 and "couldn't be measured" in result.stderr, result
assert path.read_bytes() == damaged
print('ok terminal true peak and damaged-file scan protection')
PY
mkdir -p "$OUT/journal" "$OUT/helpers" "$OUT/convert" "$OUT/organize" "$OUT/copy" "$OUT/away"
check tags "$BUILD/cordinal-core-test" tags "$OUT/source.mp3" "$OUT/journal"
check helpers "$BUILD/cordinal-core-test" helpers "$OUT/helpers"
cp "$OUT/source.flac" "$OUT/convert.flac"
check convert "$BUILD/cordinal-core-test" convert "$OUT/convert.flac" "$OUT/convert"
mkdir -p "$OUT/organize-source"
cp "$OUT/source.flac" "$OUT/organize-source/track.flac"
# These round trips require a cover file and verify its bytes, rather than decoding the image.
printf 'test cover\n' >"$OUT/organize-source/cover.jpg"
printf 'test cover\n' >"$OUT/cover.jpg"
check organize "$BUILD/cordinal-core-test" organize "$OUT/organize-source/track.flac" "$OUT/organize"
check copy "$BUILD/cordinal-core-test" copy "$OUT/source.flac" "$OUT/copy"
# A trash of its own, on the run folder's drive, so the clean-up really moves the file and nothing leaves $OUT.
check duplicates env XDG_DATA_HOME="$OUT/duplicates-data" "$BUILD/cordinal-core-test" duplicates "$OUT/source.mp3" "$OUT/duplicates"
check collection "$BUILD/cordinal-core-test" collection "$OUT/collection"
check ipod "$BUILD/cordinal-core-test" ipod "$OUT/ipod"
check away "$BUILD/cordinal-core-test" away "$OUT/away"
check metadata "$BUILD/cordinal-core-test" metadata "$OUT/metadata" "$OUT/source.mp3" "$OUT/source.flac"
echo "$failures failure(s)"
((failures == 0))
