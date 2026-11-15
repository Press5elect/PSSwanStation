// PSSwanStation - the Sinden Lightgun tracker (src/sinden_track.cpp) on the PC,
// against camera frames drawn here with a known aim.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene: a 55-inch television on a wall, its display showing a game's
// picture (made-up textures: shapes, gradients, noise, bright and dark areas)
// with the title's plain border round it, a dark bezel, and a room (dark or
// lit, with furniture, a lamp, a window). The camera: a pinhole at a known
// place, pointed at a known point of the picture and turned about its axis,
// with barrel distortion (r' = r (1 + k1 r^2), r in focal lengths: a different
// model from the tracker's own), 3x3 samples a pixel, lens blur, motion blur,
// vignetting, a backlight's banding, exposure with clipping, sensor noise and
// the camera's JPEG compression (MJPEG), 320 x 240.
//
// For each condition it reports how often the screen was found, the aim's
// error against the truth in per cent of the picture's width (median, 95th
// percentile, worst), how often an aim off the picture was called so, and the
// time a frame takes; then the edge cases, the Tracker over sequences (the
// smoothing's steadiness and lag, the lens's distortion learnt), and fails on
// numbers worse than the limits written below. With SINDEN_OUT=<folder> it
// writes some frames there as PGM, and the same with what was found drawn on
// it as PPM (green: the picture's corners found; red: the aim found; blue: the
// true aim).
//
// test [quick]: quick runs fewer frames (for the sanitizer build).
#include "../../src/sinden_track.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace fe::sinden::track;

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr int W = 320, H = 240;
int failures = 0;
bool quick = false;
std::string outDir;

#define CHECK(cond, ...)                                     \
	do                                                       \
	{                                                        \
		if (!(cond))                                         \
		{                                                    \
			std::printf("FAILED (line %d): ", __LINE__);    \
			std::printf(__VA_ARGS__);                        \
			std::printf("\n");                               \
			failures++;                                      \
		}                                                    \
	} while (0)

struct Rng
{
	std::mt19937_64 g;
	explicit Rng(uint64_t seed) : g(seed) {}
	double uni(double a, double b) { return std::uniform_real_distribution<double>(a, b)(g); }
	double normal() { return std::normal_distribution<double>(0, 1)(g); }
	int below(int n) { return (int)(g() % (uint64_t)n); }
	bool chance(double p) { return uni(0, 1) < p; }
};

struct V3
{
	double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 unit(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }

// A picture of levels 0..255, sampled nearest (the game's pixels are blocks on
// the display) or bilinear.
struct Texture
{
	int w = 0, h = 0;
	std::vector<float> v;
	void make(int width, int height, float level)
	{
		w = width;
		h = height;
		v.assign((size_t)w * h, level);
	}
	float nearest(double u, double t) const
	{
		const int x = std::clamp((int)(u * w), 0, w - 1), y = std::clamp((int)(t * h), 0, h - 1);
		return v[(size_t)y * w + x];
	}
	float bilinear(double u, double t) const
	{
		const double x = std::clamp(u * w - 0.5, 0.0, w - 1.001), y = std::clamp(t * h - 0.5, 0.0, h - 1.001);
		const int x0 = (int)x, y0 = (int)y;
		const double fx = x - x0, fy = y - y0;
		const float* r0 = &v[(size_t)y0 * w + x0];
		const float* r1 = r0 + w;
		return (float)((r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy);
	}
	void rect(double x0, double y0, double x1, double y1, float level)
	{
		for (int y = std::max(0, (int)y0); y < std::min(h, (int)y1); y++)
			for (int x = std::max(0, (int)x0); x < std::min(w, (int)x1); x++)
				v[(size_t)y * w + x] = level;
	}
	void ellipse(double cx, double cy, double rx, double ry, float level)
	{
		for (int y = std::max(0, (int)(cy - ry)); y < std::min(h, (int)(cy + ry) + 1); y++)
			for (int x = std::max(0, (int)(cx - rx)); x < std::min(w, (int)(cx + rx) + 1); x++)
			{
				const double dx = (x - cx) / rx, dy = (y - cy) / ry;
				if (dx * dx + dy * dy <= 1)
					v[(size_t)y * w + x] = level;
			}
	}
};

enum class Game
{
	Mixed,		// shapes of every level, some bright ones on the picture's edges
	Bright,		// mostly bright, white areas along the edges
	Dark,		// a dark scene
	White,		// all white
	Black,		// all black
	WhiteBar,	// a white bar along the whole top and the whole left side
};

void makeGame(Texture& t, Game kind, Rng& rng)
{
	const int gw = 320, gh = 240;	// the PS1's picture
	switch (kind)
	{
	case Game::White:
		t.make(gw, gh, 255);
		return;
	case Game::Black:
		t.make(gw, gh, 0);
		return;
	default:
		break;
	}
	const bool bright = kind == Game::Bright, dark = kind == Game::Dark;
	t.make(gw, gh, (float)(bright ? rng.uni(170, 255) : dark ? rng.uni(0, 25) : rng.uni(0, 200)));
	// A gradient over the background (a sky).
	const double g0 = rng.uni(-60, 60);
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++)
		{
			float& p = t.v[(size_t)y * gw + x];
			p = (float)std::clamp(p + g0 * y / gh, 0.0, 255.0);
		}
	const int objects = 12 + rng.below(25);
	for (int i = 0; i < objects; i++)
	{
		float level = (float)(dark ? rng.uni(0, 70) : bright ? rng.uni(60, 255) : rng.uni(0, 255));
		if (!dark && rng.chance(0.2))
			level = 255;
		const double cx = rng.uni(-20, gw + 20), cy = rng.uni(-20, gh + 20);
		const double sx = rng.uni(4, 90), sy = rng.uni(4, 70);
		switch (rng.below(4))
		{
		case 0:
			t.rect(cx - sx, cy - sy, cx + sx, cy + sy, level);
			break;
		case 1:
			t.ellipse(cx, cy, sx, sy, level);
			break;
		case 2:	// a patch of noise (foliage, water)
			for (int y = std::max(0, (int)(cy - sy)); y < std::min(gh, (int)(cy + sy)); y++)
				for (int x = std::max(0, (int)(cx - sx)); x < std::min(gw, (int)(cx + sx)); x++)
					t.v[(size_t)y * gw + x] = (float)std::clamp(level + rng.uni(-60, 60), 0.0, 255.0);
			break;
		default:	// thin lines (text, a frame)
			for (int k = 0; k < 6; k++)
				t.rect(cx, cy + k * 4, cx + sx * 2, cy + k * 4 + 1 + rng.below(2), level);
			break;
		}
	}
	// Bright areas against the picture's edge, where the border is.
	if (!dark)
		for (int i = 0; i < (bright ? 5 : 2); i++)
		{
			const double len = rng.uni(20, 120), depth = rng.uni(3, 40);
			const double at = rng.uni(0, 1);
			switch (rng.below(4))
			{
			case 0:
				t.rect(at * gw - len / 2, 0, at * gw + len / 2, depth, 255);
				break;
			case 1:
				t.rect(at * gw - len / 2, gh - depth, at * gw + len / 2, gh, 255);
				break;
			case 2:
				t.rect(0, at * gh - len / 2, depth, at * gh + len / 2, 255);
				break;
			default:
				t.rect(gw - depth, at * gh - len / 2, gw, at * gh + len / 2, 255);
				break;
			}
		}
	if (kind == Game::WhiteBar)
	{
		t.rect(0, 0, gw, 22, 255);
		t.rect(0, 0, 18, gh, 255);
	}
}

enum class Room
{
	Dark,	// a dim room
	Lit,	// a lit room: a pale wall, a window, a lamp
};

enum class Layout
{
	Pillarbox,	// a 4:3 picture in the middle with the border round it, black beside
	Full,		// the border at the display's edges round a 16:9 picture
};

struct Scene
{
	// The display's area, metres, centred on (0, 0), y down.
	double dispW = 1.218, dispH = 0.685, bezel = 0.012;
	// The picture inside the border, and the border's thickness.
	double picL = 0, picT = 0, picR = 0, picB = 0, border = 0;
	float borderLevel = 255, displayBlack = 3, bezelLevel = 12;
	Texture game, room;
	double roomW = 6, roomH = 4;

