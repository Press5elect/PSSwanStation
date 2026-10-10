/*
	PSSwanStation - the sound: the emulator's 44.1 kHz to the output's 48.

	SPDX-License-Identifier: GPL-3.0-or-later

	The emulator runs at the display's pace and the output port at its own
	48 kHz clock, and the two drift apart. push() resamples linearly into a
	ring the output thread drains, at a ratio nudged by at most 0.5 %,
	inaudibly, to keep the ring about a third full (dynamic rate control, as
	RetroArch and PSFlyCast do). When the game keeps its own exact speed (the
	pacing setting) the nudging is a fifth of that, only against the two
	clocks' slow drift. A full ring drops what does not fit instead of holding
	the emulator; a dry one plays silence.

	The interface's own sounds (sound.cpp) are 48 kHz already: they are mixed
	into what goes out, game or no game. So is the menus' music, one piece
	played round and round, which comes and goes with a fade.
*/
#include "fe.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace fe::audio
{
namespace
{
constexpr double InRate = 44100.0, OutRate = 48000.0;
constexpr size_t Capacity = 9600;		// frames: 200 ms
constexpr size_t Target = 2880;			// 60 ms
constexpr double MaxRateDelta = 0.005;

std::mutex mutex;
std::vector<int16_t> ring(Capacity * 2);
size_t readPos, writePos, filled;
double phase;
int16_t lastL, lastR;
std::atomic<bool> isPaused{true};
std::atomic<int> volume{100};
std::atomic<unsigned> dry{0};
bool primed;
bool open;

// The interface's sounds being played.
struct Voice
{
	std::shared_ptr<const std::vector<int16_t>> frames;	// stereo
	size_t at = 0;
	int fading = -1;		// frames of a fade-out left, or -1
	int gain = 100;			// percent, under the volume
};
constexpr int FadeFrames = 1920;		// 40 ms
std::vector<Voice> voices;

// The menus' music: what plays, and what should.
std::shared_ptr<const std::vector<int16_t>> musicNow, musicWanted;
size_t musicAt;
float musicGain;						// 0..1, moving towards 1 or 0
std::atomic<int> musicVolume{60};
std::atomic<unsigned> musicLaps{0};
std::atomic<bool> muted{false};
constexpr float MusicFade = 1.f / (48000.f * 1.2f);		// per frame: 1.2 s from silence to full

// With the lock held.
void mixMusic(int16_t *out, size_t frames)
{
	if (!musicNow && !musicWanted)
		return;
	const float level = (float)volume * (float)musicVolume / 10000.f;
	for (size_t i = 0; i < frames; i++)
	{
		if (musicNow != musicWanted)
		{
			// The piece changes, or ends: this one goes out first.
			musicGain -= MusicFade * 2.f;
			if (musicGain <= 0 || !musicNow)
			{
				musicNow = musicWanted;
				musicAt = 0;
				musicGain = 0;
			}
		}
		else if (musicGain < 1.f)
			musicGain = std::min(musicGain + MusicFade, 1.f);
		if (!musicNow)
			return;
		const std::vector<int16_t>& data = *musicNow;
		const size_t total = data.size() / 2;
		if (total == 0)
			return;
		const float gain = musicGain * level;
		out[i * 2] = (int16_t)std::clamp((int)(out[i * 2] + data[musicAt * 2] * gain), -32768, 32767);
		out[i * 2 + 1] = (int16_t)std::clamp((int)(out[i * 2 + 1] + data[musicAt * 2 + 1] * gain), -32768, 32767);
		if (++musicAt >= total)
		{
			// Round again: a playlist (sound.cpp) moves to its next piece now.
			musicAt = 0;
			musicLaps++;
		}
	}
}

// With the lock held.
void mixVoices(int16_t *out, size_t frames)
{
	if (voices.empty())
		return;
	for (Voice& voice : voices)
	{
		const int gain = volume * voice.gain / 100;
		const std::vector<int16_t>& data = *voice.frames;
		const size_t total = data.size() / 2;
		for (size_t i = 0; i < frames && voice.at < total; i++, voice.at++)
		{
			int l = data[voice.at * 2] * gain / 100, r = data[voice.at * 2 + 1] * gain / 100;
			if (voice.fading >= 0)
			{
				l = l * voice.fading / FadeFrames;
				r = r * voice.fading / FadeFrames;
				if (voice.fading > 0)
					voice.fading--;
			}
			out[i * 2] = (int16_t)std::clamp(out[i * 2] + l, -32768, 32767);
			out[i * 2 + 1] = (int16_t)std::clamp(out[i * 2 + 1] + r, -32768, 32767);
		}
		if (voice.fading == 0)
			voice.at = total;
	}
	voices.erase(std::remove_if(voices.begin(), voices.end(),
			[](const Voice& voice) { return voice.at >= voice.frames->size() / 2; }), voices.end());
}
}

void init()
{
	clear();
	open = platform::audioOpen();
	diag::mark("audio: output %s", open ? "open" : "not available");
}

void shutdown()
{
	if (open)
		platform::audioClose();
	open = false;
}

void clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	readPos = writePos = filled = 0;
	phase = 0;
	lastL = lastR = 0;
	primed = false;
}

