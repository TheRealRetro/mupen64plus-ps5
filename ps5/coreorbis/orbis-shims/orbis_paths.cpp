// Mupen64Plus PS5: the /data/mupen64plus folder layout and the boot log (include-orbis/OrbisPaths.h).
//
// stat(), not access(): PS5SX2 found (vk-285-46) that a sandboxed process can get EPERM from access() on
// files that exist, while stat() answers on both sides of a jailbreak. The same rule is kept here.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace
{
std::mutex s_log_mutex;
FILE* s_log = nullptr;

const char* const kSubdirs[] = {"roms", "saves", "states", "config", "data", "logs", "covers"};
} // namespace

const std::string& OrbisRoot()
{
	static const std::string root = [] {
		const char* env = getenv("N64PS5_PS5_ROOT");
		return std::string(env && *env ? env : ORBIS_ROOT_DEFAULT);
	}();
	return root;
}

std::string OrbisDir(const char* sub)
{
	return OrbisRoot() + "/" + sub;
}

bool OrbisIsDir(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool OrbisIsFile(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool OrbisMkdirs(const std::string& path)
{
	if (path.empty())
		return false;
	std::string cur;
	size_t pos = 0;
	while (pos != std::string::npos)
	{
		pos = path.find('/', pos + 1);
		cur = path.substr(0, pos);
		if (cur.empty() || OrbisIsDir(cur))
			continue;
		if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST)
			return false;
	}
	return OrbisIsDir(path);
}

bool OrbisPathsInit()
{
	if (!OrbisMkdirs(OrbisRoot()))
		return false;
	for (const char* sub : kSubdirs)
		OrbisMkdirs(OrbisDir(sub));
	return true;
}

std::vector<std::string> OrbisRomRoots()
{
	std::vector<std::string> roots;
	const std::string internal = OrbisDir("roms");
	if (OrbisIsDir(internal))
		roots.push_back(internal);
	char buf[64];
	for (int i = 0; i < 8; i++)
	{
		snprintf(buf, sizeof(buf), "/mnt/usb%d/mupen64plus/roms", i);
		if (OrbisIsDir(buf))
			roots.push_back(buf);
	}
	for (int i = 0; i < 2; i++)
	{
		snprintf(buf, sizeof(buf), "/mnt/ext%d/mupen64plus/roms", i);
		if (OrbisIsDir(buf))
			roots.push_back(buf);
	}
	return roots;
}

// Lines logged before the log file is open (the native app logs its jailbreak before /data is visible).
static std::vector<std::string>& EarlyLines()
{
	static std::vector<std::string>* lines = new std::vector<std::string>();
	return *lines;
}

void OrbisLogOpen(const char* name)
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log)
		return;
	const std::string dir = OrbisDir("logs");
	if (!OrbisIsDir(dir))
		return;
	// keep the 4 runs before (like PS5SX2's logs): <name>.prev.log is the last one, then .prev2 .. .prev4, so
	// opening the app once or twice to fetch a log doesn't lose the session that had the problem
	const std::string base = dir + "/" + name;
	const std::string cur = base + ".log";
	auto prev = [&](int n) { return base + (n == 1 ? ".prev.log" : ".prev" + std::to_string(n) + ".log"); };
	const int kKeep = 4;
	unlink(prev(kKeep).c_str());
	for (int n = kKeep - 1; n >= 1; n--)
		rename(prev(n).c_str(), prev(n + 1).c_str());
	rename(cur.c_str(), prev(1).c_str());
	s_log = fopen(cur.c_str(), "w");
	if (s_log)
	{
		for (const std::string& line : EarlyLines())
			fputs(line.c_str(), s_log);
		fflush(s_log);
		EarlyLines().clear();
	}
}

int OrbisLogFd()
{
	// the open boot.log's fd, for the crash handler's signal-safe write(); -1 before it is open
	return s_log ? fileno(s_log) : -1;
}

void OrbisLog(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const double t = ts.tv_sec + ts.tv_nsec / 1e9;

	std::lock_guard<std::mutex> lock(s_log_mutex);
	printf("[mupen64plus-ps5 %10.3f] %s\n", t, line);
	fflush(stdout);
	if (s_log)
	{
		fprintf(s_log, "[%10.3f] %s\n", t, line);
		fflush(s_log); // a crash must not lose the lines before it
	}
	else if (EarlyLines().size() < 4000)
	{
		char stamped[1100];
		snprintf(stamped, sizeof(stamped), "[%10.3f] %s\n", t, line);
		EarlyLines().push_back(stamped);
	}
}

void OrbisLogClose()
{
	std::lock_guard<std::mutex> lock(s_log_mutex);
	if (s_log)
	{
		fclose(s_log);
		s_log = nullptr;
	}
}