	double pictureWidth() const { return picR - picL; }
	double pictureHeight() const { return picB - picT; }
	float value(double x, double y) const
	{
		if (x >= picL && x < picR && y >= picT && y < picB)
			return game.nearest((x - picL) / (picR - picL), (y - picT) / (picB - picT));
		if (x >= picL - border && x < picR + border && y >= picT - border && y < picB + border)
			return borderLevel;
		if (std::fabs(x) <= dispW / 2 && std::fabs(y) <= dispH / 2)
			return displayBlack;
		if (std::fabs(x) <= dispW / 2 + bezel && std::fabs(y) <= dispH / 2 + bezel)
			return bezelLevel;
		return room.bilinear(x / roomW + 0.5, y / roomH + 0.5);
	}
};

void makeRoom(Scene& s, Room kind, Rng& rng)
{
	const int rw = 600, rh = 400;	// 1 cm a texel
	auto toX = [&](double m) { return (m / s.roomW + 0.5) * rw; };
	auto toY = [&](double m) { return (m / s.roomH + 0.5) * rh; };
	const bool lit = kind == Room::Lit;
	s.room.make(rw, rh, (float)(lit ? rng.uni(60, 130) : rng.uni(3, 18)));
	for (int y = 0; y < rh; y++)
		for (int x = 0; x < rw; x++)
		{
			float& p = s.room.v[(size_t)y * rw + x];
			p = (float)std::clamp(p * (1 + 0.3 * std::sin(x * 0.01 + y * 0.004)), 0.0, 255.0);
		}
	// Furniture and frames, not over the television.
	for (int i = 0; i < 14; i++)
	{
		const double cx = rng.uni(-3, 3), cy = rng.uni(-2, 2), sx = rng.uni(0.05, 0.6), sy = rng.uni(0.05, 0.6);
		if (std::fabs(cx) < s.dispW / 2 + sx + 0.05 && std::fabs(cy) < s.dispH / 2 + sy + 0.05)
			continue;
		s.room.rect(toX(cx - sx), toY(cy - sy), toX(cx + sx), toY(cy + sy), (float)(lit ? rng.uni(20, 200) : rng.uni(0, 45)));
	}
	// The stand under the television.
	s.room.rect(toX(-0.9), toY(s.dispH / 2 + s.bezel + 0.15), toX(0.9), toY(s.dispH / 2 + 0.6), lit ? 35 : 6);
	// A lamp beside it.
	const double lx = rng.chance(0.5) ? rng.uni(-1.6, -0.85) : rng.uni(0.85, 1.6), ly = rng.uni(-0.6, 0.2);
	for (int y = 0; y < rh; y++)
		for (int x = 0; x < rw; x++)
		{
			const double dx = (x + 0.5) / rw * s.roomW - s.roomW / 2 - lx, dy = (y + 0.5) / rh * s.roomH - s.roomH / 2 - ly;
			const double r = std::sqrt(dx * dx + dy * dy);
			float& p = s.room.v[(size_t)y * rw + x];
			p = (float)std::min(255.0, p + (r < 0.06 ? 255 : (lit ? 60 : 25) * std::exp(-r / 0.25)));
		}
	if (lit)
	{
		// A window to one side, as bright as the screen.
		const double wx = rng.chance(0.5) ? -1.9 : 1.3;
		s.room.rect(toX(wx), toY(-1.2), toX(wx + 0.6), toY(0.3), 250);
	}
}

struct Camera
{
	V3 c, xa, ya, za;	// position; right, down and forward
	double f = 256, cx = (W - 1) / 2.0, cy = (H - 1) / 2.0, k1 = 0;

