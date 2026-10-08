# Mupen64Plus PS5
#   make ps5    -> build/ps5/Mupen64PS5.elf   the payload to send with the PS5 Payload Manager / ELF loader: it
#                  installs or updates the dashboard app /data/homebrew/$(TITLE_ID)/ (mupen64plus itself, icon
#                  included) and stays running as the helper that lets the app reach /data
#   make app    -> build/app/$(TITLE_ID)/     the app folder on its own (eboot.bin, sce_module/, sce_sys/)
#   make host   -> build/host/                Linux builds of the same code (PS5 calls faked by host/sce_host.cpp)
#   make native -> ../build-native/: $(TITLE_ID)/ (the app folder), $(RELEASE_ZIP) (the release),
#                  $(TITLE_ID).debug.elf, and $(TITLE_ID).ffpfsc with FORMAT=Ffpfsc -- the layout of the Homebrew
#                  Browser's build-native.bat (../build-native.bat runs this)
#   make dist   -> make native + build/dist/Mupen64PS5-v$(VERSION).elf (the optional installer) and
#                  mupen64plus-ps5-v$(VERSION)-src.zip
#   make send   -> sends Mupen64PS5.elf to $(PS5_HOST):$(PS5_PORT) with the SDK's prospero-deploy
#
# The PS5 layer is the one of Snes9x PS5 (PS5SX2's layout): eboot.bin is the emulator, a native PS5 title (the
# console gives the controller to the title in front only), and the payload is its installer and helper.
# Build order:
#   1. build/ps5/Mupen64PS5-helper.elf   the helper alone (payload); the app sends it to the ELF loader itself
#   2. build/app/$(TITLE_ID)/             eboot.bin = mupen64plus + the helper from step 1 (native title, signed);
#                                         sce_module/libc.prx; sce_sys/param.json, icon0.png, pic0.dds, pic1.dds
#   3. build/ps5/Mupen64PS5.elf           installer + helper, with the app folder from step 2 built in
#
# What goes into eboot.bin:
#   - mupen64plus-core 2.6.0 (../mupen64plus-core, unmodified), with its x86-64 dynarec, minus the parts that need
#     SDL/OpenGL/libpng/dlopen, which n64/core/*.c replace; n64/sdl is the bit of SDL it still uses;
#   - its plugins, linked in (n64/plugins): mupen64plus-rsp-cxd4 (LLE RSP) behind mupen64plus-rsp-hle (HLE
#     audio), angrylion-rdp-plus (software RDP/VI), AudioOut and DualSense input;
#   - the frontend (shelf, menus, covers) and the PS5 layer (coreorbis).
#
# Dependencies: the ps5-payload-dev SDK (v0.42 or newer, https://github.com/ps5-payload-dev/sdk) in
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host, and python3. zlib is vendored in third_party/zlib.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 0.6.3
# param.json's contentVersion, NN.NNN.NNN (0.6.1 -> 00.006.001): the release tag, as in the Homebrew Browser; it
# must also be written in app/sce_sys/param.json (checked below)
CONTENT_VERSION := $(shell echo $(VERSION) | awk -F. '{printf "%02d.%03d.%03d", $$1, $$2, $$3}')
TITLE_ID ?= PPSA99064
# where the app looks for updates (fe_update.h) and `make release` publishes
GITHUB_REPO ?= TheRealRetro/mupen64plus-ps5
# The release zip's name, which the app's updater looks for. Up to 0.6.2 it was $(TITLE_ID).zip, but those
# versions' updater wrote files without execute permission and left the app unable to start: under this name
# they never find a newer release, so they can't break themselves (their users install 0.6.3 by hand once).
RELEASE_ZIP := Mupen64PlusPS5.zip
# The GPU renderer (paraLLEl-RDP, setting Renderer; GitHub issue #1): the app links RADV, the Vulkan driver
# mihawk-99/PS5_Vulkan builds for the console (in $(PS5_VULKAN): tools/setup-native-dependencies.sh, then
# tools/build-radv.sh release), and runs a Vulkan self-test at start (coreorbis/orbis-shims/ProsperoVulkan.h).
# VULKAN=0 builds the app without it (angrylion only, eboot.bin ~4 MB instead of ~30 MB) in folders of its own,
# with the app in ../build-native-cpu/. The host build never has it.
ifneq ($(filter host%,$(MAKECMDGOALS)),)
VULKAN ?= 0
endif
VULKAN ?= 1
PS5_VULKAN ?= /root/gpu/PS5_Vulkan
ifeq ($(VULKAN),1)
APP_BUILD := build/app
else
APP_BUILD := build/app-cpu
endif
APP_OUT := $(APP_BUILD)/$(TITLE_ID)

