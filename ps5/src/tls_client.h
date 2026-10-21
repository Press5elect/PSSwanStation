/*
	PSSwanStation - HTTP(S) requests with a TLS of the title's own (mbedTLS).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The console's HTTPS client (libSceHttp2 with libSceSsl) refuses every public
	certificate with 0x8095f00c once the title has left its sandbox through the
	sandbox elevator (as BlackBearReloaded's ps5-native-app-boilerplate found
	and documents in docs/CURL.md, where the answer is a TLS stack in the app).
	This is that stack for this title: a socket of its own, mbedTLS checking
	the server's certificate against the console's list
	(/system/common/cert/CA_LIST.cer) and HTTP/1.1 with redirects and chunked
	answers. It is used only where the console's own client cannot be
	(http_ps5.cpp decides); the PC build tests it against a local server.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe::tls
{
// Where the certificates are; the first readable one is used. On the console
// the console's list; on a PC the system's bundle (SWANSTATION_CA_FILE first).
void setCertificateFiles(const std::vector<std::string>& files);
// A certificate list read as well (PEM or DER), whichever of the above is used.
void addCertificateFile(const std::string& file);
// What it says about itself (the certificates read, a failure's reason), for
// the boot log. Called on the requesting thread.
void setLog(void (*log)(const char *message));
// What the requests say they come from (User-Agent); "PSSwanStation" until set.
void setUserAgent(const std::string& agent);

// One GET. The status code (the body is kept for error statuses too, up to
// 64 KiB; for a success up to `limit` bytes), or -1 with `why` set when no
// answer came. `seconds` limits the whole of it.
int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, size_t limit = 8u << 20,
		std::string *why = nullptr);
// Any request: `method` (POST, PATCH, PUT, DELETE...), headers of the
// caller's (Content-Length is added), and a body. As get() otherwise; a
// redirect is followed only for GET, and for a 303 as a GET.
int send(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
		const std::string& body, std::vector<uint8_t>& out, unsigned seconds, size_t limit = 8u << 20,
		std::string *why = nullptr);
// One GET written to `path` as it arrives (anything but 200 leaves no file).
// `progress` is told how far it is (total: the server's Content-Length, else
// 0) and stops it by answering false. `seconds`: the limit on each wait.
int download(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress, unsigned seconds = 60,
		std::string *why = nullptr);
}
