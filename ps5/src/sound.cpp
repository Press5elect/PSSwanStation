/*
	PSSwanStation - the interface's sounds.

	SPDX-License-Identifier: GPL-3.0-or-later

	The start-up animation is heard as well as seen: water as the swan's head
	comes up and paddles along the bottom of the screen, a dive, the lift's
	doors, its motor on the way up, its bell, the doors opening, and wings.

	Nothing here is a recording. Each sound is a few sine waves and some
	filtered noise, computed when the title starts into 48 kHz stereo and handed
	to audio.cpp to mix. The animation's sound is one piece laid out on the
	animation's own times (splashtime, fe.h), so the two cannot drift apart
	event by event.
*/
#include "fe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fe::sound
{
namespace
{

constexpr double Rate = 48000.0;
constexpr double TwoPi = 6.283185307179586;

struct Track
{
	std::vector<float> left, right;
	explicit Track(double seconds) : left((size_t)(seconds * Rate)), right((size_t)(seconds * Rate)) {}
	// `pan`: -1 left, 0 the middle, 1 right (constant power).
	void add(double at, float sample, size_t index, float pan)
	{
		const size_t i = (size_t)(at * Rate) + index;
		if (i >= left.size())
			return;
		const float angle = (pan + 1.f) * 0.7853982f;
		left[i] += sample * std::cos(angle) * 1.4142f;
		right[i] += sample * std::sin(angle) * 1.4142f;
	}
};

// The same noise every time.
struct Noise
{
	uint32_t state;
	explicit Noise(uint32_t seed) : state(seed) {}
	float next()
	{
		state = state * 1664525u + 1013904223u;
		return (float)((state >> 8) & 0xFFFF) / 32768.f - 1.f;
	}
};

// Noise between two frequencies: one low-pass less another.
struct Band
{
	float high, low, a, b;
	Band(double from, double to) : high(0), low(0), a((float)(1.0 - std::exp(-TwoPi * to / Rate))),
			b((float)(1.0 - std::exp(-TwoPi * from / Rate))) {}
	float pass(float x)
	{
		high += a * (x - high);
		low += b * (high - low);
		return high - low;
	}
};

// A drop of water: a short sine whose pitch climbs.
void drop(Track& track, double at, double from, double to, double length, float level, float pan)
{
	const size_t count = (size_t)(length * Rate);
	double phase = 0;
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate, u = t / length;
		phase += TwoPi * (from * std::pow(to / from, u)) / Rate;
		const double envelope = (1.0 - std::exp(-t / 0.002)) * std::exp(-t / (length * 0.30));
		track.add(at, (float)(std::sin(phase) * envelope) * level, i, pan);
	}
}

// Water thrown up: a burst of noise.
void splash(Track& track, double at, double length, float level, float pan, uint32_t seed)
{
	Noise noise(seed);
	Band band(900, 5200);
	const size_t count = (size_t)(length * Rate);
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		const double envelope = (1.0 - std::exp(-t / 0.006)) * std::exp(-t / (length * 0.28));
		track.add(at, band.pass(noise.next()) * (float)envelope * level, i, pan);
	}
}

// Something heavy meeting something else: a low sine that falls and dies.
void thump(Track& track, double at, double from, double to, double length, float level)
{
	const size_t count = (size_t)(length * Rate);
	double phase = 0;
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate, u = t / length;
		phase += TwoPi * (from + (to - from) * u) / Rate;
		const double envelope = (1.0 - std::exp(-t / 0.0015)) * std::exp(-t / (length * 0.25));
		// A second harmonic, so small loudspeakers have something to play.
		track.add(at, (float)((std::sin(phase) + 0.45 * std::sin(phase * 2) + 0.2 * std::sin(phase * 3)) * envelope)
				* level, i, 0);
	}
}

// A latch: a few milliseconds of bright noise.
void click(Track& track, double at, float level, uint32_t seed)
{
	Noise noise(seed);
	Band band(1800, 7000);
	const size_t count = (size_t)(0.030 * Rate);
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		track.add(at, band.pass(noise.next()) * (float)std::exp(-t / 0.005) * level, i, 0);
	}
}

// The lift's motor: a hum whose pitch rises as it gathers speed and falls as
// it slows, with the air it moves.
void motor(Track& track, double at, double length, float level)
{
	Noise noise(77);
	Band air(250, 1400);
	const size_t count = (size_t)(length * Rate);
	double phase = 0;
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate, u = t / length;
		// Its speed: nothing, all of it, nothing.
		const double speed = std::sin(3.14159265 * std::min(u * 1.15, 1.0));
		phase += TwoPi * (96.0 + 62.0 * speed) / Rate;
		const double envelope = std::min(t / 0.12, 1.0) * std::min((length - t) / 0.22, 1.0);
		const double tremble = 1.0 + 0.07 * std::sin(TwoPi * 13.0 * t);
		const double hum = std::sin(phase) * 0.50 + std::sin(phase * 2) * 0.34 + std::sin(phase * 3) * 0.20
				+ std::sin(phase * 4) * 0.12 + std::sin(phase * 5) * 0.06;
		const double wind = air.pass(noise.next()) * 0.35 * speed;
		track.add(at, (float)((hum * tremble + wind) * envelope) * level, i, 0);
	}
}

