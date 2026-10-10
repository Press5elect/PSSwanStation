/*
	PSSwanStation - the menus' music.

	SPDX-License-Identifier: GPL-3.0-or-later

	By the setting. The title's own is computed like its other sounds
	(sound.cpp): slow chords of soft sine tones over forty-eight seconds, a few
	bell notes over them, made so that its end runs into its beginning. A
	playlist is the files of <root>music (.ogg, .mp3, .wav), or the interface
	kit's three songs (PS5_VKHomebrewUI's assets/audio/music, laid into
	<title folder>assets/hui/music by the build), in an order shuffled at
	each start: each piece is decoded whole (the first six minutes of it) and
	brought to the output's 48 kHz on a thread of its own, the next one while
	this one plays, and audio.cpp crossfades to it when this one ends.

	The decoders are stb_vorbis (Sean Barrett) and dr_mp3 (David Reid), both in
	the public domain (ps5/third_party).
*/
#include "fe.h"

#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
#undef L
#undef R
#undef C

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DRMP3_API static
#define DRMP3_PRIVATE static
#include "../third_party/dr_mp3.h"

namespace fe::sound
{
namespace
{

using Frames = std::shared_ptr<const std::vector<int16_t>>;

constexpr double Rate = 48000.0;
constexpr double TwoPi = 6.283185307179586;
constexpr size_t MostFrames = (size_t)(Rate * 360);		// six minutes

std::mutex mutex;
Frames own;
bool ownAsked;
std::string fileNote = "No file yet.";
std::atomic<bool> fileBusy{false};

// A playlist: its files, shuffled; the piece playing and the next, decoded.
struct Playlist
{
	int kind = -1;					// the music setting it was made for
	std::vector<std::string> files;
	size_t at = 0;					// which file `now` is
	Frames now, next;
	bool nextAsked = false;
	unsigned rounds = 0;			// audio::musicRounds() when `now` began
};
Playlist list;

// ------------------------------------------------------------ the title's own

// A note in equal temperament: 69 is the A of 440 Hz.
double pitch(int note)
{
	return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

// Added round the end of the piece into its beginning: the piece is a ring.
struct Ring
{
	std::vector<float> left, right;
	explicit Ring(double seconds) : left((size_t)(seconds * Rate)), right(left.size()) {}
	void add(size_t index, float l, float r)
	{
		const size_t i = index % left.size();
		left[i] += l;
		right[i] += r;
	}
};

// One held tone of a chord: a sine and a quieter one an octave up, each a
// little off the other ear's, swelling and dying slowly.
void pad(Ring& ring, double at, double length, int note, float level, float pan)
{
	const double f = pitch(note);
	const size_t count = (size_t)(length * Rate), start = (size_t)(at * Rate);
	const float angle = (pan + 1.f) * 0.7853982f;
	const float l = std::cos(angle) * 1.4142f, r = std::sin(angle) * 1.4142f;
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		// Up over three seconds, down over the last five.
		const double shape = std::min(1.0, t / 3.0) * std::min(1.0, (length - t) / 5.0);
		const double swell = 0.82 + 0.18 * std::sin(TwoPi * 0.11 * t + note);
		const double a = std::sin(TwoPi * f * 0.9985 * t) + 0.22 * std::sin(TwoPi * f * 2.0 * t);
		const double b = std::sin(TwoPi * f * 1.0015 * t) + 0.22 * std::sin(TwoPi * f * 2.003 * t);
		const float gain = (float)(shape * shape * swell) * level;
		ring.add(start + i, (float)a * gain * l, (float)b * gain * r);
	}
}

// A struck note over the chords, as the lift's bell is made, softer and longer.
void chime(Ring& ring, double at, int note, float level, float pan)
{
	static const double ratio[4] = { 1.0, 2.0, 3.01, 4.17 };
	static const double weight[4] = { 1.0, 0.30, 0.12, 0.05 };
	static const double decay[4] = { 1.6, 0.9, 0.45, 0.25 };
	const double f = pitch(note);
	const size_t count = (size_t)(6.0 * Rate), start = (size_t)(at * Rate);
	const float angle = (pan + 1.f) * 0.7853982f;
	const float l = std::cos(angle) * 1.4142f, r = std::sin(angle) * 1.4142f;
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		double sum = 0;
		for (int k = 0; k < 4; k++)
			sum += weight[k] * std::sin(TwoPi * f * ratio[k] * t) * std::exp(-t / decay[k]);
		const float sample = (float)(sum * (1.0 - std::exp(-t / 0.004)) * std::min(1.0, (6.0 - t) / 0.5)) * level;
		ring.add(start + i, sample * l, sample * r);
	}
}

Frames makeOwn()
{
	// Four chords of twelve seconds, each running five seconds into the next:
	// D major with its ninth, B minor seventh, G major seventh, A with a
	// fourth that resolves. Low, slow, and never the same in both ears.
	constexpr double Length = 48.0;
	Ring ring(Length);
	static const int chords[4][4] = {
		{ 50, 57, 64, 66 },		// D  A  E  F#
		{ 47, 57, 62, 66 },		// B  A  D  F#
		{ 43, 55, 62, 66 },		// G  G  D  F#
		{ 45, 57, 62, 64 },		// A  A  D  E
	};
	for (int chord = 0; chord < 4; chord++)
		for (int voice = 0; voice < 4; voice++)
			pad(ring, chord * 12.0, 17.0, chords[chord][voice], voice == 0 ? 0.085f : 0.050f,
					voice == 0 ? 0.f : (voice - 2) * 0.45f);
	// The bell notes: the chords' own, high up, a few to each chord.
	static const struct
	{
		double at;
		int note;
		float pan;
	} notes[] = {
		{ 2.0, 78, -0.4f }, { 5.5, 81, 0.3f }, { 9.0, 76, 0.5f },
		{ 14.0, 78, 0.2f }, { 17.5, 74, -0.5f }, { 21.5, 81, 0.4f },
		{ 26.0, 79, -0.3f }, { 29.0, 74, 0.5f }, { 33.0, 78, -0.1f },
		{ 38.0, 76, 0.4f }, { 41.0, 81, -0.4f }, { 44.5, 73, 0.1f },
	};
	for (const auto& note : notes)
		chime(ring, note.at, note.note, 0.030f, note.pan);
	auto out = std::make_shared<std::vector<int16_t>>(ring.left.size() * 2);
	for (size_t i = 0; i < ring.left.size(); i++)
	{
		(*out)[i * 2] = (int16_t)(std::tanh(ring.left[i]) * 32000.f);
		(*out)[i * 2 + 1] = (int16_t)(std::tanh(ring.right[i]) * 32000.f);
	}
	return out;
}

// ------------------------------------------------------------- the user's file

// Interleaved frames of any rate and one or two channels to 48 kHz stereo.
Frames toOutput(const int16_t *in, size_t frames, int channels, int rate)
{
	if (frames == 0 || channels < 1 || rate < 4000)
		return nullptr;
	const double step = (double)rate / Rate;
	const size_t outFrames = std::min((size_t)((double)frames / step), MostFrames);
	auto out = std::make_shared<std::vector<int16_t>>(outFrames * 2);
	for (size_t i = 0; i < outFrames; i++)
	{
		const double at = (double)i * step;
		const size_t a = (size_t)at, b = std::min(a + 1, frames - 1);
		const double part = at - (double)a;
		for (int side = 0; side < 2; side++)
		{
			const int channel = channels > 1 ? side : 0;
			const double x = in[a * (size_t)channels + (size_t)channel], y = in[b * (size_t)channels + (size_t)channel];
			(*out)[i * 2 + (size_t)side] = (int16_t)(x + (y - x) * part);
		}
	}
	// Its end fades into its beginning's silence, so the turn is not a click.
	const size_t fade = std::min(outFrames / 4, (size_t)(Rate * 0.05));
	for (size_t i = 0; i < fade; i++)
		for (int side = 0; side < 2; side++)
		{
			(*out)[i * 2 + (size_t)side] = (int16_t)((*out)[i * 2 + (size_t)side] * (int)i / (int)fade);
			const size_t end = (outFrames - 1 - i) * 2 + (size_t)side;
			(*out)[end] = (int16_t)((*out)[end] * (int)i / (int)fade);
		}
	return out;
}

uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A WAV file of 8, 16, 24 or 32 bit whole numbers, or 32 bit fractions.
Frames decodeWav(const std::vector<uint8_t>& data)
{
	if (data.size() < 44 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0)
		return nullptr;
	int format = 0, channels = 0, rate = 0, bits = 0;
	size_t at = 12;
	while (at + 8 <= data.size())
	{
		const uint32_t size = le32(data.data() + at + 4);
		const uint8_t *body = data.data() + at + 8;
		const size_t room = data.size() - at - 8;
		if (memcmp(data.data() + at, "fmt ", 4) == 0 && size >= 16 && room >= 16)
		{
			format = body[0] | (body[1] << 8);
			channels = body[2] | (body[3] << 8);
			rate = (int)le32(body + 4);
			bits = body[14] | (body[15] << 8);
			// The extensible form names the real format further on.
			if (format == 0xfffe && size >= 26 && room >= 26)
				format = body[24] | (body[25] << 8);
		}
		else if (memcmp(data.data() + at, "data", 4) == 0)
		{
			const size_t bytes = std::min((size_t)size, room);
			const int width = bits / 8;
			if (channels < 1 || channels > 8 || width < 1 || width > 4 || (format != 1 && format != 3))
				return nullptr;
			const size_t frames = std::min(bytes / ((size_t)width * (size_t)channels),
					(size_t)((double)MostFrames * rate / Rate) + 1);
			std::vector<int16_t> pcm(frames * (size_t)channels);
			for (size_t i = 0; i < pcm.size(); i++)
			{
				const uint8_t *s = body + i * (size_t)width;
				if (format == 3 && width == 4)
				{
					float value;
					memcpy(&value, s, 4);
					pcm[i] = (int16_t)(std::clamp(value, -1.f, 1.f) * 32767.f);
				}
				else if (width == 1)
					pcm[i] = (int16_t)((s[0] - 128) << 8);
				else
					pcm[i] = (int16_t)(s[width - 2] | (s[width - 1] << 8));
			}
			return toOutput(pcm.data(), frames, channels, rate);
		}
		at += 8 + (size_t)size + (size & 1);
	}
	return nullptr;
}

Frames decodeOgg(const std::vector<uint8_t>& data)
{
	int channels = 0, rate = 0;
	short *pcm = nullptr;
	const int frames = stb_vorbis_decode_memory(data.data(), (int)data.size(), &channels, &rate, &pcm);
	if (frames <= 0 || pcm == nullptr)
		return nullptr;
	Frames out = toOutput(pcm, (size_t)frames, channels, rate);
	free(pcm);
	return out;
}

Frames decodeMp3(const std::vector<uint8_t>& data)
{
	drmp3_config config{};
	drmp3_uint64 frames = 0;
	drmp3_int16 *pcm = drmp3_open_memory_and_read_pcm_frames_s16(data.data(), data.size(), &config, &frames, nullptr);
	if (pcm == nullptr || frames == 0)
		return nullptr;
	Frames out = toOutput(pcm, (size_t)frames, (int)config.channels, (int)config.sampleRate);
	drmp3_free(pcm, nullptr);
	return out;
}

Frames decodeFile(const std::string& path, std::string& note)
{
	std::vector<uint8_t> data;
	const std::string name = baseName(path);
	// A piece of music, not an album: a file this large is not read.
	if (!readFile(path, data) || data.size() > (96u << 20))
	{
		note = name + " could not be read (it may be too large: 96 MB at most).";
		return nullptr;
	}
	Frames frames = extension(path) == ".ogg" ? decodeOgg(data) : extension(path) == ".mp3" ? decodeMp3(data)
			: decodeWav(data);
	note = frames ? format("%s, %d:%02d", name.c_str(), (int)(frames->size() / 2 / 48000 / 60),
			(int)(frames->size() / 2 / 48000 % 60))
			: name + " is not a sound file this can play.";
	return frames;
}

std::vector<std::string> musicFiles(const std::string& dir)
{
	std::vector<std::string> files;
	if (DIR *d = opendir(dir.c_str()))
	{
		while (const dirent *entry = readdir(d))
		{
			const std::string name = entry->d_name;
			const std::string ext = extension(name);
			if (name[0] != '.' && (ext == ".ogg" || ext == ".mp3" || ext == ".wav"))
				files.push_back(dir + name);
		}
		closedir(d);
	}
	std::sort(files.begin(), files.end());
	return files;
}

// Decodes file `index` of the playlist into its `next` (or `now`, when nothing plays).
void decodeInto(size_t index, bool first)
{
	std::string path, note;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (index >= list.files.size())
			return;
		path = list.files[index];
	}
	Frames frames = decodeFile(path, note);
	diag::mark("music: %s", note.c_str());
	std::lock_guard<std::mutex> lock(mutex);
	fileNote = format("%zu piece%s, in an order shuffled at each start. Now: %s", list.files.size(),
			list.files.size() == 1 ? "" : "s", note.c_str());
	if (first)
		list.now = frames;
	else
		list.next = frames;
}

