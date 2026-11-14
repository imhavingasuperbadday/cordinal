#!/bin/sh
# Installs cordinal for the current user on 64-bit glibc Linux. From a terminal:
#   curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
# Run it again to update. Options (after `sh -s --` when piped):
#   --version X.Y.Z   a release other than the latest (must publish a checksum)
#   --no-deps         do not install a font package when the system has no font
#   --no-ffmpeg       accepted for older commands; every decoder is built in
#   --dry-run         print what would be installed, then stop
#   --uninstall       remove cordinal; the library and settings stay
#   --self-test       check this script's own checks, then stop (used by CI)
#
# The published program needs only glibc 2.36 or newer (Debian 12, Ubuntu 24.04 and anything
# newer): windows (X11 and Wayland), drawing, HTTPS, font matching, text layout and the PipeWire
# and PulseAudio clients are compiled in, and so is the C++ runtime. The one package it may install
# is a font, from repositories already configured on the machine, when the system has none. The
# script does not add a repository, does not turn signature checks off, and does not upgrade the system.
set -eu

REPO="imhavingasuperbadday/cordinal"
ARCHIVE="cordinal-linux-x64.tar.gz"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}"
BIN="$HOME/.local/bin"
ICON="$DATA/icons/hicolor/512x512/apps/cordinal.png"
DESKTOP="$DATA/applications/cordinal.desktop"
PW_CONF="$CONFIG/pipewire/pipewire.conf.d/50-cordinal-rates.conf"
# The release archive is a few megabytes. Refuse a larger download or a larger unpacked member.
MAX_ARCHIVE=80000000
MAX_PROGRAM=120000000

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
fail() { printf 'cordinal: %s\n' "$*" >&2; exit 1; }

version_ok() {
    printf '%s' "$1" | grep -Eq '^[0-9]{1,4}(\.[0-9]{1,4}){2,3}$'
}

# ID and ID_LIKE from /etc/os-release, without executing that file.
read_os() {
    OS_ID=unknown
    OS_LIKE=
    [ -r /etc/os-release ] || return 0
    while IFS= read -r line; do
        case "$line" in
            ID=*|ID_LIKE=*) ;;
            *) continue ;;
        esac
        key=${line%%=*}
        val=${line#*=}
        val=${val#\"}
        val=${val%\"}
        case "$val" in
            *[!a-zA-Z0-9_.\ -]*) continue ;;
        esac
        case "$key" in
            ID) OS_ID=$val ;;
            ID_LIKE) OS_LIKE=$val ;;
        esac
    done < /etc/os-release
}

# pacman, apt, dnf, zypper, or a name this script will not try to modify.
distro_kind() {
    id=$1
    like=$2
    case "$id" in
        steamos) printf '%s\n' steamos; return ;;
        nixos|guix) printf '%s\n' immutable; return ;;
        alpine) printf '%s\n' alpine; return ;;
    esac
    case " $id $like " in
        *" arch "*|*" archlinux "*) printf '%s\n' pacman; return ;;
        *" debian "*|*" ubuntu "*) printf '%s\n' apt; return ;;
        *" fedora "*|*" rhel "*|*" centos "*) printf '%s\n' dnf; return ;;
        *" suse "*|*" opensuse "*|*" sles "*) printf '%s\n' zypper; return ;;
    esac
    case "$id" in
        arch|manjaro|endeavouros|garuda|cachyos|omarchy|arcolinux|archcraft|rebornos|biglinux)
            printf '%s\n' pacman ;;
        debian|ubuntu|linuxmint|pop|elementary|zorin|kali|neon|raspbian|pureos|trisquel|deepin|mx|linuxlite|peppermint|bodhi|parrot|tails|devuan)
            printf '%s\n' apt ;;
        fedora|nobara|rhel|centos|rocky|almalinux|ol|amzn|mageia|ultramarine)
            printf '%s\n' dnf ;;
        opensuse*|sles*|sled*)
            printf '%s\n' zypper ;;
        *) printf '%s\n' unknown ;;
    esac
}

