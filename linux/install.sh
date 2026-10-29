#!/bin/sh
# Installs cordinal (native 64-bit Linux build) for the current user. From a terminal:
#   curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
# Run it again to update. Options (after `sh -s --` when piped):
#   --version X.Y.Z   a release other than the latest
#   --no-deps         don't install the libraries with pacman
#   --no-ffmpeg       accepted for older commands; all decoders are now built in
#   --uninstall       remove cordinal; your library and settings stay
set -e
REPO="imhavingasuperbadday/cordinal"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}"
BIN="$HOME/.local/bin"
ICON="$DATA/icons/hicolor/512x512/apps/cordinal.png"
PW_CONF="$CONFIG/pipewire/pipewire.conf.d/50-cordinal-rates.conf"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
fail() { printf 'cordinal: %s\n' "$*" >&2; exit 1; }

version="" deps=1
while [ $# -gt 0 ]; do
    case "$1" in
        --version) version="$2"; shift ;;
        --no-deps) deps="" ;;
        --no-ffmpeg) ;;
        --uninstall)
            rm -f "$BIN/cordinal" "$DATA/applications/cordinal.desktop" "$ICON" "$CONFIG/autostart/cordinal.desktop" "$PW_CONF"
            say "Removed cordinal. Your library is still in $DATA/cordinal and your settings in $CONFIG/cordinal."
            exit 0 ;;
        *) fail "unknown option $1" ;;
    esac
    shift
done

[ "$(uname -m)" = x86_64 ] || fail "cordinal's Linux build is for 64-bit x86 (this is $(uname -m))."
command -v curl >/dev/null 2>&1 || fail "curl is required"

# --- libraries ------------------------------------------------------------------------------
# sdl3 + sdl3_ttf: the window and text; pipewire: sound; systemd-libs: media keys and the tray
# icon (D-Bus); fontconfig + a font; openssl: HTTPS for listening away from home. Decoding needs no
# external tools.
if [ -n "$deps" ] && command -v pacman >/dev/null 2>&1; then
    want="sdl3 sdl3_ttf pipewire systemd-libs fontconfig openssl noto-fonts"
    missing=""
    for p in $want; do
        pacman -Q "$p" >/dev/null 2>&1 || missing="$missing $p"
    done
    # noto-fonts only matters when no sans-serif font is installed at all.
    case "$missing" in *noto-fonts*) fc-list 2>/dev/null | grep -q . && missing=$(echo "$missing" | sed "s/ noto-fonts//") ;; esac
    if [ -n "$missing" ]; then
        say "Installing$missing (sudo may ask for your password)"
        sudo pacman -S --needed --noconfirm $missing || sudo pacman -Syu --needed --noconfirm $missing
    fi
elif [ -n "$deps" ]; then
    say "Not Arch: make sure SDL3, SDL3_ttf, PipeWire, libsystemd and fontconfig are installed."
fi

# --- cordinal -------------------------------------------------------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
here="$(cd "$(dirname "$0")" 2>/dev/null && pwd || true)"
if [ -n "$here" ] && [ -x "$here/cordinal" ] && [ -f "$here/cordinal.desktop" ]; then
    src="$here"  # run from an unpacked release
else
    if [ -n "$version" ]; then url="https://github.com/$REPO/releases/download/v$version/cordinal-linux-x64.tar.gz"
    else url="https://github.com/$REPO/releases/latest/download/cordinal-linux-x64.tar.gz"; fi
    say "Downloading cordinal${version:+ $version}"
    curl -fL --progress-bar -o "$tmp/cordinal.tar.gz" "$url" || fail "could not download $url"
    tar -xzf "$tmp/cordinal.tar.gz" -C "$tmp"
    src="$tmp/cordinal"
fi
mkdir -p "$BIN" "$DATA/applications" "$(dirname "$ICON")"
install -m 755 "$src/cordinal" "$BIN/cordinal"
install -m 644 "$src/cordinal.desktop" "$DATA/applications/cordinal.desktop"
install -m 644 "$src/cordinal.png" "$ICON"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$DATA/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$DATA/icons/hicolor" || true

# --- bit-perfect playback -------------------------------------------------------------------------
# PipeWire runs at 48 kHz and resamples everything else. Allowing it to switch to the stream's rate
# lets cordinal's exclusive output (menu > Exclusive output) reach the device untouched.
if command -v pipewire >/dev/null 2>&1 && [ ! -f "$PW_CONF" ] && ! grep -rqs allowed-rates "$CONFIG/pipewire" /etc/pipewire; then
    mkdir -p "$(dirname "$PW_CONF")"
    cat > "$PW_CONF" <<'CONF'
# From the cordinal installer: PipeWire switches to a stream's sample rate when nothing else is
# playing, so music at 44.1, 88.2, 96 or 192 kHz plays without resampling. Delete this file to undo.
context.properties = {
    default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 352800 384000 ]
}
CONF
    if systemctl --user -q is-active pipewire 2>/dev/null; then
        say "Letting PipeWire follow each track's sample rate (sound stops for a moment)"
        systemctl --user restart pipewire pipewire-pulse wireplumber 2>/dev/null || true
    fi
fi

say "Installed. Open cordinal from the app launcher (Super+Space on Omarchy) or run: cordinal"
case ":$PATH:" in *":$BIN:"*) ;; *) say "Add $BIN to your PATH to run cordinal from a terminal." ;; esac
