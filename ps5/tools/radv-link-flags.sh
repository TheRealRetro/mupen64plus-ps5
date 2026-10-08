#!/usr/bin/env bash
# Mupen64Plus PS5: prints the linker inputs and flags that link RADV into eboot.bin (`make VULKAN=1`), taken
# from mihawk-99/PS5_Vulkan's own recipe (tools/radv-link.sh) so the app links it exactly as its titles do:
# the archive linked whole, its C++ runtime, the payload SDK fork's platform layer (libps5platform.a) and the
# --wrap/--defsym flags that route malloc and the libc functions no system module exports to it.
#
#   tools/radv-link-flags.sh PS5_VULKAN_ROOT
#
# PS5_VULKAN_ROOT has run tools/setup-native-dependencies.sh and tools/build-radv.sh release.
# SPDX-License-Identifier: MIT

set -euo pipefail
root=$1
sdk_root="$root/.deps/native/ps5-payload-sdk"
archive="$root/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
[[ -f $archive ]] || { echo "missing $archive: run tools/build-radv.sh release in $root" >&2; exit 2; }
PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
export PS5_CLANG
# shellcheck source=/dev/null
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive" >&2
printf '%s ' "${radv_link_flags[@]}" "${radv_link_inputs[@]}"
echo
