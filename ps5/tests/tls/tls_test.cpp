/*
	PSSwanStation - the PC's test of tls_client.cpp.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	tls-test CA_FILE URL [OUT_FILE]: one GET (or a download to OUT_FILE with
	the server's certificate checked against CA_FILE alone. Prints the status,
	the size and the first bytes, or why it failed.
*/
#include "tls_client.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "tls-test CA_FILE URL [OUT_FILE]\n");
		return 2;
	}
	fe::tls::setCertificateFiles({ argv[1] });
	fe::tls::setLog([](const char *line) { fprintf(stderr, "%s\n", line); });
	std::string why;
	if (argc > 3)
	{
		uint64_t seen = 0, announced = 0;
		const int status = fe::tls::download(argv[2], argv[3], 1ull << 32,
				[&](uint64_t done, uint64_t total) {
					seen = done;
					announced = total;
					return true;
				},
				20, &why);
		printf("status %d, %llu bytes (announced %llu)%s%s\n", status, (unsigned long long)seen,
				(unsigned long long)announced, why.empty() ? "" : ": ", why.c_str());
		return status == 200 ? 0 : 1;
	}
	std::vector<uint8_t> body;
	const int status = fe::tls::get(argv[2], body, 20, 8u << 20, &why);
	printf("status %d, %zu bytes%s%s\n", status, body.size(), why.empty() ? "" : ": ", why.c_str());
	if (!body.empty())
		printf("body: %.*s\n", (int)std::min<size_t>(body.size(), 80), (const char *)body.data());
	return status > 0 ? 0 : 1;
}
