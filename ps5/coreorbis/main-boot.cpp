// Mupen64Plus PS5: the app's entry point (eboot.bin).
//
// mupen64plus-core runs as the dashboard app itself, a native PS5 title built the way PS5SX2 builds its
// eboot: the console gives the controller only to the title in front. Mupen64PS5.elf is the installer and
// the helper (installer/installer_main.cpp), like PS5SX2's installer and helper payloads.
//
// It first asks to leave the sandbox (ProsperoJailbreak.h: /data and USB drives), then brings the PS5 layer
// up in order -- log, folders, video, audio, pads -- starts the core and hands over to the frontend: the
// shelf, the game (with its pause menu), and back. It ends through the system
// (sceSystemServiceLoadExec("exit")), as PS5SX2 learned it must.
//
//   eboot.bin [rom]     with a ROM path, the game starts at once (no shelf; the host build uses it)
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include "fe_emu.h"
#include "fe_covers.h"
#include "fe_menu.h"
#include "fe_prefetch.h"
#include "fe_update.h"
#include "fe_shelf.h"
#include "fe_settings.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>

#ifndef N64PS5_VERSION
#define N64PS5_VERSION "dev"
#endif

namespace
{
void WritePid()
{
	const std::string path = OrbisRoot() + "/pid.txt";
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fprintf(f, "%d\n", int(getpid()));
		fclose(f);
	}
}

// One game, until the player leaves it. Returns false when the player chose "quit Mupen64Plus".
bool PlayGame(const std::string& rom)
{
	std::string error;
	switch (emu::RunGame(rom, &error))
	{
		case emu::ExitReason::BackToList: return true;
		case emu::ExitReason::Quit: return false;
		case emu::ExitReason::Error: break;
	}
	ProsperoNotify("Mupen64Plus: could not open %s", rom.c_str());
	fe::MessageBox("Could not start the game", error.empty() ? rom : error);
	return true;
}

// The end of the app: through the system, then _exit() if it never comes (PS5SX2's OrbisExitApp: on the
// console a title's exit() or return from main ends in SIGSYS).
[[noreturn]] void ExitApp(int status)
{
	OrbisLog("[boot] exit %d", status);
	ProsperoNotifyFlush();
	OrbisLogClose();
	fflush(stdout);
	fflush(stderr);
	const int rc = sceSystemServiceLoadExec("exit", nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	_exit(status);
}

struct Args
{
	int argc;
	char** argv;
	int status;
};

int Run(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	crashlog::Install();
	N64_STAGE(Boot, "start");
	OrbisLog("[boot] Mupen64Plus PS5 %s (mupen64plus-core 2.6.0, built %s %s), pid %d", N64PS5_VERSION, __DATE__, __TIME__, int(getpid()));

	// Covers first, as PS5SX2 (orbis_frontend_prefetch_covers, 30 s, before its jailbreak): HTTPS works here,
	// and on the 1.6.1 console it failed on the shelf, after the request below (fe_prefetch.h).
	const fe::PrefetchResult prefetch = fe::PrefetchCovers(30.0);
	// A newer release on GitHub, downloaded now for the same reason (fe_update.h); offered further down.
	N64_STAGE(Boot, "update check");
	fe::UpdateOffer update = fe::CheckForUpdate();

	// out of the sandbox, like PS5SX2: before it the app sees no /data and no USB drives
	N64_STAGE(Boot, "jailbreak");
	std::string how;
	const bool freed = jailbreak::RequestForSelf(how);
	OrbisLog("[boot] jailbreak: %s", freed ? how.c_str() : "nobody answered");
	const bool have_data = OrbisPathsInit();
	OrbisLogOpen(); // also writes the lines above
	fe::SavePrefetched(prefetch);
	fe::RememberPrefetch(prefetch);
	// the shelf downloads nothing after the jailbreak (PS5SX2's download_usb_only); without one (the host
	// tests, no helper) it still does
	fe::SetShelfDownloads(!freed);
	if (!have_data)
		OrbisLog("[boot] can't create %s: no access to /data", OrbisRoot().c_str());
	else
		WritePid();
	fe::Config().Load();

	N64_STAGE(Boot, "video");
	if (!ps5video::Init())
	{
		OrbisLog("[boot] video init failed");
		ProsperoNotify("Mupen64Plus PS5: video failed to start (see %s/logs/boot.log)", OrbisRoot().c_str());
		return 1;
	}
	OrbisLog("[boot] splash screen hidden: %x", unsigned(sceSystemServiceHideSplashScreen()));
	N64_STAGE(Boot, "audio");
	if (!ps5audio::Init())
		OrbisLog("[boot] audio init failed: running without sound");
	N64_STAGE(Boot, "pad");
	if (!ps5input::Init())
		OrbisLog("[boot] pad init failed");

	if (!have_data)
	{
		// the jailbreak didn't happen: say so on the screen (the pad works, this is the title in front)
		fe::MessageBox("Mupen64Plus PS5 has no access to /data",
			"The app unlocks itself through the ELF loader on port 9021 (elfldr, etaHEN or the Payload Manager's), and "
			"none answered. Start the ELF loader (or etaHEN), then open Mupen64Plus PS5 again.");
		ps5input::Shutdown();
		ps5audio::Shutdown();
		ps5video::Shutdown();
		return 2;
	}

	N64_STAGE(Boot, "update");
	fe::OfferUpdate(update, fe::Config().updates);
	update.zip.clear();
	update.zip.shrink_to_fit();

	N64_STAGE(Boot, "emu init");
	if (!emu::InitCore())
	{
		ProsperoNotify("Mupen64Plus PS5: the emulator failed to start (see %s/logs/boot.log)", OrbisRoot().c_str());
		ps5audio::Shutdown();
		ps5video::Shutdown();
		return 1;
	}

	std::string rom = (argc > 1 && argv[1] && OrbisIsFile(argv[1])) ? argv[1] : "";
	for (;;)
	{
		if (rom.empty())
			rom = fe::Shelf();
		if (rom.empty())
			break; // OPTIONS on the shelf
		const bool again = PlayGame(rom);
		rom.clear();
		if (!again)
			break;
	}

	OrbisLog("[boot] leaving");
	fe::ShelfShutdown();
	fe::Config().Save();
	emu::DeinitCore();
	ps5input::Shutdown();
	ps5audio::Shutdown();
	ps5video::Shutdown();
	return 0;
}

void* RunThread(void* p)
{
	Args* a = static_cast<Args*>(p);
	a->status = Run(a->argc, a->argv);
	return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
	// The emulator and the shelf run on a thread with a stack of their own (8 MiB): a title's main thread
	// gets a small one (PS5SX2 runs PCSX2 on a 16 MiB worker for the same reason).
	Args args = {argc, argv, 1};
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
	pthread_t t;
	if (pthread_create(&t, &attr, RunThread, &args) == 0)
		pthread_join(t, nullptr);
	else
		args.status = Run(argc, argv);
	pthread_attr_destroy(&attr);
	ExitApp(args.status);
}
