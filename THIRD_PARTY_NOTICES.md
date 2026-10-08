# Third-party components

Mupen64Plus PS5 as a whole (the `eboot.bin` it builds) is distributed under the **GNU GPL version 3**
([LICENSE](LICENSE)). It is built from these parts:

| Part | Where | License |
| --- | --- | --- |
| mupen64plus-core 2.6.0 (unmodified) | `mupen64plus-core/` | GPL-2.0-or-later (`mupen64plus-core/LICENSES`) |
| The PS5 port: plugins glue, PS5 layer, frontend, build | `ps5/` | MIT (per-file SPDX headers) |
| ps5-native-app-boilerplate tooling (startup code, eboot builder and signer, libc.prx builder) | `ps5/proto/native/` | GPL-3.0-or-later |
| mupen64plus-rsp-cxd4 | `ps5/third_party/rsp-cxd4/` | CC0-1.0 (`COPYING`) |
| angrylion-rdp-plus | `ps5/third_party/angrylion-rdp-plus/` | MAME license (`MAME License.txt`) |
| paraLLEl-RDP (standalone tree, with parts of Granite, volk and the Vulkan headers; Vulkan builds only) | `ps5/third_party/parallel-rdp/` | MIT (`LICENSE`); Vulkan headers Apache-2.0 / MIT |
| RADV, Mesa's Vulkan driver, as built by mihawk-99/PS5_Vulkan (linked, not included here; Vulkan builds only) | outside the repository (`PS5_VULKAN`) | MIT (Mesa) / GPL-3.0-or-later (PS5_Vulkan, its SDK fork) |
| zlib | `ps5/third_party/zlib/` | zlib license (`LICENSE`) |
| stb_image, stb_image_resize2, stb_truetype | `ps5/frontend/third_party/` | MIT / public domain (in each file) |
| Roboto Regular | `ps5/frontend/assets/fonts/` | Apache-2.0 (`Roboto-Regular-copyright`) |
| PromptFont | `ps5/frontend/assets/fonts/` | SIL OFL 1.1 (`promptfont-license`) |
| Font Awesome Brands | `ps5/frontend/assets/fonts/` | SIL OFL 1.1 (`FontAwesome-LICENSE.txt`) |

The app downloads box art at run time from libretro-thumbnails; none is included here.
No games or BIOS files are included.
