#!/usr/bin/env bash
# build-native.sh - builds the PS5 native application (runs in Linux / WSL), in the Homebrew Browser's layout.
#
# Usage (from WSL):  ps5/tools/build-native.sh [Folder|Ffpfsc]
# Windows:           build-native.bat
#
# Output in build-native/ (next to ps5/): PPSA99064/, PPSA99064.zip, PPSA99064.debug.elf, and PPSA99064.ffpfsc
# with Ffpfsc. The version (param.json's contentVersion) comes from VERSION in ps5/Makefile.
#
# Environment:
#   PS5_PAYLOAD_SDK         the ps5-payload-sdk (default: the boilerplate's, below)
#   PS5_NATIVE_BOILERPLATE  checkout of ps5-native-app-boilerplate (default /root/ps5-native; only for Ffpfsc)
set -euo pipefail

ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
format=${1:-Folder}
case "${format,,}" in folder|ffpfsc) ;; *)
    echo "usage: build-native.sh [Folder|Ffpfsc]" >&2
    exit 2
esac
bp=${PS5_NATIVE_BOILERPLATE:-/root/ps5-native}
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$bp/.deps/native/ps5-payload-sdk}
export PATH=/usr/lib/llvm-18/bin:$PATH

cd "$ps5"
make -j"$(nproc)" native FORMAT="$format" PS5_NATIVE_BOILERPLATE="$bp"
