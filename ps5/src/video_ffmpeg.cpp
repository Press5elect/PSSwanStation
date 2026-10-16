/*
	PSSwanStation - plays a video for video.cpp with FFmpeg's decoders (H.264
	and AAC or MP3 in an MP4 file), on the processor, on the console and on
	the PC alike.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	FFmpeg (libavformat, libavcodec, libavutil 9.0.2, LGPL-2.1-or-later) is
	built by ps5/tools/build-ffmpeg.sh with only the MP4 demuxer, the H.264,
	AAC and MP3 decoders and their parsers. The console has a player of its
	own (libSceAvPlayer) but refuses it to a title like this one, so the
	videos are decoded here.

	A thread of its own reads the file (on the console, a USB drive or a
	network share, through the same smb::File the games are read with),
	decodes it, and keeps a few pictures ahead, each as NV12 with its time,
	and the sound as 16-bit samples with theirs. The frame's thread takes the
	picture whose time has come and the sound up to a little past it. The
	clock starts at the first picture, so a slow share delays a video rather
	than cutting its start. A video that loops is read again from its start,
	its times carried on from where it ended.
*/
#include "fe.h"
#include "video_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fe::video::backend
{
namespace
{

constexpr size_t PicturesAhead = 6;
constexpr double SoundAhead = 2.0;		// seconds decoded ahead at most
constexpr double SoundLead = 0.08;		// handed to the output this far ahead of the picture
constexpr int IoBuffer = 64 * 1024;

struct Frame
{
	double time = 0;
	int width = 0, height = 0;
	std::vector<uint8_t> nv12;		// width x height brightness, then the colour at half the size
};

struct Sound
{
	double time = 0;
	int channels = 2, rate = 48000;
	std::vector<int16_t> samples;
};

// The file being read: local, or on a share.
struct Source
{
	FILE *local = nullptr;
	smb::File *remote = nullptr;
	int64_t size = -1;

	~Source()
	{
		if (local != nullptr)
			fclose(local);
		delete remote;
	}
};

int readPacket(void *opaque, uint8_t *buffer, int bytes)
{
	Source *s = (Source *)opaque;
	const size_t got = s->remote != nullptr ? s->remote->read(buffer, (size_t)bytes) : fread(buffer, 1, (size_t)bytes, s->local);
	return got == 0 ? AVERROR_EOF : (int)got;
}

int64_t seekTo(void *opaque, int64_t offset, int whence)
{
	Source *s = (Source *)opaque;
	if (whence & AVSEEK_SIZE)
		return s->size;
	whence &= ~AVSEEK_FORCE;
	if (s->remote != nullptr)
	{
		if (s->remote->seek(offset, whence) < 0)
			return -1;
		return s->remote->tell();
	}
	if (fseeko(s->local, offset, whence) != 0)
		return -1;
	return ftello(s->local);
}

// One video being played.
struct Player
{
	std::string path;
	bool loop = false;

	std::thread thread;
	std::mutex mutex;
	std::condition_variable room;		// the decoder waits here for room ahead
	std::atomic<bool> quit{false};
	bool done = false;		// the decoder has stopped (its end, or a failure)
	std::string failure;
	std::deque<Frame> frames;
	std::deque<Sound> sounds;
	std::vector<std::vector<uint8_t>> spare;		// pictures' buffers to use again

	// The frame's thread's own.
	Frame shown;
	bool clockStarted = false;
	double startedAt = 0, firstTime = 0;

	double soundAhead() const
	{
		double seconds = 0;
		for (const Sound& s : sounds)
			seconds += (double)s.samples.size() / s.channels / s.rate;
		return seconds;
	}
	bool full() const
	{
		return frames.size() >= PicturesAhead || soundAhead() >= SoundAhead;
	}

	void run();
	void decode();
};

std::unique_ptr<Player> player;
std::atomic<int> logged{0};

void logLine(void *, int level, const char *format, va_list args)
{
	if (level > AV_LOG_ERROR || logged.fetch_add(1) >= 20)
		return;
	char line[512];
	vsnprintf(line, sizeof(line), format, args);
	size_t n = strlen(line);
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		line[--n] = 0;
	diag::mark("video: ffmpeg: %s", line);
}

std::string errorText(int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = "";
	av_strerror(code, text, sizeof(text));
	return text;
}

// A decoded picture into NV12: 8-bit 4:2:0 only (what such videos are).
bool toNv12(const AVFrame *f, std::vector<uint8_t>& out, int& width, int& height)
{
	width = f->width & ~1;
	height = f->height & ~1;
	if (width <= 0 || height <= 0)
		return false;
	out.resize((size_t)width * height * 3 / 2);
	uint8_t *y = out.data(), *uv = out.data() + (size_t)width * height;
	switch (f->format)
	{
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		av_image_copy_plane(y, width, f->data[0], f->linesize[0], width, height);
		for (int row = 0; row < height / 2; row++)
		{
			const uint8_t *u = f->data[1] + (size_t)row * f->linesize[1];
			const uint8_t *v = f->data[2] + (size_t)row * f->linesize[2];
			uint8_t *dst = uv + (size_t)row * width;
			for (int x = 0; x < width / 2; x++)
			{
				dst[x * 2] = u[x];
				dst[x * 2 + 1] = v[x];
			}
		}
		return true;
	case AV_PIX_FMT_NV12:
		av_image_copy_plane(y, width, f->data[0], f->linesize[0], width, height);
		av_image_copy_plane(uv, width, f->data[1], f->linesize[1], width, height / 2);
		return true;
	default:
		return false;
	}
}

inline int16_t toSample(float v)
{
	v *= 32767.0f;
	return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
}

// Decoded sound into interleaved 16-bit samples.
bool toSamples(const AVFrame *f, std::vector<int16_t>& out, int channels)
{
	const int n = f->nb_samples;
	out.resize((size_t)n * channels);
	int16_t *dst = out.data();
	switch (f->format)
	{
	case AV_SAMPLE_FMT_FLTP:
		for (int c = 0; c < channels; c++)
		{
			const float *src = (const float *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = toSample(src[i]);
		}
		return true;
	case AV_SAMPLE_FMT_FLT:
		for (int i = 0; i < n * channels; i++)
			dst[i] = toSample(((const float *)f->extended_data[0])[i]);
		return true;
	case AV_SAMPLE_FMT_S16P:
		for (int c = 0; c < channels; c++)
		{
			const int16_t *src = (const int16_t *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = src[i];
		}
		return true;
	case AV_SAMPLE_FMT_S16:
		memcpy(dst, f->extended_data[0], (size_t)n * channels * 2);
		return true;
	case AV_SAMPLE_FMT_S32P:
		for (int c = 0; c < channels; c++)
		{
			const int32_t *src = (const int32_t *)f->extended_data[c];
			for (int i = 0; i < n; i++)
				dst[i * channels + c] = (int16_t)(src[i] >> 16);
		}
		return true;
	default:
		return false;
	}
}

void Player::run()
{
	decode();
	std::lock_guard<std::mutex> lock(mutex);
	done = true;
}

void Player::decode()
{
	Source source;
	if (smb::isNetworkPath(path))
	{
		source.remote = smb::open(path);
		if (source.remote == nullptr || source.remote->failed())
		{
			std::lock_guard<std::mutex> lock(mutex);
			failure = "the share would not open it";
			return;
		}
		source.size = source.remote->size();
	}
	else
	{
		source.local = fopen(path.c_str(), "rb");
		if (source.local == nullptr)
		{
			std::lock_guard<std::mutex> lock(mutex);
			failure = "it would not open";
			return;
		}
		if (fseeko(source.local, 0, SEEK_END) == 0)
			source.size = ftello(source.local);
		fseeko(source.local, 0, SEEK_SET);
	}

	uint8_t *ioBuffer = (uint8_t *)av_malloc(IoBuffer);
	AVIOContext *io = avio_alloc_context(ioBuffer, IoBuffer, 0, &source, readPacket, nullptr, seekTo);
	AVFormatContext *format = avformat_alloc_context();
	AVCodecContext *video = nullptr, *audio = nullptr;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int videoStream = -1, audioStream = -1;
	std::string why;

	const auto openDecoder = [&](int stream) -> AVCodecContext * {
		const AVCodecParameters *par = format->streams[stream]->codecpar;
		const AVCodec *codec = avcodec_find_decoder(par->codec_id);
		if (codec == nullptr)
			return nullptr;
		AVCodecContext *ctx = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(ctx, par);
		ctx->pkt_timebase = format->streams[stream]->time_base;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			// Three threads: the console's processor has eight cores, the
			// emulator is not running while the library is shown.
			ctx->thread_count = 3;
			ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		}
		if (avcodec_open2(ctx, codec, nullptr) < 0)
		{
			avcodec_free_context(&ctx);
			return nullptr;
		}
		return ctx;
	};

	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	int err = avformat_open_input(&format, nullptr, nullptr, nullptr);
	if (err < 0)
	{
		why = "not a video FFmpeg reads (" + errorText(err) + ")";
		format = nullptr;		// freed by avformat_open_input
	}
	else if ((err = avformat_find_stream_info(format, nullptr)) < 0)
		why = "its streams could not be read (" + errorText(err) + ")";
	else
	{
		videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		audioStream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoStream, nullptr, 0);
		if (videoStream < 0)
			why = "no picture in it";
		else if ((video = openDecoder(videoStream)) == nullptr)
			why = std::string("no decoder for its pictures (") + avcodec_get_name(format->streams[videoStream]->codecpar->codec_id) + ")";
		else if (audioStream >= 0 && (audio = openDecoder(audioStream)) == nullptr)
			diag::mark("video: no decoder for its sound (%s); played without it",
					avcodec_get_name(format->streams[audioStream]->codecpar->codec_id));
	}

	const AVRational videoBase = videoStream >= 0 ? format->streams[videoStream]->time_base : AVRational{1, 1};
	const AVRational audioBase = audioStream >= 0 ? format->streams[audioStream]->time_base : AVRational{1, 1};
	double offset = 0, lastEnd = 0;		// a loop's times carry on from the end of the last round
	bool anyPicture = false;

	// Waits for room ahead; false when the player is closing.
	const auto waitForRoom = [&]() {
		std::unique_lock<std::mutex> lock(mutex);
		room.wait(lock, [&] { return quit.load() || !full(); });
		return !quit.load();
	};

	const auto receive = [&](AVCodecContext *ctx, bool isVideo) {
		while (avcodec_receive_frame(ctx, frame) == 0)
		{
			const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
			if (isVideo)
			{
				Frame f;
				f.time = offset + (pts == AV_NOPTS_VALUE ? lastEnd - offset : pts * av_q2d(videoBase));
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (!spare.empty())
					{
						f.nv12 = std::move(spare.back());
						spare.pop_back();
					}
				}
				if (toNv12(frame, f.nv12, f.width, f.height))
				{
					const double length = frame->duration > 0 ? frame->duration * av_q2d(videoBase) : 1.0 / 30;
					lastEnd = std::max(lastEnd, f.time + length);
					anyPicture = true;
					if (!waitForRoom())
						return false;
					std::lock_guard<std::mutex> lock(mutex);
					frames.push_back(std::move(f));
				}
				else if (!anyPicture)
				{
					why = std::string("its pictures are ") + (av_get_pix_fmt_name((AVPixelFormat)frame->format) ?: "?")
							+ ", not 8-bit 4:2:0";
					return false;
				}
			}
			else
			{
				Sound s;
				s.channels = std::clamp(frame->ch_layout.nb_channels, 1, 8);
				s.rate = frame->sample_rate > 0 ? frame->sample_rate : 48000;
				s.time = offset + (pts == AV_NOPTS_VALUE ? 0 : pts * av_q2d(audioBase));
				if (toSamples(frame, s.samples, s.channels))
				{
					lastEnd = std::max(lastEnd, s.time + (double)frame->nb_samples / s.rate);
					if (!waitForRoom())
						return false;
					std::lock_guard<std::mutex> lock(mutex);
					sounds.push_back(std::move(s));
				}
			}
			av_frame_unref(frame);
		}
		return true;
	};

	if (why.empty())
		diag::mark("video: %s, %d x %d, %s%s", avcodec_get_name(video->codec_id), video->width, video->height,
				audio != nullptr ? avcodec_get_name(audio->codec_id) : "no sound", loop ? ", looped" : "");

	bool going = why.empty();
	while (going && !quit.load())
	{
		err = av_read_frame(format, packet);
		if (err == AVERROR_EOF || (err < 0 && anyPicture))
		{
			// The end: what the decoders still hold, then again from the start or stop.
			avcodec_send_packet(video, nullptr);
			going = receive(video, true);
			if (audio != nullptr)
			{
				avcodec_send_packet(audio, nullptr);
				going = going && receive(audio, false);
			}
			if (!going || !loop || !anyPicture)
				break;
			if (av_seek_frame(format, -1, 0, AVSEEK_FLAG_BACKWARD) < 0)
				break;
			avcodec_flush_buffers(video);
			if (audio != nullptr)
				avcodec_flush_buffers(audio);
			offset = lastEnd;
			continue;
		}
		if (err < 0)
		{
			why = "it could not be read (" + errorText(err) + ")";
			break;
		}
		if (packet->stream_index == videoStream)
		{
			if (avcodec_send_packet(video, packet) == AVERROR(EAGAIN))
				going = receive(video, true) && avcodec_send_packet(video, packet) >= 0;
			going = going && receive(video, true);
		}
		else if (audio != nullptr && packet->stream_index == audioStream)
		{
			if (avcodec_send_packet(audio, packet) >= 0)
				going = receive(audio, false);
		}
		av_packet_unref(packet);
	}

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&video);
	avcodec_free_context(&audio);
	if (format != nullptr)
		avformat_close_input(&format);
	if (io != nullptr)
	{
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	if (!why.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		failure = why;
	}
}

} // namespace

