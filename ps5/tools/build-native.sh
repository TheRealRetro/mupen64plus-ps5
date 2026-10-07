#!/usr/bin/env bash
# build-native.sh - builds the PS5 native application (runs in Linux / WSL), in the Homebrew Browser's layout.
#
# Usage (from WSL):  ps5/tools/build-native.sh [Folder|Ffpfsc] [Clean]
# Windows:           build-native.bat [Folder|Ffpfsc] [Clean]
#
# Clean deletes ps5/build first, so nothing from an earlier build is reused. Releases always build clean
# (release.bat): 0.6.1 was published from an incremental build that didn't start on the console, while a clean
# build of the same source did.
#
# Output in build-native/ (next to ps5/): PPSA99064/, Mupen64PlusPS5.zip, PPSA99064.debug.elf, and PPSA99064.ffpfsc
# with Ffpfsc. The version (param.json's contentVersion) comes from VERSION in ps5/Makefile.
#
# Environment:
#   PS5_PAYLOAD_SDK         the ps5-payload-sdk (default: the boilerplate's, below)
#   PS5_NATIVE_BOILERPLATE  checkout of ps5-native-app-boilerplate (default /root/ps5-native; only for Ffpfsc)
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
format=Folder
clean=0
for arg in "$@"; do
    case "${arg,,}" in
        folder|ffpfsc) format=$arg ;;
        clean) clean=1 ;;
        *) echo "usage: build-native.sh [Folder|Ffpfsc] [Clean]" >&2; exit 2 ;;
    esac
done
bp=${PS5_NATIVE_BOILERPLATE:-/root/ps5-native}
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$bp/.deps/native/ps5-payload-sdk}
export PATH=/usr/lib/llvm-18/bin:$PATH

cd "$ps5"
if [[ $clean == 1 ]]; then
    echo "==> clean build: removing ps5/build"
    make clean
fi
make -j"$(nproc)" native FORMAT="$format" PS5_NATIVE_BOILERPLATE="$bp"
