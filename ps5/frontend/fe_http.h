// Mupen64Plus PS5 frontend: HTTPS GET through the console's own libSceHttp2 + libSceSsl, as PS5SX2's
// fe_ps5.cpp does it (and the payload SDK's http2_get sample).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fe
{
class Http
{
public:
	~Http() { Term(); }
	// The HTTP status (200 on success), or -1 when the request could not be made (no network...).
	// Only one thread may call Get.
	int Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes = 8u << 20);

private:
	int GetOnce(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, int attempt);

public:
	// Any thread: fails the request in flight (shutdown).
	void Abort();
	void Term();
	bool Offline() const { return m_tried && !m_ok; }

private:
	bool Init();
	bool m_tried = false, m_ok = false, m_netctl = false;
	int m_pool = -1, m_ssl = -1, m_ctx = -1, m_tmpl = -1;
	std::atomic<int> m_active{-1};
	std::atomic<bool> m_stopping{false};
	std::mutex m_abort;
};

// "Super Metroid (Japan, USA) (En,Ja)" -> "Super%20Metroid%20%28Japan%2C%20USA%29%20%28En%2CJa%29"
std::string UrlEncode(const std::string& s);
} // namespace fe
