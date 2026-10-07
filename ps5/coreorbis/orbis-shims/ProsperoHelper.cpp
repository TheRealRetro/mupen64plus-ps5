// Mupen64Plus PS5: the helper payload's side of the jailbreak (ProsperoJailbreak.h).
//
// Runs in Mupen64PS5.elf (after it installs the app) and in Mupen64PS5-helper.elf (which the app sends to the
// ELF loader when no helper answers). One request at a time on 127.0.0.1:9064 only.
//
// What the jailbreak changes in the Mupen64Plus PS5 process, with the payload SDK's kernel access (the same calls
// ps5-payload-dev's elfldr makes for the payloads it starts): the root and jail folders become the kernel's
// root (so /data and /mnt/usbN are visible), uid/gid 0, and every SCE capability. The authid stays the
// app's own, so the system keeps treating it as the foreground title (pads, video, sound).
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

#include "OrbisPaths.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef N64PS5_TITLE_ID
#define N64PS5_TITLE_ID "PPSA99064"
#endif

#ifdef __PROSPERO__
#include <ps5/kernel.h>
extern "C" int sceKernelGetAppInfo(int pid, void* info);
#endif

namespace jailbreak
{
namespace
{
// The first PPSA/CUSA-style title id (4 letters + 5 digits) in a buffer, or "".
std::string FindTitleId(const unsigned char* p, size_t n)
{
	for (size_t i = 0; i + 9 <= n; i++)
	{
		bool ok = true;
		for (size_t k = 0; k < 4 && ok; k++)
			ok = p[i + k] >= 'A' && p[i + k] <= 'Z';
		for (size_t k = 4; k < 9 && ok; k++)
			ok = p[i + k] >= '0' && p[i + k] <= '9';
		if (ok)
			return std::string(reinterpret_cast<const char*>(p + i), 9);
	}
	return "";
}

// The title the process belongs to: sceKernelGetAppInfo's title id. "" when it can't be told.
std::string TitleOf(int pid)
{
#ifdef __PROSPERO__
	unsigned char info[512];
	memset(info, 0, sizeof(info));
	const int rc = sceKernelGetAppInfo(pid, info);
	if (rc != 0)
	{
		OrbisLog("[helper] sceKernelGetAppInfo(%d) -> %x", pid, unsigned(rc));
		return "";
	}
	return FindTitleId(info, sizeof(info));
#else
	// host: the test says which title the asking process is
	if (kill(pid, 0) != 0)
		return "";
	const char* t = getenv("N64PS5_HOST_JB_TITLE");
	const std::string title = t ? t : N64PS5_TITLE_ID;
	return FindTitleId(reinterpret_cast<const unsigned char*>(title.data()), title.size());
#endif
}
} // namespace

bool JailbreakProcess(int pid, std::string& why)
{
	if (pid <= 0)
	{
		why = "bad pid";
		return false;
	}
	const std::string title = TitleOf(pid);
	if (!title.empty() && title != N64PS5_TITLE_ID)
	{
		why = "title " + title + " is not Mupen64Plus PS5";
		return false;
	}
	OrbisLog("[helper] pid %d (title %s): letting it out", pid, title.empty() ? "unknown" : title.c_str());
#ifdef __PROSPERO__
	if (kernel_get_proc(pid) == 0)
	{
		why = "no such process";
		return false;
	}
	uint8_t caps[16];
	memset(caps, 0xff, sizeof(caps));
	const intptr_t root = kernel_get_root_vnode();
	const int r_caps = kernel_set_ucred_caps(pid, caps);
	const int r_uid = kernel_set_ucred_uid(pid, 0);
	const int r_ruid = kernel_set_ucred_ruid(pid, 0);
	const int r_svuid = kernel_set_ucred_svuid(pid, 0);
	const int r_rgid = kernel_set_ucred_rgid(pid, 0);
	const int r_svgid = kernel_set_ucred_svgid(pid, 0);
	const int r_root = root ? kernel_set_proc_rootdir(pid, root) : -1;
	const int r_jail = root ? kernel_set_proc_jaildir(pid, root) : -1;
	OrbisLog("[helper]   caps %d, uid %d/%d/%d, gid %d/%d, root vnode %lx: rootdir %d, jaildir %d", r_caps, r_uid,
		r_ruid, r_svuid, r_rgid, r_svgid, (unsigned long)root, r_root, r_jail);
	if (r_root != 0)
	{
		why = "could not change the process's root folder";
		return false;
	}
#endif
	return true;
}

bool ServeHelper(void (*on_ready)())
{
	signal(SIGPIPE, SIG_IGN);
	const char* env = getenv("N64PS5_HELPER_PORT");
	const int port = (env && *env) ? atoi(env) : kHelperPort;
	const int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0)
	{
		OrbisLog("[helper] socket: errno %d", errno);
		return false;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
#ifdef __FreeBSD__
	sa.sin_len = sizeof(sa);
#endif
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(0x7F000001); // this console only
	if (bind(srv, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || listen(srv, 4) != 0)
	{
		OrbisLog("[helper] port %d is taken (errno %d): a helper is already running", port, errno);
		close(srv);
		return false;
	}
	OrbisLog("[helper] listening on 127.0.0.1:%d, pid %d", port, int(getpid()));
	if (on_ready)
		on_ready();
	for (;;)
	{
		const int c = accept(srv, nullptr, nullptr);
		if (c < 0)
		{
			if (errno == EINTR)
				continue;
			OrbisLog("[helper] accept: errno %d", errno);
			usleep(200 * 1000);
			continue;
		}
		timeval tv = {5, 0};
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		Request req;
		memset(&req, 0, sizeof(req));
		size_t got = 0;
		while (got < sizeof(req))
		{
			const ssize_t n = recv(c, reinterpret_cast<char*>(&req) + got, sizeof(req) - got, 0);
			if (n <= 0)
				break;
			got += size_t(n);
		}
		if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdWantedCovers)
		{
			// covers/wanted.txt, written by the app after its last scan: a fixed file of ours, nothing else
			std::string text;
			if (FILE* f = fopen((OrbisDir("covers") + "/wanted.txt").c_str(), "rb"))
			{
				char buf[16384];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < size_t(kMaxWantedBytes))
					text.append(buf, n);
				fclose(f);
			}
			if (text.size() > size_t(kMaxWantedBytes))
				text.resize(size_t(kMaxWantedBytes));
			req.ret = int32_t(text.size());
			OrbisLog("[helper] pid %d: wanted-covers list, %zu bytes", req.pid, text.size());
			bool ok = true;
			const char* p = reinterpret_cast<const char*>(&req);
			for (size_t left = sizeof(req); ok && left > 0;)
			{
				const ssize_t n = send(c, p, left, 0);
				ok = n > 0;
				if (ok)
				{
					p += n;
					left -= size_t(n);
				}
			}
			for (size_t off = 0; ok && off < text.size();)
			{
				const ssize_t n = send(c, text.data() + off, text.size() - off, 0);
				ok = n > 0;
				if (ok)
					off += size_t(n);
			}
		}
		else if (got == sizeof(req) && req.magic == kMagic && req.cmd == kCmdJailbreak)
		{
			std::string why;
			const bool ok = JailbreakProcess(req.pid, why);
			req.ret = ok ? 0 : -1;
			memset(req.msg1, 0, sizeof(req.msg1));
			snprintf(req.msg1, sizeof(req.msg1), "%s", ok ? "ok" : why.c_str());
			OrbisLog("[helper] pid %d: %s", req.pid, req.msg1);
			const char* p = reinterpret_cast<const char*>(&req);
			size_t left = sizeof(req);
			while (left > 0)
			{
				const ssize_t n = send(c, p, left, 0);
				if (n <= 0)
					break;
				p += n;
				left -= size_t(n);
			}
		}
		else
			OrbisLog("[helper] ignored a request of %zu bytes (magic %x, cmd %d)", got, req.magic, req.cmd);
		close(c);
	}
}
} // namespace jailbreak