# Shared libraries the program needs from the distribution, beyond glibc: none. SDL3, OpenSSL,
# fontconfig, SDL3_ttf/FreeType/HarfBuzz, PipeWire/PulseAudio/ALSA and libX11/libwayland were each
# replaced by code in the program. A library added here needs its package names in pkg_for.
LIBS=""

# Package names for one soname in LIBS. Apt may need a second name from the t64 transition.
# Printed as one line: "primary" or "primary|alternate".
pkg_for() {
    kind=$1
    soname=$2
    case "$kind:$soname" in
        *) return 1 ;;
    esac
}

font_pkg() {
    case "$1" in
        pacman) printf '%s\n' noto-fonts ;;
        apt) printf '%s\n' fonts-noto-core ;;
        dnf) printf '%s\n' google-noto-sans-fonts ;;
        zypper) printf '%s\n' noto-sans-fonts ;;
        *) return 1 ;;
    esac
}

# Debian and Ubuntu often leave /usr/sbin off a user's PATH, so look there too.
ldconfig_bin() {
    if command -v ldconfig >/dev/null 2>&1; then
        command -v ldconfig
        return 0
    fi
    for candidate in /usr/sbin/ldconfig /sbin/ldconfig; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

have_lib() {
    bin=$(ldconfig_bin) || return 1
    "$bin" -p 2>/dev/null | grep -F "$1 " | grep -q 'x86-64'
}

# A TrueType or OpenType font where the player looks for one (src/linux/fontmatch.cpp).
have_font() {
    data_home=${XDG_DATA_HOME:-${HOME:-/nonexistent}/.local/share}
    set -- /usr/share/fonts /usr/local/share/fonts "$data_home/fonts" "${HOME:-/nonexistent}/.fonts"
    old_ifs=$IFS
    IFS=:
    for data_dir in ${XDG_DATA_DIRS:-/usr/local/share:/usr/share}; do
        set -- "$@" "$data_dir/fonts"
    done
    IFS=$old_ifs
    for font_dir in "$@"; do
        [ -d "$font_dir" ] || continue
        found=$(find -L "$font_dir" -type f \( -iname '*.ttf' -o -iname '*.otf' -o -iname '*.ttc' -o -iname '*.otc' \) 2>/dev/null | head -n 1)
        [ -n "$found" ] && return 0
    done
    return 1
}

glibc_loader() {
    for candidate in /lib64/ld-linux-x86-64.so.2 /usr/lib64/ld-linux-x86-64.so.2 /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

# A regular x86-64 ELF executable (including PIE). Does not run it.
elf_ok() {
    [ -f "$1" ] || return 1
    [ -L "$1" ] && return 1
    magic=$(od -An -t x1 -N 4 "$1" 2>/dev/null | tr -d ' \n') || return 1
    [ "$magic" = "7f454c46" ] || return 1
    class=$(od -An -t x1 -j 4 -N 1 "$1" 2>/dev/null | tr -d ' \n') || return 1
    [ "$class" = "02" ] || return 1
    kind=$(od -An -t x1 -j 16 -N 4 "$1" 2>/dev/null | tr -d ' \n') || return 1
    case "$kind" in
        02003e00|03003e00) return 0 ;;
        *) return 1 ;;
    esac
}

member_name_ok() {
    case "$1" in
        *[!a-zA-Z0-9._/-]*) return 1 ;;
        cordinal|cordinal/|cordinal/cordinal|cordinal/install.sh|cordinal/cordinal.desktop|cordinal/cordinal.png|cordinal/LICENSE)
            return 0 ;;
        *) return 1 ;;
    esac
}