M64 := ../mupen64plus-core
M64SRC := $(M64)/src
ZLIB := third_party/zlib
CXD4 := third_party/rsp-cxd4
RSPHLE := third_party/rsp-hle/src
ANGRYLION := third_party/angrylion-rdp-plus/src

# ---- mupen64plus-core (the unix Makefile's list, without vidext/eventloop/screenshot/sdl_key_converter and the
# dlopen osal, which n64/core replaces)
CORE_SRC := \
	api/callbacks.c api/common.c api/config.c api/debugger.c api/frontend.c \
	backends/api/video_capture_backend.c backends/plugins_compat/audio_plugin_compat.c \
	backends/plugins_compat/input_plugin_compat.c backends/clock_ctime_plus_delta.c backends/dummy_video_capture.c \
	backends/file_storage.c \
	device/cart/cart.c device/cart/af_rtc.c device/cart/cart_rom.c device/cart/eeprom.c device/cart/flashram.c \
	device/cart/is_viewer.c device/cart/sram.c \
	device/controllers/game_controller.c device/controllers/vru_controller.c device/controllers/paks/biopak.c \
	device/controllers/paks/mempak.c device/controllers/paks/rumblepak.c device/controllers/paks/transferpak.c \
	device/dd/dd_controller.c device/dd/disk.c device/device.c \
	device/gb/gb_cart.c device/gb/mbc3_rtc.c device/gb/m64282fp.c \
	device/memory/memory.c \
	device/pif/bootrom_hle.c device/pif/cic.c device/pif/n64_cic_nus_6105.c device/pif/pif.c \
	device/r4300/cached_interp.c device/r4300/cp0.c device/r4300/cp1.c device/r4300/cp2.c device/r4300/idec.c \
	device/r4300/interrupt.c device/r4300/pure_interp.c device/r4300/r4300_core.c device/r4300/tlb.c \
	device/rcp/ai/ai_controller.c device/rcp/mi/mi_controller.c device/rcp/pi/pi_controller.c \
	device/rcp/rdp/fb.c device/rcp/rdp/rdp_core.c device/rcp/ri/ri_controller.c device/rcp/rsp/rsp_core.c \
	device/rcp/si/si_controller.c device/rcp/vi/vi_controller.c device/rdram/rdram.c \
	main/main.c main/util.c main/cheat.c main/rom.c main/savestates.c main/workqueue.c \
	plugin/plugin.c plugin/dummy_video.c plugin/dummy_audio.c plugin/dummy_input.c plugin/dummy_rsp.c \
	osal/files_unix.c device/r4300/new_dynarec/new_dynarec.c
CORE_SUB_SRC := md5/md5.c minizip/ioapi.c minizip/unzip.c minizip/zip.c
# the port's side of the core (C)
N64_SRC := n64/sdl/sdl_shim.c n64/core/perf.c n64/core/jit_ps5.c n64/core/vidext_ps5.c n64/core/eventloop_ps5.c n64/core/screenshot_ps5.c \
	n64/core/osal_dynlib_static.c
# the plugins
PLUGIN_C_SRC := n64/plugins/rsp_cxd4_ps5.c n64/plugins/rsp_hle_ps5.c n64/plugins/gfx_ps5.c
PLUGIN_CXX_SRC := n64/plugins/audio_ps5.cpp n64/plugins/input_ps5.cpp
ANGRYLION_C_SRC := core/n64video.c
# mupen64plus-rsp-hle's core (its plugin.c is replaced by n64/plugins/rsp_hle_ps5.c): HLE audio etc.
RSPHLE_SRC := alist.c alist_audio.c alist_naudio.c alist_nead.c audio.c cicx105.c hle.c hvqm.c jpeg.c memory.c mp3.c \
	musyx.c re2.c
ANGRYLION_CXX_SRC := core/parallel.cpp

# the x86-64 new dynarec (DYNAREC, NEW_DYNAREC=2): its code cache is made executable by n64/core/jit_ps5.c
CORE_DEFS := -DNDEBUG -DNOCRYPT -DNOUNCRYPT -DM64P_PARALLEL -DDYNAREC -DNEW_DYNAREC=2
# new_dynarec.c's only mprotect (RWX on its code cache) goes to the port
DYNAREC_FLAGS := -Dmprotect=n64ps5_jit_mprotect
CORE_INCS := -In64/sdl -I$(M64SRC) -I$(M64)/subprojects/md5 -I$(M64)/subprojects/xxhash -I$(M64)/subprojects/minizip -I$(ZLIB)
CXD4_DEFS := -DM64P_PLUGIN_API -DARCH_MIN_SSE2 -DM64PS5_NO_RDRAM_PROBE -DNDEBUG
CXD4_INCS := -I$(CXD4) -I$(RSPHLE) -I$(M64SRC)/api -In64 -In64/core -I$(M64SRC)
GFX_INCS := -I$(ANGRYLION) -I$(M64SRC) -In64 -In64/core