void makePlaylist(int kind)
{
	const std::string dir = kind == 2 ? rootDir + "music/" : appDir + "assets/hui/music/";
	std::vector<std::string> files = musicFiles(dir);
	// Shuffled once a start: the same order then goes round.
	uint32_t seed = (uint32_t)(now() * 1000.0) | 1u;
	for (size_t i = files.size(); i > 1; i--)
	{
		seed = seed * 1664525u + 1013904223u;
		std::swap(files[i - 1], files[(seed >> 8) % i]);
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		list = Playlist();
		list.kind = kind;
		list.files = files;
		if (files.empty())
			fileNote = kind == 2 ? "No music yet: put .ogg, .mp3 or .wav files in " + shownRoot() + "music."
					: "The interface kit's songs are missing from the title's folder (assets/hui/music).";
	}
	if (!files.empty())
	{
		fileBusy = true;
		std::thread([] {
			decodeInto(0, true);
			fileBusy = false;
		}).detach();
	}
}

}

void music(bool wanted)
{
	const options::Frontend& settings = options::frontend();
	audio::setMusicVolume(settings.musicVolume);
	Frames piece;
	if (wanted && settings.music == 1)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!ownAsked)
		{
			ownAsked = true;
			std::thread([] {
				Frames made = makeOwn();
				std::lock_guard<std::mutex> inner(mutex);
				own = made;
			}).detach();
		}
		piece = own;
	}
	else if (wanted && (settings.music == 2 || settings.music == 3))
	{
		bool fresh = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			fresh = list.kind != settings.music;
		}
		if (fresh)
			makePlaylist(settings.music);
		std::lock_guard<std::mutex> lock(mutex);
		piece = list.now;
		if (list.now && list.files.size() > 1)
		{
			// The next piece is read while this one plays...
			if (!list.nextAsked && !fileBusy)
			{
				list.nextAsked = true;
				const size_t index = (list.at + 1) % list.files.size();
				fileBusy = true;
				std::thread([index] {
					decodeInto(index, false);
					fileBusy = false;
				}).detach();
			}
			// ...and takes its place when this one has played through.
			if (list.next && audio::musicRounds() != list.rounds)
			{
				list.now = list.next;
				list.next = nullptr;
				list.nextAsked = false;
				list.at = (list.at + 1) % list.files.size();
				piece = list.now;
			}
		}
	}
	static Frames playing;
	if (piece != playing)
	{
		playing = piece;
		audio::setMusic(piece);
		std::lock_guard<std::mutex> lock(mutex);
		list.rounds = audio::musicRounds();
	}
}

std::string musicStatus()
{
	if (fileBusy)
		return "Reading the file\xe2\x80\xa6";
	std::lock_guard<std::mutex> lock(mutex);
	return fileNote;
}

#if defined(SWANSTATION_HOST)
// For a test run: the title's own piece, to listen to and to look at its turn.
std::shared_ptr<const std::vector<int16_t>> ownMusicForTest()
{
	return makeOwn();
}
#endif

}