# Names, types and sizes only. Extraction is separate, after this returns 0.
archive_ok() {
    tarfile=$1
    list=$2
    tar -tzf "$tarfile" >"$list" 2>/dev/null || return 1
    count=0
    while IFS= read -r member; do
        count=$((count + 1))
        [ "$count" -le 16 ] || return 1
        member_name_ok "$member" || return 1
    done < "$list"
    [ "$count" -ge 1 ] || return 1
    # GNU tar -tv: mode, owner, size, date, time, name. LC_ALL keeps the columns stable.
    # Reject links and anything whose declared size is not a small decimal.
    LC_ALL=C tar -tvzf "$tarfile" >"$list.v" 2>/dev/null || return 1
    total=0
    while IFS= read -r row; do
        case "$row" in
            l*|h*|*" link to "*) return 1 ;;
        esac
        size=$(printf '%s\n' "$row" | awk '{print $3; exit}')
        case "$size" in
            ''|*[!0-9]*) return 1 ;;
        esac
        [ "$size" -le "$MAX_PROGRAM" ] || return 1
        total=$((total + size))
        [ "$total" -le "$MAX_PROGRAM" ] || return 1
    done < "$list.v"
}

# One checksum line: 64 lowercase hex digits, two spaces, the archive name.
sum_hash() {
    file=$1
    [ -f "$file" ] || return 1
    bytes=$(wc -c < "$file")
    [ "$bytes" -le 256 ] || return 1
    n=$(tr -d '\r' < "$file" | grep -c . || true)
    [ "$n" = 1 ] || return 1
    line=$(tr -d '\r' < "$file" | grep .)
    case "$line" in
        *"  $ARCHIVE") ;;
        *) return 1 ;;
    esac
    hash=${line%"  $ARCHIVE"}
    printf '%s' "$hash" | grep -Eq '^[0-9a-f]{64}$' || return 1
    printf '%s\n' "$hash"
}

desktop_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/ /\\s/g' -e 's/	/\\t/g'
}

# Package managers must not read the script when it was started from a pipe.
# sudo and doas still ask for a password on the terminal.
# /dev/tty can exist without opening, when there is no controlling terminal (CI, cron, containers).
have_tty() {
    (: < /dev/tty) 2>/dev/null
}

as_root() {
    if command -v sudo >/dev/null 2>&1; then
        if have_tty; then sudo -- "$@" < /dev/tty
        else sudo -- "$@" < /dev/null
        fi
    elif command -v doas >/dev/null 2>&1; then
        if have_tty; then doas "$@" < /dev/tty
        else doas "$@" < /dev/null
        fi
    else
        fail "sudo is required to install libraries. Install them as root, then re-run with --no-deps."
    fi
}

apt_resolves() {
    apt-get install -s -y -qq "$1" >/dev/null 2>&1
}

# Resolve apt's t64 rename, then print one package name. Does not use the caller's arguments.
pick_pkg() {
    spec=$1
    if [ "$KIND" != apt ]; then
        printf '%s\n' "$spec"
        return 0
    fi
    rest=$spec
    while [ -n "$rest" ]; do
        case "$rest" in
            *\|*) name=${rest%%|*}; rest=${rest#*|} ;;
            *) name=$rest; rest= ;;
        esac
        if apt_resolves "$name"; then
            printf '%s\n' "$name"
            return 0
        fi
    done
    return 1
}

libs_help() {
    cat <<'EOF'
cordinal needs no libraries from the distribution beyond glibc 2.36 or newer on
64-bit x86 (Debian 12, Ubuntu 24.04, Fedora 37 and newer systems), plus at least
one TrueType or OpenType font. It runs on X11 and on Wayland. Alpine and other
musl systems cannot run this build.
EOF
}

refuse_special() {
    case "$KIND" in
        alpine)
            fail "Alpine uses musl. This build is linked against glibc and cannot run there." ;;
    esac
}

# Systems this installer will not change (it only ever installs a font package on them).
read_only_system() {
    case "$KIND" in
        steamos|immutable) return 0 ;;
    esac
    [ -e /run/ostree-booted ]
}

