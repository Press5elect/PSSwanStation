/*
	PSSwanStation - the interface's sounds.

	SPDX-License-Identifier: GPL-3.0-or-later

	The start-up animation is heard as well as seen: water as the swan's head
	comes up and paddles along the bottom of the screen, a dive, the lift's
	doors, its motor on the way up, its bell, the doors opening, and wings.

	The title's own sounds are not recordings. Each is a few sine waves and
	some filtered noise, computed when the title starts into 48 kHz stereo and
	handed to audio.cpp to mix. The animation's sound is one piece laid out on
	the animation's own times (splashtime, fe.h), so the two cannot drift apart
	event by event.

	The menus' sounds can instead be one of the interface kit's two recorded
	sets (PS5_VKHomebrewUI's assets/audio/sfx, GPL-3.0-or-later, laid into
	<title folder>assets/hui/sfx by the build): Glass, soft tuned chimes, and
	Paper, warm and wooden. They are read on a thread of their own when the
	title starts; a sound one set lacks is the other's, and the title's own
	until they are read.
*/
#include "fe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <thread>

namespace fe::sound
{
#if defined(SWANSTATION_HOST)
std::shared_ptr<const std::vector<int16_t>> ownMusicForTest();
#endif
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

// ------------------------------------------------------ the kit's recordings

using Frames = std::shared_ptr<const std::vector<int16_t>>;

// Each cue's takes, by the cue's name ("focus" for focus_01.wav, focus_02.wav).
struct Bank
{
	std::map<std::string, std::vector<Frames>> cues;
	std::map<std::string, unsigned> turn;
};
Bank banks[2];		// Glass, Paper
std::mutex bankMutex;
bool banksAsked = false;

uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The kit's recordings: 48 kHz, 16-bit, one or two channels.
Frames readWav(const std::string& path)
{
	std::vector<uint8_t> data;
	if (!readFile(path, data) || data.size() < 44 || memcmp(data.data(), "RIFF", 4) != 0
			|| memcmp(data.data() + 8, "WAVE", 4) != 0)
		return nullptr;
	int channels = 0, bits = 0;
	size_t at = 12;
	while (at + 8 <= data.size())
	{
		const uint32_t size = le32(data.data() + at + 4);
		const uint8_t *body = data.data() + at + 8;
		const size_t room = data.size() - at - 8;
		if (memcmp(data.data() + at, "fmt ", 4) == 0 && room >= 16)
		{
			channels = body[2] | (body[3] << 8);
			bits = body[14] | (body[15] << 8);
		}
		else if (memcmp(data.data() + at, "data", 4) == 0)
		{
			if (bits != 16 || channels < 1 || channels > 2)
				return nullptr;
			const size_t frames = std::min((size_t)size, room) / (size_t)(2 * channels);
			auto out = std::make_shared<std::vector<int16_t>>(frames * 2);
			for (size_t i = 0; i < frames; i++)
				for (int side = 0; side < 2; side++)
				{
					const uint8_t *s = body + (i * (size_t)channels + (size_t)(channels > 1 ? side : 0)) * 2;
					(*out)[i * 2 + (size_t)side] = (int16_t)(s[0] | (s[1] << 8));
				}
			return out;
		}
		at += 8 + (size_t)size + (size & 1);
	}
	return nullptr;
}

void loadBanks()
{
	static const char *const names[2] = { "glass", "paper" };
	for (int set = 0; set < 2; set++)
	{
		const std::string dir = appDir + "assets/hui/sfx/" + names[set] + "/";
		Bank bank;
		int count = 0;
		if (DIR *list = opendir(dir.c_str()))
		{
			while (const dirent *entry = readdir(list))
			{
				const std::string name = entry->d_name;
				if (extension(name) != ".wav")
					continue;
				// focus_01.wav: the cue is what comes before the last underscore.
				const size_t cut = name.rfind('_');
				if (cut == std::string::npos)
					continue;
				if (Frames frames = readWav(dir + name))
				{
					bank.cues[name.substr(0, cut)].push_back(frames);
					count++;
				}
			}
			closedir(list);
		}
		diag::mark("sound: the kit's %s set: %d recordings", names[set], count);
		std::lock_guard<std::mutex> lock(bankMutex);
		banks[set] = std::move(bank);
	}
}

// What each of the menus' sounds is called in the kit's sets.
const char *cueOf(Id id)
{
	switch (id)
	{
	case Move: return "focus";
	case Select: return "select";
	case Back: return "back";
	case Tab: return "tab";
	case Key: return "type";
	case Unlock: return "notify";
	case Open: return "modal_open";
	case Close: return "modal_close";
	case Toggle: return "toggle";
	case Refuse: return "error";
	case Launch: return "launch";
	case FavouriteOn: return "favorite_on";
	case FavouriteOff: return "favorite_off";
	case Saved: return "saved";
	default: return nullptr;
	}
}

// The next take of a cue from set (0 Glass, 1 Paper), or the other set's.
Frames kitSound(int set, Id id)
{
	const char *cue = cueOf(id);
	if (cue == nullptr)
		return nullptr;
	std::lock_guard<std::mutex> lock(bankMutex);
	for (int which : { set, 1 - set })
	{
		Bank& bank = banks[which];
		const auto found = bank.cues.find(cue);
		if (found != bank.cues.end() && !found->second.empty())
			return found->second[bank.turn[cue]++ % found->second.size()];
	}
	return nullptr;
}

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

// An achievement: the bell three times, climbing.
void makeUnlock()
{
	Track track(2.6);
	bell(track, 0.02, 783.99, 0.17f);
	bell(track, 0.16, 987.77, 0.17f);
	bell(track, 0.30, 1318.5, 0.20f);
	sounds[Unlock] = finished(track);
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
	{
		// A page opened: a drop that climbs, and another above it.
		Track track(0.22);
		drop(track, 0.002, 560, 980, 0.080, 0.15f, -0.15f);
		drop(track, 0.070, 840, 1460, 0.070, 0.10f, 0.15f);
		sounds[Open] = finished(track);
	}
	{
		// Closed: the same, falling.
		Track track(0.22);
		drop(track, 0.002, 1100, 760, 0.080, 0.13f, 0.15f);
		drop(track, 0.070, 820, 520, 0.080, 0.10f, -0.15f);
		sounds[Close] = finished(track);
	}
	{
		// A switch: a tick and a short drop.
		Track track(0.10);
		click(track, 0.002, 0.06f, 83);
		drop(track, 0.006, 980, 1240, 0.030, 0.09f, 0);
		sounds[Toggle] = finished(track);
	}
	{
		// Refused: low and soft, so the end of a list does not scold.
		Track track(0.14);
		drop(track, 0.002, 330, 250, 0.090, 0.10f, 0);
		sounds[Refuse] = finished(track);
	}
	{
		// A favourite: two bright drops up, or down when taken away.
		Track up(0.20), down(0.20);
		drop(up, 0.002, 990, 1480, 0.050, 0.11f, -0.2f);
		drop(up, 0.060, 1320, 1980, 0.060, 0.09f, 0.2f);
		drop(down, 0.002, 1480, 1100, 0.050, 0.10f, 0.2f);
		drop(down, 0.060, 1100, 760, 0.060, 0.08f, -0.2f);
		sounds[FavouriteOn] = finished(up);
		sounds[FavouriteOff] = finished(down);
	}
	{
		// Saved: a soft tick under a drop.
		Track track(0.14);
		click(track, 0.002, 0.04f, 97);
		drop(track, 0.010, 700, 1050, 0.070, 0.10f, 0);
		sounds[Saved] = finished(track);
	}
	// A game starting rings the lift's bell.
	sounds[Launch] = sounds[Chime];
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
	makeUnlock();
	makeMenu();
#if defined(SWANSTATION_HOST)
	if (const char *dir = getenv("SWANSTATION_SOUND_DUMP"))
	{
		static const char *names[Count] = { "splash", "flight", "chime", "move", "select", "back", "tab", "key", "unlock",
			"open", "close", "toggle", "refuse", "launch", "favourite-on", "favourite-off", "saved" };
		for (int i = 0; i < Count; i++)
			writeWav(std::string(dir) + "/" + names[i] + ".wav", *sounds[i]);
		writeWav(std::string(dir) + "/music.wav", *ownMusicForTest());
	}
#endif
}

void play(Id id)
{
	const options::Frontend& settings = options::frontend();
	if (id < 0 || id >= Count || !(id <= Chime ? settings.splashSound : settings.uiSounds))
		return;
	if (id <= Chime)
	{
		audio::playSound(sounds[id]);
		return;
	}
	if (!banksAsked)
	{
		banksAsked = true;
		std::thread(loadBanks).detach();
	}
	// The set chosen, or the theme's (the title's own theme: its own sounds).
	const int set = settings.soundSet != 0 ? settings.soundSet : ui::themeSounds();
	Frames frames = set >= 2 ? kitSound(set - 2, id) : nullptr;
	// The kit's takes are recorded quieter than the title's own: lifted a little.
	audio::playSound(frames ? frames : sounds[id], frames ? settings.soundVolume * 3 / 2 : settings.soundVolume);
}

std::vector<std::string> setNames()
{
	return { "As the theme has it", "PSSwanStation", "Glass", "Paper" };
}

std::vector<std::string> musicNames()
{
	return { "None", "The title's own", "My music folder", "The interface kit's songs" };
}

void stop()
{
	audio::stopSounds();
}

void unlock()
{
	play(Unlock);
}

}