	void aimAt(V3 from, V3 target, double rollDeg)
	{
		c = from;
		za = unit(target - from);
		const V3 down{ 0, 1, 0 };
		V3 x0 = unit(cross(down, za));
		V3 y0 = cross(za, x0);
		const double r = rollDeg * PI / 180;
		xa = x0 * std::cos(r) + y0 * std::sin(r);
		ya = x0 * -std::sin(r) + y0 * std::cos(r);
	}
	// Where a pixel looks, through the distortion (solved by Newton's method).
	bool look(double px, double py, double& wx, double& wy) const
	{
		const double mx = (px - cx) / f, my = (py - cy) / f;
		const double rd = std::sqrt(mx * mx + my * my);
		double ru = rd;
		for (int i = 0; i < 6 && rd > 0; i++)
			ru -= (ru + k1 * ru * ru * ru - rd) / (1 + 3 * k1 * ru * ru);
		const double s = rd > 0 ? ru / rd : 1;
		const V3 d = xa * (mx * s) + ya * (my * s) + za;
		if (d.z <= 1e-9)
			return false;
		const double t = -c.z / d.z;
		wx = c.x + t * d.x;
		wy = c.y + t * d.y;
		return true;
	}
	bool project(V3 p, double& px, double& py) const
	{
		const V3 q = p - c;
		const double z = dot(q, za);
		if (z <= 1e-9)
			return false;
		const double mx = dot(q, xa) / z, my = dot(q, ya) / z;
		const double r2 = mx * mx + my * my, s = 1 + k1 * r2;
		px = cx + f * mx * s;
		py = cy + f * my * s;
		return true;
	}
};

struct Look
{
	double blur = 0.8, motion = 0, motionAngle = 0, noise = 3, exposure = 1, vignette = 0.25, banding = 0, bandPeriod = 50,
		   bandPhase = 0;
	bool jpeg = true;
};

// The JPEG's luma table (quality 50); the camera's is taken as quality 75.
const int JPEG_LUMA[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17,
	22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99 };

void jpegLike(std::vector<float>& img)
{
	static double cosTable[8][8];
	static bool ready = false;
	if (!ready)
	{
		for (int x = 0; x < 8; x++)
			for (int u = 0; u < 8; u++)
				cosTable[x][u] = std::cos((2 * x + 1) * u * PI / 16) * (u == 0 ? std::sqrt(0.125) : 0.5);
		ready = true;
	}
	for (int by = 0; by < H; by += 8)
		for (int bx = 0; bx < W; bx += 8)
		{
			double block[8][8], tmp[8][8];
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					block[y][x] = std::clamp(img[(size_t)(by + y) * W + bx + x], 0.0f, 255.0f) - 128;
			for (int y = 0; y < 8; y++)
				for (int u = 0; u < 8; u++)
				{
					double s = 0;
					for (int x = 0; x < 8; x++)
						s += block[y][x] * cosTable[x][u];
					tmp[y][u] = s;
				}
			for (int v = 0; v < 8; v++)
				for (int u = 0; u < 8; u++)
				{
					double s = 0;
					for (int y = 0; y < 8; y++)
						s += tmp[y][u] * cosTable[y][v];
					const double q = std::max(1.0, std::floor(JPEG_LUMA[v * 8 + u] * 0.5 + 0.5));
					block[v][u] = std::round(s / q) * q;
				}
			for (int v = 0; v < 8; v++)
				for (int x = 0; x < 8; x++)
				{
					double s = 0;
					for (int u = 0; u < 8; u++)
						s += block[v][u] * cosTable[x][u];
					tmp[v][x] = s;
				}
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
				{
					double s = 0;
					for (int v = 0; v < 8; v++)
						s += tmp[v][x] * cosTable[y][v];
					img[(size_t)(by + y) * W + bx + x] = (float)(s + 128);
				}
		}
}

void gaussian(std::vector<float>& img, double sigma)
{
	if (sigma < 0.05)
		return;
	const int r = (int)std::ceil(3 * sigma);
	std::vector<double> k(2 * r + 1);
	double sum = 0;
	for (int i = -r; i <= r; i++)
		sum += k[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
	for (double& v : k)
		v /= sum;
	std::vector<float> tmp(img.size());
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			double s = 0;
			for (int i = -r; i <= r; i++)
				s += k[i + r] * img[(size_t)y * W + std::clamp(x + i, 0, W - 1)];
			tmp[(size_t)y * W + x] = (float)s;
		}
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			double s = 0;
			for (int i = -r; i <= r; i++)
				s += k[i + r] * tmp[(size_t)std::clamp(y + i, 0, H - 1) * W + x];
			img[(size_t)y * W + x] = (float)s;
		}
}

void motionBlur(std::vector<float>& img, double lengthPx, double angle)
{
	if (lengthPx < 0.5)
		return;
	const int taps = std::max(2, (int)std::ceil(lengthPx * 2));
	const double dx = std::cos(angle), dy = std::sin(angle);
	std::vector<float> out(img.size(), 0);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			double s = 0;
			for (int t = 0; t < taps; t++)
			{
				const double o = (t / (double)(taps - 1) - 0.5) * lengthPx;
				const double sx = std::clamp(x + o * dx, 0.0, W - 1.001), sy = std::clamp(y + o * dy, 0.0, H - 1.001);
				const int x0 = (int)sx, y0 = (int)sy;
				const double fx = sx - x0, fy = sy - y0;
				const float* r0 = &img[(size_t)y0 * W + x0];
				s += (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r0[W] * (1 - fx) + r0[W + 1] * fx) * fy;
			}
			out[(size_t)y * W + x] = (float)(s / taps);
		}
	img.swap(out);
}

// The camera's frame of the scene.
void render(const Scene& scene, const Camera& cam, const Look& look, Rng& rng, std::vector<uint8_t>& frame)
{
	std::vector<float> img((size_t)W * H);
	const int S = 3;
	auto rows = [&](int y0, int y1) {
		for (int y = y0; y < y1; y++)
			for (int x = 0; x < W; x++)
			{
				double sum = 0;
				for (int sy = 0; sy < S; sy++)
					for (int sx = 0; sx < S; sx++)
					{
						double wx, wy;
						const double px = x - 0.5 + (sx + 0.5) / S, py = y - 0.5 + (sy + 0.5) / S;
						sum += cam.look(px, py, wx, wy) ? scene.value(wx, wy) : 0;
					}
				img[(size_t)y * W + x] = (float)(sum / (S * S));
			}
	};
	const int threads = 4;
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; t++)
		pool.emplace_back(rows, H * t / threads, H * (t + 1) / threads);
	for (std::thread& t : pool)
		t.join();
	gaussian(img, look.blur);
	motionBlur(img, look.motion, look.motionAngle);
	const double halfDiag2 = (W * W + H * H) / 4.0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
		{
			const double dx = x - (W - 1) / 2.0, dy = y - (H - 1) / 2.0;
			double v = img[(size_t)y * W + x] * look.exposure * (1 - look.vignette * (dx * dx + dy * dy) / halfDiag2);
			v *= 1 + look.banding * std::sin(2 * PI * y / look.bandPeriod + look.bandPhase);
			v = std::min(v, 255.0) + 2;	// the sensor clips, and has a black level
			const double sigma = std::sqrt(look.noise * look.noise + 0.03 * std::max(v, 0.0));
			img[(size_t)y * W + x] = (float)(v + sigma * rng.normal());
		}
	if (look.jpeg)
		jpegLike(img);
	frame.resize((size_t)W * H);
	for (size_t i = 0; i < img.size(); i++)
		frame[i] = (uint8_t)std::clamp((int)std::lround(img[i]), 0, 255);
}

