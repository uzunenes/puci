#!/bin/sh
# scripts/check-deps.sh - verify build dependencies for puci.
#
# Never installs anything (that needs root and is the package manager's job).
# If something is missing, prints the exact command for this OS and exits 1.
set -u

missing=""
have() { command -v "$1" >/dev/null 2>&1; }

CC_BIN="${CC:-cc}"
have "$CC_BIN" || have gcc || have clang || missing="$missing compiler"
have meson || missing="$missing meson"
have ninja || have ninja-build || missing="$missing ninja"
have pkg-config || have pkgconf || missing="$missing pkg-config"
if have pkg-config || have pkgconf; then
    PKG=$(command -v pkg-config || command -v pkgconf)
    "$PKG" --exists libcurl 2>/dev/null || missing="$missing libcurl"
fi

if [ -z "$missing" ]; then
    echo "puci: all build dependencies found."
    exit 0
fi

echo "puci: missing build dependencies:$missing" >&2
echo >&2

os=$(uname -s)
id=""; id_like=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    id="${ID:-}"; id_like="${ID_LIKE:-}"
fi

cmd=""
case "$os" in
Darwin)
    cmd="brew install meson ninja pkg-config curl" ;;
FreeBSD)
    cmd="sudo pkg install meson ninja pkgconf curl" ;;
*)
    case " $id $id_like " in
    *" debian "*|*" ubuntu "*)
        cmd="sudo apt install build-essential meson ninja-build pkg-config libcurl4-openssl-dev" ;;
    *" fedora "*|*" rhel "*|*" centos "*)
        cmd="sudo dnf install gcc meson ninja-build pkgconf-pkg-config libcurl-devel" ;;
    *" arch "*)
        cmd="sudo pacman -S --needed base-devel meson ninja curl" ;;
    *" opensuse"*|*" suse "*)
        cmd="sudo zypper install gcc meson ninja pkg-config libcurl-devel" ;;
    *" alpine "*)
        cmd="sudo apk add build-base meson ninja pkgconf curl-dev" ;;
    *" void "*)
        cmd="sudo xbps-install -S base-devel meson ninja pkg-config libcurl-devel" ;;
    esac ;;
esac

if [ -n "$cmd" ]; then
    echo "Install them with:" >&2
    echo >&2
    echo "    $cmd" >&2
else
    echo "Install a C17 compiler, meson (>= 1.0), ninja, pkg-config and the" >&2
    echo "libcurl development package with your system package manager." >&2
fi
echo >&2
echo "No root? meson and ninja also install per-user: pipx install meson ninja" >&2
exit 1
