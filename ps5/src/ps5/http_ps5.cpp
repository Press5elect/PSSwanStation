/*
	PSSwanStation - HTTP(S) through the console's own client.

	SPDX-License-Identifier: GPL-3.0-or-later

	libSceHttp2 on the calling thread, with the calls, pool sizes and time-outs
	of PSFlyCast's cover downloader (shell/ps5/ps5_covers.cpp, after PS5SX2's
	cover fetcher), which runs in a title's sandbox. One request at a time.
*/
#include "fe.h"

#include <algorithm>
#include <mutex>

extern "C"
{
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t poolSize);
int sceHttp2Init(int netPool, int sslContext, size_t poolSize, int maxRequests);
int sceHttp2CreateTemplate(int context, const char *userAgent, int httpVersion, int autoProxy);
int sceHttp2CreateRequestWithURL(int templateId, const char *method, const char *url, uint64_t contentLength);
int sceHttp2DeleteRequest(int request);
int sceHttp2SendRequest(int request, const void *data, size_t size);
int sceHttp2GetStatusCode(int request, int *status);
int sceHttp2ReadData(int request, void *data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceNetCtlInit(void);
int sceNetCtlGetState(int *state);
}

namespace fe::platform
{
namespace
{
std::mutex mutex;
bool tried, ok;
int templateId = -1;
int reported;

bool init()
{
	if (tried)
		return ok;
	tried = true;
	// Is there a network at all? An offline console must not wait on the
	// resolver. State 3 is "address obtained".
	const int nc = sceNetCtlInit();
	int state[4] = { -1, 0, 0, 0 };
	const int gs = sceNetCtlGetState(state);
	if (gs == 0 && state[0] >= 0 && state[0] < 3)
	{
		diag::mark("http: the console is not connected (netctl %#x, state %d)", (unsigned)nc, state[0]);
		return false;
	}
	const int net = sceNetInit();	// an error only means it was up already
	const int pool = sceNetPoolCreate("psswanstation-http", 64 * 1024, 0);
	const int ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
	const int context = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
	templateId = context >= 0 ? sceHttp2CreateTemplate(context, "PSSwanStation/1.0", 3, 1) : -1;
	diag::mark("http: net %#x pool %#x ssl %#x http2 %#x template %#x", (unsigned)net, (unsigned)pool, (unsigned)ssl,
			(unsigned)context, (unsigned)templateId);
	ok = templateId >= 0;
	return ok;
}
}

bool httpAvailable()
{
	std::lock_guard<std::mutex> lock(mutex);
	return init();
}

int httpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, int *error)
{
	std::lock_guard<std::mutex> lock(mutex);
	out.clear();
	if (error != nullptr)
		*error = 0;
	if (!init())
		return -1;
	const int request = sceHttp2CreateRequestWithURL(templateId, "GET", url.c_str(), 0);
	if (request < 0)
	{
		if (error != nullptr)
			*error = request;
		return -1;
	}
	const unsigned phase = std::min(seconds, 10u) * 1000 * 1000;
	sceHttp2SetResolveTimeOut(request, phase);
	sceHttp2SetConnectTimeOut(request, phase);
	sceHttp2SetSendTimeOut(request, phase);
	sceHttp2SetRecvTimeOut(request, phase);
	sceHttp2SetTimeOut(request, seconds * 1000 * 1000);
	sceHttp2SetAutoRedirect(request, 1);
	int status = -1;
	const int sent = sceHttp2SendRequest(request, nullptr, 0);
	const int got = sent == 0 ? sceHttp2GetStatusCode(request, &status) : -1;
	if (sent != 0 || got != 0)
	{
		if (error != nullptr)
			*error = sent != 0 ? sent : got;
		if (reported++ < 6)
			diag::mark("http: request failed: send %#x, status %#x (%s)", (unsigned)sent, (unsigned)got,
					url.substr(0, 60).c_str());
		status = -1;
	}
	else if (status >= 200 && status < 300)
	{
		std::vector<uint8_t> chunk(64 * 1024);
		for (;;)
		{
			const int n = sceHttp2ReadData(request, chunk.data(), chunk.size());
			if (n < 0)
			{
				if (reported++ < 6)
					diag::mark("http: reading the answer failed: %#x", (unsigned)n);
				status = -1;
				break;
			}
			if (n == 0)
				break;
			out.insert(out.end(), chunk.begin(), chunk.begin() + n);
			if (out.size() > (8u << 20))
			{
				status = -1;
				break;
			}
		}
	}
	sceHttp2DeleteRequest(request);
	return status;
}

}