void writePgm(const std::string& name, const std::vector<uint8_t>& frame)
{
	if (outDir.empty())
		return;
	FILE* f = std::fopen((outDir + "/" + name + ".pgm").c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "P5\n%d %d\n255\n", W, H);
	std::fwrite(frame.data(), 1, frame.size(), f);
	std::fclose(f);
}

// The point (u, v) of the unit square through the projective map onto four
// corners (top-left, top-right, bottom-right, bottom-left), Heckbert's.
bool quadPoint(const Point q[4], double u, double v, double& x, double& y)
{
	const double sx = q[0].x - q[1].x + q[2].x - q[3].x, sy = q[0].y - q[1].y + q[2].y - q[3].y;
	const double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x, dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
	const double den = dx1 * dy2 - dx2 * dy1;
	if (std::fabs(den) < 1e-12)
		return false;
	const double g = (sx * dy2 - dx2 * sy) / den, h = (dx1 * sy - sx * dy1) / den;
	const double a = q[1].x - q[0].x + g * q[1].x, b = q[3].x - q[0].x + h * q[3].x, c = q[0].x;
	const double d = q[1].y - q[0].y + g * q[1].y, e = q[3].y - q[0].y + h * q[3].y, f = q[0].y;
	const double z = g * u + h * v + 1;
	if (std::fabs(z) < 1e-12)
		return false;
	x = (a * u + b * v + c) / z;
	y = (d * u + e * v + f) / z;
	return true;
}

// The frame with what was found drawn on it, twice the size.
void writeOverlay(const std::string& name, const std::vector<uint8_t>& frame, const Result& r, double tx, double ty, const Camera& cam,
		const Scene& scene)
{
	if (outDir.empty())
		return;
	const int Z = 2;
	std::vector<uint8_t> rgb((size_t)W * Z * H * Z * 3);
	for (int y = 0; y < H * Z; y++)
		for (int x = 0; x < W * Z; x++)
			for (int c = 0; c < 3; c++)
				rgb[((size_t)y * W * Z + x) * 3 + c] = frame[(size_t)(y / Z) * W + x / Z];
	auto plot = [&](double px, double py, const uint8_t colour[3]) {
		const int x = (int)std::lround(px * Z + (Z - 1) / 2.0), y = (int)std::lround(py * Z + (Z - 1) / 2.0);
		if (x < 0 || y < 0 || x >= W * Z || y >= H * Z)
			return;
		std::memcpy(&rgb[((size_t)y * W * Z + x) * 3], colour, 3);
	};
	auto line = [&](Point a, Point b, const uint8_t colour[3]) {
		const int steps = (int)(std::hypot(b.x - a.x, b.y - a.y) * Z * 2) + 1;
		for (int i = 0; i <= steps; i++)
			plot(a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps, colour);
	};
	auto cross = [&](double px, double py, const uint8_t colour[3]) {
		for (int i = -6; i <= 6; i++)
		{
			plot(px + i * 0.5, py, colour);
			plot(px, py + i * 0.5, colour);
		}
	};
	const uint8_t blue[3] = { 40, 90, 255 }, red[3] = { 255, 40, 40 }, green[3] = { 0, 230, 0 }, darkGreen[3] = { 0, 120, 0 };
	// Blue: where the gun points (the frame's middle: the true aim). Red: where
	// the corners found put the picture's point the tracker reported, through
	// the projective map of those corners (the lens left aside: near enough
	// for a look). Apart by the error.
	cross(cam.cx, cam.cy, blue);
	(void)scene;
	(void)tx;
	(void)ty;
	if (r.found)
	{
		for (int i = 0; i < 4; i++)
			line(r.corners[i], r.corners[(i + 1) % 4], green);
		if (r.outerFound)
			for (int i = 0; i < 4; i++)
				line(r.outer[i], r.outer[(i + 1) % 4], darkGreen);
		double px, py;
		if (quadPoint(r.corners, (r.x + 1) / 2, (r.y + 1) / 2, px, py))
			cross(px, py, red);
	}
	FILE* f = std::fopen((outDir + "/" + name + ".ppm").c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "P6\n%d %d\n255\n", W * Z, H * Z);
	std::fwrite(rgb.data(), 1, rgb.size(), f);
	std::fclose(f);
}

struct Condition
{
	const char* name;
	int frames = 150;
	double distMin = 1.8, distMax = 3.0;	// metres from the screen
	double lateral = 0.6, vertical = 0.3;	// the camera's offset from the screen's middle (seen at an angle)
	double roll = 15;			// degrees either way
	double aim = 1.0;			// aimed within this of the picture's middle (1: its edges)
	bool aimOff = false;			// aimed off the picture, the screen still in view
	bool corners = false;			// aimed at the picture's corners (aim from them)
	double blurMin = 0.7, blurMax = 1.2, motion = 0, noise = 3, expoMin = 0.75, expoMax = 1.25, vignette = 0.25, banding = 0;
	bool jpeg = true;
	double k1 = -0.1;			// the lens's barrel distortion
	bool tellLens = true;			// the tracker is given it
	double focal = 0.8;			// of the frame's width
	Game game = Game::Mixed;
	Room room = Room::Dark;
	Layout layout = Layout::Pillarbox;
	double borderMin = 0.02, borderMax = 0.05;	// of the display's height
	float borderLevel = 255;
	bool tellBorder = true;
	bool needFull = true;			// count only frames that see the whole border
	bool needPartial = false;		// count only frames that do not
	// The limits this run must meet (per cent of the picture's width).
	double minFound = 0.97, maxMedian = 0.3, maxP95 = 1.0;
};

struct Pose
{
	Scene scene;
	Camera cam;
	Look look;
	double tx = 0, ty = 0;	// the true aim
	bool full = true;	// the whole border is in the frame
	double picturePx = 0;	// the picture's width in the frame, about
	Options options;
};

