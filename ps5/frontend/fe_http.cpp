// Mupen64Plus PS5 frontend: HTTPS GET (fe_http.h).
//
// The sequence is PS5SX2's (vk-285-41 .. 110): ask libSceNetCtl first whether the console has an IP
// address, so an offline console never waits on the resolver; one net pool, one SSL context, one HTTP/2
// context and template; per request 10 s timeouts per phase and 20 s overall, redirects followed; every step
// logged with its result.
//
// SPDX-License-Identifier: MIT

#include "fe_http.h"

#include "OrbisPaths.h"
#include "ProsperoCrash.h"
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
} // namespace

std::string UrlEncode(const std::string& s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~')
			out += char(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool Http::Init()
{
	if (m_tried)
		return m_ok;
	m_tried = true;
	N64_STAGE(Cover, "http: netctl init");
	const int nc = sceNetCtlInit();
	m_netctl = nc == 0;
	int state[4] = {-1, 0, 0, 0};
	const int gs = sceNetCtlGetState(state);
	OrbisLog("[http] netctl init %x, state %d (%x)", unsigned(nc), state[0], unsigned(gs));
	if (gs == 0 && state[0] >= 0 && state[0] < 3)
	{
		OrbisLog("[http] the console is not connected: no downloads this time");
		return false;
	}
	N64_STAGE(Cover, "http: net init");
	const int net = sceNetInit(); // an error only means it was up already
	N64_STAGE(Cover, "http: net pool");
	m_pool = sceNetPoolCreate("n64-covers", 64 * 1024, 0);
	N64_STAGE(Cover, "http: ssl init");
	m_ssl = m_pool >= 0 ? sceSslInit(256 * 1024) : -1;
	N64_STAGE(Cover, "http: http2 init");
	m_ctx = m_ssl >= 0 ? sceHttp2Init(m_pool, m_ssl, 256 * 1024, 1) : -1;
	N64_STAGE(Cover, "http: http2 template");
	m_tmpl = m_ctx >= 0 ? sceHttp2CreateTemplate(m_ctx, "Mupen64PS5/" N64PS5_VERSION, 3, 1) : -1;
	OrbisLog("[http] net %x pool %x ssl %x http2 %x template %x", unsigned(net), unsigned(m_pool), unsigned(m_ssl),
		unsigned(m_ctx), unsigned(m_tmpl));
	m_ok = m_tmpl >= 0;
	return m_ok;
}

void Http::Abort()
{
	m_stopping.store(true);
	std::lock_guard<std::mutex> lock(m_abort);
	const int req = m_active.load();
	if (req >= 0)
		OrbisLog("[http] abort %x -> %x", unsigned(req), unsigned(sceHttp2AbortRequest(req)));
}

void Http::Term()
{
	if (m_tmpl >= 0)
		sceHttp2DeleteTemplate(m_tmpl);
	if (m_ctx >= 0)
		sceHttp2Term(m_ctx);
	if (m_ssl >= 0)
		sceSslTerm(m_ssl);
	if (m_pool >= 0)
		sceNetPoolDestroy(m_pool);
	if (m_netctl)
		sceNetCtlTerm();
	m_tmpl = m_ctx = m_ssl = m_pool = -1;
	m_tried = m_ok = m_netctl = false;
}

int Http::Get(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes)
{
	// A transport failure (send or status refused) is tried again after a pause: the console's network
	// can still be settling when the shelf opens. Every step logs its return code, as PS5SX2's fetcher does.
	int status = -1;
	for (int attempt = 1; attempt <= 3 && !m_stopping; attempt++)
	{
		status = GetOnce(url, out, max_bytes, attempt);
		if (status != -1 || Offline()) // no network: no point waiting to try again
			break;
		for (int i = 0; i < 15 && !m_stopping; i++)
			usleep(100 * 1000);
	}
	return status;
}

int Http::GetOnce(const std::string& url, std::vector<uint8_t>& out, size_t max_bytes, int attempt)
{
	out.clear();
	if (m_stopping || !Init())
		return -1;
	const double t0 = Now();
	const int req = sceHttp2CreateRequestWithURL(m_tmpl, "GET", url.c_str(), 0);
	if (req < 0)
	{
		OrbisLog("[http] try %d: request for %s -> %x", attempt, url.c_str(), unsigned(req));
		return -1;
	}
	const int t1 = sceHttp2SetResolveTimeOut(req, 10 * 1000 * 1000);
	const int t2 = sceHttp2SetConnectTimeOut(req, 10 * 1000 * 1000);
	const int t3 = sceHttp2SetSendTimeOut(req, 10 * 1000 * 1000);
	const int t4 = sceHttp2SetRecvTimeOut(req, 10 * 1000 * 1000);
	const int t5 = sceHttp2SetTimeOut(req, 20 * 1000 * 1000);
	const int rd = sceHttp2SetAutoRedirect(req, 1);
	m_active = req;
	int status = -1, sent = -1, got = -1, read_err = 0;
	if (!m_stopping)
	{
		sent = sceHttp2SendRequest(req, nullptr, 0);
		got = sent == 0 ? sceHttp2GetStatusCode(req, &status) : -1;
		if (sent != 0 || got != 0)
			status = -1;
		else if (status == 200)
		{
			uint8_t buf[16384];
			int n;
			while ((n = sceHttp2ReadData(req, buf, sizeof(buf))) > 0)
			{
				out.insert(out.end(), buf, buf + n);
				if (out.size() > max_bytes)
				{
					status = -2;
					break;
				}
			}
			if (n < 0)
			{
				read_err = n;
				status = -1;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(m_abort);
		m_active = -1;
	}
	sceHttp2DeleteRequest(req);
	OrbisLog("[http] try %d: GET %s -> %d (request %x, timeouts %x %x %x %x %x, redirect %x, send %x, status call %x, "
			 "read %x), %zu bytes, %.0f ms",
		attempt, url.c_str(), status, unsigned(req), unsigned(t1), unsigned(t2), unsigned(t3), unsigned(t4),
		unsigned(t5), unsigned(rd), unsigned(sent), unsigned(got), unsigned(read_err), out.size(),
		(Now() - t0) * 1000.0);
	if (status != 200)
		out.clear();
	return status;
}
} // namespace fe
