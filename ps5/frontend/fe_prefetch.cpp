// Mupen64Plus PS5: the cover prefetch (fe_prefetch.h).
//
// SPDX-License-Identifier: MIT

#include "fe_prefetch.h"

#include "fe_covers.h"
#include "fe_http.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"
#include "ProsperoJailbreak.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"

#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace fe
{
namespace
{
double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// A file name we accept from the list: a plain name in the covers folder, ending in .png.
bool SafeName(const std::string& f)
{
	return !f.empty() && f.size() < 250 && f.find('/') == std::string::npos && f.find("..") == std::string::npos &&
		   f.size() > 4 && f.compare(f.size() - 4, 4, ".png") == 0;
}

bool WriteAtomic(const std::string& path, const std::vector<uint8_t>& data)
{
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	fclose(f);
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		return false;
	}
	return true;
}
PrefetchResult g_last;
bool g_have_last = false;
} // namespace

void RememberPrefetch(const PrefetchResult& result)
{
	g_last.helper_ok = result.helper_ok;
	g_last.attempted = result.attempted;
	g_have_last = true;
}

void CoversRestartIfNeeded(const std::vector<GameInfo>& games, bool downloads_on, bool force, void (*show)(const char*))
{
	if (!downloads_on)
	{
		WriteWantedList({});
		return;
	}
	const std::vector<WantedCover> wanted = MissingCovers(games);
	WriteWantedList(wanted);
	int fresh = 0;
	for (const WantedCover& w : wanted)
		fresh += g_last.attempted.count(w.file) ? 0 : 1;
	if (wanted.empty() || (!force && fresh == 0))
		return;
	if (!g_have_last || !g_last.helper_ok)
	{
		OrbisLog("[covers] %zu cover(s) wanted, but no helper answered at start: not restarting", wanted.size());
		return;
	}
	if (!NetConnected())
	{
		OrbisLog("[covers] %zu cover(s) wanted; the console is offline: next start", wanted.size());
		return;
	}
	// A guard against a loop: no second restart within two minutes (one for Square: ten seconds).
	const std::string stamp = OrbisDir("covers") + "/restart.stamp";
	const time_t now = time(nullptr);
	if (FILE* f = fopen(stamp.c_str(), "r"))
	{
		long long t = 0;
		const int n = fscanf(f, "%lld", &t);
		fclose(f);
		if (n == 1 && now - time_t(t) >= 0 && now - time_t(t) < (force ? 10 : 120))
		{
			OrbisLog("[covers] restarted %lld s ago: not again", (long long)(now - time_t(t)));
			return;
		}
	}
	if (FILE* f = fopen(stamp.c_str(), "w"))
	{
		fprintf(f, "%lld\n", (long long)now);
		fclose(f);
	}
	const char* path = "/data/homebrew/" N64PS5_TITLE_ID "/eboot.bin";
	if (access(path, F_OK) != 0)
		path = "/app0/eboot.bin";
	OrbisLog("[covers] %d new cover(s) to fetch (%zu wanted): restarting %s so the prefetch gets them", fresh,
		wanted.size(), path);
	if (show)
		show("Downloading covers...");
	ProsperoNotifyFlush();
	OrbisLogClose();
	const int rc = sceSystemServiceLoadExec(path, nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	OrbisLogOpen("boot-after-restart");
	OrbisLog("[covers] sceSystemServiceLoadExec(%s) -> %x: carrying on without the new covers", path, unsigned(rc));
}

bool NetConnected()
{
	const int nc = sceNetCtlInit();
	int state[4] = {-1, 0, 0, 0};
	const int gs = sceNetCtlGetState(state);
	if (nc == 0)
		sceNetCtlTerm();
	return gs == 0 && state[0] == 3;
}

PrefetchResult PrefetchCovers(double budget_s)
{
	N64_STAGE(Boot, "cover prefetch");
	PrefetchResult r;
	const double t0 = Now();
	std::string text;
	r.helper_ok = jailbreak::FetchWantedCovers(text);

	std::vector<WantedCover> wanted;
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(start, end - start);
		start = end + 1;
		const size_t tab = line.find('\t');
		if (tab == std::string::npos)
			continue;
		WantedCover w{line.substr(0, tab), line.substr(tab + 1)};
		if (SafeName(w.file) && (w.url.compare(0, 8, "https://") == 0 || w.url.compare(0, 7, "http://") == 0))
			wanted.push_back(w);
	}
	OrbisLog("[prefetch] %zu cover(s) to fetch before asking for /data (budget %.0f s)", wanted.size(), budget_s);
	if (wanted.empty())
	{
		r.seconds = Now() - t0;
		return r;
	}
	if (wanted.size() == 1)
		ProsperoNotify("Mupen64Plus PS5: downloading 1 cover...");
	else
		ProsperoNotify("Mupen64Plus PS5: downloading %d covers...", int(wanted.size()));

	Http http;
	int saved = 0;
	for (const WantedCover& w : wanted)
	{
		if (Now() - t0 > budget_s)
		{
			OrbisLog("[prefetch] out of time: the rest next start");
			break;
		}
		PrefetchItem item;
		item.file = w.file;
		item.url = w.url;
		item.status = FetchCoverUrl(http, w.url, w.file.substr(0, w.file.size() - 4), item.data);
		r.attempted.insert(w.file);
		if (item.status != 200)
			item.data.clear();
		else
			saved++;
		OrbisLog("[prefetch] %s -> %d (%zu bytes)", w.file.c_str(), item.status, item.data.size());
		r.items.push_back(std::move(item));
		if (http.Offline())
		{
			OrbisLog("[prefetch] the console is offline: stopping");
			break;
		}
	}
	http.Term();
	r.seconds = Now() - t0;
	OrbisLog("[prefetch] %d of %zu fetched in %.1f s", saved, wanted.size(), r.seconds);
	return r;
}

void SavePrefetched(const PrefetchResult& result)
{
	if (result.items.empty())
		return;
	const std::string dir = OrbisDir("covers");
	OrbisMkdirs(dir);
	int saved = 0, missing = 0;
	for (const PrefetchItem& it : result.items)
	{
		const std::string path = dir + "/" + it.file;
		if (it.status == 200 && !it.data.empty())
		{
			if (WriteAtomic(path, it.data))
				saved++;
			else
				OrbisLog("[prefetch] can't write %s", path.c_str());
		}
		else if (it.status == 404)
		{
			// as the shelf does: no new try for 30 days (Square on the shelf asks again)
			const std::string marker = path.substr(0, path.size() - 4) + ".missing";
			if (FILE* f = fopen(marker.c_str(), "w"))
			{
				fprintf(f, "%s\n", it.url.c_str());
				fclose(f);
			}
			missing++;
		}
	}
	OrbisLog("[prefetch] saved %d cover(s) in %s, %d not on the server", saved, dir.c_str(), missing);
}
} // namespace fe
