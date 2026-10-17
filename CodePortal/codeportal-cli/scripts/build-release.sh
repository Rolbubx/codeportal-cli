#!/usr/bin/env bash
# Builds everything a release needs, from Linux (MinGW-w64 + NSIS), into dist/:
#   codeportal.exe                      static Windows x64 executable, with icon and version info
#   codeportal-chat.html                the web chat page
#   CodePortal-Setup-<version>.exe      the installer
#   codeportal-<version>-windows-x64.zip  portable zip (codeportal.exe + web/)
#   SHA256SUMS.txt                      checksums, checked by `codeportal --update`
#
# Needs: g++-mingw-w64-x86-64-posix, nsis, zip.   Usage: scripts/build-release.sh   (from any folder)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define CODEPORTAL_VERSION "\(.*\)"/\1/p' src/version.hpp)
[ -n "$VERSION" ] || { echo "Cannot read CODEPORTAL_VERSION from src/version.hpp" >&2; exit 1; }
echo "Building CodePortal $VERSION"

CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
rm -rf dist && mkdir -p dist obj

"$WINDRES" res/codeportal.rc -O coff -o obj/codeportal.res.o
"$CXX" -std=gnu++17 -O2 -Wall -Wextra -pthread src/main.cpp obj/codeportal.res.o -o dist/codeportal.exe \
  -static -s -lwinhttp -lshell32 -lws2_32 -lbcrypt
rm -rf obj

# The executable must not need any DLL that Windows does not ship.
x86_64-w64-mingw32-objdump -p dist/codeportal.exe | grep "DLL Name" | grep -viE \
  "kernel32|user32|advapi32|shell32|winhttp|ws2_32|bcrypt|msvcrt" && { echo "Unexpected DLL dependency" >&2; exit 1; }

cp web/codeportal-chat.html dist/
makensis -V2 -DVERSION="$VERSION" -DEXE="../dist/codeportal.exe" -DPAGE="../dist/codeportal-chat.html" \
  -DOUTFILE="../dist/CodePortal-Setup-$VERSION.exe" installer/codeportal.nsi

(
  tmp=$(mktemp -d)
  mkdir -p "$tmp/CodePortal/web"
  cp dist/codeportal.exe "$tmp/CodePortal/"
  cp dist/codeportal-chat.html "$tmp/CodePortal/web/"
  out="$PWD/dist/codeportal-$VERSION-windows-x64.zip"
  cd "$tmp" && zip -qr "$out" CodePortal
  rm -rf "$tmp"
)

(cd dist && sha256sum codeportal.exe codeportal-chat.html "CodePortal-Setup-$VERSION.exe" \
  "codeportal-$VERSION-windows-x64.zip" > SHA256SUMS.txt)
ls -l dist
