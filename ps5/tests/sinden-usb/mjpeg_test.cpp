// PSSwanStation - the Sinden Lightgun's MJPEG pictures decoded to brightness
// (src/sinden_mjpeg.cpp) with the title's own FFmpeg build, on a PC.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   mjpeg_test <expected.gray> <picture.jpg> [<picture without its Huffman tables>.jpg]
//
// expected.gray: 320 x 240 bytes of brightness; the JPEG pictures were made
// from it (run.sh does this with the PC's ffmpeg). The second one has its
// Huffman tables taken out, as a UVC camera sends MJPEG.
#include "../../src/sinden_usb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace fe::sinden::usb;

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += ok ? 0 : 1;
}

bool load(const char *path, std::vector<uint8_t>& out)
{
	FILE *f = fopen(path, "rb");
	if (f == nullptr)
		return false;
	uint8_t b[65536];
	size_t n;
	out.clear();
	while ((n = fread(b, 1, sizeof(b), f)) > 0)
		out.insert(out.end(), b, b + n);
	fclose(f);
	return true;
}

// The mean difference of two brightness planes.
double difference(const uint8_t *a, const uint8_t *b, int w, int h, int strideB)
{
	double sum = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			sum += std::abs((int)a[y * w + x] - (int)b[y * strideB + x]);
	return sum / (w * h);
}
}

int main(int argc, char **argv)
{
	const int w = 320, h = 240, stride = 336;
	std::vector<uint8_t> expected;
	if (argc < 3 || !load(argv[1], expected) || expected.size() != (size_t)w * h)
	{
		fprintf(stderr, "usage: mjpeg_test expected.gray picture.jpg [picture-without-tables.jpg]\n");
		return 2;
	}
	Mjpeg m;
	std::string error;
	check(m.open(error), "FFmpeg's MJPEG decoder opens");
	std::vector<uint8_t> luma((size_t)stride * h, 0xEE);
	for (int i = 2; i < argc; i++)
	{
		std::vector<uint8_t> jpeg;
		if (!load(argv[i], jpeg))
		{
			check(false, "a picture to decode");
			continue;
		}
		// As the assembler hands it over: room after it, zeroed.
		const size_t bytes = jpeg.size();
		jpeg.resize(bytes + Assembler::Padding, 0);
		// Twice: the decoder is used again for the next picture.
		for (int round = 0; round < 2; round++)
		{
			const bool ok = m.decode(jpeg.data(), bytes, luma.data(), w, h, stride, error);
			if (!ok)
				printf("     | %s\n", error.c_str());
			const double d = ok ? difference(expected.data(), luma.data(), w, h, stride) : 999;
			printf("     | %s: %zu bytes, mean difference %.2f\n", argv[i], bytes, d);
			check(ok && d < 3.0, i == 2 ? "a JPEG picture decoded to its brightness" : "one without Huffman tables, as UVC sends them");
		}
		check(luma[w] == 0xEE, "rows written `stride` apart, the rest left");
	}
	std::vector<uint8_t> broken = { 0xFF, 0xD8, 0xFF, 0xD9 };
	broken.resize(broken.size() + Assembler::Padding, 0);
	check(!m.decode(broken.data(), 4, luma.data(), w, h, stride, error) && !error.empty(), "a broken picture refused, and why");
	printf("     | %s\n", error.c_str());
	printf("%s: %d failed\n", failures == 0 ? "sinden-usb MJPEG passed" : "sinden-usb MJPEG FAILED", failures);
	return failures == 0 ? 0 : 1;
}