# the app (eboot.bin): the frontend and the PS5 layer
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoUpdateJob.cpp \
	frontend/fe_emu.cpp frontend/fe_menu.cpp frontend/fe_settings.cpp frontend/fe_text.cpp \
	frontend/fe_games.cpp frontend/fe_http.cpp frontend/fe_covers.cpp frontend/fe_shelf.cpp \
	frontend/fe_prefetch.cpp frontend/fe_update.cpp
# the payloads: the helper, and the installer (= helper + install)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoUpdateJob.cpp
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp

ZLIB_SRC := $(wildcard $(ZLIB)/*.c)

INCS := -I$(M64SRC) -I$(M64)/subprojects/minizip -I$(ZLIB) \
	-Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ifrontend -In64 -In64/core
DEFS := -DN64PS5_VERSION='"$(VERSION)"' -DN64PS5_TITLE_ID='"$(TITLE_ID)"' \
	-DN64PS5_CONTENT_VERSION='"$(CONTENT_VERSION)"' -DN64PS5_GITHUB_REPO='"$(GITHUB_REPO)"' \
	-DN64PS5_RELEASE_ZIP='"$(RELEASE_ZIP)"'
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers
ifeq ($(VULKAN),1)
VULKAN_SRC := coreorbis/orbis-shims/ProsperoVulkan.cpp
DEFS += -DN64PS5_VULKAN
endif

# generated: the game-name table (from the core's ROM catalog) built into fe_games.o
GEN := build/gen
GAMEDB := $(GEN)/n64-gamedb.tsv
M64_INI := $(M64)/data/mupen64plus.ini
$(GAMEDB): tools/make_n64db.py $(M64_INI)
	@mkdir -p $(dir $@)
	python3 tools/make_n64db.py $(M64_INI) $@

%/port/frontend/fe_games.o: EXTRA_FLAGS = -DGAMEDB_TSV='"$(abspath $(GAMEDB))"'
# the ROM catalog itself goes into fe_emu.o (written to /data/mupen64plus/data for the core)
%/port/frontend/fe_emu.o: EXTRA_FLAGS = -DM64P_INI_PATH='"$(abspath $(M64_INI))"'
# stb_image in fe_covers.o has warnings of its own
%/port/frontend/fe_covers.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough
# the UI fonts (PS5SX2's: PCSX2's Roboto Regular and PromptFont) built into fe_text.o
%/port/frontend/fe_text.o: EXTRA_FLAGS = -DFE_FONT_DIR='"$(abspath frontend/assets/fonts)"' -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/frontend/fe_text.o: frontend/assets/fonts/Roboto-Regular.ttf frontend/assets/fonts/promptfont.otf \
	frontend/assets/fonts/fa-brands-400.otf
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DN64PS5_HELPER_ONLY

# a file named after the version: everything that prints it is rebuilt when VERSION changes
VERSION_STAMP := build/version-$(VERSION).stamp
$(VERSION_STAMP):
	@mkdir -p build && rm -f build/version-*.stamp && touch $@

PORT_HDRS := $(wildcard coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h n64/*.h n64/core/*.h) 	$(VERSION_STAMP)

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
ifeq ($(VULKAN),1)
PS5_OUT := build/ps5
else
PS5_OUT := build/ps5-cpu
endif
# The PS5's CPU is Zen 2 (PS5SX2 builds with the same -march).
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
# the payloads: notifications, and the kernel access the helper needs (libkernel_sys: sceKernelGetAppInfo)
PS5_PAYLOAD_LIBS := -lkernel_sys

PS5_CORE_OBJ := $(patsubst %.c,$(PS5_OUT)/m64/%.o,$(CORE_SRC)) $(patsubst %.c,$(PS5_OUT)/m64sub/%.o,$(CORE_SUB_SRC)) \
	$(PS5_OUT)/n64/n64/core/linkage_x64.o \
	$(patsubst %.c,$(PS5_OUT)/n64/%.o,$(N64_SRC)) \
	$(patsubst %.c,$(PS5_OUT)/n64/%.o,$(PLUGIN_C_SRC)) $(patsubst %.cpp,$(PS5_OUT)/n64/%.o,$(PLUGIN_CXX_SRC)) \
	$(patsubst %.c,$(PS5_OUT)/angrylion/%.o,$(ANGRYLION_C_SRC)) $(patsubst %.cpp,$(PS5_OUT)/angrylion/%.o,$(ANGRYLION_CXX_SRC)) \
	$(patsubst %.c,$(PS5_OUT)/rsphle/%.o,$(RSPHLE_SRC))
PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC) $(VULKAN_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC))
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC))
PS5_ZLIB_OBJ := $(patsubst $(ZLIB)/%.c,$(PS5_OUT)/zlib/%.o,$(ZLIB_SRC))

ps5: $(PS5_OUT)/Mupen64PS5.elf

# mupen64plus-core: third-party C, its own warnings are not ours
$(PS5_OUT)/m64/%.o: $(M64SRC)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(PS5_OUT)/m64/device/r4300/new_dynarec/new_dynarec.o: $(M64SRC)/device/r4300/new_dynarec/new_dynarec.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) $(DYNAREC_FLAGS) -w -c -o $@ $<

# the struct offsets the dynarec's assembly needs (the core's tools/gen_asm_script.sh, on our compiler's object)
$(PS5_OUT)/gen/asm_defines.o: $(M64SRC)/asm_defines/asm_defines.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<
$(PS5_OUT)/gen/asm_defines_gas.h: $(PS5_OUT)/gen/asm_defines.o
	rm -f $(dir $@)asm_defines_*.h && bash $(M64)/tools/gen_asm_script.sh $(dir $@) $<

$(PS5_OUT)/n64/n64/core/linkage_x64.o: n64/core/linkage_x64.S $(PS5_OUT)/gen/asm_defines_gas.h
	@mkdir -p $(dir $@)
	$(PS5_CC) -I$(PS5_OUT)/gen -c -o $@ $<

$(PS5_OUT)/m64sub/%.o: $(M64)/subprojects/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(PS5_OUT)/n64/n64/plugins/rsp_cxd4_ps5.o: n64/plugins/rsp_cxd4_ps5.c $(wildcard $(CXD4)/*.c $(CXD4)/*.h $(CXD4)/vu/*) n64/core/static_dynlib.h n64/n64_bridge.h
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CXD4_DEFS) $(CXD4_INCS) -w -c -o $@ $<

$(PS5_OUT)/n64/n64/plugins/gfx_ps5.o: n64/plugins/gfx_ps5.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(GFX_INCS) -Wall -Wextra -Werror -Wno-unused-parameter -c -o $@ $<

$(PS5_OUT)/n64/%.o: %.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -In64 -In64/core -Wall -Wextra -Werror -Wno-unused-parameter -c -o $@ $<

$(PS5_OUT)/n64/%.o: %.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -c -o $@ $<

$(PS5_OUT)/rsphle/%.o: $(RSPHLE)/%.c $(wildcard $(RSPHLE)/*.h)
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) -DNDEBUG -I$(RSPHLE) -w -c -o $@ $<

$(PS5_OUT)/angrylion/%.o: $(ANGRYLION)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) -I$(ANGRYLION) -w -c -o $@ $<

$(PS5_OUT)/angrylion/%.o: $(ANGRYLION)/%.cpp
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) -DM64PS5_BIG_STACKS -I$(ANGRYLION) -w -c -o $@ $<

$(PS5_OUT)/port/%.o: %.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# the Vulkan self-test (VULKAN=1): RADV's headers, and its compute shader compiled to SPIR-V
RADV := $(PS5_VULKAN)/.deps/native/radv-release
$(PS5_OUT)/gen/probe_comp.h: coreorbis/orbis-shims/shaders/probe.comp
	@mkdir -p $(dir $@)
	glslangValidator -V --target-env vulkan1.1 --vn kProbeComp -o $@ $< > /dev/null

$(PS5_OUT)/port/coreorbis/orbis-shims/ProsperoVulkan.o: coreorbis/orbis-shims/ProsperoVulkan.cpp \
		$(PS5_OUT)/gen/probe_comp.h $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) -I$(RADV)/include -I$(PS5_OUT)/gen $(PORT_WARN) -c -o $@ $<

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -O2 -fPIC -march=znver2 -DHAVE_UNISTD_H -I$(ZLIB) -w -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/Mupen64PS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/Mupen64PS5-helper.elf
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(PS5_OUT)/Mupen64PS5-helper.elf)"' -c -o $@ $<

# 3. the installer + helper, with the app folder built in
APP_FILES = $(APP_OUT)/eboot.bin $(APP_OUT)/sce_sys/param.json $(APP_OUT)/sce_sys/icon0.png $(APP_OUT)/sce_sys/pic0.dds \
	$(APP_OUT)/sce_sys/pic1.dds \
	$(APP_OUT)/sce_module/libc.prx
APP_DATA_DEFS = -DAPP_EBOOT='"$(abspath $(APP_OUT)/eboot.bin)"' -DAPP_PARAM='"$(abspath $(APP_OUT)/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath $(APP_OUT)/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath $(APP_OUT)/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath $(APP_OUT)/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"'

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(APP_DATA_DEFS) -c -o $@ $<

$(PS5_OUT)/Mupen64PS5.elf: $(PS5_INSTALLER_OBJ) $(PS5_OUT)/install_data.o
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)
	@echo "built $@ ($$(stat -c %s $@) bytes)"

$(PS5_OUT)/Mupen64PS5-stripped.elf: $(PS5_OUT)/Mupen64PS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip -o $@ $<

strip: $(PS5_OUT)/Mupen64PS5-stripped.elf

# The release, in the Homebrew Browser's layout (its native/build-native.sh), in ../build-native/:
#   $(TITLE_ID)/            the app folder: copied to /data/homebrew/ (or a USB drive's homebrew/), ShadowMountPlus
#                           lists it
#   $(RELEASE_ZIP)         the release artifact: just $(TITLE_ID)/ (the browser installs a release's
#                           $(RELEASE_ZIP); the tag is the contentVersion, $(CONTENT_VERSION))
#   $(TITLE_ID).debug.elf   eboot.bin's program with its symbols, for reading crash reports
#   $(TITLE_ID).ffpfsc      with FORMAT=Ffpfsc: a compressed image of the folder (the boilerplate's mkpfs)
#   release-assets/$(CONTENT_VERSION)/$(RELEASE_ZIP)   a copy kept per version
ifeq ($(VULKAN),1)
NATIVE_OUT := ../build-native
else
NATIVE_OUT := ../build-native-cpu
endif
FORMAT ?= Folder
PS5_NATIVE_BOILERPLATE ?= /root/ps5-native
native: $(APP_FILES) $(APP_BUILD)/mupen64plus-pie.elf
	@mkdir -p "$(abspath $(NATIVE_OUT))"
	rm -rf "$(abspath $(NATIVE_OUT))/$(TITLE_ID)" "$(abspath $(NATIVE_OUT))/$(RELEASE_ZIP)"
	cp -r $(APP_OUT) "$(abspath $(NATIVE_OUT))/"
	cd "$(abspath $(NATIVE_OUT))" && python3 -m zipfile -c $(RELEASE_ZIP) $(TITLE_ID)
	cp $(APP_BUILD)/mupen64plus-pie.elf "$(abspath $(NATIVE_OUT))/$(TITLE_ID).debug.elf"
	mkdir -p "$(abspath $(NATIVE_OUT))/release-assets/$(CONTENT_VERSION)"
	cp "$(abspath $(NATIVE_OUT))/$(RELEASE_ZIP)" "$(abspath $(NATIVE_OUT))/release-assets/$(CONTENT_VERSION)/"
	@if [ "$$(echo $(FORMAT) | tr A-Z a-z)" = ffpfsc ]; then \
		mkpfs=$$(bash $(PS5_NATIVE_BOILERPLATE)/tools/setup-packaging-dependencies.sh ffpfsc) && \
		rm -f "$(abspath $(NATIVE_OUT))/$(TITLE_ID).ffpfsc" && \
		"$$mkpfs" pack folder --no-adjust-output-file-extension --version PS5 --verify \
			"$(abspath $(NATIVE_OUT))/$(TITLE_ID)" "$(abspath $(NATIVE_OUT))/$(TITLE_ID).ffpfsc"; \
	fi
	@python3 -m zipfile -l "$(abspath $(NATIVE_OUT))/$(RELEASE_ZIP)"
	@echo "Native app: $(abspath $(NATIVE_OUT))/$(TITLE_ID)  (copy this folder to /data/homebrew/), version $(CONTENT_VERSION)"

# the extra release files: the optional installer payload and the sources
DIST := build/dist
dist: native $(PS5_OUT)/Mupen64PS5.elf
	@mkdir -p $(DIST)
	cp $(PS5_OUT)/Mupen64PS5.elf $(DIST)/Mupen64PS5-v$(VERSION).elf
	cd .. && top="$$(basename "$$(pwd)")" && cd .. && rm -f "$(abspath $(DIST))/mupen64plus-ps5-v$(VERSION)-src.zip" && \
		zip -qr "$(abspath $(DIST))/mupen64plus-ps5-v$(VERSION)-src.zip" "$$top" \
		-x '*/ps5/build/*' '*/proto/native/build/*' '*.o' '*.z64' '*.n64' '*.v64'
	@ls -l $(DIST)

send: $(PS5_OUT)/Mupen64PS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

# ---------------- the app (eboot.bin = mupen64plus) ----------------
# 2. A native PS5 title linked the way PS5SX2 links its eboot (link-vk.sh): the boilerplate's app_crt.cpp
# (_start -> _init_env -> main) and app_cpp_runtime.cpp, the emulator's objects, a PIE laid out by ps5-pie.ld
# (+ PS5SX2's ehframe.ld) against the SDK's system-module stubs and its static libc++/libc++abi/libunwind/libc,
# every defined symbol local (localize.map); then ps5-native-tool link (SCE dynamic tables, process
# parameters) and self --sign. The folder carries sce_module/libc.prx, the C runtime module a native title
# needs.
NATIVE := proto/native
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
LIBC_BUILDER := $(NATIVE)/build/host/libc-builder
LLD ?= ld.lld
NATIVE_CLANG = PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(NATIVE)/tooling/prospero-clang18
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
# every system-module stub but the two PS5SX2 leaves out (link-vk.sh)
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,$(sort $(wildcard $(SDK_LIB)/*.so)))

$(NATIVE_TOOL): $(NATIVE)/tooling/native/native_app_builder.cpp $(NATIVE)/tooling/native/elf_object.cpp \
		$(NATIVE)/tooling/native/self_container.cpp $(NATIVE)/tooling/native/sce_module_writer.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $^ -lz
HOST_CXX_NATIVE ?= clang++

$(LIBC_BUILDER): $(NATIVE)/tooling/native/libc_builder.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -Wall -Wextra -Werror -o $@ $<

# sce_module/libc.prx, deterministic: it must match the published digest (proto/native/libc.prx.sha256)
build/app/libc.prx: $(LIBC_BUILDER) $(NATIVE_TOOL) $(wildcard $(NATIVE)/tooling/native/runtime/*.txt)
	@mkdir -p $(dir $@)
	$(LIBC_BUILDER) $(NATIVE)/tooling/native/runtime/api-surface.txt $(NATIVE)/tooling/native/runtime/imports.txt build/app/libc.raw.elf
	$(NATIVE_TOOL) self --sign --in build/app/libc.raw.elf --out $@
	cd build/app && sha256sum --check --strict "$(abspath $(NATIVE)/libc.prx.sha256)"

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

$(APP_BUILD)/app_crt.o $(APP_BUILD)/app_cpp_runtime.o: $(APP_BUILD)/%.o: $(NATIVE)/tooling/native/%.cpp
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

# VULKAN=1: RADV and what it needs (tools/radv-link-flags.sh prints PS5_Vulkan's recipe), and link stubs that
# name the AGC system modules it calls (PS5_Vulkan's tools/build-radv-title.sh builds the same two)
ifeq ($(VULKAN),1)
ifeq ($(filter host% clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(RADV)/lib/libvulkan_radeon.ps5.a),)
$(error RADV is missing ($(RADV)/lib/libvulkan_radeon.ps5.a): check out mihawk-99/PS5_Vulkan in PS5_VULKAN \
	and run its tools/setup-native-dependencies.sh and tools/build-radv.sh release, or build with VULKAN=0)
endif
endif
VULKAN_STUBS := $(APP_BUILD)/stubs/libSceAgc.so $(APP_BUILD)/stubs/libSceAgcDriver.so
VULKAN_LINK := $$(bash tools/radv-link-flags.sh $(PS5_VULKAN))
# the RADV archive has zlib built in (its shader cache), which stands in for ours
EBOOT_ZLIB :=
# the GPU renderer: paraLLEl-RDP (third_party/parallel-rdp, its standalone tree; config.mk lists its files)
# and its plugin glue (n64/plugins/gfx_parallel_ps5.cpp)
PARALLEL_RDP_IMPLEMENTATION := third_party/parallel-rdp
include $(PARALLEL_RDP_IMPLEMENTATION)/config.mk
VULKAN_OBJ := $(patsubst %.cpp,$(PS5_OUT)/prdp/%.o,$(PARALLEL_RDP_SOURCES_CXX)) \
	$(patsubst %.c,$(PS5_OUT)/prdp/%.o,$(PARALLEL_RDP_SOURCES_C)) $(PS5_OUT)/n64/n64/plugins/gfx_parallel_ps5.o
# its headers, so a change to one rebuilds what includes it
PRDP_HDRS := $(wildcard $(PARALLEL_RDP_IMPLEMENTATION)/*/*.hpp $(PARALLEL_RDP_IMPLEMENTATION)/*/*/*.hpp)
$(PS5_OUT)/prdp/%.o: %.cpp $(PRDP_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(PARALLEL_RDP_CXXFLAGS) $(PARALLEL_RDP_INCLUDE_DIRS) -w -c -o $@ $<
$(PS5_OUT)/prdp/%.o: %.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=gnu11 $(PS5_OPT) $(PARALLEL_RDP_CFLAGS) $(PARALLEL_RDP_INCLUDE_DIRS) -w -c -o $@ $<
$(PS5_OUT)/n64/n64/plugins/gfx_parallel_ps5.o: n64/plugins/gfx_parallel_ps5.cpp $(PORT_HDRS) $(PRDP_HDRS)
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PARALLEL_RDP_INCLUDE_DIRS) $(PORT_WARN) -c -o $@ $<
$(APP_BUILD)/stubs/libSceAgc.so: $(PS5_VULKAN)/vendor/ps5/sdk/stubs/agc_canary_link_stub.c
$(APP_BUILD)/stubs/libSceAgcDriver.so: $(PS5_VULKAN)/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
$(VULKAN_STUBS):
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c11 -O2 -fPIC -c -o $@.o $<
	$(PS5_PAYLOAD_SDK)/bin/prospero-lld --shared -soname $(basename $(notdir $@)).prx -o $@ $@.o
