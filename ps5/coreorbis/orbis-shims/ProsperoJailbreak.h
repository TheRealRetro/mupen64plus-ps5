// Mupen64Plus PS5: the dashboard app asks to be let out of its sandbox (ProsperoJailbreak.cpp), and the payload
// that does it (ProsperoHelper.cpp).
//
// The app (eboot.bin, a native title like PS5SX2's) starts inside the console's sandbox: no /data, no USB
// drives. PS5SX2 asks a jailbreak daemon for full rights when it starts (its main-boot.cpp, orbis_try_jailbreak:
// the PS5SX2 Helper or etaHEN, 127.0.0.1:9028/9069, a 0xDEADBEEF command 5 with the app's pid). Mupen64Plus PS5 does the
// same with its own helper. The app carries it (Mupen64PS5-helper.elf, built in): when nobody answers on 9064, it
// sends it to the console's ELF loader (127.0.0.1:9021) and asks again, so eboot.bin needs no payload sent by
// hand (0.6). Only when that fails does it ask etaHEN and the 9069 daemon. Mupen64PS5.elf, the optional
// installer, also stays running as the helper.
//
// The helper only lets out the Mupen64Plus PS5 title (PPSA99064): it gives the process root's folder view
// (/data, /mnt/usbN) and uid 0. It never touches another title.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace jailbreak
{
// Mupen64Plus PS5's helper (N64PS5_HELPER_PORT on the host)
constexpr int kHelperPort = 9064;
// the ELF loader the helper is sent to when it isn't running (N64PS5_ELFLDR_PORT on the host)
constexpr int kElfLoaderPort = 9021;

// The request, PS5SX2's/etaHEN's layout (cmd 5 = jailbreak the process pid); ret is 0 when done.
struct Request
{
	uint32_t magic;
	int32_t cmd;
	int32_t pid;
	int32_t ret;
	char msg1[0x500];
	char msg2[0x500];
};
constexpr uint32_t kMagic = 0xDEADBEEF;
constexpr int32_t kCmdJailbreak = 5;
// The port's own: the helper answers with covers/wanted.txt (ret = its length, then the bytes), so the app can
// prefetch covers before it asks for /data, as PS5SX2 does (fe_prefetch.h).
constexpr int32_t kCmdWantedCovers = 6;
constexpr int32_t kMaxWantedBytes = 1 << 20;
constexpr int32_t kRetUntouched = -1337;

// The app side: ask for this process. Returns true when a helper answered yes; `how` names it.
// Every step goes to OrbisLog (kept in memory until boot.log can be opened).
bool RequestForSelf(std::string& how);

// The app side, before the jailbreak: the wanted-covers list from the helper (starting the helper through the
// ELF loader if none answers). False when no helper could be reached; `text` may be empty (nothing wanted).
bool FetchWantedCovers(std::string& text);

// The helper ELF built into the app (helper_data.cpp in the native eboot; nothing elsewhere).
struct Blob
{
	const unsigned char* data;
	size_t size;
};
Blob EmbeddedHelper();

// The payload side: serve requests on 127.0.0.1:kHelperPort until the process ends. Returns false at once
// when the port is taken (a helper is already running). `on_ready` runs once the port is bound.
bool ServeHelper(void (*on_ready)());

// Lets one process out (payload only; the host build fakes it). False with a reason when it refuses.
bool JailbreakProcess(int pid, std::string& why);
} // namespace jailbreak