const char *name()
{
	static const std::string text = std::string("FFmpeg ") + av_version_info();
	return text.c_str();
}

bool broken()
{
	return false;
}

bool load(std::string&)
{
	av_log_set_level(AV_LOG_ERROR);
	av_log_set_callback(logLine);
	return true;
}

bool open(const std::string& path, bool loop, std::string& why)
{
	close();
	if (path.empty())
	{
		why = "no file";
		return false;
	}
	player = std::make_unique<Player>();
	player->path = path;
	player->loop = loop;
	Player *p = player.get();
	player->thread = std::thread([p] { p->run(); });
	return true;
}

void close()
{
	if (!player)
		return;
	player->quit = true;
	player->room.notify_all();
	if (player->thread.joinable())
		player->thread.join();
	player.reset();
}

bool take(Picture& out)
{
	if (!player)
		return false;
	Player& p = *player;
	std::unique_lock<std::mutex> lock(p.mutex);
	if (!p.failure.empty())
	{
		diag::mark("video: %s cannot be played: %s", p.path.substr(p.path.rfind('/') + 1).c_str(), p.failure.c_str());
		p.failure.clear();
	}
	if (p.frames.empty())
		return false;
	if (!p.clockStarted)
	{
		p.clockStarted = true;
		p.firstTime = p.frames.front().time;
		p.startedAt = now();
	}
	const double clock = p.firstTime + (now() - p.startedAt);
	if (p.frames.front().time > clock && !p.shown.nv12.empty())
		return false;
	// The newest picture whose time has come; those before it are passed over.
	do
	{
		if (!p.shown.nv12.empty())
			p.spare.push_back(std::move(p.shown.nv12));
		p.shown = std::move(p.frames.front());
		p.frames.pop_front();
	} while (!p.frames.empty() && p.frames.front().time <= clock);
	lock.unlock();
	p.room.notify_all();
	out.y = p.shown.nv12.data();
	out.uv = p.shown.nv12.data() + (size_t)p.shown.width * p.shown.height;
	out.width = p.shown.width;
	out.height = p.shown.height;
	out.pitch = out.uvPitch = p.shown.width;
	return true;
}

void drainAudio(const std::function<void(const int16_t *, size_t, int, int)>& sink)
{
	if (!player)
		return;
	Player& p = *player;
	std::vector<Sound> due;
	{
		std::lock_guard<std::mutex> lock(p.mutex);
		if (!p.clockStarted)
			return;
		const double clock = p.firstTime + (now() - p.startedAt) + SoundLead;
		while (!p.sounds.empty() && p.sounds.front().time <= clock)
		{
			due.push_back(std::move(p.sounds.front()));
			p.sounds.pop_front();
		}
	}
	if (due.empty())
		return;
	p.room.notify_all();
	if (sink)
		for (const Sound& s : due)
			sink(s.samples.data(), s.samples.size() / s.channels, s.channels, s.rate);
}

bool active()
{
	if (!player)
		return false;
	std::lock_guard<std::mutex> lock(player->mutex);
	return !(player->done && player->frames.empty());
}

} // namespace fe::video::backend