install_selected() {
    # $1 is the tool name; the package names are the remaining words in $selected.
    selected=$1
    shift
    [ $# -ge 1 ] || return 0
    say "Installing $*"
    case "$selected" in
        pacman)
            as_root pacman -S --needed --noconfirm -- "$@" ||
                fail "pacman could not install $*. Update the system yourself, then run this installer again." ;;
        apt)
            as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends -o DPkg::Lock::Timeout=120 -- "$@" ||
                fail "apt could not install $*." ;;
        dnf)
            as_root dnf install -y --setopt=install_weak_deps=False -- "$@" ||
                fail "dnf could not install $*." ;;
        zypper)
            as_root zypper --non-interactive install --no-recommends -- "$@" ||
                fail "zypper could not install $*." ;;
        *)
            libs_help >&2
            fail "This installer does not know how to install packages on $OS_ID." ;;
    esac
}

install_libs() {
    needed=""
    for soname in $LIBS; do
        have_lib "$soname" || needed="$needed $soname"
    done
    font=""
    if ! have_font; then
        font=$(font_pkg "$KIND" || true)
    fi
    if read_only_system; then
        [ -z "$needed" ] || fail "$OS_ID is read-only and lacks$needed. Install it with the system's own tool, then re-run with --no-deps."
        [ -z "$font" ] || say "This system has no font, and this installer will not change $OS_ID. Add a font with its own tool."
        return 0
    fi
    [ -z "$needed" ] && [ -z "$font" ] && return 0
    need_tool
    if [ "$KIND" = apt ] && { [ -n "$needed" ] || [ -n "$font" ]; }; then
        say "Updating package lists"
        as_root env DEBIAN_FRONTEND=noninteractive apt-get update ||
            fail "apt-get update failed. Package sources were not changed."
    fi
    if [ -n "$needed" ]; then
        set --
        for soname in $needed; do
            spec=$(pkg_for "$KIND" "$soname") || fail "No package mapping for $soname."
            if ! picked=$(pick_pkg "$spec"); then
                libs_help >&2
                fail "No package in the configured repositories provides $soname ($spec)."
            fi
            set -- "$@" "$picked"
        done
        install_selected "$KIND" "$@"
    fi
    if [ -n "$font" ] && ! have_font; then
        say "Installing $font so the interface has a typeface"
        case "$KIND" in
            pacman) as_root pacman -S --needed --noconfirm -- "$font" || say "No font package was installed. Text may not appear until a font is." ;;
            apt) as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends -- "$font" || say "No font package was installed. Text may not appear until a font is." ;;
            dnf) as_root dnf install -y --setopt=install_weak_deps=False -- "$font" || say "No font package was installed. Text may not appear until a font is." ;;
            zypper) as_root zypper --non-interactive install --no-recommends -- "$font" || say "No font package was installed. Text may not appear until a font is." ;;
        esac
    fi
}

need_tool() {
    case "$KIND" in
        pacman) command -v pacman >/dev/null 2>&1 || fail "pacman is not installed." ;;
        apt) command -v apt-get >/dev/null 2>&1 || fail "apt-get is not installed." ;;
        dnf) command -v dnf >/dev/null 2>&1 || fail "dnf is not installed." ;;
        zypper) command -v zypper >/dev/null 2>&1 || fail "zypper is not installed." ;;
    esac
}

fetch() {
    url=$1
    dest=$2
    limit=$3
    silent=$4
    if [ "$silent" = 1 ]; then
        curl --proto '=https' --proto-redir '=https' --tlsv1.2 --fail --location --silent --show-error \
            --retry 3 --retry-delay 2 --max-time 180 --max-filesize "$limit" --output "$dest" "$url" || return 1
    else
        curl --proto '=https' --proto-redir '=https' --tlsv1.2 --fail --location --progress-bar \
            --retry 3 --retry-delay 2 --max-time 180 --max-filesize "$limit" --output "$dest" "$url" || return 1
    fi
    bytes=$(wc -c < "$dest")
    [ "$bytes" -gt 0 ] && [ "$bytes" -le "$limit" ]
}