void makePose(const Condition& c, Rng& rng, Pose& p)
{
	Scene& s = p.scene;
	const double t = rng.uni(c.borderMin, c.borderMax) * s.dispH;
	s.border = t;
	if (c.layout == Layout::Pillarbox)
	{
		const double ph = s.dispH - 2 * t, pw = ph * 4 / 3;
		s.picL = -pw / 2;
		s.picR = pw / 2;
		s.picT = -ph / 2;
		s.picB = ph / 2;
	}
	else
	{
		s.picL = -s.dispW / 2 + t;
		s.picR = s.dispW / 2 - t;
		s.picT = -s.dispH / 2 + t;
		s.picB = s.dispH / 2 - t;
	}
	s.borderLevel = c.borderLevel;
	s.bezel = c.room == Room::Lit ? rng.uni(0.005, 0.012) : rng.uni(0.008, 0.03);
	s.bezelLevel = (float)rng.uni(6, 20);
	makeGame(s.game, c.game, rng);
	makeRoom(s, c.room, rng);

	if (c.aimOff)
	{
		// Off the picture by up to a fifth of it, on one side or a corner.
		const double side = rng.uni(1.03, 1.25);
		switch (rng.below(3))
		{
		case 0:
			p.tx = rng.chance(0.5) ? side : -side;
			p.ty = rng.uni(-0.9, 0.9);
			break;
		case 1:
			p.ty = rng.chance(0.5) ? side : -side;
			p.tx = rng.uni(-0.9, 0.9);
			break;
		default:
			p.tx = rng.chance(0.5) ? side : -side;
			p.ty = rng.chance(0.5) ? side : -side;
			break;
		}
	}
	else if (c.corners)
	{
		p.tx = rng.chance(0.5) ? c.aim : -c.aim;
		p.ty = rng.chance(0.5) ? c.aim : -c.aim;
	}
	else
	{
		p.tx = rng.uni(-c.aim, c.aim);
		p.ty = rng.uni(-c.aim, c.aim);
	}
	const V3 target{ s.picL + (p.tx + 1) / 2 * s.pictureWidth(), s.picT + (p.ty + 1) / 2 * s.pictureHeight(), 0 };
	const double dist = rng.uni(c.distMin, c.distMax);
	const V3 from{ rng.uni(-c.lateral, c.lateral), rng.uni(-c.vertical, c.vertical), -dist };
	p.cam.f = c.focal * W;
	p.cam.k1 = c.k1;
	p.cam.aimAt(from, target, rng.uni(-c.roll, c.roll));

	Look& l = p.look;
	l.blur = rng.uni(c.blurMin, c.blurMax);
	l.motion = c.motion > 0 ? rng.uni(0, c.motion) : 0;
	l.motionAngle = rng.uni(0, PI);
	l.noise = c.noise;
	l.exposure = rng.uni(c.expoMin, c.expoMax);
	l.vignette = c.vignette;
	l.banding = c.banding;
	l.bandPeriod = rng.uni(25, 80);
	l.bandPhase = rng.uni(0, 2 * PI);
	l.jpeg = c.jpeg;

	// Is the whole border, and a little of what is round it, in the frame?
	p.full = true;
	const double o = s.border + 0.004;
	const int steps = 40;
	for (int i = 0; i <= steps && p.full; i++)
	{
		const double f = (double)i / steps;
		const V3 edge[4] = { { s.picL - o + f * (s.pictureWidth() + 2 * o), s.picT - o, 0 },
			{ s.picR + o, s.picT - o + f * (s.pictureHeight() + 2 * o), 0 },
			{ s.picL - o + f * (s.pictureWidth() + 2 * o), s.picB + o, 0 },
			{ s.picL - o, s.picT - o + f * (s.pictureHeight() + 2 * o), 0 } };
		for (const V3& e : edge)
		{
			double px, py;
			if (!p.cam.project(e, px, py) || px < 1 || py < 1 || px > W - 2 || py > H - 2)
				p.full = false;
		}
	}
	double ax, ay, bx, by;
	if (p.cam.project({ s.picL, 0, 0 }, ax, ay) && p.cam.project({ s.picR, 0, 0 }, bx, by))
		p.picturePx = std::hypot(bx - ax, by - ay);

	p.options = Options();
	p.options.pictureAspect = (float)(s.pictureWidth() / s.pictureHeight());
	if (c.tellBorder)
	{
		p.options.borderX = (float)(s.border / s.pictureWidth());
		p.options.borderY = (float)(s.border / s.pictureHeight());
	}
	// The tracker's model of the same lens, to first order: its radius is in
	// half frame widths, the scene's in focal lengths.
	p.options.lensK = c.tellLens ? (float)(c.k1 * std::pow(W / 2.0 / p.cam.f, 2)) : 0;
	p.options.focal = (float)c.focal;
}

// The aim's error in per cent of the picture's width.
double errorPct(const Result& r, const Pose& p)
{
	const double ex = (r.x - p.tx) / 2 * 100;
	const double ey = (r.y - p.ty) / 2 * 100 * p.scene.pictureHeight() / p.scene.pictureWidth();
	return std::sqrt(ex * ex + ey * ey);
}

double percentile(std::vector<double> v, double q)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))];
}

double now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Stats
{
	int frames = 0, counted = 0, found = 0, offRight = 0, placed = 0;
	std::vector<double> err, ms, px;
};

Stats runCondition(const Condition& c, uint64_t seed, int writeFrames)
{
	Stats st;
	Rng rng(seed);
	std::vector<uint8_t> frame;
	const int frames = quick ? std::max(10, c.frames / 6) : c.frames;
	int written = 0;
	for (int i = 0; i < frames; i++)
	{
		Pose p;
		makePose(c, rng, p);
		if ((c.needFull && !p.full) || (c.needPartial && p.full))
		{
			i--;
			continue;
		}
		render(p.scene, p.cam, p.look, rng, frame);
		const Frame f{ frame.data(), W, H, W };
		const double t0 = now();
		const Result r = detect(f, p.options);
		for (const SideSource side : r.sides)
			st.placed += r.found && side == SideSource::Placed;
		st.ms.push_back((now() - t0) * 1000);
		st.frames++;
		st.counted++;
		st.px.push_back(p.picturePx);
		const bool trulyOff = std::fabs(p.tx) > 1 || std::fabs(p.ty) > 1;
		if (r.found)
		{
			st.found++;
			if (!trulyOff)
				st.err.push_back(errorPct(r, p));
			if (r.offScreen == trulyOff)
				st.offRight++;
		}
		if (written < writeFrames)
		{
			// The condition's name in letters, digits and dashes.
			std::string name;
			for (const char* k = c.name; *k; k++)
				if (std::isalnum((unsigned char)*k))
					name += *k;
				else if (!name.empty() && name.back() != '-')
					name += '-';
			if (!name.empty() && name.back() == '-')
				name.pop_back();
			name += "-" + std::to_string(written);
			writePgm(name, frame);
			writeOverlay(name, frame, r, p.tx, p.ty, p.cam, p.scene);
			written++;
		}
	}
	return st;
}

