#!/usr/bin/env bash
# The Linux player's code profile, for linux/hot-code.ld (docs/memory.md, "Linux code layout"), as
# tools/make-code-profile.py is for the Windows build. Run by `linux.bat profile`, or natively:
#   bash linux/code-profile.sh BUILD_DIR OUT_DIR
# The player runs on a virtual X display (and under weston's headless backend when weston is
# installed) with a private profile and a virtual sound server: it starts playing, gives its startup
# pages back, and from then on keeps exactly the code pages it runs (memory.cpp, PrepareCodeProfile)
# while it plays, pauses, plays again and moves to the next song. OUT_DIR/hot-code.ld is the result; copy it over linux/hot-code.ld.
# Never shows a window on a desktop or captures images.
set -euo pipefail
BUILD=${1:?usage: code-profile.sh BUILD_DIR OUT_DIR}
OUT=${2:?usage: code-profile.sh BUILD_DIR OUT_DIR}
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
PROGRAM=$BUILD/cordinal
TOOL=$ROOT/linux/code-profile.py
for tool in Xvfb pipewire wireplumber playerctl dbus-launch ffmpeg readelf nm python3; do
    command -v "$tool" >/dev/null || { echo "SETUP: $tool is missing" >&2; exit 2; }
done
[ -x "$PROGRAM" ] || { echo "SETUP: no player at $PROGRAM" >&2; exit 2; }
mkdir -p "$OUT"
T=$(mktemp -d)
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
export HOME=$T/home XDG_RUNTIME_DIR=$T/run XDG_CONFIG_HOME=$T/home/.config XDG_DATA_HOME=$T/home/.local/share
export XDG_CACHE_HOME=$T/home/.cache
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/cordinal" "$T/music"
chmod 700 "$XDG_RUNTIME_DIR"
# Songs in the formats people play most, long enough to keep playing through the profile.
# CD's rate, so the resampler to the sound server's 48 kHz runs too; Opus has only 48 kHz.
for ext in flac mp3 m4a opus ogg; do
    rate=44100
    [ "$ext" != opus ] || rate=48000
    ffmpeg -loglevel error -f lavfi -i "sine=frequency=440:duration=40" -ac 2 -ar "$rate" "$T/music/song.$ext"
done
printf 'folder0=%s\n' "$T/music" >"$XDG_CONFIG_HOME/cordinal/settings.ini"
eval "$(dbus-launch --sh-syntax)"
pids+=("$DBUS_SESSION_BUS_PID")
pipewire >"$T/pipewire.log" 2>&1 & pids+=($!)
sleep 1
wireplumber >"$T/wireplumber.log" 2>&1 & pids+=($!)
sleep 1
: >"$OUT/pages.txt"

profile() {
    local label=$1
    shift
    env "$@" CORDINAL_CODE_PROFILE=1 "$PROGRAM" "$T"/music/song.* >"$OUT/$label.log" 2>&1 &
    local pid=$!
    pids+=("$pid")
    # The first trim, about 20 seconds in, gives startup's pages back and drops the program from the
    # page cache (memory.cpp, PrepareCodeProfile): wait for it, as the program's resident pages halve.
    mapped() { awk -v p="$PROGRAM" '$6 == p {f = 1; next} /^[0-9a-f]+-/ {f = 0} f && /^Rss:/ {s += $2} END {print s + 0}' "/proc/$pid/smaps"; }
    sleep 8
    local started
    started=$(mapped)
    for _ in $(seq 60); do
        (( $(mapped) * 2 < started )) && break
        sleep 1
    done
    (( $(mapped) * 2 < started )) || { echo "PROFILE: the player never gave its startup pages back" >&2; exit 1; }
    echo "$label: $started kB of the program resident at first, $(mapped) kB once it gave startup's back"
    sleep 10
    playerctl -p cordinal pause || true
    sleep 4
    playerctl -p cordinal play || true
    sleep 6
    playerctl -p cordinal next || true
    sleep 6
    : >"$T/pages"
    python3 "$TOOL" pages "$pid" "$PROGRAM" "$T/pages"
    echo "$label: $(wc -l <"$T/pages") pages of the program ran, $(mapped) kB resident"
    cat "$T/pages" >>"$OUT/pages.txt"
    kill "$pid"
    wait "$pid" 2>/dev/null || true
    echo "profiled $label"
}

Xvfb -displayfd 3 -screen 0 1920x1080x24 3>"$T/display" >"$T/xvfb.log" 2>&1 & pids+=($!)
sleep 1
profile x11 -u WAYLAND_DISPLAY DISPLAY=":$(cat "$T/display")"
if command -v weston >/dev/null; then
    weston --backend=headless --width=1920 --height=1080 --socket=cordinal-profile >"$T/weston.log" 2>&1 & pids+=($!)
    sleep 2
    profile wayland -u DISPLAY WAYLAND_DISPLAY=cordinal-profile
else
    echo "note: weston isn't installed, so only the X11 window was profiled"
fi
python3 "$TOOL" write "$PROGRAM" "$OUT/pages.txt" "$OUT/hot-code.ld"