else
EBOOT_ZLIB = $(PS5_ZLIB_OBJ)
endif

EBOOT_OBJS = $(APP_BUILD)/app_crt.o $(APP_BUILD)/app_cpp_runtime.o $(PS5_CORE_OBJ) $(PS5_EMU_OBJ) $(EBOOT_ZLIB) \
	$(VULKAN_OBJ) $(PS5_OUT)/helper_data.o
$(APP_BUILD)/mupen64plus-pie.elf: $(EBOOT_OBJS) $(NATIVE)/tooling/native/ps5-pie.ld $(NATIVE)/tooling/native/ehframe.ld \
		$(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map $(VULKAN_STUBS)
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker -o $@ $(EBOOT_OBJS) $(VULKAN_STUBS) $(VULKAN_LINK) \
		--as-needed $(APP_STUBS) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread

$(APP_BUILD)/eboot.elf: $(APP_BUILD)/mupen64plus-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) $(addprefix --stub ,$(VULKAN_STUBS)) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output $(APP_BUILD)/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: $(APP_BUILD)/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

# app/sce_sys/param.json is committed with the contentVersion the release ships: the homebrew.page catalog reads
# it from the repository at the release tag. It must match VERSION; the build stops when it doesn't.
$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json $(VERSION_STAMP)
	@mkdir -p $(dir $@)
	@grep -q '"contentVersion": "$(CONTENT_VERSION)"' $< || 		{ echo "app/sce_sys/param.json: set \"contentVersion\" to \"$(CONTENT_VERSION)\" (VERSION $(VERSION))" >&2; exit 1; }
	sed 's/PPSA99064/$(TITLE_ID)/g' $< > $@