program_loads() {
    bin=$1
    out=$2
    err=$3
    elf_ok "$bin" || fail "The downloaded program is not a 64-bit Linux executable."
    loader=$(glibc_loader) || fail "This system has no glibc dynamic linker for x86-64. The build cannot run on musl."
    status=0
    "$loader" --list "$bin" >"$out" 2>"$err" || status=$?
    if [ "$status" -ne 0 ] || grep -q "not found" "$out" || grep -q "not found" "$err"; then
        cat "$err" >&2
        grep "not found" "$out" >&2 || true
        if grep -q "GLIBC_" "$err"; then
            say "This system's glibc is older than 2.36 (Debian 12), the oldest the published build runs on."
        fi
        libs_help >&2
        fail "cordinal cannot load here, so it was not installed over any copy already present."
    fi
}

# Shell functions share variables with their caller, so these names must not reuse install_files' src.
write_desktop() {
    desk_src=$1
    desk_dest=$2
    desk_stage=$3
    exec_line="Exec=$(desktop_escape "$BIN/cordinal") %F"
    icon_line="Icon=$(desktop_escape "$ICON")"
    found_exec=0
    found_icon=0
    : > "$desk_stage"
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            Exec=*) line=$exec_line; found_exec=1 ;;
            Icon=*) line=$icon_line; found_icon=1 ;;
        esac
        printf '%s\n' "$line" >> "$desk_stage"
    done < "$desk_src"
    [ "$found_exec" = 1 ] || printf '%s\n' "$exec_line" >> "$desk_stage"
    [ "$found_icon" = 1 ] || printf '%s\n' "$icon_line" >> "$desk_stage"
    [ ! -L "$desk_dest" ] || fail "$desk_dest is a symbolic link. Remove it and run the installer again."
    install -m 644 "$desk_stage" "$desk_dest"
}

install_files() {
    src=$1
    work=$2
    for name in cordinal cordinal.desktop cordinal.png; do
        [ -f "$src/$name" ] && [ ! -L "$src/$name" ] || fail "The release is missing $name."
    done
    bytes=$(wc -c < "$src/cordinal")
    [ "$bytes" -ge 100000 ] && [ "$bytes" -le "$MAX_PROGRAM" ] || fail "The program has an unexpected size."
    bytes=$(wc -c < "$src/cordinal.desktop")
    [ "$bytes" -gt 0 ] && [ "$bytes" -le 100000 ] || fail "The launcher entry has an unexpected size."
    bytes=$(wc -c < "$src/cordinal.png")
    [ "$bytes" -gt 0 ] && [ "$bytes" -le 2000000 ] || fail "The icon has an unexpected size."
    program_loads "$src/cordinal" "$work/ld.out" "$work/ld.err"
    mkdir -p "$BIN" "$DATA/applications" "$(dirname "$ICON")"
    [ ! -L "$BIN/cordinal" ] || fail "$BIN/cordinal is a symbolic link. Remove it and run the installer again."
    stage="$BIN/.cordinal.installing.$$"
    install -m 755 "$src/cordinal" "$stage"
    mv -f "$stage" "$BIN/cordinal"
    write_desktop "$src/cordinal.desktop" "$DESKTOP" "$work/desktop"
    [ ! -L "$ICON" ] || fail "$ICON is a symbolic link. Remove it and run the installer again."
    install -m 644 "$src/cordinal.png" "$ICON"
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q "$DATA/applications" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t "$DATA/icons/hicolor" || true
    fi
}

pipewire_rates() {
    if ! command -v pipewire >/dev/null 2>&1; then
        return 0
    fi
    if [ -f "$PW_CONF" ] || grep -rqs allowed-rates "$CONFIG/pipewire" /etc/pipewire 2>/dev/null; then
        return 0
    fi
    mkdir -p "$(dirname "$PW_CONF")"
    cat > "$PW_CONF" <<'CONF'
# From the cordinal installer: PipeWire switches to a stream's sample rate when nothing else is
# playing, so music at 44.1, 88.2, 96 or 192 kHz plays without resampling. Delete this file to undo.
context.properties = {
    default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 352800 384000 ]
}
CONF
    if command -v systemctl >/dev/null 2>&1 && systemctl --user -q is-active pipewire 2>/dev/null; then
        say "Letting PipeWire follow each track's sample rate (sound stops for a moment)"
        systemctl --user restart pipewire pipewire-pulse wireplumber 2>/dev/null || true
    fi
}

