#!/bin/sh
# scripts/check-deps.sh - check what `make` needs: a C compiler, (GNU) make
# and libcurl's development files. Never installs anything; if something is
# missing it prints the command for this OS and exits 1.
set -u

missing=""
have() { command -v "$1" >/dev/null 2>&1; }

CC_BIN="${CC:-cc}"
have "$CC_BIN" || have gcc || have clang || missing="$missing compiler"
have make || have gmake || missing="$missing make"
if have pkg-config && pkg-config --exists libcurl 2>/dev/null; then :
elif have curl-config; then :
elif [ -n "${CURL_LIBS:-}" ]; then :
else
    # Last resort: is <curl/curl.h> visible to the compiler?
    printf '#include <curl/curl.h>\nint main(void){return 0;}\n' |
        "$CC_BIN" -x c -fsyntax-only - 2>/dev/null || missing="$missing libcurl-dev"
fi

if [ -z "$missing" ]; then
    echo "puci: build dependencies found."
    exit 0
fi

echo "puci: missing:$missing" >&2
echo >&2
id=""; id_like=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    id="${ID:-}"; id_like="${ID_LIKE:-}"
fi
cmd=""
case "$(uname -s)" in
Darwin)  cmd="xcode-select --install   # compiler + make; libcurl ships with macOS" ;;
FreeBSD) cmd="pkg install gmake curl pkgconf" ;;
OpenBSD) cmd="pkg_add gmake curl" ;;
NetBSD)  cmd="pkgin install gmake curl" ;;
*)
    case " $id $id_like " in
    *" debian "*|*" ubuntu "*) cmd="sudo apt install build-essential libcurl4-openssl-dev pkg-config" ;;
    *" fedora "*|*" rhel "*|*" centos "*) cmd="sudo dnf install gcc make libcurl-devel pkgconf-pkg-config" ;;
    *" arch "*)    cmd="sudo pacman -S --needed base-devel curl" ;;
    *" opensuse"*|*" suse "*) cmd="sudo zypper install gcc make libcurl-devel pkg-config" ;;
    *" alpine "*)  cmd="sudo apk add build-base curl-dev pkgconf" ;;
    *" void "*)    cmd="sudo xbps-install -S base-devel libcurl-devel pkg-config" ;;
    *" gentoo "*)  cmd="emerge --ask net-misc/curl   # toolchain is already there" ;;
    esac ;;
esac
if [ -n "$cmd" ]; then
    echo "Install with:" >&2
    echo "    $cmd" >&2
else
    echo "Install a C17 compiler, make and libcurl's development package." >&2
fi
exit 1