void report(const Condition& c, const Stats& st, bool offTest)
{
	const double found = st.counted ? (double)st.found / st.counted : 0;
	std::printf("%-22s %4d  %5.1f%%  %6.3f %6.3f %6.3f   %5.1f%%  %4.0f px  %5.2f / %5.2f ms\n", c.name, st.counted, found * 100,
			percentile(st.err, 0.5), percentile(st.err, 0.95), st.err.empty() ? 0 : *std::max_element(st.err.begin(), st.err.end()),
			st.found ? 100.0 * st.offRight / st.found : 0, percentile(st.px, 0.5), percentile(st.ms, 0.5),
			st.ms.empty() ? 0 : *std::max_element(st.ms.begin(), st.ms.end()));
	CHECK(found >= c.minFound, "%s: found %.1f%% < %.1f%%", c.name, found * 100, c.minFound * 100);
	if (!offTest)
	{
		CHECK(percentile(st.err, 0.5) <= c.maxMedian, "%s: median %.3f%% > %.3f%%", c.name, percentile(st.err, 0.5), c.maxMedian);
		CHECK(percentile(st.err, 0.95) <= c.maxP95, "%s: 95th %.3f%% > %.3f%%", c.name, percentile(st.err, 0.95), c.maxP95);
	}
	else
		CHECK(st.found == 0 || (double)st.offRight / st.found >= 0.97, "%s: off-screen called right %d of %d", c.name, st.offRight, st.found);
}

std::vector<Condition> allConditions()
{
	std::vector<Condition> conditions;
	{
		Condition c;
		c.name = "ideal";
		c.blurMin = 0.5;
		c.blurMax = 0.6;
		c.noise = 1;
		c.vignette = 0;
		c.jpeg = false;
		c.lateral = 0.1;
		c.vertical = 0.05;
		c.roll = 3;
		c.k1 = 0;
		c.maxMedian = 0.15;
		c.maxP95 = 0.4;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "typical";
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "full-display-border";
		c.layout = Layout::Full;
		c.distMin = 2.2;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "far (3-4.5 m)";
		c.distMin = 3.0;
		c.distMax = 4.5;
		c.maxMedian = 0.5;
		c.maxP95 = 1.5;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "angle (to 40 deg)";
		c.lateral = 1.8;
		c.vertical = 1.0;
		c.distMin = 2.0;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "roll (to 45 deg)";
		c.roll = 45;
		c.distMin = 2.2;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "bright game";
		c.game = Game::Bright;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "dark game";
		c.game = Game::Dark;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "white bars on 2 sides";
		c.game = Game::WhiteBar;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "border not told";
		c.minFound = 0.85;
		c.maxMedian = 0.7;
		c.maxP95 = 3.5;
		c.tellBorder = false;
		c.game = Game::Bright;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "underexposed";
		c.expoMin = 0.25;
		c.expoMax = 0.4;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "overexposed";
		c.maxMedian = 0.45;
		c.maxP95 = 1.5;
		c.expoMin = 1.8;
		c.expoMax = 3.0;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "blur and noise";
		c.blurMin = 1.4;
		c.blurMax = 2.0;
		c.noise = 7;
		c.motion = 6;
		c.maxMedian = 0.6;
		c.maxP95 = 2.0;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "backlight banding";
		c.banding = 0.15;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "lit room, thin bezel";
		c.room = Room::Lit;
		c.minFound = 0.9;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "coloured border";
		c.maxMedian = 0.6;
		c.maxP95 = 2.5;
		c.borderLevel = 105;	// a mid blue or red's luma, the game's white brighter
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "strong barrel";
		c.k1 = -0.25;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "lens not told";
		c.tellLens = false;
		c.maxMedian = 0.5;
		c.maxP95 = 1.5;
		conditions.push_back(c);
	}
	{
		Condition c;
		c.name = "narrow lens (f=1.2W)";
		c.focal = 1.2;
		c.distMin = 2.6;
		c.distMax = 4.0;
		conditions.push_back(c);
	}
	return conditions;
}

// The edge cases, each with its own limits.
std::vector<Condition> edgeCases()
{
	std::vector<Condition> cases;
	{
		Condition c;
		c.name = "aim at the corners";
		c.corners = true;
		c.aim = 0.97;
		c.frames = 80;
		cases.push_back(c);
	}
	{
		// Close to the screen, aimed near its edges: a side out of view, placed
		// from the other three.
		Condition c;
		c.name = "a side out of view";
		c.needFull = false;
		c.needPartial = true;
		c.distMin = 1.0;
		c.distMax = 1.5;
		c.aim = 0.9;
		c.frames = 100;
		c.minFound = 0.5;
		c.maxMedian = 1.5;
		c.maxP95 = 4;
		cases.push_back(c);
	}
	{
		Condition c;
		c.name = "all-white game";
		c.game = Game::White;
		c.frames = 60;
		c.maxMedian = 0.6;
		c.maxP95 = 1.5;
		cases.push_back(c);
	}
	{
		Condition c;
		c.name = "black game";
		c.game = Game::Black;
		c.frames = 60;
		cases.push_back(c);
	}
	{
		// Aimed off the picture (to reload), the screen still in view.
		Condition c;
		c.name = "aimed off the picture";
		c.aimOff = true;
		c.distMin = 2.4;
		c.distMax = 3.5;
		c.frames = 100;
		cases.push_back(c);
	}
	return cases;
}

// One pose, rendered.
struct Shot
{
	Pose pose;
	std::vector<uint8_t> frame;
};

void shoot(const Condition& c, Rng& rng, Shot& shot)
{
	do
		makePose(c, rng, shot.pose);
	while (c.needFull && !shot.pose.full);
	render(shot.pose.scene, shot.pose.cam, shot.pose.look, rng, shot.frame);
}

double stddev(const std::vector<double>& v)
{
	double mean = 0, sq = 0;
	for (const double x : v)
		mean += x / v.size();
	for (const double x : v)
		sq += (x - mean) * (x - mean) / v.size();
	return std::sqrt(sq);
}