self_test() {
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    version_ok "0.1.9" || fail "version 0.1.9 rejected"
    version_ok "2.2.0" || fail "version 2.2.0 rejected"
    version_ok "10.20.30" || fail "version 10.20.30 rejected"
    version_ok "1.2" && fail "short version accepted"
    version_ok "../1.2.3" && fail "path version accepted"
    version_ok "1.2.3;rm" && fail "metacharacter version accepted"
    [ "$(distro_kind debian "")" = apt ] || fail "debian"
    [ "$(distro_kind ubuntu "debian")" = apt ] || fail "ubuntu"
    [ "$(distro_kind linuxmint "ubuntu debian")" = apt ] || fail "mint"
    [ "$(distro_kind arch "")" = pacman ] || fail "arch"
    [ "$(distro_kind endeavouros "arch")" = pacman ] || fail "endeavouros"
    [ "$(distro_kind artix "arch")" = pacman ] || fail "artix"
    [ "$(distro_kind fedora "")" = dnf ] || fail "fedora"
    [ "$(distro_kind rocky "rhel centos fedora")" = dnf ] || fail "rocky"
    [ "$(distro_kind opensuse-tumbleweed "opensuse suse")" = zypper ] || fail "tumbleweed"
    [ "$(distro_kind nixos "")" = immutable ] || fail "nixos"
    [ "$(distro_kind alpine "")" = alpine ] || fail "alpine"
    [ "$(distro_kind steamos "arch")" = steamos ] || fail "steamos"
    [ "$(distro_kind slackware "")" = unknown ] || fail "slackware"
    [ -z "$LIBS" ] || fail "the program needs no distribution libraries"
    for kind in apt pacman dnf zypper; do
        for soname in libfontconfig.so.1 libssl.so.3 libSDL3_ttf.so.0 libpipewire-0.3.so.0 libpulse.so.0 libasound.so.2 \
            libSDL3.so.0 libX11.so.6 libxcb.so.1 libwayland-client.so.0 libxkbcommon.so.0 libstdc++.so.6; do
            pkg_for "$kind" "$soname" >/dev/null && fail "$soname is compiled in, not installed ($kind)"
        done
    done
    [ "$(font_pkg apt)" = fonts-noto-core ] || fail "apt font"
    [ "$(font_pkg zypper)" = noto-sans-fonts ] || fail "zypper font"
    font_pkg alpine >/dev/null && fail "font package for alpine"
    (KIND=steamos; read_only_system) || fail "steamos is read-only"
    (KIND=immutable; read_only_system) || fail "nixos is read-only"
    if [ ! -e /run/ostree-booted ]; then (KIND=apt; read_only_system) && fail "apt systems are not read-only"; fi
    (KIND=alpine; refuse_special) 2>/dev/null && fail "alpine accepted"
    (KIND=steamos; refuse_special) || fail "steamos refused"
    libs_help | grep -q "glibc 2.36" || fail "libs_help names the glibc it needs"
    libs_help | grep -q SDL3 && fail "libs_help still asks for SDL3"
    member_name_ok "cordinal/cordinal" || fail "member"
    member_name_ok "cordinal/../../etc/passwd" && fail "traversal accepted"
    member_name_ok "/etc/passwd" && fail "absolute member accepted"
    printf 'not an elf\n' > "$work/text"
    elf_ok "$work/text" && fail "text accepted as elf"
    found_elf=0
    for candidate in /bin/true /usr/bin/true /bin/dash /usr/bin/dash; do
        if [ -x "$candidate" ] && elf_ok "$candidate"; then
            found_elf=1
            break
        fi
    done
    if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; then
        [ "$found_elf" = 1 ] || fail "no x86-64 ELF found for the checker"
    fi
    [ "$(desktop_escape "/home/a b/cordinal")" = '/home/a\sb/cordinal' ] || fail "desktop escape"
    printf 'a' > "$work/payload"
    hash=$(sha256sum "$work/payload" | awk '{print $1; exit}')
    printf '%s  %s\n' "$hash" "$ARCHIVE" > "$work/good.sha256"
    got=$(sum_hash "$work/good.sha256") || fail "good checksum rejected"
    [ "$got" = "$hash" ] || fail "checksum mismatch"
    printf '%s %s\n' "$hash" "$ARCHIVE" > "$work/one-space.sha256"
    sum_hash "$work/one-space.sha256" >/dev/null && fail "one-space checksum accepted"
    printf '%s  other.tar.gz\n' "$hash" > "$work/wrong-name.sha256"
    sum_hash "$work/wrong-name.sha256" >/dev/null && fail "wrong checksum name accepted"
    printf 'ABCDEF%s  %s\n' "0123456789abcdef0123456789abcdef0123456789abcdef0123456789" "$ARCHIVE" > "$work/upper.sha256"
    sum_hash "$work/upper.sha256" >/dev/null && fail "uppercase checksum accepted"
    printf '%s  %s\n%s  extra\n' "$hash" "$ARCHIVE" "$hash" > "$work/two.sha256"
    sum_hash "$work/two.sha256" >/dev/null && fail "extra checksum line accepted"
    mkdir -p "$work/cordinal"
    printf '#!/bin/sh\n' > "$work/cordinal/cordinal"
    printf '[Desktop Entry]\nExec=cordinal\n' > "$work/cordinal/cordinal.desktop"
    printf 'png\n' > "$work/cordinal/cordinal.png"
    printf 'license\n' > "$work/cordinal/LICENSE"
    tar -czf "$work/good.tar.gz" -C "$work" cordinal
    archive_ok "$work/good.tar.gz" "$work/list" || fail "good archive rejected"
    rm -f "$work/cordinal/cordinal"
    # Windows Git cannot always make a symlink. Linux must, and the archive
    # check has to reject that member. A copied regular file is not this test.
    if ln -s /etc/passwd "$work/cordinal/cordinal" 2>/dev/null && [ -L "$work/cordinal/cordinal" ]; then
        tar -czf "$work/link.tar.gz" -C "$work" cordinal
        archive_ok "$work/link.tar.gz" "$work/list" && fail "symlink archive accepted"
    elif [ "$(uname -s)" = Linux ]; then
        fail "could not build the symlink archive fixture"
    fi
    # The font check reads the directories the player scans, without fontconfig's tools.
    mkdir -p "$work/home/.local/share/fonts/sub"
    : > "$work/home/.local/share/fonts/sub/Face.TTF"
    (HOME="$work/home"; unset XDG_DATA_HOME; XDG_DATA_DIRS="$work/none"; have_font) || fail "a font under ~/.local/share/fonts was missed"
    (HOME="$work/none"; XDG_DATA_HOME="$work/none"; XDG_DATA_DIRS="$work/home/.local/share"; have_font) || fail "a font under XDG_DATA_DIRS was missed"
    printf 'self-test ok\n'
}