$(APP_OUT)/sce_sys/%.png: app/sce_sys/%.png
	@mkdir -p $(dir $@)
	cp $< $@

# pic0.dds (the home screen's background while the icon is selected) and pic1.dds (the launch background):
# 3840x2160 BC7_UNORM DX10 DDS without mipmaps (ps5-native-app-boilerplate's tools/prepare-assets.sh --background).
$(APP_OUT)/sce_sys/%.dds: app/sce_sys/%.dds
	@mkdir -p $(dir $@)
	cp $< $@

# ---------------- host ----------------
# The same code on Linux, the PS5 calls faked by host/sce_host.cpp: build/host/mupen64plus-ps5-host [rom]
HOST_CC ?= gcc
HOST_CXX ?= g++
HOST_OUT := build/host
HOST_SAN ?=
HOST_OPT := -O2 -g -mavx2 -fno-strict-aliasing
# gcc 13 sees a 0-byte region in ProsperoAudio's atomics (a false positive clang doesn't report)
HOST_PORT_WARN := $(PORT_WARN) -Wno-stringop-overflow

HOST_CORE_OBJ := $(patsubst %.c,$(HOST_OUT)/m64/%.o,$(CORE_SRC)) $(patsubst %.c,$(HOST_OUT)/m64sub/%.o,$(CORE_SUB_SRC)) \
	$(HOST_OUT)/n64/n64/core/linkage_x64.o \
	$(patsubst %.c,$(HOST_OUT)/n64/%.o,$(N64_SRC)) \
	$(patsubst %.c,$(HOST_OUT)/n64/%.o,$(PLUGIN_C_SRC)) $(patsubst %.cpp,$(HOST_OUT)/n64/%.o,$(PLUGIN_CXX_SRC)) \
	$(patsubst %.c,$(HOST_OUT)/angrylion/%.o,$(ANGRYLION_C_SRC)) $(patsubst %.cpp,$(HOST_OUT)/angrylion/%.o,$(ANGRYLION_CXX_SRC)) \
	$(patsubst %.c,$(HOST_OUT)/rsphle/%.o,$(RSPHLE_SRC))
HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp)
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp)
HOST_ZLIB_OBJ := $(patsubst $(ZLIB)/%.c,$(HOST_OUT)/zlib/%.o,$(ZLIB_SRC))

host: $(HOST_OUT)/mupen64plus-ps5-host $(HOST_OUT)/mupen64plus-ps5-installer $(HOST_OUT)/mupen64plus-ps5-helper

$(HOST_OUT)/m64/%.o: $(M64SRC)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(HOST_OUT)/m64/device/r4300/new_dynarec/new_dynarec.o: $(M64SRC)/device/r4300/new_dynarec/new_dynarec.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CORE_DEFS) $(CORE_INCS) $(DYNAREC_FLAGS) -w -c -o $@ $<

$(HOST_OUT)/gen/asm_defines.o: $(M64SRC)/asm_defines/asm_defines.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<
$(HOST_OUT)/gen/asm_defines_gas.h: $(HOST_OUT)/gen/asm_defines.o
	rm -f $(dir $@)asm_defines_*.h && bash $(M64)/tools/gen_asm_script.sh $(dir $@) $<

$(HOST_OUT)/n64/n64/core/linkage_x64.o: n64/core/linkage_x64.S $(HOST_OUT)/gen/asm_defines_gas.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -I$(HOST_OUT)/gen -c -o $@ $<

