// Mupen64Plus PS5 frontend: covers, as PS5SX2's fe_covers does it for PS2 discs.
//
// A worker thread finds each game's cover, best first:
//   1. manual: /data/mupen64plus/covers/<ROM file name>.png/.jpg  (your own art always wins)
//   2. beside: <ROM file name>.png/.jpg next to the ROM
//   3. cache:  /data/mupen64plus/covers/<No-Intro name>.png        (a download from before)
//   4. download from libretro-thumbnails (Named_Boxarts/<No-Intro name>.png), saved to the cache; a 404 leaves
//      <name>.missing so it isn't asked again for 30 days
// decodes it (stb_image), scales it to the shelf's texture size with two smaller levels, and hands it to
// the shelf. Games near the selection come first; the rest of the library is downloaded afterwards in the
// background, so the second start is instant. A game with no cover gets a placeholder card with its title.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_games.h"
#include "fe_http.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include "ProsperoThread.h"
#include <thread>
#include <vector>

namespace fe
{
// A cover ready to draw: three levels, each stored column by column (a column is contiguous), because
// the shelf draws covers as vertical spans.
struct CoverTex
{
	static constexpr int kLevels = 3;
	int w[kLevels] = {}, h[kLevels] = {};
	std::vector<uint32_t> px[kLevels]; // px[l][x * h[l] + y], A8B8G8R8
	uint32_t average = 0; // for the glow
	bool real = false; // false: a placeholder
	const char* source = ""; // "manual", "beside", "cache", "download", "placeholder"
};
using CoverPtr = std::shared_ptr<const CoverTex>;

constexpr int kCoverTexH = 512; // level 0 height

// "Super Metroid (Japan, USA) (En,Ja)" -> its file name in libretro-thumbnails (&*/:`<>?\| become _).
std::string ThumbnailName(const std::string& nointro);

// The download address of a game's box art (the libretro-thumbnails template, N64PS5_COVER_URL on the host).
std::string CoverUrlFor(const std::string& nointro);
// One GET, following libretro-thumbnails' git symlinks (an "image" that is the real file's name). The HTTP
// status; `data` is the image when it is 200.
int FetchCoverUrl(Http& http, std::string url, const std::string& label, std::vector<uint8_t>& data);

// A cover the library still needs: its file in /data/mupen64plus/covers and where to get it. The list of them is
// covers/wanted.txt ("file<TAB>url" lines), which the next start's prefetch reads (fe_prefetch.h).
struct WantedCover
{
	std::string file; // "Super Mario World (USA).png"
	std::string url;
};
std::vector<WantedCover> MissingCovers(const std::vector<GameInfo>& games);
void WriteWantedList(const std::vector<WantedCover>& wanted);
std::string WantedListPath();

// The shelf's own downloads. Off on the console, as PS5SX2's shelf (download_usb_only): there every cover
// comes from the prefetch, before the app asks for /data (main-boot.cpp). On by default for the host.
void SetShelfDownloads(bool on);
bool ShelfDownloads();

class CoverService
{
public:
	~CoverService() { Stop(); }
	void Start(const std::vector<GameInfo>& games, bool allow_download);
	void Stop();

	// The shelf, every frame: the selection (the worker loads around it) ...
	void SetFocus(int index) { m_focus = index; m_wake.notify_one(); }
	// ... and takes what is ready. Textures far from the focus are dropped to save memory and come back
	// from the disk cache when needed.
	void Collect(std::vector<CoverPtr>& slots, int keep_radius);
	// Forget game i's downloaded cover and fetch it again (Square on the shelf).
	void Refetch(int index);

	// For the status line.
	int Downloaded() const { return m_downloaded; }
	int ToDownload() const { return m_to_download; }
	bool Offline() const { return m_offline; }

private:
	void Run();
	CoverPtr Load(int i, bool* downloaded);
	bool Download(int i);
	std::string CachePath(int i) const;

	std::vector<GameInfo> m_games;
	bool m_allow_download = true;
	ps5::BigThread m_thread;
	std::mutex m_lock;
	std::condition_variable m_wake;
	std::atomic<bool> m_quit{false};
	std::atomic<int> m_focus{0};
	std::vector<uint8_t> m_state; // 0 to load, 1 loading, 2 loaded (resident on the shelf)
	std::vector<uint8_t> m_fetched; // download tried this session
	std::deque<std::pair<int, CoverPtr>> m_ready;
	std::atomic<int> m_downloaded{0}, m_to_download{0};
	std::atomic<bool> m_offline{false};
	Http m_http;
};
} // namespace fe
