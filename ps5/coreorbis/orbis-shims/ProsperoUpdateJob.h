// Mupen64Plus PS5: installing a downloaded update (frontend/fe_update.h) so the console starts it.
//
// Replacing the files of an installed app in place leaves it unable to start ("Can't start the game or app",
// confirmed on the console with 0.6.0 -> 0.6.1 and 0.6.1 -> 0.6.2, whether the app or an FTP client wrote
// them): ShadowMountPlus keeps the installation it made from the old files. A fresh copy works because
// ShadowMountPlus sees the folder disappear ("source removed", its mount link /user/app/<TITLE>/mount.lnk goes)
// and installs it again when it comes back ("Installed NEW!"). The update does the same:
//   1. the app unpacks the new version into a staging folder next to the app folder
//      (<parent>/.mupen64plus-update/new/PPSA99064, too deep for ShadowMountPlus's scan), writes a job file,
//      starts its helper payload through the ELF loader, and closes;
//   2. the helper (RunIfPending) waits for the app to exit, moves the app folder aside (.../old/PPSA99064),
//      asks ShadowMountPlus to rescan (its HTTP API, 127.0.0.1:10101) until the mount link is gone, moves the
//      new folder in, rescans until the link is back (installed again), removes the old copy and says so in
//      a notification. Anything failing puts the old folder back.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace updatejob
{
struct Target
{
	std::string dir;     // the app folder, e.g. /data/homebrew/PPSA99064
	std::string staging; // the new version, unpacked
	std::string old;     // where the app folder goes while ShadowMountPlus forgets it
};

struct Job
{
	int pid = 0;         // the app, which must have exited before the folders move
	std::string tag;     // 00.006.003
	std::string version; // 0.6.3
	std::vector<Target> targets;
};

// The staging and old paths for an app folder.
Target TargetFor(const std::string& app_dir);

// /data/mupen64plus/update/job.txt
std::string JobPath();
bool Write(const Job& job, std::string& why);

// Deletes a folder tree, only inside a ".mupen64plus-update" folder (staging and old copies).
bool RemoveUpdateTree(const std::string& path);

// Gives every file and folder in a tree the permissions an FTP copy gets (0777). Files a program writes with
// fopen() have no execute permission (0666 at most), and the console doesn't start an eboot.bin without it:
// that, not the files' contents, is why every in-app update so far left the app unable to start
// ("Can't start the game or app", CE-107750-0). The number of entries changed.
int MakeRunnable(const std::string& tree);

// eboot.bin's permissions in a folder, for the logs ("0777", or "missing").
std::string EbootMode(const std::string& app_dir);

// The helper payload, at start: performs a pending job (and deletes it). Returns at once when there is none.
void RunIfPending();
} // namespace updatejob
