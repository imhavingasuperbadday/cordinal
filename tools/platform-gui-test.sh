#!/bin/bash
# The platform layer against real window systems: runs cordinal-platform-probe (a window that
# prints its events) on a private Xvfb with xfwm4 and on a private headless sway, drives it with
# xdotool there and tools/wl-inject.py (a virtual pointer and keyboard) here, plus the clipboard
# tools, and checks what arrives.
#
#   tools/platform-gui-test.sh path/to/cordinal-platform-probe [x11|wayland|all] [screenshot dir]
#
# X11 needs Xvfb, xdotool, xclip, xrdb, xprop, xwininfo (and xfwm4, optional); Wayland needs
# sway, xkbcli (libxkbcommon-tools), wl-clipboard (wl-copy, wl-paste) and grim for the screenshot.
# Exits non-zero if any check failed.
set -u
# xdotool, xprop and xclip read and print non-ASCII text (é, €, ✓) only in a UTF-8 locale, and a bare
# container (release CI's debian:13) has none set. C.UTF-8 is built into glibc, so nothing to install.
export LC_ALL=C.UTF-8
PROBE=$(readlink -f "${1:?usage: platform-gui-test.sh path/to/cordinal-platform-probe [x11|wayland|all] [dir]}")
WHICH=${2:-all}
SHOTS=${3:-}
WORK=$(mktemp -d /tmp/cordinal-gui.XXXXXX)
mkdir -p "$WORK/home" "$WORK/run"; chmod 700 "$WORK/run"
FAILS=0 PASSES=0 PIDS=()
cleanup() { exec 7>&- 8>&- 2>/dev/null; for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
[ -n "${KEEP:-}" ] && trap 'cp "$WORK"/out-* "$WORK"/*.log /tmp/ 2>/dev/null; cleanup' EXIT

ok() { PASSES=$((PASSES + 1)); echo "  ok    $1"; }
bad() { FAILS=$((FAILS + 1)); echo "  FAIL  $1"; }
# expect NAME REGEX: waits up to 3 s for a line matching REGEX in the probe's output after MARK.
expect() {
    for _ in $(seq 30); do
        if tail -n +$MARK "$OUT" | grep -qE -- "$2"; then ok "$1"; return 0; fi
        sleep 0.1
    done
    bad "$1 (no line matching: $2)"; return 1
}
mark() { MARK=$(($(wc -l < "$OUT") + 1)); }
send() { echo "$*" >&7; }

start_probe() {  # start_probe ENV...: the probe with a fifo for its commands
    OUT=$WORK/out-$BACKEND; rm -f "$WORK/in"; mkfifo "$WORK/in"
    env -i PATH=/usr/bin:/bin HOME=$WORK/home XDG_RUNTIME_DIR=$WORK/run LANG=C.UTF-8 "$@" "$PROBE" probe-window < "$WORK/in" > "$OUT" 2>&1 &
    PROBE_PID=$!; PIDS+=($PROBE_PID)
    exec 7>"$WORK/in"
    MARK=1
}
stop_probe() { exec 7>&-; sleep 0.3; kill $PROBE_PID 2>/dev/null; wait $PROBE_PID 2>/dev/null; }

run_x11() {
    BACKEND=x11
    echo "x11:"
    local n=$((60 + RANDOM % 30))
    while [ -e /tmp/.X11-unix/X$n ]; do n=$((n + 1)); done
    mkdir -p "$WORK/fb"
    Xvfb :$n -screen 0 1280x800x24 -fbdir "$WORK/fb" -nolisten tcp > "$WORK/xvfb.log" 2>&1 & PIDS+=($!)
    for _ in $(seq 50); do [ -e /tmp/.X11-unix/X$n ] && break; sleep 0.1; done
    export DISPLAY=:$n
    # xfwm4 reads its settings (Alt+F4, window placement) from xfconfd over D-Bus; a bare container has
    # no session bus, so give it one of its own when dbus-run-session is there.
    local wm=(xfwm4 --compositor=off)
    command -v dbus-run-session > /dev/null && wm=(dbus-run-session -- "${wm[@]}")
    if command -v xfwm4 > /dev/null; then "${wm[@]}" > "$WORK/wm.log" 2>&1 & PIDS+=($!); sleep 1.5; fi
    xdotool mousemove 5 5
    start_probe DISPLAY=:$n
    expect "starts on x11" '^ready driver=x11 scale=1.000'
    expect "first frame" '^frame 1 400x300'
    local wid=$(xwininfo -name probe-window 2>/dev/null | awk '/Window id:/{print $4}')
    [ -n "$wid" ] && ok "window found ($wid)" || { bad "window found"; stop_probe; return; }
    xprop -id $wid WM_CLASS WM_PROTOCOLS _NET_WM_PID > "$WORK/props"
    grep -q '"cordinal", "cordinal"' "$WORK/props" && grep -q "WM_DELETE_WINDOW, _NET_WM_PING" "$WORK/props" && ok "WM_CLASS, WM_PROTOCOLS" || bad "WM_CLASS, WM_PROTOCOLS: $(cat "$WORK/props")"
    mark; send "title Renamed ✓"; sleep 0.4
    xprop -id $wid _NET_WM_NAME | grep -q '"Renamed ✓"' && ok "UTF-8 title" || bad "UTF-8 title"

    xdotool mousemove 1279 799; sleep 0.2  # off the window wherever it was placed, so moving in is an enter
    mark; xdotool mousemove --window $wid 50 60; xdotool click 1
    expect "enter" '^enter'; expect "motion" '^motion 50 60'
    expect "left click" '^button up 1 clicks=1 at 50 60'
    mark; xdotool click --repeat 2 --delay 80 3
    expect "double right click" '^button down 3 clicks=2'
    mark; xdotool click 4 click 5 click 6 click 7 click 8 click 9
    expect "wheel up" '^wheel 0.00 1.00'; expect "wheel down" '^wheel 0.00 -1.00'
    expect "wheel left" '^wheel -1.00 0.00'; expect "wheel right" '^wheel 1.00 0.00'
    expect "back button" '^button down 4'; expect "forward button" '^button down 5'

    mark; xdotool key a shift+a ctrl+c Return Escape F5 BackSpace Delete Left super+x
    expect "key a" '^key down sc=4 key=0x61 name=A mod=0x0 repeat=0'; expect "text a" '^text \[a\]'
    expect "shift+a" '^key down sc=4 key=0x61 name=A mod=0x1 '; expect "text A" '^text \[A\]'
    expect "ctrl+c" '^key down sc=6 key=0x63 name=C mod=0x40'
    expect "Return" '^key down sc=40 key=0xd '; expect "Escape" '^key down sc=41 key=0x1b '
    expect "F5" '^key down sc=62 key=0x4000003e name=F5'; expect "Backspace" '^key down sc=42 key=0x8 '
    expect "Delete" '^key down sc=76 key=0x7f '; expect "Left" '^key down sc=80 '
    expect "super modifier" '^key down sc=27 key=0x78 name=X mod=0x400'
    tail -n +$MARK "$OUT" | grep -q '^text \[c\]' && bad "no text for ctrl+c" || ok "no text for ctrl+c"
    mark; xdotool type --delay 40 'zé€ß!'
    expect "typed z" '^text \[z\]'; expect "typed é (keymap change)" '^text \[é\]'
    expect "typed €" '^text \[€\]'; expect "typed ß" '^text \[ß\]'; expect "typed !" '^text \[!\]'
    mark; xdotool keydown b; sleep 0.8; xdotool keyup b
    expect "key repeat" '^key down sc=5 key=0x62 name=B mod=0x0 repeat=1'
    expect "repeat ends with one release" '^key up sc=5'
    [ "$(tail -n +$MARK "$OUT" | grep -c '^key up sc=5')" = 1 ] && ok "detectable auto-repeat" || bad "detectable auto-repeat"

    mark; send "clip hello from the probe ✓"; expect "set clipboard" '^setclip 1'
    [ "$(timeout 3 xclip -o -selection clipboard)" = "hello from the probe ✓" ] && ok "xclip reads our clipboard" || bad "xclip reads our clipboard"
    timeout 3 xclip -o -selection clipboard -t TARGETS | grep -q UTF8_STRING && ok "TARGETS" || bad "TARGETS"
    printf 'from xclip ü' | xclip -i -selection clipboard; sleep 0.3
    mark; send get; expect "read another client's clipboard" '^clip: \[from xclip ü\]'
    head -c 300000 /dev/urandom | base64 -w0 > "$WORK/big"; xclip -i -selection clipboard < "$WORK/big"; sleep 0.3
    mark; send get; expect "large clipboard (INCR)" '^clip: \['
    [ "$(tail -n +$MARK "$OUT" | grep '^clip: ' | head -1 | wc -c)" = 400009 ] && ok "INCR transfer complete (400000 bytes)" || bad "INCR transfer complete"
    mark; send types; expect "clipboard types" '^types text/plain;charset=utf-8=1 text/uri-list=0'

    mark; echo 'Xft.dpi: 144' | xrdb -merge; expect "scale from Xft.dpi" '^scale 1.500'
    mark; echo 'Xft.dpi: 96' | xrdb -merge; expect "scale back" '^scale 1.000'
    mark; send "size 500 350"; expect "resize" '^resized 500 350'; expect "redraw" '^frame [0-9]+ 500x350'
    if [ -n "$SHOTS" ]; then
        sleep 0.5; python3 - "$WORK/fb/Xvfb_screen0" "$SHOTS/probe-x11.png" <<'PY' && ok "screenshot $SHOTS/probe-x11.png" || bad "screenshot"
import struct, sys
from PIL import Image
d = open(sys.argv[1], 'rb').read(); h = struct.unpack('>25I', d[:100])
img = Image.frombuffer('RGBX', (h[4], h[5]), d[h[0] + h[19] * 12:], 'raw', 'BGRX', h[12], 1).convert('RGB'); img.save(sys.argv[2])
PY
    fi
    mark; send shape; expect "shape" '^shape 1'; send unshape
    mark; xdotool key alt+F4 2>/dev/null; xdotool windowclose $wid 2>/dev/null || true
    if command -v xfwm4 > /dev/null; then expect "close button / WM_DELETE_WINDOW" '^close requested'; fi
    stop_probe
    unset DISPLAY
}

run_wayland() {
    BACKEND=wayland
    echo "wayland:"
    if ! command -v sway > /dev/null || ! command -v xkbcli > /dev/null; then bad "sway and xkbcli are needed"; return; fi
    local run=$WORK/run wl
    printf 'output HEADLESS-1 resolution 1280x800 scale 1\ndefault_border none\n' > "$WORK/sway.cfg"
    env -i PATH=/usr/bin:/bin HOME=$WORK/home XDG_RUNTIME_DIR=$run WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
        WLR_RENDERER=pixman sway -c "$WORK/sway.cfg" > "$WORK/sway.log" 2>&1 & PIDS+=($!)
    for _ in $(seq 50); do wl=$(ls "$run" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1); [ -n "$wl" ] && break; sleep 0.1; done
    [ -n "$wl" ] || { bad "sway started"; return; }
    sleep 0.5
    local E=(env -i PATH=/usr/bin:/bin HOME=$WORK/home XDG_RUNTIME_DIR=$run WAYLAND_DISPLAY=$wl LANG=C.UTF-8)
    local S=("${E[@]}" SWAYSOCK=$(ls "$run"/sway-ipc.* | head -1))
    start_probe XDG_RUNTIME_DIR=$run WAYLAND_DISPLAY=$wl
    expect "starts on wayland" '^ready driver=wayland scale=1.000'
    expect "first frame" '^frame 1 400x300'
    expect "tiled by sway" '^resized 1280 800'
    "${S[@]}" swaymsg -t get_tree > "$WORK/tree"
    grep -q '"app_id": "cordinal"' "$WORK/tree" && grep -q '"shell": "xdg_shell"' "$WORK/tree" && ok "xdg_toplevel with app_id" || bad "xdg_toplevel with app_id"
    send "title Renamed ✓"; sleep 0.4
    "${S[@]}" swaymsg -t get_tree | grep -q '"name": "Renamed ✓"' && ok "UTF-8 title" || bad "UTF-8 title"

    # One virtual pointer and keyboard for the rest of the run (fd 8 feeds it).
    rm -f "$WORK/inj"; mkfifo "$WORK/inj"
    "${E[@]}" python3 "$(dirname "$(readlink -f "$0")")/wl-inject.py" < "$WORK/inj" > "$WORK/inj.log" 2>&1 & PIDS+=($!)
    mark; exec 8>"$WORK/inj"
    inj() { printf '%s\n' "$@" sync >&8; }
    expect "keyboard focus" '^focus in'; expect "pointer enter" '^enter'
    sleep 0.3
    mark; inj "move 50 60" "button 0x110 1" "button 0x110 0"
    expect "motion" '^motion 50 60'
    expect "left click" '^button up 1 clicks=1 at 50 60'
    mark; inj "button 0x111 1" "button 0x111 0" "button 0x111 1" "button 0x111 0"
    expect "double right click" '^button down 3 clicks=2'
    mark; inj "wheel -1" "wheel 1" "wheel 0 -1" "wheel 0 1" "button 0x113 1" "button 0x113 0" "button 0x114 1" "button 0x114 0"
    expect "wheel up" '^wheel 0.00 1.00'; expect "wheel down" '^wheel 0.00 -1.00'
    expect "wheel left" '^wheel -1.00 0.00'; expect "wheel right" '^wheel 1.00 0.00'
    expect "back button" '^button down 4'; expect "forward button" '^button down 5'

    # evdev codes: a 30, shift 42, ctrl 29, c 46, Return 28, Escape 1, F5 63, BackSpace 14, Delete 111,
    # Left 105, super 125, x 45, b 48
    mark; inj "key 30 1" "key 30 0" "key 42 1" "key 30 1" "key 30 0" "key 42 0" "key 29 1" "key 46 1" "key 46 0" "key 29 0" \
        "key 28 1" "key 28 0" "key 1 1" "key 1 0" "key 63 1" "key 63 0" "key 14 1" "key 14 0" "key 111 1" "key 111 0" \
        "key 105 1" "key 105 0" "key 125 1" "key 45 1" "key 45 0" "key 125 0"
    expect "key a" '^key down sc=4 key=0x61 name=A mod=0x0 repeat=0'; expect "text a" '^text \[a\]'
    expect "shift+a" '^key down sc=4 key=0x61 name=A mod=0x1 '; expect "text A" '^text \[A\]'
    expect "ctrl+c" '^key down sc=6 key=0x63 name=C mod=0x40'
    expect "Return" '^key down sc=40 key=0xd '; expect "Escape" '^key down sc=41 key=0x1b '
    expect "F5" '^key down sc=62 key=0x4000003e name=F5'; expect "Backspace" '^key down sc=42 key=0x8 '
    expect "Delete" '^key down sc=76 key=0x7f '; expect "Left" '^key down sc=80 '
    expect "super modifier" '^key down sc=27 key=0x78 name=X mod=0x400'
    tail -n +$MARK "$OUT" | grep -q '^text \[c\]' && bad "no text for ctrl+c" || ok "no text for ctrl+c"
    mark; inj "key 48 1" "sleep 800" "key 48 0"
    expect "client-side key repeat" '^key down sc=5 key=0x62 name=B mod=0x0 repeat=1'
    expect "repeat ends with one release" '^key up sc=5'
    [ "$(tail -n +$MARK "$OUT" | grep -c '^key up sc=5')" = 1 ] && ok "one release" || bad "one release"

    # A second keyboard with a German keymap: the compositor sends its keymap when it's used.
    mark; printf '%s\n' "sleep 300" "key 44 1" "key 44 0" "key 26 1" "key 26 0" "key 100 1" "key 18 1" "key 18 0" "key 100 0" \
        "key 42 1" "key 3 1" "key 3 0" "key 42 0" "sleep 200" sync | "${E[@]}" WL_INJECT_LAYOUT=de python3 "$(dirname "$(readlink -f "$0")")/wl-inject.py" > /dev/null 2>&1
    expect "keymap change: de y on the z key" '^text \[y\]'; expect "de ü" '^text \[ü\]'
    expect "AltGr+e: €" '^text \[€\]'; expect "Shift+2: \"" '^text \["\]'

    mark; send "clip hello from the probe ✓"; expect "set clipboard" '^setclip 1'; sleep 0.3
    [ "$("${E[@]}" timeout 3 wl-paste -n)" = "hello from the probe ✓" ] && ok "wl-paste reads our clipboard" || bad "wl-paste reads our clipboard"
    "${E[@]}" timeout 3 wl-paste -l | grep -q 'text/plain;charset=utf-8' && ok "offered types" || bad "offered types"
    printf 'from wl-copy ü' | "${E[@]}" wl-copy; sleep 0.3
    mark; send get; expect "read another client's clipboard" '^clip: \[from wl-copy ü\]'
    head -c 300000 /dev/urandom | base64 -w0 | "${E[@]}" wl-copy; sleep 0.3
    mark; send get; expect "large clipboard" '^clip: \['
    [ "$(tail -n +$MARK "$OUT" | grep '^clip: ' | head -1 | wc -c)" = 400009 ] && ok "large transfer complete (400000 bytes)" || bad "large transfer complete"
    mark; send types; expect "clipboard types" '^types text/plain;charset=utf-8=1 text/uri-list=0'

    mark; "${S[@]}" swaymsg output HEADLESS-1 scale 1.5 > /dev/null
    expect "fractional scale" '^scale 1.500'; expect "logical size at 1.5" '^resized 853 533'
    mark; "${S[@]}" swaymsg output HEADLESS-1 scale 2 > /dev/null; expect "integer scale" '^scale 2.000'
    mark; "${S[@]}" swaymsg output HEADLESS-1 scale 1 > /dev/null; expect "scale back" '^scale 1.000'
    expect "size back" '^resized 1280 800'
    if [ -n "$SHOTS" ]; then
        sleep 0.5; "${E[@]}" grim "$SHOTS/probe-wayland.png" && ok "screenshot $SHOTS/probe-wayland.png" || bad "screenshot"
    fi
    mark; send shape; expect "shape" '^shape 1'; send unshape
    mark; "${S[@]}" swaymsg '[app_id=cordinal] kill' > /dev/null; expect "close requested by the compositor" '^close requested'
    exec 8>&-
    stop_probe
}

[ "$WHICH" = x11 ] || [ "$WHICH" = all ] && run_x11
[ "$WHICH" = wayland ] || [ "$WHICH" = all ] && run_wayland
echo "$PASSES passed, $FAILS failed"
[ $FAILS = 0 ]
