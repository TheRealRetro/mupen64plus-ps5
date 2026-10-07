// Mupen64Plus PS5: updates from the project's GitHub releases (github.com/TheRealRetro/mupen64plus-ps5).
//
// A release is tagged with param.json's contentVersion (NN.NNN.NNN, 0.6.0 -> 00.006.000) and carries
// PPSA99064.zip (the PPSA99064/ app folder), as the Homebrew Browser's releases do; GitHub publishes each
// asset's SHA-256 ("digest") with the release.
//
// Two steps, because of where HTTPS works (fe_prefetch.h: before the app asks for /data, not after):
//   1. CheckForUpdate, at start-up before the jailbreak: asks api.github.com for the latest release; when its
//      tag is newer than this build, downloads PPSA99064.zip and checks its size and SHA-256 (in memory).
//   2. OfferUpdate, once /data is visible: "Version X is available" with the release notes; on Cross it
//      writes the new files over every copy of the app folder it finds (/data/homebrew/PPSA99064, the USB
//      drives' homebrew/PPSA99064...), each file through a temporary name, and starts the app again.
// The setting "Check for updates" (Off) skips the question; the download in step 1 still happens, since the
// settings can't be read before the jailbreak.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
struct UpdateOffer
{
	bool available = false; // a newer release, downloaded and verified
	std::string tag;        // "00.007.000"
	std::string version;    // "0.7.0"
	std::string notes;      // the release's description
	std::vector<uint8_t> zip;
};

// Step 1 (before the jailbreak). Never blocks for long when the console is offline.
UpdateOffer CheckForUpdate();

// Step 2 (with /data). Returns when there is nothing to do, the user said later, or the install failed;
// after a successful install it restarts the app and doesn't return (unless the restart fails).
void OfferUpdate(UpdateOffer& offer, bool ask);

// "00.006.000" -> {0, 6, 0}; also accepts "v00.006.000" and "0.6.0". False when it isn't a version.
bool ParseVersion(const std::string& text, int out[3]);
// This build's contentVersion ("00.006.000").
const char* BuildContentVersion();
} // namespace fe