version=""
deps=1
dry=0
while [ $# -gt 0 ]; do
    case "$1" in
        --version)
            [ $# -ge 2 ] || fail "--version needs a release number, such as 0.1.9"
            version=$2
            version_ok "$version" || fail "--version must look like 1.2.3"
            shift ;;
        --no-deps) deps="" ;;
        --no-ffmpeg) ;;
        --dry-run) dry=1 ;;
        --self-test) self_test; exit 0 ;;
        --uninstall)
            rm -f "$BIN/cordinal" "$DESKTOP" "$ICON" "$CONFIG/autostart/cordinal.desktop" "$PW_CONF"
            say "Removed cordinal. The library is still in $DATA/cordinal and settings are in $CONFIG/cordinal."
            exit 0 ;;
        *) fail "unknown option $1" ;;
    esac
    shift
done

[ "$(id -u)" -eq 0 ] && fail "Run this as your user account. It installs into the home directory and uses sudo only for a font package."
case "$HOME" in
    *'
'*|*'
'*) fail "HOME contains a line break." ;;
esac
[ "$(uname -m)" = x86_64 ] || fail "This Linux build is for 64-bit x86 (this machine is $(uname -m))."
glibc_loader >/dev/null || fail "This system has no glibc dynamic linker for x86-64. Alpine and other musl systems cannot run this build."