$(HOST_OUT)/m64sub/%.o: $(M64)/subprojects/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(HOST_OUT)/n64/n64/plugins/rsp_cxd4_ps5.o: n64/plugins/rsp_cxd4_ps5.c $(wildcard $(CXD4)/*.c $(CXD4)/*.h $(CXD4)/vu/*) n64/core/static_dynlib.h n64/n64_bridge.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CXD4_DEFS) $(CXD4_INCS) -w -c -o $@ $<

$(HOST_OUT)/n64/n64/plugins/gfx_ps5.o: n64/plugins/gfx_ps5.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(GFX_INCS) -Wall -Wextra -Werror -Wno-unused-parameter -c -o $@ $<

$(HOST_OUT)/n64/%.o: %.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) $(CORE_DEFS) $(CORE_INCS) -In64 -In64/core -Wall -Wextra -Werror -Wno-unused-parameter -c -o $@ $<

$(HOST_OUT)/n64/%.o: %.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) -c -o $@ $<

$(HOST_OUT)/rsphle/%.o: $(RSPHLE)/%.c $(wildcard $(RSPHLE)/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) -I$(RSPHLE) -w -c -o $@ $<

$(HOST_OUT)/angrylion/%.o: $(ANGRYLION)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 $(HOST_OPT) -I$(ANGRYLION) -w -c -o $@ $<

$(HOST_OUT)/angrylion/%.o: $(ANGRYLION)/%.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) -DM64PS5_BIG_STACKS -I$(ANGRYLION) -w -c -o $@ $<

$(HOST_OUT)/port/%.o: %.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(HOST_PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -DHAVE_UNISTD_H -I$(ZLIB) -w -c -o $@ $<

# The host installer carries a stand-in app folder (host/fake-eboot.bin, host/fake-libc.prx + the real
# param/icon/pic); the host app carries the host helper.
HOST_APP_DEFS = -DAPP_EBOOT='"$(abspath host/fake-eboot.bin)"' -DAPP_PARAM='"$(abspath app/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath app/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath app/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath app/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath host/fake-libc.prx)"'

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h \
		host/fake-eboot.bin host/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png app/sce_sys/pic0.dds app/sce_sys/pic1.dds
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/mupen64plus-ps5-helper
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/mupen64plus-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/mupen64plus-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/mupen64plus-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/mupen64plus-ps5-host: $(HOST_CORE_OBJ) $(HOST_EMU_OBJ) $(HOST_ZLIB_OBJ) $(HOST_OUT)/helper_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread -lm

# the files built into fe_games.o / fe_emu.o
$(PS5_OUT)/port/frontend/fe_games.o $(HOST_OUT)/port/frontend/fe_games.o: $(GAMEDB)
$(PS5_OUT)/port/frontend/fe_emu.o $(HOST_OUT)/port/frontend/fe_emu.o: $(M64_INI)

clean:
	rm -rf build

.PHONY: ps5 host send strip app native dist clean