// The single-frame checks: a frame with nothing, the inverted polarity, the
// calibration's arithmetic.
void plainChecks()
{
	std::printf("\nChecks\n");
	Rng rng(77);
	// A camera that sees nothing (covered, or the screen off): noise only.
	{
		std::vector<uint8_t> frame((size_t)W * H);
		for (uint8_t& v : frame)
			v = (uint8_t)std::clamp((int)std::lround(4 + 3 * rng.normal()), 0, 255);
		const Result r = detect({ frame.data(), W, H, W });
		std::printf("  black camera frame: found %d\n", r.found);
		CHECK(!r.found, "a black camera frame found a screen");
		std::fill(frame.begin(), frame.end(), 255);
		CHECK(!detect({ frame.data(), W, H, W }).found, "a white camera frame found a screen");
		CHECK(!detect({ nullptr, W, H, W }).found, "no frame found a screen");
		CHECK(!detect({ frame.data(), 8, 8, 8 }).found, "a tiny frame found a screen");
	}
	Condition c;
	c.name = "checks";
	Shot shot;
	shoot(c, rng, shot);
	const Pose& p = shot.pose;
	const Frame f{ shot.frame.data(), W, H, W };
	const Result base = detect(f, p.options);
	CHECK(base.found, "the checks' frame was not found");
	// A stride wider than the width.
	{
		std::vector<uint8_t> wide((size_t)(W + 13) * H);
		for (int y = 0; y < H; y++)
			std::memcpy(&wide[(size_t)y * (W + 13)], &shot.frame[(size_t)y * W], W);
		const Result r = detect({ wide.data(), W, H, W + 13 }, p.options);
		CHECK(r.found && r.x == base.x && r.y == base.y, "a padded stride changed the result");
	}
	// A dark border on a bright surround, read with borderBrighter off.
	{
		std::vector<uint8_t> inverted(shot.frame);
		for (uint8_t& v : inverted)
			v = (uint8_t)(255 - v);
		Options o = p.options;
		o.borderBrighter = false;
		const Result r = detect({ inverted.data(), W, H, W }, o);
		std::printf("  dark border on a bright surround: found %d, error %.3f%%\n", r.found, r.found ? errorPct(r, p) : -1.0);
		CHECK(r.found && std::fabs(r.x - base.x) < 1e-6 && std::fabs(r.y - base.y) < 1e-6, "the inverted frame read differently");
	}
	// The calibration in the picture: hundredths of a per cent of its width
	// and height, added.
	{
		Calibration cal;
		cal.offsetX = 150;
		cal.offsetY = -275;
		const Result r = detect(f, p.options, cal);
		CHECK(r.found && std::fabs(r.x - base.x - 0.03) < 1e-5 && std::fabs(r.y - base.y + 0.055) < 1e-5, "picture calibration: %.5f %.5f",
				r.x - base.x, r.y - base.y);
	}
	// In the camera: the point the frame's middle is moved to is where the
	// gun points; its truth is where that pixel sees the screen.
	{
		Calibration cal;
		cal.inCamera = true;
		cal.offsetX = 600;	// 6% of the frame's width: 19.2 px right
		cal.offsetY = -450;	// 4.5% of its height: 10.8 px up
		const Result r = detect(f, p.options, cal);
		double wx = 0, wy = 0;
		p.cam.look(p.cam.cx + 0.06 * W, p.cam.cy - 0.045 * H, wx, wy);
		Pose moved = p;
		moved.tx = (wx - p.scene.picL) / p.scene.pictureWidth() * 2 - 1;
		moved.ty = (wy - p.scene.picT) / p.scene.pictureHeight() * 2 - 1;
		const double err = r.found ? errorPct(r, moved) : -1;
		std::printf("  calibration in the camera (19 px right, 11 px up): error %.3f%% (at the frame's middle %.3f%%)\n", err,
				errorPct(base, p));
		CHECK(r.found && err < errorPct(base, p) + 0.3, "camera calibration: error %.3f%%", err);
	}
}

