// Mupen64Plus PS5: installing a downloaded update (ProsperoUpdateJob.h).
//
// SPDX-License-Identifier: MIT

#include "ProsperoUpdateJob.h"

#include "OrbisPaths.h"
#include "ProsperoNotify.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef N64PS5_TITLE_ID
#define N64PS5_TITLE_ID "PPSA99064"
#endif

namespace updatejob
{
namespace
{
constexpr const char* kUpdateDir = ".mupen64plus-update";

bool Exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

// /user/app/<TITLE>/mount.lnk: ShadowMountPlus's record of where an installed folder app lives
// (N64PS5_APP_LINK_DIR replaces /user/app in the host tests).
std::string MountLink()
{
	const char* dir = getenv("N64PS5_APP_LINK_DIR");
	return std::string(dir && *dir ? dir : "/user/app") + "/" N64PS5_TITLE_ID "/mount.lnk";
}

// POST /api/v1/scan to ShadowMountPlus (N64PS5_SMP_PORT replaces 10101 in the host tests). The answer's
// status line goes to the log; false when nobody listens.
bool AskForScan()
{
	const char* p = getenv("N64PS5_SMP_PORT");
	const int port = p && *p ? atoi(p) : 10101;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	timeval tv = {3, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
#ifdef __FreeBSD__
	sa.sin_len = sizeof(sa);
#endif
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0)
	{
		close(fd);
		OrbisLog("[update-job] ShadowMountPlus API (port %d): not there (errno %d)", port, errno);
		return false;
	}
	static const char req[] = "POST /api/v1/scan HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
							  "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
	bool ok = send(fd, req, sizeof(req) - 1, 0) == ssize_t(sizeof(req) - 1);
	char buf[256] = {};
	const ssize_t n = ok ? recv(fd, buf, sizeof(buf) - 1, 0) : -1;
	close(fd);
	if (n > 0)
	{
		buf[n] = 0;
		buf[strcspn(buf, "\r\n")] = 0;
	}
	OrbisLog("[update-job] ShadowMountPlus rescan: %s", n > 0 ? buf : "no answer");
	return n > 0;
}

// Waits (asking for rescans every 15 s) until the mount link exists == want. False after `seconds`.
bool WaitForLink(bool want, int seconds)
{
	const std::string link = MountLink();
	for (int i = 0; i <= seconds; i++)
	{
		if (Exists(link) == want)
		{
			OrbisLog("[update-job] mount link %s after %d s", want ? "back" : "gone", i);
			return true;
		}
		if (i % 15 == 0)
			AskForScan();
		sleep(1);
	}
	OrbisLog("[update-job] mount link still %s after %d s", want ? "missing" : "there", seconds);
	return false;
}

bool ProcessAlive(int pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH);
}

bool RemoveTreeRaw(const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlink(path.c_str()) == 0;
	bool ok = true;
	if (DIR* d = opendir(path.c_str()))
	{
		while (dirent* e = readdir(d))
		{
			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			ok = RemoveTreeRaw(path + "/" + e->d_name) && ok;
		}
		closedir(d);
	}
	return rmdir(path.c_str()) == 0 && ok;
}

bool Read(Job& job)
{
	FILE* f = fopen(JobPath().c_str(), "r");
	if (!f)
		return false;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		const std::string s = line;
		if (s.rfind("pid=", 0) == 0)
			job.pid = atoi(s.c_str() + 4);
		else if (s.rfind("tag=", 0) == 0)
			job.tag = s.substr(4);
		else if (s.rfind("version=", 0) == 0)
			job.version = s.substr(8);
		else if (s.rfind("target=", 0) == 0)
		{
			const std::string v = s.substr(7);
			const size_t a = v.find('|'), b = v.find('|', a == std::string::npos ? a : a + 1);
			if (a == std::string::npos || b == std::string::npos)
				continue;
			Target t;
			t.dir = v.substr(0, a);
			t.staging = v.substr(a + 1, b - a - 1);
			t.old = v.substr(b + 1);
			job.targets.push_back(t);
		}
	}
	fclose(f);
	return !job.targets.empty();
}
} // namespace

Target TargetFor(const std::string& app_dir)
{
	Target t;
	t.dir = app_dir;
	const size_t slash = app_dir.rfind('/');
	const std::string parent = slash == std::string::npos ? "." : app_dir.substr(0, slash);
	const std::string name = slash == std::string::npos ? app_dir : app_dir.substr(slash + 1);
	t.staging = parent + "/" + kUpdateDir + "/new/" + name;
	t.old = parent + "/" + kUpdateDir + "/old/" + name;
	return t;
}

std::string JobPath()
{
	return OrbisDir("update") + "/job.txt";
}

bool Write(const Job& job, std::string& why)
{
	OrbisMkdirs(OrbisDir("update"));
	const std::string path = JobPath(), tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		why = "can't write " + tmp;
		return false;
	}
	fprintf(f, "pid=%d\ntag=%s\nversion=%s\n", job.pid, job.tag.c_str(), job.version.c_str());
	for (const Target& t : job.targets)
		fprintf(f, "target=%s|%s|%s\n", t.dir.c_str(), t.staging.c_str(), t.old.c_str());
	const bool ok = fclose(f) == 0;
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		unlink(tmp.c_str());
		why = "can't write " + path;
		return false;
	}
	return true;
}