read_os
KIND=$(distro_kind "$OS_ID" "$OS_LIKE")
if [ -n "$deps" ]; then
    refuse_special
fi

if [ "$dry" = 1 ]; then
    say "Distribution $OS_ID ($KIND)."
    if [ -n "$deps" ]; then
        planned=""
        for soname in $LIBS; do
            if have_lib "$soname"; then
                continue
            fi
            planned="$planned $(pkg_for "$KIND" "$soname" || printf '%s' "$soname")"
        done
        if [ -n "$planned" ]; then
            say "Would install$planned"
        else
            say "Needs no libraries beyond glibc."
        fi
        if ! have_font; then
            if read_only_system; then say "No font found; would not change $OS_ID."
            else say "Would install a font: $(font_pkg "$KIND" || printf '%s' none)"
            fi
        fi
    else
        say "Would not install a font package (--no-deps)."
    fi
    say "Would install the program to $BIN/cordinal"
    exit 0
fi

if [ -n "$deps" ]; then
    if [ "$KIND" = unknown ]; then
        absent=""
        for soname in $LIBS; do
            have_lib "$soname" || absent="$absent $soname"
        done
        if [ -n "$absent" ]; then
            libs_help >&2
            fail "Missing$absent. Install them from the distribution, then re-run with --no-deps."
        fi
        have_font || say "No TrueType or OpenType font was found. Install one from the distribution so text appears."
    else
        install_libs
    fi
fi

work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$BIN/.cordinal.installing."*' EXIT
here=$(CDPATH= cd "$(dirname "$0")" 2>/dev/null && pwd || true)
if [ -n "$here" ] && [ -f "$here/cordinal" ] && [ -f "$here/cordinal.desktop" ] && [ -f "$here/cordinal.png" ]; then
    src=$here
else
    command -v curl >/dev/null 2>&1 || fail "curl is required to download the release."
    command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is required to check the download."
    if [ -n "$version" ]; then
        base="https://github.com/$REPO/releases/download/v$version"
    else
        base="https://github.com/$REPO/releases/latest/download"
    fi
    say "Downloading cordinal${version:+ $version}"
    fetch "$base/$ARCHIVE.sha256" "$work/archive.sha256" 4096 1 ||
        fail "Could not download the release checksum. Refusing to install an unchecked program."
    expected=$(sum_hash "$work/archive.sha256") ||
        fail "The release checksum is not in the expected form. Refusing to install."
    fetch "$base/$ARCHIVE" "$work/$ARCHIVE" "$MAX_ARCHIVE" 0 ||
        fail "Could not download $base/$ARCHIVE"
    actual=$(sha256sum "$work/$ARCHIVE" | awk '{print $1; exit}')
    [ "$actual" = "$expected" ] ||
        fail "The download does not match the release checksum. Refusing to install it."
    archive_ok "$work/$ARCHIVE" "$work/members" ||
        fail "The archive contains a member this installer will not unpack."
    mkdir -p "$work/out"
    tar -xzf "$work/$ARCHIVE" -C "$work/out" --no-same-owner --no-same-permissions ||
        fail "Could not unpack the archive."
    src="$work/out/cordinal"
    [ -d "$src" ] && [ ! -L "$src" ] || fail "The archive did not contain the cordinal directory."
fi

install_files "$src" "$work"
pipewire_rates
say "Installed. Open cordinal from the application menu, or run: cordinal"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) say "This terminal does not have $BIN on PATH yet. The application menu uses the full path."
       say "For this terminal: export PATH=\"$BIN:\$PATH\"" ;;
esac
