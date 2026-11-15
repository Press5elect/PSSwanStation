/*
	PSSwanStation - the Sinden Lightgun's camera pictures in MJPEG, decoded to
	brightness with FFmpeg's MJPEG decoder (libavcodec 9.0.2,
	LGPL-2.1-or-later, built by ps5/tools/build-ffmpeg.sh).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	A UVC camera's MJPEG pictures usually leave out their Huffman tables; FFmpeg's
	decoder then uses the standard ones, as the UVC MJPEG payload specification
	says to. One decoder for the whole stream, on one thread, with no frame
	threads (they would hold pictures back). The brightness plane is copied
	out; the colour is decoded too, since FFmpeg is built without its
	grey-only option.
*/
#include "sinden_usb.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fe::sinden::usb
{

Mjpeg::~Mjpeg()
{
	close();
}

bool Mjpeg::open(std::string& error)
{
	close();
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
	if (codec == nullptr)
	{
		error = "FFmpeg has no MJPEG decoder";
		return false;
	}
	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (c == nullptr)
	{
		error = "no memory for the decoder";
		return false;
	}
	c->thread_count = 1;
	const int rc = avcodec_open2(c, codec, nullptr);
	if (rc < 0)
	{
		char text[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(rc, text, sizeof(text));
		error = std::string("the decoder did not open: ") + text;
		avcodec_free_context(&c);
		return false;
	}
	context_ = c;
	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (frame_ == nullptr || packet_ == nullptr)
	{
		error = "no memory for the decoder";
		close();
		return false;
	}
	return true;
}

void Mjpeg::close()
{
	AVCodecContext *c = (AVCodecContext *)context_;
	AVFrame *f = (AVFrame *)frame_;
	AVPacket *p = (AVPacket *)packet_;
	if (c != nullptr)
		avcodec_free_context(&c);
	if (f != nullptr)
		av_frame_free(&f);
	if (p != nullptr)
		av_packet_free(&p);
	context_ = frame_ = packet_ = nullptr;
}

bool Mjpeg::decode(const uint8_t *jpeg, size_t bytes, uint8_t *luma, int width, int height, int stride, std::string& error)
{
	AVCodecContext *c = (AVCodecContext *)context_;
	AVFrame *f = (AVFrame *)frame_;
	AVPacket *p = (AVPacket *)packet_;
	if (c == nullptr)
	{
		error = "the decoder is not open";
		return false;
	}
	// The picture is not in a buffer of FFmpeg's: FFmpeg copies it (with the
	// padding after it) and keeps nothing that points into it.
	p->data = const_cast<uint8_t *>(jpeg);
	p->size = (int)bytes;
	int rc = avcodec_send_packet(c, p);
	p->data = nullptr;
	p->size = 0;
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	if (rc < 0)
	{
		av_strerror(rc, text, sizeof(text));
		error = std::string("FFmpeg did not take the picture: ") + text;
		return false;
	}
	rc = avcodec_receive_frame(c, f);
	if (rc < 0)
	{
		av_strerror(rc, text, sizeof(text));
		error = std::string("FFmpeg gave no picture: ") + text;
		return false;
	}
	bool ok = true;
	if (f->width != width || f->height != height)
	{
		char b[96];
		snprintf(b, sizeof(b), "the picture is %dx%d, not %dx%d", f->width, f->height, width, height);
		error = b;
		ok = false;
	}
	else
		for (int y = 0; y < height; y++)
			memcpy(luma + (size_t)y * stride, f->data[0] + (size_t)y * f->linesize[0], (size_t)width);
	av_frame_unref(f);
	return ok;
}

}
