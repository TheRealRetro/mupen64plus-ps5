// Mupen64Plus PS5: PS5 toast notifications.
//
// The kernel toast, sceKernelSendNotificationRequest(0, request, 0xc30, 0): a 0xc30-byte request whose text
// starts at byte 0x2d (PS4-Notify's layout, the one PS5SX2 has used since its start). Callers only queue;
// a worker thread makes the system call, as in PS5SX2, so a slow system UI never stalls a frame.
//
// SPDX-License-Identifier: MIT

#include "ProsperoNotify.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace
{
#pragma pack(push, 1)
struct KernelToast
{
	int32_t type; // 0: a plain message
	int32_t req_id;
	int32_t priority;
	int32_t msg_id;
	int32_t target_id;
	int32_t user_id;
	int32_t unk1;
	int32_t unk2;
	int32_t app_id;
	int32_t error_num;
	int32_t unk3;
	uint8_t use_icon_image_uri; // 0x2c
	char message[1024]; // 0x2d
	char icon_uri[1024]; // 0x42d
	char unk[1024];
	uint8_t pad[3];
};
#pragma pack(pop)
static_assert(sizeof(KernelToast) == 0xc30, "the kernel toast request is 0xc30 bytes");
static_assert(offsetof(KernelToast, message) == 0x2d, "its text starts at 0x2d");

struct Shared
{
	std::mutex lock;
	std::condition_variable wake;
	std::deque<std::string> queue;
	bool started = false;
	bool quit = false;
	std::thread worker;
};

// Never freed: the worker may still wait on it while static destructors run.
Shared& S()
{
	static Shared* const s = new Shared();
	return *s;
}

void Send(const std::string& text)
{
	KernelToast req;
	memset(&req, 0, sizeof(req));
	snprintf(req.message, sizeof(req.message), "%s", text.c_str());
	const int r = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
	OrbisLog("[notify] \"%s\" -> %d", text.c_str(), r);
}

void Worker()
{
	Shared& s = S();
	std::unique_lock<std::mutex> lock(s.lock);
	for (;;)
	{
		s.wake.wait(lock, [&] { return s.quit || !s.queue.empty(); });
		while (!s.queue.empty())
		{
			std::string text = std::move(s.queue.front());
			s.queue.pop_front();
			lock.unlock();
			Send(text);
			lock.lock();
		}
		if (s.quit)
			return;
	}
}
} // namespace

void ProsperoNotify(const char* fmt, ...)
{
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	Shared& s = S();
	std::lock_guard<std::mutex> lock(s.lock);
	if (s.quit)
		return;
	if (s.queue.size() >= 8)
		s.queue.pop_front(); // a burst of toasts: keep the newest
	s.queue.emplace_back(text);
	if (!s.started)
	{
		s.started = true;
		s.worker = std::thread(Worker);
	}
	s.wake.notify_one();
}

void ProsperoNotifyFlush()
{
	Shared& s = S();
	{
		std::lock_guard<std::mutex> lock(s.lock);
		if (!s.started || s.quit)
			return;
		s.quit = true;
		s.wake.notify_one();
	}
	if (s.worker.joinable())
		s.worker.join();
}