// The lift's bell: a struck note and the overtones a small bell has.
void bell(Track& track, double at, double pitch, float level)
{
	static const double ratio[5] = { 1.0, 2.0, 3.01, 4.17, 5.43 };
	static const double weight[5] = { 1.0, 0.42, 0.24, 0.13, 0.07 };
	static const double ring[5] = { 0.55, 0.36, 0.22, 0.12, 0.07 };		// seconds to fall to a third
	const size_t count = (size_t)(1.9 * Rate);
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		double sum = 0;
		for (int k = 0; k < 5; k++)
			sum += weight[k] * std::sin(TwoPi * pitch * ratio[k] * t) * std::exp(-t / ring[k]);
		const double attack = 1.0 - std::exp(-t / 0.0012);
		const double tail = std::min((1.9 - t) / 0.2, 1.0);
		track.add(at, (float)(sum * attack * tail) * level, i, 0);
	}
}

// Doors sliding: a hush that brightens, and the stop at the end.
void doors(Track& track, double at, double length, float level)
{
	Noise noise(4242);
	Band band(500, 3200);
	const size_t count = (size_t)(length * Rate);
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		const double envelope = std::min(t / 0.05, 1.0) * std::min((length - t) / 0.06, 1.0)
				* (0.6 + 0.4 * t / length);
		track.add(at, band.pass(noise.next()) * (float)envelope * level, i, 0);
	}
	click(track, at + length - 0.01, level * 1.4f, 99);
}

// One beat of the wings: air pushed, soft and low.
void wingbeat(Track& track, double at, float level, float pan, uint32_t seed)
{
	Noise noise(seed);
	Band band(220, 1500);
	const size_t count = (size_t)(0.24 * Rate);
	for (size_t i = 0; i < count; i++)
	{
		const double t = (double)i / Rate;
		const double envelope = std::pow(std::min(t / 0.045, 1.0), 2.0) * std::exp(-std::max(t - 0.045, 0.0) / 0.055);
		track.add(at, band.pass(noise.next()) * (float)envelope * level, i, pan);
	}
}

std::shared_ptr<const std::vector<int16_t>> finished(const Track& track)
{
	auto out = std::make_shared<std::vector<int16_t>>(track.left.size() * 2);
	for (size_t i = 0; i < track.left.size(); i++)
	{
		// Loud moments are rounded off, not cut.
		(*out)[i * 2] = (int16_t)(std::tanh(track.left[i]) * 32000.f);
		(*out)[i * 2 + 1] = (int16_t)(std::tanh(track.right[i]) * 32000.f);
	}
	return out;
}

std::shared_ptr<const std::vector<int16_t>> sounds[Count];

// Where the head is on its way along the bottom, as the picture has it:
// 1 at the right of the screen, 0 in the middle.
float headPan(double time)
{
	double x = std::clamp((time - splashtime::Swim) / (splashtime::Arrive - splashtime::Swim), 0.0, 1.0);
	x = x * x * x * (x * (x * 6 - 15) + 10);
	return (float)(0.8 * (1.0 - x));
}