// Frame after frame with the Tracker.
void trackerChecks()
{
	std::printf("\nTracker\n");
	// Steadiness at rest: one pose, fresh noise each frame, raw and smoothed.
	{
		Condition c;
		c.name = "rest";
		Rng rng(91);
		Pose p;
		do
			makePose(c, rng, p);
		while (!p.full);
		Options smooth = p.options;
		smooth.smoothCutoff = 1.0f;
		Tracker raw(p.options), filtered(smooth);
		std::vector<double> xr, yr, xs, ys, ms;
		std::vector<uint8_t> frame;
		const int frames = quick ? 30 : 120;
		for (int i = 0; i < frames; i++)
		{
			render(p.scene, p.cam, p.look, rng, frame);
			const Frame f{ frame.data(), W, H, W };
			const double t0 = now();
			const Result a = raw.update(f, {}, i / 60.0);
			ms.push_back((now() - t0) * 1000);
			const Result b = filtered.update(f, {}, i / 60.0);
			if (a.found)
			{
				xr.push_back(a.x);
				yr.push_back(a.y);
			}
			if (b.found && i >= 20)
			{
				xs.push_back(b.x);
				ys.push_back(b.y);
			}
		}
		const double toWidth = 50, toHeight = 50 * p.scene.pictureHeight() / p.scene.pictureWidth();
		std::printf("  at rest, %d frames: jitter (standard deviation, %% of the width) x %.4f y %.4f; smoothed (1 Hz) x %.4f y %.4f\n", frames,
				stddev(xr) * toWidth, stddev(yr) * toHeight, stddev(xs) * toWidth, stddev(ys) * toHeight);
		std::printf("  the Tracker's time a frame: median %.2f ms, worst %.2f ms\n", percentile(ms, 0.5), *std::max_element(ms.begin(), ms.end()));
		CHECK((int)xr.size() == frames, "at rest: found %zu of %d", xr.size(), frames);
		CHECK(stddev(xr) * toWidth < 0.1 && stddev(yr) * toHeight < 0.1, "at rest: jitter too large");
	}
	// A sweep across the picture at a steady speed (a picture's width in a
	// second, 60 frames a second): the error while moving, raw and smoothed.
	{
		Condition c;
		c.name = "sweep";
		c.distMin = c.distMax = 2.4;
		c.lateral = c.vertical = 0;
		c.roll = 0;
		Rng rng(92);
		Pose base;
		makePose(c, rng, base);
		Options smooth = base.options;
		smooth.smoothCutoff = 1.0f;
		Tracker raw(base.options), filtered(smooth);
		std::vector<double> er, es;
		std::vector<uint8_t> frame;
		const int frames = 54;	// 0.9 s: from -0.9 to 0.9
		for (int i = 0; i < frames; i++)
		{
			Pose p = base;
			p.tx = -0.9 + i * 2.0 / 60;
			p.ty = 0.2;
			const V3 target{ p.scene.picL + (p.tx + 1) / 2 * p.scene.pictureWidth(), p.scene.picT + (p.ty + 1) / 2 * p.scene.pictureHeight(), 0 };
			p.cam.aimAt(base.cam.c, target, 0);
			render(p.scene, p.cam, p.look, rng, frame);
			const Frame f{ frame.data(), W, H, W };
			const Result a = raw.update(f, {}, i / 60.0), b = filtered.update(f, {}, i / 60.0);
			if (i >= 10 && a.found && b.found)
			{
				er.push_back(errorPct(a, p));
				es.push_back(errorPct(b, p));
			}
		}
		std::printf("  sweeping (a picture's width a second): error median %.3f%% raw, %.3f%% smoothed (its lag)\n", percentile(er, 0.5),
				percentile(es, 0.5));
		CHECK(percentile(er, 0.5) < 0.3, "sweep: raw error %.3f%%", percentile(er, 0.5));
	}
	// A flickering game (the picture black every other frame) at rest.
	{
		Condition c;
		c.name = "flicker";
		Rng rng(93);
		Pose p;
		do
			makePose(c, rng, p);
		while (!p.full);
		Pose dark = p;
		makeGame(dark.scene.game, Game::Black, rng);
		Tracker t(p.options);
		std::vector<double> x;
		std::vector<uint8_t> frame;
		int found = 0;
		const int frames = quick ? 20 : 60;
		for (int i = 0; i < frames; i++)
		{
			const Pose& q = i % 2 ? dark : p;
			render(q.scene, q.cam, q.look, rng, frame);
			const Result r = t.update({ frame.data(), W, H, W }, {}, i / 60.0);
			found += r.found;
			if (r.found)
				x.push_back(errorPct(r, p));
		}
		std::printf("  a game flickering black every other frame: found %d of %d, error median %.3f%%, worst %.3f%%\n", found, frames,
				percentile(x, 0.5), x.empty() ? 0 : *std::max_element(x.begin(), x.end()));
		CHECK(found == frames, "flicker: found %d of %d", found, frames);
	}
	// What the Tracker learns: the lens's distortion (strong barrel, not
	// told) and the border's thickness (not told, a bright game).
	for (int what = 0; what < 2; what++)
	{
		Condition c;
		c.name = what == 0 ? "learn lens" : "learn border";
		if (what == 0)
		{
			c.k1 = -0.25;
			c.tellLens = false;
			c.distMin = 1.6;
			c.distMax = 2.2;
		}
		else
		{
			c.tellBorder = false;
			c.game = Game::Bright;
		}
		// The same television and border all along, the gun moving about.
		Rng rng(94 + what);
		Pose first;
		do
			makePose(c, rng, first);
		while (!first.full);
		Tracker t(first.options);
		std::vector<double> early, late;
		std::vector<uint8_t> frame;
		int foundLate = 0, late0 = 0;
		Result r;
		const int frames = quick ? 40 : 160;
		for (int i = 0; i < frames; i++)
		{
			Pose p;
			do
				makePose(c, rng, p);
			while (!p.full);
			Pose q = first;
			q.tx = p.tx;
			q.ty = p.ty;
			q.cam = p.cam;
			q.look = p.look;
			const V3 target{ q.scene.picL + (q.tx + 1) / 2 * q.scene.pictureWidth(), q.scene.picT + (q.ty + 1) / 2 * q.scene.pictureHeight(), 0 };
			q.cam.aimAt(p.cam.c, target, 0);
			if (i % 4 == 0)
				makeGame(q.scene.game, c.game, rng);
			render(q.scene, q.cam, q.look, rng, frame);
			r = t.update({ frame.data(), W, H, W }, {}, i / 30.0);
			if (i < frames / 4)
			{
				if (r.found)
					early.push_back(errorPct(r, q));
			}
			else if (i >= frames / 2)
			{
				late0++;
				foundLate += r.found;
				if (r.found)
					late.push_back(errorPct(r, q));
			}
		}
		if (what == 0)
			std::printf("  learning the lens (k1 %.2f, the tracker's model's first-order equal %.3f): learnt %.3f; error median %.3f%% in "
					"the first quarter, %.3f%% in the second half\n",
					c.k1, c.k1 * std::pow(W / 2.0 / (c.focal * W), 2), r.lensK, percentile(early, 0.5), percentile(late, 0.5));
		else
			std::printf("  learning the border (not told, %.4f x %.4f of the picture): seen %.4f x %.4f; error median %.3f%% (p95 %.3f%%) in "
					"the first quarter, %.3f%% (p95 %.3f%%) in the second half, found %d of %d\n",
					first.scene.border / first.scene.pictureWidth(), first.scene.border / first.scene.pictureHeight(), r.borderX, r.borderY,
					percentile(early, 0.5), percentile(early, 0.95), percentile(late, 0.5), percentile(late, 0.95), foundLate, late0);
		CHECK(percentile(late, 0.5) <= percentile(early, 0.5) + 0.05, "%s: no better after learning", c.name);
	}
}


}

int main(int argc, char** argv)
{
	quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;
	if (const char* out = std::getenv("SINDEN_OUT"))
		outDir = out;

	std::printf("Conditions (error: per cent of the picture's width; px: the picture's width in the frame; time: median / worst)\n");
	std::printf("%-22s %4s  %6s  %6s %6s %6s   %6s  %7s  %s\n", "condition", "n", "found", "median", "p95", "worst", "off ok", "px",
			"time");
	std::vector<double> times;
	uint64_t seed = 1000;
	for (const Condition& c : allConditions())
	{
		const Stats st = runCondition(c, seed++, 2);
		report(c, st, false);
		times.insert(times.end(), st.ms.begin(), st.ms.end());
	}
	std::printf("\nEdge cases\n");
	for (const Condition& c : edgeCases())
	{
		const Stats st = runCondition(c, seed++, 2);
		report(c, st, c.aimOff);
		if (c.needPartial)
			std::printf("%22s placed a side in %d of %d found\n", "", st.placed, st.found);
		times.insert(times.end(), st.ms.begin(), st.ms.end());
	}
	plainChecks();
	trackerChecks();
	std::printf("\nTime a frame (detect, one thread, all conditions): median %.2f ms, 95th percentile %.2f ms, worst %.2f ms\n",
			percentile(times, 0.5), percentile(times, 0.95), *std::max_element(times.begin(), times.end()));
#ifndef SINDEN_SANITIZED	// the sanitizers slow it several times over
	CHECK(percentile(times, 0.95) < 15, "too slow");
#endif
	std::printf(failures ? "\n%d FAILED\n" : "\nAll passed\n", failures);
	return failures ? 1 : 0;
}