int MakeRunnable(const std::string& tree)
{
	struct stat st;
	if (lstat(tree.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
		return 0;
	int changed = 0;
	if ((st.st_mode & 07777) != 0777)
		changed += chmod(tree.c_str(), 0777) == 0;
	if (S_ISDIR(st.st_mode))
		if (DIR* d = opendir(tree.c_str()))
		{
			while (dirent* e = readdir(d))
				if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
					changed += MakeRunnable(tree + "/" + e->d_name);
			closedir(d);
		}
	return changed;
}

std::string EbootMode(const std::string& app_dir)
{
	struct stat st;
	if (stat((app_dir + "/eboot.bin").c_str(), &st) != 0)
		return "missing";
	char buf[16];
	snprintf(buf, sizeof(buf), "%04o", unsigned(st.st_mode & 07777));
	return buf;
}

bool RemoveUpdateTree(const std::string& path)
{
	if (path.find(std::string("/") + kUpdateDir + "/") == std::string::npos)
		return false; // never anything else
	return RemoveTreeRaw(path);
}

void RunIfPending()
{
	Job job;
	if (!Read(job))
		return;
	// take the job first: a second helper started meanwhile must not run it too
	const std::string taken = JobPath() + ".running";
	if (rename(JobPath().c_str(), taken.c_str()) != 0)
		return;
	OrbisLog("[update-job] installing %s (%s) for pid %d, %zu folder(s)", job.version.c_str(), job.tag.c_str(),
		job.pid, job.targets.size());

	// 1. the app closes itself right after starting this payload
	int waited = 0;
	while (ProcessAlive(job.pid) && waited < 60)
	{
		sleep(1);
		waited++;
	}
	if (ProcessAlive(job.pid))
	{
		OrbisLog("[update-job] pid %d still running after 60 s: giving up", job.pid);
		ProsperoNotify("Mupen64Plus PS5: the update to %s was not installed (the app didn't close). Open the app to "
					   "try again.",
			job.version.c_str());
		unlink(taken.c_str());
		return;
	}
	sleep(2); // ShadowMountPlus releases the app's runtime mounts after it exits
	ProsperoNotify("Mupen64Plus PS5: installing version %s...", job.version.c_str());

	const bool tracked = Exists(MountLink());
	OrbisLog("[update-job] ShadowMountPlus mount link %s: %s", MountLink().c_str(),
		tracked ? "present" : "absent (folders are swapped without waiting for it)");

	// 2. the app folders aside
	std::vector<Target> moved;
	std::string why;
	for (const Target& t : job.targets)
	{
		if (!Exists(t.staging))
		{
			why = "the new version is missing (" + t.staging + ")";
			break;
		}
		const std::string before = EbootMode(t.staging);
		const int changed = MakeRunnable(t.staging);
		OrbisLog("[update-job] %s: eboot.bin %s, installed copy %s; new version's permissions set (%d changed): "
				 "eboot.bin now %s",
			t.dir.c_str(), before.c_str(), EbootMode(t.dir).c_str(), changed, EbootMode(t.staging).c_str());
		RemoveUpdateTree(t.old);
		OrbisMkdirs(t.old.substr(0, t.old.rfind('/')));
		if (rename(t.dir.c_str(), t.old.c_str()) != 0)
		{
			why = "can't move " + t.dir + " aside (errno " + std::to_string(errno) + ")";
			break;
		}
		moved.push_back(t);
	}
	auto roll_back = [&]() {
		for (const Target& t : moved)
		{
			if (!Exists(t.dir))
				rename(t.old.c_str(), t.dir.c_str());
		}
	};
	if (!why.empty())
	{
		OrbisLog("[update-job] %s: putting everything back", why.c_str());
		roll_back();
		if (tracked)
			AskForScan();
		ProsperoNotify("Mupen64Plus PS5: the update to %s failed (%s).", job.version.c_str(), why.c_str());
		unlink(taken.c_str());
		return;
	}

	// 3. ShadowMountPlus forgets the old installation
	if (tracked)
		WaitForLink(false, 90);

	// 4. the new version in place, installed again
	for (const Target& t : moved)
	{
		if (rename(t.staging.c_str(), t.dir.c_str()) != 0)
		{
			why = "can't move the new version into " + t.dir + " (errno " + std::to_string(errno) + ")";
			break;
		}
	}
	if (!why.empty())
	{
		OrbisLog("[update-job] %s: putting the old version back", why.c_str());
		for (const Target& t : moved)
			if (Exists(t.dir) && !Exists(t.staging))
				rename(t.dir.c_str(), t.staging.c_str()); // the new copy back out of the way
		roll_back();
		if (tracked)
			WaitForLink(true, 120);
		ProsperoNotify("Mupen64Plus PS5: the update to %s failed (%s).", job.version.c_str(), why.c_str());
		unlink(taken.c_str());
		return;
	}
	const bool installed = !tracked || WaitForLink(true, 120);

	// 5. tidy up
	for (const Target& t : moved)
	{
		RemoveUpdateTree(t.old);
		const std::string root = t.old.substr(0, t.old.rfind("/old/"));
		rmdir((root + "/old").c_str());
		rmdir((root + "/new").c_str());
		rmdir(root.c_str());
	}
	unlink(taken.c_str());
	OrbisLog("[update-job] version %s installed%s", job.version.c_str(),
		installed ? "" : " (ShadowMountPlus hasn't confirmed it yet)");
	if (installed)
		ProsperoNotify("Mupen64Plus PS5 updated to %s. Open it from the home screen.", job.version.c_str());
	else
		ProsperoNotify("Mupen64Plus PS5 updated to %s. Wait a minute before opening it (or restart the PS5 if it "
					   "doesn't start).",
			job.version.c_str());
}
} // namespace updatejob