void makeSplash()
{
	namespace st = splashtime;
	Track track(st::Ding + 2.0);
	// The head breaks the water in the corner.
	splash(track, st::Rise + 0.04, 0.42, 0.50f, 0.8f, 11);
	drop(track, st::Rise + 0.16, 620, 1250, 0.07, 0.15f, 0.7f);
	drop(track, st::Rise + 0.27, 760, 1500, 0.06, 0.11f, 0.9f);
	// It paddles along: drops, each a little different, coming to the middle.
	static const double pitches[6] = { 540, 690, 600, 780, 560, 720 };
	int stroke = 0;
	for (double at = st::Swim + 0.06; at < st::Arrive - 0.10; at += 0.235, stroke++)
	{
		const double pitch = pitches[stroke % 6];
		drop(track, at, pitch, pitch * 2.1, 0.075, 0.21f, headPan(at));
		splash(track, at + 0.01, 0.14, 0.11f, headPan(at), 100 + (uint32_t)stroke);
	}
	// It dives: a falling note, the water closing, bubbles after it.
	{
		const size_t count = (size_t)(0.34 * Rate);
		double phase = 0;
		for (size_t i = 0; i < count; i++)
		{
			const double t = (double)i / Rate, u = t / 0.34;
			phase += TwoPi * (560.0 * std::pow(150.0 / 560.0, u)) / Rate;
			const double envelope = std::min(t / 0.012, 1.0) * std::pow(1.0 - u, 1.4);
			track.add(st::Dive + 0.05, (float)(std::sin(phase) * envelope) * 0.24f, i, 0);
		}
		splash(track, st::Dive + 0.16, 0.36, 0.38f, 0, 23);
		for (int i = 0; i < 4; i++)
			drop(track, st::Dive + 0.30 + 0.055 * i, 420 + 90 * i, 900 + 160 * i, 0.05, 0.11f - 0.018f * (float)i,
					i % 2 ? 0.15f : -0.15f);
	}
	// The lift: its doors shut, it travels, it stops, its bell, its doors open.
	thump(track, st::Clunk, 150, 70, 0.20, 0.42f);
	click(track, st::Clunk + 0.004, 0.16f, 5);
	motor(track, st::LiftStart, st::LiftStop - st::LiftStart + 0.05, 0.17f);
	thump(track, st::LiftStop - 0.01, 120, 62, 0.16, 0.26f);
	bell(track, st::Ding, 1046.5, 0.26f);
	doors(track, st::DoorsOpen, st::DoorsDone - st::DoorsOpen, 0.10f);
	sounds[Splash] = finished(track);
}

void makeFlight()
{
	Track track(splashtime::Flight);
	// Four beats on the way, quieter as it gets smaller, towards the left
	// where its corner is.
	for (int i = 0; i < 4; i++)
		wingbeat(track, 0.05 + 0.31 * i, 0.62f - 0.11f * (float)i, -0.22f * (float)i, 300 + (uint32_t)i);
	sounds[Flight] = finished(track);
}

void makeChime()
{
	Track track(2.0);
	bell(track, 0.02, 1046.5, 0.24f);
	sounds[Chime] = finished(track);
}

// The menus' sounds are the water's: small drops, kept short and quiet so a
// held D-pad is a patter and not a noise.
void makeMenu()
{
	{
		// The cursor moved: the smallest drop.
		Track track(0.07);
		drop(track, 0.002, 880, 1180, 0.040, 0.090f, 0);
		sounds[Move] = finished(track);
	}
	{
		// Chosen: a fuller drop that climbs, with a little of its splash.
		Track track(0.20);
		drop(track, 0.002, 620, 1320, 0.085, 0.18f, 0);
		drop(track, 0.060, 930, 1650, 0.060, 0.07f, 0);
		splash(track, 0.010, 0.10, 0.04f, 0, 61);
		sounds[Select] = finished(track);
	}
	{
		// A step back: the drop the other way, falling.
		Track track(0.18);
		drop(track, 0.002, 900, 470, 0.095, 0.16f, 0);
		sounds[Back] = finished(track);
	}
	{
		// Another tab: two drops, the second to the side the first was not.
		Track track(0.18);
		drop(track, 0.002, 700, 980, 0.050, 0.12f, -0.35f);
		drop(track, 0.055, 880, 1240, 0.050, 0.12f, 0.35f);
		sounds[Tab] = finished(track);
	}
	{
		// A letter typed: a dry tick with a drop under it.
		Track track(0.09);
		click(track, 0.002, 0.07f, 71);
		drop(track, 0.004, 760, 1010, 0.035, 0.10f, 0);
		sounds[Key] = finished(track);
	}
}

#if defined(SWANSTATION_HOST)
// A test run can ask for the sounds as files, to look at and listen to.
void writeWav(const std::string& path, const std::vector<int16_t>& frames)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
		return;
	const uint32_t bytes = (uint32_t)(frames.size() * 2), rate = 48000, byteRate = rate * 4, riff = 36 + bytes, sixteen = 16;
	const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f);
	fwrite(&pcm, 2, 1, f);
	fwrite(&channels, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&byteRate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&bytes, 4, 1, f);
	fwrite(frames.data(), 2, frames.size(), f);
	fclose(f);
}
#endif

}

void init()
{
	makeSplash();
	makeFlight();
	makeChime();
	makeMenu();
#if defined(SWANSTATION_HOST)
	if (const char *dir = getenv("SWANSTATION_SOUND_DUMP"))
	{
		static const char *names[Count] = { "splash", "flight", "chime", "move", "select", "back", "tab", "key" };
		for (int i = 0; i < Count; i++)
			writeWav(std::string(dir) + "/" + names[i] + ".wav", *sounds[i]);
	}
#endif
}

void play(Id id)
{
	if (id < 0 || id >= Count || !(id <= Chime ? options::frontend().splashSound : options::frontend().uiSounds))
		return;
	audio::playSound(sounds[id]);
}

void stop()
{
	audio::stopSounds();
}

}