void setPaused(bool paused)
{
	isPaused = paused;
}

void setVolume(int percent)
{
	volume = std::clamp(percent, 0, 100);
}

float fill()
{
	std::lock_guard<std::mutex> lock(mutex);
	return (float)filled / Capacity;
}

unsigned underruns()
{
	return dry;
}

void push(const int16_t *samples, size_t frames)
{
	if (muted)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	double deviation = ((double)filled - (double)Target) / (double)Target;
	deviation = std::clamp(deviation, -1.0, 1.0);
	if (options::frontend().pacing != 0)
		deviation *= 0.2;
	// More than the target in the ring: consume the input a little faster.
	const double step = InRate / OutRate * (1.0 + MaxRateDelta * deviation);
	for (size_t i = 0; i < frames; i++)
	{
		const int16_t l = samples[i * 2], r = samples[i * 2 + 1];
		while (phase < 1.0)
		{
			if (filled >= Capacity)
				break;		// running fast: this output frame is dropped
			ring[writePos * 2] = (int16_t)(lastL + (l - lastL) * phase);
			ring[writePos * 2 + 1] = (int16_t)(lastR + (r - lastR) * phase);
			writePos = (writePos + 1) % Capacity;
			filled++;
			phase += step;
		}
		if (phase >= 1.0)
			phase -= 1.0;
		else
			phase = 0;
		lastL = l;
		lastR = r;
	}
}

void playSound(std::shared_ptr<const std::vector<int16_t>> frames, int gain)
{
	if (!frames || frames->size() < 2 || gain <= 0)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (voices.size() >= 12)
		return;
	Voice voice;
	voice.frames = std::move(frames);
	voice.gain = std::clamp(gain, 0, 200);
	voices.push_back(std::move(voice));
}

void setMusic(std::shared_ptr<const std::vector<int16_t>> frames)
{
	std::lock_guard<std::mutex> lock(mutex);
	musicWanted = std::move(frames);
}

void setMusicVolume(int percent)
{
	musicVolume = std::clamp(percent, 0, 100);
}

unsigned musicRounds()
{
	return musicLaps;
}

void setMuted(bool mute)
{
	if (muted.exchange(mute) != mute)
		clear();
}

void stopSounds()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (Voice& voice : voices)
		if (voice.fading < 0)
			voice.fading = FadeFrames;
}

namespace
{
// The game's part of a grain, with the lock held.
void renderGame(int16_t *out, size_t frames)
{
	if (isPaused)
	{
		memset(out, 0, frames * 4);
		return;
	}
	// After a start or an underrun, wait for the ring to reach its target
	// once, so the output does not chase an empty ring grain by grain.
	if (!primed)
	{
		if (filled < Target)
		{
			memset(out, 0, frames * 4);
			return;
		}
		primed = true;
	}
	const size_t available = std::min(filled, frames);
	const int gain = volume;
	for (size_t i = 0; i < available; i++)
	{
		int l = ring[readPos * 2], r = ring[readPos * 2 + 1];
		if (gain != 100)
		{
			l = l * gain / 100;
			r = r * gain / 100;
		}
		out[i * 2] = (int16_t)l;
		out[i * 2 + 1] = (int16_t)r;
		readPos = (readPos + 1) % Capacity;
	}
	filled -= available;
	if (available < frames)
	{
		memset(out + available * 2, 0, (frames - available) * 4);
		dry++;
		primed = false;
	}
}
}

void render(int16_t *out, size_t frames)
{
	std::lock_guard<std::mutex> lock(mutex);
	renderGame(out, frames);
	mixMusic(out, frames);
	mixVoices(out, frames);
}

}
