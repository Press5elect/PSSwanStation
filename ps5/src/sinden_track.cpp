// PSSwanStation - where a Sinden Lightgun points, from its camera's picture of
// the border round the game's picture (sinden_track.h; the method and what was
// measured: docs/dev/sinden-lightgun.md, "The tracker").
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sinden_track.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fe::sinden
{

namespace
{

constexpr double PI = 3.14159265358979323846;
constexpr int TOP = 0, RIGHT = 1, BOTTOM = 2, LEFT = 3;
// Rays cast from a bright part's middle to find its outer outline.
constexpr int RAYS = 180;
// Where between the frame's dark and bright levels the thresholds are tried,
// the most likely first; the first that gives a clean ring ends the search.
// The last ones fall between the first: a window as bright as the screen sets
// the frame's bright level, and a dimmer border may then stand clear of a lit
// wall only in a narrow range.
constexpr double FRACTIONS[] = { 0.5, 0.32, 0.68, 0.84, 0.18, 0.41, 0.59, 0.76 };
constexpr int FRACTION_COUNT = 8;
// Steps along a profile across an edge, in pixels.
constexpr double STEP = 0.5;

struct V2
{
	double x = 0, y = 0;
};
V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }
V2 operator*(V2 a, double s) { return { a.x * s, a.y * s }; }
double dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
double length(V2 a) { return std::sqrt(dot(a, a)); }
V2 unit(V2 a)
{
	const double l = length(a);
	return l > 0 ? a * (1 / l) : V2{};
}

// A straight line n.p = d, n of length one; for a side of the screen, n points
// out of the picture.
struct Line
{
	V2 n;
	double d = 0;
};

double signedDistance(const Line& l, V2 p) { return dot(l.n, p) - l.d; }

Line through(V2 a, V2 b)
{
	const V2 t = unit(b - a);
	const V2 n{ -t.y, t.x };
	return { n, dot(n, a) };
}

Line facingAway(Line l, V2 inside)
{
	if (signedDistance(l, inside) > 0)
	{
		l.n = l.n * -1;
		l.d = -l.d;
	}
	return l;
}

bool meet(const Line& a, const Line& b, V2& p)
{
	const double det = a.n.x * b.n.y - a.n.y * b.n.x;
	if (std::fabs(det) < 1e-9)
		return false;
	p = { (a.d * b.n.y - a.n.y * b.d) / det, (a.n.x * b.d - a.d * b.n.x) / det };
	return true;
}

// The direction along a side, clockwise round the picture (y is down).
V2 along(const Line& l) { return { -l.n.y, l.n.x }; }

// A small fixed-seed generator, so the same frame always gives the same answer.
struct Random
{
	uint32_t s = 0x9e3779b9u;
	uint32_t next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	int below(int n) { return n > 0 ? (int)(next() % (uint32_t)n) : 0; }
};

struct Image
{
	const uint8_t* p = nullptr;
	int w = 0, h = 0, stride = 0;

	const uint8_t* row(int y) const { return p + (size_t)y * stride; }
	// Bilinear, pixel centres at whole numbers; clamped to the frame.
	float at(V2 q) const
	{
		const double x = std::clamp(q.x, 0.0, w - 1.001), y = std::clamp(q.y, 0.0, h - 1.001);
		const int x0 = (int)x, y0 = (int)y;
		const float fx = (float)(x - x0), fy = (float)(y - y0);
		const uint8_t* r0 = row(y0) + x0;
		const uint8_t* r1 = r0 + stride;
		const float top = r0[0] + (r0[1] - r0[0]) * fx;
		const float bottom = r1[0] + (r1[1] - r1[0]) * fx;
		return top + (bottom - top) * fy;
	}
};

// The lens's barrel distortion, the division model about the frame's centre.
struct Lens
{
	V2 c;
	double scale = 1, k = 0;

	V2 undistort(V2 p) const
	{
		if (k == 0)
			return p;
		const V2 d = p - c;
		const double f = 1 + k * dot(d, d) / (scale * scale);
		return c + d * (1 / std::max(f, 0.2));
	}
	V2 distort(V2 p) const
	{
		if (k == 0)
			return p;
		const V2 d = p - c;
		const double ru = length(d) / scale;
		if (ru < 1e-9)
			return p;
		// ru = rd / (1 + k rd^2), solved for rd.
		const double disc = 1 - 4 * k * ru * ru;
		const double rd = disc > 0 ? (1 - std::sqrt(disc)) / (2 * k * ru) : 1 / (2 * k * ru);
		return c + d * (rd / ru);
	}
};

// A run of bright pixels in a row, and a connected part made of runs.
struct Run
{
	int y, x0, x1;
};

struct Part
{
	int count = 0, minX = 0, maxX = 0, minY = 0, maxY = 0;
	double sumX = 0, sumY = 0;
};

struct RayHit
{
	V2 p;
	double run = 0, reach = 0;
	bool hit = false, cut = false;
};

// One part at one threshold, read as the border's outer outline.
struct Candidate
{
	Line lines[4];
	bool has[4] = {};
	int support[4] = {};
	double score = 0, band = 0, roll = 0;
	int threshold = 0, fraction = -1;
	V2 centre;
	bool clean = false;
	bool holdsMiddle = false;	// the frame's middle is inside its outline
};

// What a Tracker passes from one frame to the next.
struct Hints
{
	int fraction = -1;
	bool haveRoll = false;
	double roll = 0;
	double tau[4] = {};		// the border's thickness learnt on each side, a fraction of the outline across
	double seen[4] = {};		// out: as this frame saw it (0: not seen)
	bool haveBox = false;
	double box[4] = {};		// the last outer outline's bounds
};

// The edge points kept for measuring the lens.
struct EdgeSet
{
	std::vector<V2> points[8];	// as seen: outer sides 0-3, inner 4-7
	int count = 0;
};

struct Workspace
{
	std::vector<uint8_t> inverted;
	std::vector<Run> runs;
	std::vector<int> rowStart, parent, partOfRun, partIndex, labels;
	std::vector<Part> parts;
	std::vector<RayHit> hits;
	std::vector<float> profile;
	std::vector<V2> points, undistorted;
	std::vector<double> values;
};

int findRoot(std::vector<int>& parent, int i)
{
	while (parent[i] != i)
	{
		parent[i] = parent[parent[i]];
		i = parent[i];
	}
	return i;
}

// The parts (8-connected) of pixels at or above the threshold, and a label for
// each pixel (-1 for none).
void labelParts(const Image& im, int threshold, Workspace& ws)
{
	ws.runs.clear();
	ws.rowStart.resize(im.h + 1);
	for (int y = 0; y < im.h; y++)
	{
		ws.rowStart[y] = (int)ws.runs.size();
		const uint8_t* row = im.row(y);
		int x = 0;
		while (x < im.w)
		{
			while (x < im.w && row[x] < threshold)
				x++;
			if (x == im.w)
				break;
			const int x0 = x;
			while (x < im.w && row[x] >= threshold)
				x++;
			ws.runs.push_back({ y, x0, x - 1 });
		}
	}
	ws.rowStart[im.h] = (int)ws.runs.size();
	const int n = (int)ws.runs.size();
	ws.parent.resize(n);
	for (int i = 0; i < n; i++)
		ws.parent[i] = i;
	for (int y = 1; y < im.h; y++)
	{
		int i = ws.rowStart[y - 1], j = ws.rowStart[y];
		const int iEnd = ws.rowStart[y], jEnd = ws.rowStart[y + 1];
		while (i < iEnd && j < jEnd)
		{
			const Run& a = ws.runs[i];
			const Run& b = ws.runs[j];
			if (a.x1 + 1 < b.x0)
				i++;
			else if (b.x1 + 1 < a.x0)
				j++;
			else
			{
				const int ra = findRoot(ws.parent, i), rb = findRoot(ws.parent, j);
				if (ra != rb)
					ws.parent[std::max(ra, rb)] = std::min(ra, rb);
				if (a.x1 < b.x1)
					i++;
				else
					j++;
			}
		}
	}
	ws.parts.clear();
	ws.partIndex.assign(n, -1);
	ws.partOfRun.resize(n);
	for (int r = 0; r < n; r++)
	{
		const int root = findRoot(ws.parent, r);
		if (ws.partIndex[root] < 0)
		{
			ws.partIndex[root] = (int)ws.parts.size();
			Part p;
			p.minX = im.w;
			p.minY = im.h;
			p.maxX = p.maxY = -1;
			ws.parts.push_back(p);
		}
		const int index = ws.partIndex[root];
		Part& p = ws.parts[index];
		const Run& run = ws.runs[r];
		const int len = run.x1 - run.x0 + 1;
		p.count += len;
		p.minX = std::min(p.minX, run.x0);
		p.maxX = std::max(p.maxX, run.x1);
		p.minY = std::min(p.minY, run.y);
		p.maxY = std::max(p.maxY, run.y);
		p.sumX += 0.5 * (run.x0 + run.x1) * len;
		p.sumY += (double)run.y * len;
		ws.partOfRun[r] = index;
	}
	ws.labels.assign((size_t)im.w * im.h, -1);
	for (int r = 0; r < n; r++)
	{
		const Run& run = ws.runs[r];
		int* row = ws.labels.data() + (size_t)run.y * im.w;
		std::fill(row + run.x0, row + run.x1 + 1, ws.partOfRun[r]);
	}
}

// Rays from the part's middle: on each, the part's outermost pixel (found by
// walking in from the frame's edge), whether the part reaches the frame's edge
// there (cut: that point is not its outline), and how far the part runs inward
// from it (short for a ring, long for a filled shape).
void castRays(const Image& im, Workspace& ws, int part, V2 c)
{
	ws.hits.assign(RAYS, RayHit());
	const double step = 0.7;
	for (int i = 0; i < RAYS; i++)
	{
		const double a = 2 * PI * (i + 0.5) / RAYS;
		const V2 d{ std::cos(a), std::sin(a) };
		double reach = 1e9;
		if (d.x > 1e-9)
			reach = std::min(reach, (im.w - 1 - c.x) / d.x);
		else if (d.x < -1e-9)
			reach = std::min(reach, -c.x / d.x);
		if (d.y > 1e-9)
			reach = std::min(reach, (im.h - 1 - c.y) / d.y);
		else if (d.y < -1e-9)
			reach = std::min(reach, -c.y / d.y);
		auto labelAt = [&](double t, int& x, int& y) {
			x = std::clamp((int)std::lround(c.x + t * d.x), 0, im.w - 1);
			y = std::clamp((int)std::lround(c.y + t * d.y), 0, im.h - 1);
			return ws.labels[(size_t)y * im.w + x];
		};
		RayHit& hit = ws.hits[i];
		int x = 0, y = 0;
		double t = reach;
		for (; t > 0; t -= step)
			if (labelAt(t, x, y) == part)
				break;
		if (t <= 0)
			continue;
		hit.hit = true;
		hit.cut = x == 0 || y == 0 || x == im.w - 1 || y == im.h - 1;
		hit.p = c + d * (t + 0.5 * step);
		hit.reach = t;
		double u = t;
		while (u > 0 && labelAt(u, x, y) == part)
			u -= step;
		hit.run = t - std::max(u, 0.0);
	}
}

// Weighted total least squares (w may be null for equal weights).
bool fitLine(const V2* p, const double* w, int n, Line& out)
{
	double sw = 0, mx = 0, my = 0;
	for (int i = 0; i < n; i++)
	{
		const double wi = w ? w[i] : 1;
		sw += wi;
		mx += wi * p[i].x;
		my += wi * p[i].y;
	}
	if (sw <= 0)
		return false;
	mx /= sw;
	my /= sw;
	double sxx = 0, sxy = 0, syy = 0;
	for (int i = 0; i < n; i++)
	{
		const double wi = w ? w[i] : 1;
		const double dx = p[i].x - mx, dy = p[i].y - my;
		sxx += wi * dx * dx;
		sxy += wi * dx * dy;
		syy += wi * dy * dy;
	}
	const double angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
	const V2 normal{ -std::sin(angle), std::cos(angle) };
	out = { normal, normal.x * mx + normal.y * my };
	return true;
}

struct LineFit
{
	Line line;
	int support = 0;
};

// Up to maxLines straight lines through the outline's points, the best
// supported first (sequential RANSAC; neighbouring points propose a line).
int findLines(const std::vector<V2>& pts, double tol, int minSupport, int maxLines, LineFit* out)
{
	const int n = (int)pts.size();
	std::vector<char> used(n, 0);
	std::vector<V2> in;
	Random rng;
	int found = 0;
	for (; found < maxLines; found++)
	{
		int left = 0;
		for (int i = 0; i < n; i++)
			left += !used[i];
		if (left < minSupport)
			break;
		int bestCount = 0;
		Line best;
		for (int it = 0; it < 64; it++)
		{
			const int i = rng.below(n), j = (i + 2 + rng.below(5)) % n;
			if (used[i] || used[j] || length(pts[i] - pts[j]) < 3)
				continue;
			const Line l = through(pts[i], pts[j]);
			int count = 0;
			for (int k = 0; k < n; k++)
				count += !used[k] && std::fabs(signedDistance(l, pts[k])) <= tol;
			if (count > bestCount)
			{
				bestCount = count;
				best = l;
			}
		}
		if (bestCount < minSupport)
			break;
		int count = 0;
		for (int pass = 0; pass < 2; pass++)
		{
			in.clear();
			for (int k = 0; k < n; k++)
				if (!used[k] && std::fabs(signedDistance(best, pts[k])) <= tol)
					in.push_back(pts[k]);
			count = (int)in.size();
			if (count >= 2)
				fitLine(in.data(), nullptr, count, best);
		}
		for (int k = 0; k < n; k++)
			if (!used[k] && std::fabs(signedDistance(best, pts[k])) <= tol)
				used[k] = 1;
		out[found] = { best, count };
	}
	return found;
}

// The lines as the screen's sides: each line's outward normal, less the
// screen's turn in the frame, points right, down, left or up.
bool assignSides(LineFit* fits, int nf, V2 inside, const Options& o, const Hints& hints, Candidate& c)
{
	double s4 = 0, c4 = 0, phi[8] = {};
	for (int f = 0; f < nf; f++)
	{
		fits[f].line = facingAway(fits[f].line, inside);
		phi[f] = std::atan2(fits[f].line.n.y, fits[f].line.n.x);
		s4 += fits[f].support * std::sin(4 * phi[f]);
		c4 += fits[f].support * std::cos(4 * phi[f]);
	}
	double roll = 0.25 * std::atan2(s4, c4);
	if (hints.haveRoll)
		for (const double turn : { roll + PI / 2, roll - PI / 2 })
			if (std::fabs(turn - hints.roll) < std::fabs(roll - hints.roll) && std::fabs(turn) < PI / 2)
				roll = turn;
	auto place = [&](double r) {
		for (int s = 0; s < 4; s++)
		{
			c.has[s] = false;
			c.support[s] = 0;
		}
		for (int f = 0; f < nf; f++)
		{
			int k = (int)std::lround((phi[f] - r) / (PI / 2));
			k = ((k % 4) + 4) % 4;
			const int side = k == 0 ? RIGHT : k == 1 ? BOTTOM : k == 2 ? LEFT : TOP;
			if (fits[f].support > c.support[side])
			{
				c.lines[side] = fits[f].line;
				c.support[side] = fits[f].support;
				c.has[side] = true;
			}
		}
	};
	place(roll);
	// Turned near 45 degrees either way could be read as turned the other way
	// with its sides swapped: the picture's longer sides are its top and bottom.
	const bool all = c.has[0] && c.has[1] && c.has[2] && c.has[3];
	if (!hints.haveRoll && all && std::fabs(roll) > PI / 6 && std::fabs(std::log(o.pictureAspect)) > 0.1)
	{
		V2 tl, tr, br, bl;
		if (meet(c.lines[LEFT], c.lines[TOP], tl) && meet(c.lines[TOP], c.lines[RIGHT], tr)
				&& meet(c.lines[RIGHT], c.lines[BOTTOM], br) && meet(c.lines[BOTTOM], c.lines[LEFT], bl))
		{
			const double across = length(tr - tl) + length(br - bl), down = length(bl - tl) + length(br - tr);
			if ((o.pictureAspect > 1) != (across > down))
			{
				roll -= std::copysign(PI / 2, roll);
				place(roll);
			}
		}
	}
	c.roll = roll;
	int sides = 0;
	for (int s = 0; s < 4; s++)
		sides += c.has[s];
	return sides >= 3;
}

double median(std::vector<double>& v)
{
	if (v.empty())
		return 0;
	const size_t m = v.size() / 2;
	std::nth_element(v.begin(), v.begin() + m, v.end());
	return v[m];
}

// Reads one bright part as the border: its outline's sides and how much it
// looks like the border (a large ring of four straight sides).
bool evaluatePart(const Image& im, Workspace& ws, int part, const Options& o, const Hints& hints, Candidate& c)
{
	const Part& p = ws.parts[part];
	const V2 centre{ p.sumX / p.count, p.sumY / p.count };
	castRays(im, ws, part, centre);
	std::vector<V2>& pts = ws.points;
	pts.clear();
	ws.values.clear();
	int cut = 0, ring = 0;
	for (const RayHit& hit : ws.hits)
	{
		if (!hit.hit)
			continue;
		if (hit.cut)
		{
			cut++;
			continue;
		}
		pts.push_back(hit.p);
		if (hit.run < 0.4 * hit.reach)
		{
			ring++;
			ws.values.push_back(hit.run);
		}
	}
	if (pts.size() < 16)
		return false;
	const double size = std::sqrt((double)(p.maxX - p.minX + 1) * (p.maxY - p.minY + 1));
	const double tol = 1.5 + 0.012 * size;
	LineFit fits[6];
	const int nf = findLines(pts, tol, std::max(5, (int)pts.size() / 30), 6, fits);
	c = Candidate();
	if (!assignSides(fits, nf, centre, o, hints, c))
		return false;
	int support = 0, sides = 0;
	for (int s = 0; s < 4; s++)
		if (c.has[s])
		{
			support += c.support[s];
			sides++;
		}
	const double fit = (double)support / pts.size();
	const double coverage = (double)(pts.size() + cut) / RAYS;
	const double ringShare = (double)ring / pts.size();
	c.band = median(ws.values);
	c.centre = centre;
	// Where the gun points (the frame's middle) is usually on the screen.
	const V2 middle{ (im.w - 1) / 2.0, (im.h - 1) / 2.0 };
	bool holdsMiddle = true;
	for (int s = 0; s < 4; s++)
		if (c.has[s] && signedDistance(c.lines[s], middle) > 0)
			holdsMiddle = false;
	bool nearLast = false;
	if (hints.haveBox)
	{
		const double x0 = std::max<double>(p.minX, hints.box[0]), y0 = std::max<double>(p.minY, hints.box[1]);
		const double x1 = std::min<double>(p.maxX, hints.box[2]), y1 = std::min<double>(p.maxY, hints.box[3]);
		const double shared = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
		const double mine = (double)(p.maxX - p.minX) * (p.maxY - p.minY);
		const double last = (hints.box[2] - hints.box[0]) * (hints.box[3] - hints.box[1]);
		nearLast = shared > 0.5 * std::max(mine, last);
	}
	c.score = fit * fit * coverage * (sides == 4 ? 1.0 : 0.7) * (0.35 + 0.65 * ringShare)
			* std::sqrt(std::min(1.0, size / std::min(im.w, im.h))) * (holdsMiddle ? 1.25 : 1.0) * (nearLast ? 1.25 : 1.0);
	c.clean = sides == 4 && fit > 0.9 && coverage > 0.95 && ringShare > 0.7;
	c.holdsMiddle = holdsMiddle;
	return true;
}

// Along one profile across a side (q at s = s0 + k STEP, s growing into the
// picture): the outer edge, the steepest rise from the darker outside into the
// border near s = 0 (the outermost of the strong ones), and the inner edge,
// where the profile leaves the border's level.
//
// Each edge is placed where the profile crosses half way between the levels on
// its two sides, interpolated: this is right to a tenth of a pixel even for a
// sharp edge, where the steepest point sticks to the pixel grid (up to 0.4 px
// off, measured). A border clipped by too high an exposure moves that half-way
// point outward, so there the steepest point is taken instead: it stays put
// until the border's true level is twice the clipped one.
struct Edges
{
	double sOut = 0, sIn = 0, width = 0;
	float level = 0, outside = 0;
	bool in = false;
};

// The interpolated position where q crosses level between k and k + 1.
double crossing(const float* q, int k, float level, double s0)
{
	const float d = q[k + 1] - q[k];
	return s0 + STEP * (k + (std::fabs(d) > 1e-6f ? (level - q[k]) / d : 0.5));
}

// The interpolated peak of slope(k) at kBest.
template <typename Slope>
double slopePeak(Slope slope, int kBest, double s0)
{
	const float g0 = slope(kBest - 1), g1 = slope(kBest), g2 = slope(kBest + 1);
	const float curve = g0 - 2 * g1 + g2;
	const double offset = curve < 0 ? std::clamp(0.5 * (g0 - g2) / curve, -0.5, 0.5) : 0;
	return s0 + STEP * (kBest + offset);
}

bool readProfile(const float* q, int n, double s0, double win, double wMin, double wMax, double minContrast, float level, Edges& e)
{
	auto index = [&](double s) { return (int)std::lround((s - s0) / STEP); };
	auto clampIndex = [&](int k) { return std::clamp(k, 0, n - 1); };
	const int perPixel = (int)std::lround(1 / STEP);
	const int k0 = std::max(2, index(-win)), k1 = std::min(n - 3, index(win));
	auto rise = [&](int k) { return q[k + 1] - q[k - 1]; };
	float steepest = 0;
	for (int k = k0; k <= k1; k++)
		steepest = std::max(steepest, rise(k));
	if (steepest <= 0)
		return false;
	int kEdge = -1;
	for (int k = k0; k <= k1 && kEdge < 0; k++)
		if (rise(k) >= 0.5f * steepest && rise(k) >= rise(k - 1) && rise(k) >= rise(k + 1))
			kEdge = k;
	if (kEdge < 0)
		return false;
	float outside = q[kEdge], high = q[kEdge];
	for (int k = clampIndex(kEdge - 4 * perPixel); k <= kEdge; k++)
		outside = std::min(outside, q[k]);
	// The border's level: where the rise ends (its slope falls well below
	// the edge's; noise and a smear of motion make it wobble on the way),
	// not the brightest within reach, which may be the picture past a thin
	// border of a colour darker than the picture's whites. The second pass
	// knows the side's level from the first.
	int kEnd = kEdge;
	while (kEnd + 1 < std::min(n - 2, kEdge + 8 * perPixel) && rise(kEnd + 1) > 0.35f * rise(kEdge))
		kEnd++;
	int kPeak = kEdge;
	for (int k = kEdge; k <= clampIndex(kEnd + perPixel); k++)
		if (q[k] > high)
		{
			high = q[k];
			kPeak = k;
		}
	e.level = high;
	e.outside = outside;
	// Brighter than the border is known to be on this side: a thin border
	// whose rise has run into the picture's brighter rise past it.
	if (level > 0 && high > 1.15f * level)
		high = level;
	if (high - outside < minContrast)
		return false;
	const bool clipped = high >= 248;
	// The edge's width (a quarter to three quarters of the rise), for how far
	// past the inner edge the picture's own level is read.
	double width = 1;
	if (clipped)
		e.sOut = slopePeak(rise, kEdge, s0);
	else
	{
		// The outermost crossing near the steepest rise.
		const float mid = 0.5f * (high + outside), lowQ = outside + 0.25f * (high - outside), highQ = outside + 0.75f * (high - outside);
		bool got = false;
		double sLow = 0, sHigh = 0;
		for (int k = clampIndex(kEdge - 3 * perPixel); k < clampIndex(kEdge + 3 * perPixel); k++)
		{
			if (q[k] < mid && q[k + 1] >= mid && !got)
			{
				e.sOut = crossing(q, k, mid, s0);
				got = true;
			}
			if (q[k] < lowQ && q[k + 1] >= lowQ)
				sLow = crossing(q, k, lowQ, s0);
			if (q[k] < highQ && q[k + 1] >= highQ && sHigh == 0)
				sHigh = crossing(q, k, highQ, s0);
		}
		if (!got)
			return false;
		if (sHigh > sLow && sLow != 0)
			width = std::clamp(sHigh - sLow, 0.5, 6.0);
		e.width = width;
	}
	// The inner edge: the first sample past the border's peak that is clearly
	// off its level, within the thickness the border can have.
	const float tol = std::max(0.2f * (high - outside), 0.5f * (float)minContrast);
	const int kStop = std::min(n - 1, index(e.sOut + wMax));
	int kLeave = -1;
	for (int k = kPeak; k <= kStop; k++)
		if (std::fabs(q[k] - high) > tol)
		{
			kLeave = k;
			break;
		}
	if (kLeave < 0)
		return true;	// the picture is as bright as the border here
	const bool falling = q[kLeave] < high;
	// The picture's level just past the edge, the edge's width on from it.
	const int kFrom = std::min(n - 1, kLeave + (int)std::lround((0.5 + 1.2 * width) / STEP));
	const int kTo = std::min(n - 1, kFrom + perPixel);
	float beyond = 0;
	for (int k = kFrom; k <= kTo; k++)
		beyond += q[k];
	beyond /= kTo - kFrom + 1;
	if (std::fabs(high - beyond) < std::max(0.75f * (float)minContrast, 0.25f * (high - outside)))
		return true;
	if (clipped)
	{
		auto slope = [&](int k) { return falling ? q[k - 1] - q[k + 1] : q[k + 1] - q[k - 1]; };
		int kBest = -1;
		float best = 0;
		for (int k = std::max(kPeak + 1, kLeave - perPixel); k <= std::min(n - 3, kLeave + 3 * perPixel); k++)
			if (slope(k) > best)
			{
				best = slope(k);
				kBest = k;
			}
		if (kBest < 2)
			return true;
		e.sIn = slopePeak(slope, kBest, s0);
	}
	else
	{
		const float mid = 0.5f * (high + beyond);
		bool got = false;
		for (int k = kPeak; k < std::min(n - 1, kFrom); k++)
			if (falling ? (q[k] >= mid && q[k + 1] < mid) : (q[k] <= mid && q[k + 1] > mid))
			{
				e.sIn = crossing(q, k, mid, s0);
				got = true;
				break;
			}
		if (!got)
			return true;
	}
	e.in = e.sIn - e.sOut >= wMin;
	return true;
}

// Where the segment p + s d, s in [lo, hi], is inside the frame (less margin).
bool clipToFrame(const Image& im, V2 p, V2 d, double margin, double& lo, double& hi)
{
	const double box[4] = { margin, margin, im.w - 1 - margin, im.h - 1 - margin };
	for (int axis = 0; axis < 2; axis++)
	{
		const double pa = axis == 0 ? p.x : p.y, da = axis == 0 ? d.x : d.y;
		const double mn = box[axis], mx = box[axis + 2];
		if (std::fabs(da) < 1e-12)
		{
			if (pa < mn || pa > mx)
				return false;
			continue;
		}
		double t0 = (mn - pa) / da, t1 = (mx - pa) / da;
		if (t0 > t1)
			std::swap(t0, t1);
		lo = std::max(lo, t0);
		hi = std::min(hi, t1);
	}
	return lo < hi;
}

struct SideEdges
{
	std::vector<V2> outer, inner;	// as seen
	std::vector<double> band;	// pixels
	std::vector<float> level;	// the border's level at each outer edge point
	std::vector<float> outside;	// and the level just outside it
	std::vector<double> width;	// how wide the outer edge's rise is
	int profiles = 0;
};

// Profiles across one side, between its corners (less a margin at each corner
// that is one, so a profile does not cross the next side's border).
void measureSide(const Image& im, const Lens& lens, const Line& line, V2 a, V2 b, double marginA, double marginB,
		double win, double wMin, double wMax, double minContrast, float level, std::vector<float>& prof, SideEdges& out)
{
	out = SideEdges();
	const double len = length(b - a);
	if (len < 4)
		return;
	const V2 u = (b - a) * (1 / len);
	const double from = marginA, to = len - marginB;
	if (to - from < 4)
		return;
	const double spacing = std::max(1.0, (to - from) / 100);
	const double sLo = -(win + 5), sHi = win + wMax + 5;
	for (double t = from; t <= to; t += spacing)
	{
		const V2 onLine = a + u * t;
		const V2 q = lens.distort(onLine);
		const V2 tangent = unit(lens.distort(onLine + u) - q);
		V2 inward{ -tangent.y, tangent.x };
		if (dot(inward, line.n) > 0)
			inward = inward * -1;
		double lo = sLo, hi = sHi;
		if (!clipToFrame(im, q, inward, 0.5, lo, hi))
			continue;
		if (lo > -(win + 1.5) || hi < std::min(sHi, win + wMin + 3))
			continue;
		const double s0 = sLo + STEP * std::ceil((lo - sLo) / STEP - 1e-9);
		const int n = (int)std::floor((hi - s0) / STEP) + 1;
		if (n < 8)
			continue;
		prof.resize(n);
		for (int k = 0; k < n; k++)
			prof[k] = im.at(q + inward * (s0 + k * STEP));
		out.profiles++;
		Edges e;
		if (!readProfile(prof.data(), n, s0, win, wMin, wMax, minContrast, level, e))
			continue;
		out.outer.push_back(q + inward * e.sOut);
		out.level.push_back(e.level);
		out.outside.push_back(e.outside);
		if (e.width > 0)
			out.width.push_back(e.width);
		if (e.in)
		{
			out.inner.push_back(q + inward * e.sIn);
			out.band.push_back(e.sIn - e.sOut);
		}
	}
}

struct SideFit
{
	Line line;
	bool ok = false;
	int inliers = 0;
	double rms = 0;
	std::vector<char> inlier;
};

// A line through the points that ignores those far off it: the best of a few
// lines through pairs of points, then iterated with Tukey's weights.
bool robustFit(const std::vector<V2>& p, const Line* guess, Random& rng, std::vector<double>& w, SideFit& f, bool fromGuess = false)
{
	f = SideFit();
	const int n = (int)p.size();
	if (n < 5)
		return false;
	Line best;
	int bestCount = -1;
	auto countNear = [&](const Line& l, double tol) {
		int count = 0;
		for (const V2& q : p)
			count += std::fabs(signedDistance(l, q)) <= tol;
		return count;
	};
	if (guess)
	{
		best = *guess;
		bestCount = countNear(best, 0.75);
	}
	for (int it = 0; it < (fromGuess && guess ? 0 : 40); it++)
	{
		const int i = rng.below(n), j = rng.below(n);
		if (std::abs(i - j) < n / 4)
			continue;
		const Line l = through(p[i], p[j]);
		const int count = countNear(l, 0.75);
		if (count > bestCount)
		{
			bestCount = count;
			best = l;
		}
	}
	if (bestCount < 2)
		return false;
	w.resize(n);
	Line l = best;
	double scale = 0.3;
	for (int it = 0; it < 6; it++)
	{
		std::vector<double> absNear;
		absNear.reserve(n);
		for (int i = 0; i < n; i++)
		{
			const double r = std::fabs(signedDistance(l, p[i]));
			if (r < 2)
				absNear.push_back(r);
		}
		if (absNear.size() < 3)
			return false;
		scale = std::max(0.04, 1.4826 * median(absNear));
		const double c = std::clamp(4 * scale, 0.6, 2.0);
		for (int i = 0; i < n; i++)
		{
			const double r = signedDistance(l, p[i]) / c;
			w[i] = std::fabs(r) < 1 ? (1 - r * r) * (1 - r * r) : 0;
		}
		Line next;
		if (!fitLine(p.data(), w.data(), n, next))
			return false;
		if (dot(next.n, l.n) < 0)
		{
			next.n = next.n * -1;
			next.d = -next.d;
		}
		l = next;
	}
	f.line = l;
	f.inlier.assign(n, 0);
	const double tol = std::max(3 * scale, 0.5);
	double sq = 0;
	for (int i = 0; i < n; i++)
	{
		const double r = signedDistance(l, p[i]);
		if (std::fabs(r) <= tol)
		{
			f.inlier[i] = 1;
			f.inliers++;
			sq += r * r;
		}
	}
	f.rms = f.inliers ? std::sqrt(sq / f.inliers) : 0;
	f.ok = f.inliers >= 5;
	return f.ok;
}

// The projective map from the unit square ((0,0) top-left, (1,0) top-right,
// (1,1) bottom-right, (0,1) bottom-left) to four corners (Heckbert's).
struct Homography
{
	double m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

	bool apply(V2 p, V2& out) const
	{
		const double z = m[6] * p.x + m[7] * p.y + m[8];
		if (std::fabs(z) < 1e-12)
			return false;
		out = { (m[0] * p.x + m[1] * p.y + m[2]) / z, (m[3] * p.x + m[4] * p.y + m[5]) / z };
		return true;
	}
};

bool squareToQuad(const V2 q[4], Homography& h)
{
	const double sx = q[0].x - q[1].x + q[2].x - q[3].x, sy = q[0].y - q[1].y + q[2].y - q[3].y;
	double g = 0, k = 0;
	if (std::fabs(sx) > 1e-12 || std::fabs(sy) > 1e-12)
	{
		const double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x, dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
		const double den = dx1 * dy2 - dx2 * dy1;
		if (std::fabs(den) < 1e-12)
			return false;
		g = (sx * dy2 - dx2 * sy) / den;
		k = (dx1 * sy - sx * dy1) / den;
	}
	h.m[0] = q[1].x - q[0].x + g * q[1].x;
	h.m[1] = q[3].x - q[0].x + k * q[3].x;
	h.m[2] = q[0].x;
	h.m[3] = q[1].y - q[0].y + g * q[1].y;
	h.m[4] = q[3].y - q[0].y + k * q[3].y;
	h.m[5] = q[0].y;
	h.m[6] = g;
	h.m[7] = k;
	h.m[8] = 1;
	return true;
}

bool invert(const Homography& h, Homography& inv)
{
	const double* m = h.m;
	const double a = m[4] * m[8] - m[5] * m[7], b = m[5] * m[6] - m[3] * m[8], c = m[3] * m[7] - m[4] * m[6];
	const double det = m[0] * a + m[1] * b + m[2] * c;
	if (std::fabs(det) < 1e-15)
		return false;
	const double r = 1 / det;
	inv.m[0] = a * r;
	inv.m[1] = (m[2] * m[7] - m[1] * m[8]) * r;
	inv.m[2] = (m[1] * m[5] - m[2] * m[4]) * r;
	inv.m[3] = b * r;
	inv.m[4] = (m[0] * m[8] - m[2] * m[6]) * r;
	inv.m[5] = (m[2] * m[3] - m[0] * m[5]) * r;
	inv.m[6] = c * r;
	inv.m[7] = (m[1] * m[6] - m[0] * m[7]) * r;
	inv.m[8] = (m[0] * m[4] - m[1] * m[3]) * r;
	return true;
}

using V3 = std::array<double, 3>;
V3 cross3(const V3& a, const V3& b)
{
	return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
double dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 unit3(const V3& a)
{
	const double l = std::sqrt(dot3(a, a));
	return l > 0 ? V3{ a[0] / l, a[1] / l, a[2] / l } : a;
}

// A side out of the camera's view, from the other three: the two sides that
// run towards it meet at their vanishing point, which gives their direction in
// space; the opposite side's direction is square to it in the plane its line
// sees; the picture's shape then says how far the hidden side is.
bool placeSide(const Line inner[4], int missing, V2 c, double focal, double aspect, Line& out)
{
	const int opp = (missing + 2) % 4, a1 = (missing + 1) % 4, a2 = (missing + 3) % 4;
	auto hom = [](const Line& l) { return V3{ l.n.x, l.n.y, -l.d }; };
	const V3 v = cross3(hom(inner[a1]), hom(inner[a2]));
	V3 dir = unit3({ (v[0] - c.x * v[2]) / focal, (v[1] - c.y * v[2]) / focal, v[2] });
	V2 c1, c2;
	if (!meet(inner[opp], inner[a1], c1) || !meet(inner[opp], inner[a2], c2))
		return false;
	const V3 r1{ (c1.x - c.x) / focal, (c1.y - c.y) / focal, 1 }, r2{ (c2.x - c.x) / focal, (c2.y - c.y) / focal, 1 };
	// Towards the hidden side: its image moves away from the opposite side.
	const V2 moves{ dir[0] - r1[0] * dir[2], dir[1] - r1[1] * dir[2] };
	if (dot(moves, inner[opp].n) > 0)
		dir = { -dir[0], -dir[1], -dir[2] };
	const Line& o = inner[opp];
	const V3 seen{ focal * o.n.x, focal * o.n.y, c.x * o.n.x + c.y * o.n.y - o.d };
	const V3 e = unit3(cross3(seen, dir));
	// r2 * l2 - e * m = r1, by least squares.
	const double aa = dot3(r2, r2), ab = -dot3(r2, e), bb = dot3(e, e), ar = dot3(r2, r1), br = -dot3(e, r1);
	const double det = aa * bb - ab * ab;
	if (std::fabs(det) < 1e-12)
		return false;
	const double l2 = (ar * bb - ab * br) / det;
	if (l2 <= 0)
		return false;
	const V3 x1 = r1, x2{ r2[0] * l2, r2[1] * l2, r2[2] * l2 };
	const V3 side{ x2[0] - x1[0], x2[1] - x1[1], x2[2] - x1[2] };
	const double oppLength = std::sqrt(dot3(side, side));
	const double ratio = (missing == LEFT || missing == RIGHT) ? aspect : 1 / aspect;
	const double across = ratio * oppLength;
	const V3 f1{ x1[0] + dir[0] * across, x1[1] + dir[1] * across, x1[2] + dir[2] * across };
	const V3 f2{ x2[0] + dir[0] * across, x2[1] + dir[1] * across, x2[2] + dir[2] * across };
	if (f1[2] <= 1e-6 || f2[2] <= 1e-6)
		return false;
	const V2 p1{ c.x + focal * f1[0] / f1[2], c.y + focal * f1[1] / f1[2] };
	const V2 p2{ c.x + focal * f2[0] / f2[2], c.y + focal * f2[1] / f2[2] };
	if (length(p2 - p1) < 2)
		return false;
	out = facingAway(through(p1, p2), (c1 + c2) * 0.5);
	return true;
}

// The corner after side s (clockwise): top-right after the top, and so on.
// Output order is top-left, top-right, bottom-right, bottom-left.
constexpr int CORNER_OUT[4] = { 1, 2, 3, 0 };

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// The screen's outline at the best of the thresholds: candidates whose lines
// are as seen (with the lens's distortion), the best first, at most one for
// each threshold.
int findScreen(const Image& im, const Options& o, Hints& hints, Workspace& ws, Candidate* found, int most)
{
	// The frame's dark and bright levels (every other pixel), for the
	// thresholds: they follow the camera's exposure.
	uint32_t hist[256] = {};
	uint32_t total = 0;
	for (int y = 0; y < im.h; y += 2)
	{
		const uint8_t* row = im.row(y);
		for (int x = 0; x < im.w; x += 2)
			hist[row[x]]++;
		total += (im.w + 1) / 2;
	}
	auto percentile = [&](double q) {
		const uint64_t rank = std::max<uint64_t>(1, (uint64_t)std::ceil(q * total));
		uint64_t sum = 0;
		for (int v = 0; v < 256; v++)
		{
			sum += hist[v];
			if (sum >= rank)
				return v;
		}
		return 255;
	};
	const int dark = percentile(0.05), bright = percentile(0.999);
	if (bright - dark < o.minContrast)
		return 0;
	// The bright parts at each threshold, the three largest read as the border;
	// the threshold of the last frame first.
	const double minSide = std::max(12.0, 0.07 * std::min(im.w, im.h));
	int order[FRACTION_COUNT], tries = 0;
	if (hints.fraction >= 0 && hints.fraction < FRACTION_COUNT)
		order[tries++] = hints.fraction;
	for (int f = 0; f < FRACTION_COUNT; f++)
		if (f != hints.fraction)
			order[tries++] = f;
	int count = 0;
	for (int t = 0; t < tries; t++)
	{
		const int threshold = std::max(dark + 1, (int)std::lround(dark + FRACTIONS[order[t]] * (bright - dark)));
		labelParts(im, threshold, ws);
		int pick[3] = { -1, -1, -1 };
		double area[3] = { 0, 0, 0 };
		for (int i = 0; i < (int)ws.parts.size(); i++)
		{
			const Part& p = ws.parts[i];
			const double bw = p.maxX - p.minX + 1, bh = p.maxY - p.minY + 1;
			if (bw < minSide || bh < minSide || p.count < 4 * minSide)
				continue;
			const double a = bw * bh;
			for (int s = 0; s < 3; s++)
				if (a > area[s])
				{
					for (int m = 2; m > s; m--)
					{
						area[m] = area[m - 1];
						pick[m] = pick[m - 1];
					}
					area[s] = a;
					pick[s] = i;
					break;
				}
		}
		Candidate best;
		for (int s = 0; s < 3; s++)
		{
			Candidate c;
			if (pick[s] < 0 || !evaluatePart(im, ws, pick[s], o, hints, c))
				continue;
			if (c.score > best.score)
			{
				best = c;
				best.threshold = threshold;
				best.fraction = order[t];
			}
		}
		if (best.score > 0.02 && (count < most || best.score > found[most - 1].score))
		{
			int at = std::min(count, most - 1);
			while (at > 0 && found[at - 1].score < best.score)
			{
				if (at < most)
					found[at] = found[at - 1];
				at--;
			}
			found[at] = best;
			count = std::min(count + 1, most);
		}
		if (best.clean)
			break;
	}
	return count;
}

// The thinnest of the thicknesses the border is seen at often (a quarter as
// often as the most common at least): where the picture beside the border is
// darker than it. Where the picture is as bright the border reads thicker;
// the scatter of noise and blur is round the true one, not under it.
// Values are taken as alike within 12% of the median, and at least within
// least of each other.
double lowestMode(std::vector<double>& v, double least)
{
	const int n = (int)v.size();
	if (n < 6)
		return 0;
	std::sort(v.begin(), v.end());
	const double reach = std::max(least, 0.12 * v[n / 2]);
	std::vector<int> near(n);
	int most = 0;
	for (int i = 0, lo = 0, hi = 0; i < n; i++)
	{
		while (v[lo] < v[i] - reach)
			lo++;
		while (hi + 1 < n && v[hi + 1] <= v[i] + reach)
			hi++;
		near[i] = hi - lo + 1;
		most = std::max(most, near[i]);
	}
	for (int i = 0; i < n; i++)
	{
		if (4 * near[i] < most)
			continue;
		// The densest point within reach of the first dense one.
		int at = i;
		for (int j = i + 1; j < n && v[j] <= v[i] + reach; j++)
			if (near[j] > near[at])
				at = j;
		double sum = 0;
		int count = 0;
		for (int j = 0; j < n; j++)
			if (std::fabs(v[j] - v[at]) <= reach)
			{
				sum += v[j];
				count++;
			}
		return sum / count;
	}
	return v[n / 2];
}

// The screen's sides measured along profiles across them.
struct Sides
{
	Line outer[4];
	bool has[4] = {};
	SideFit outerFit[4], innerFit[4];
	SideEdges seen[4];
	double band[4] = {}, wLo[4] = {}, wHi[4] = {};
	V2 middle[4];
	V2 inside;
};

int axisOf(int side) { return side == TOP || side == BOTTOM ? 0 : 1; }

// The outline's sides, out of the lens's distortion, measured twice: the
// second time along the first's lines, with the border's level and thickness
// it saw. Then the checks that each is the border's own side: where one is an
// edge of the picture's, it is dropped, or moved out to the true side.
void measureSides(const Image& im, const Lens& lens, const Options& o, const Candidate& best, Workspace& ws,
		const double tauExpected[4], Sides& sd)
{
	// A point inside the picture, to say which way the sides face: the
	// outline's middle, or with a side out of view the bright part's.
	sd.inside = lens.undistort(best.centre);
	{
		V2 sum, p;
		int count = 0;
		for (int s = 0; s < 4; s++)
			if (best.has[s] && best.has[(s + 1) % 4] && meet(best.lines[s], best.lines[(s + 1) % 4], p))
			{
				sum = sum + p;
				count++;
			}
		if (count == 4)
			sd.inside = lens.undistort(sum * 0.25);
	}
	for (int s = 0; s < 4; s++)
	{
		sd.has[s] = best.has[s];
		if (!sd.has[s])
			continue;
		const Line& l = best.lines[s];
		const V2 foot = l.n * l.d;
		const V2 base = foot + along(l) * dot(along(l), lens.c - foot);
		sd.outer[s] = facingAway(through(lens.undistort(base - along(l) * 40), lens.undistort(base + along(l) * 40)), sd.inside);
	}
	// The frame's bounds without the distortion (barrel widens it most at its corners).
	double box[4] = { 1e9, 1e9, -1e9, -1e9 };
	for (const V2 corner : { V2{ 0, 0 }, V2{ (double)im.w - 1, 0 }, V2{ (double)im.w - 1, (double)im.h - 1 }, V2{ 0, (double)im.h - 1 } })
	{
		const V2 u = lens.undistort(corner);
		box[0] = std::min(box[0], u.x);
		box[1] = std::min(box[1], u.y);
		box[2] = std::max(box[2], u.x);
		box[3] = std::max(box[3], u.y);
	}
	const Image bounds{ nullptr, (int)std::ceil(box[2] - box[0]) + 1, (int)std::ceil(box[3] - box[1]) + 1, 0 };
	Random rng;
	// One side, along its line as it stands, between its corners with the
	// sides next to it as they stand.
	auto measure = [&](int s, int pass) {
		V2 corner[4];
		bool cornerOk[4];
		for (int k = 0; k < 4; k++)
			cornerOk[k] = sd.has[k] && sd.has[(k + 1) % 4] && meet(sd.outer[k], sd.outer[(k + 1) % 4], corner[k]);
		{
			const Line l = sd.outer[s];
			const V2 dir = along(l);
			const V2 foot = l.n * l.d;
			// Where the line leaves the frame's bounds, for a side whose
			// neighbour is out of view.
			double lo = -1e6, hi = 1e6;
			if (!clipToFrame(bounds, foot - V2{ box[0], box[1] }, dir, -2, lo, hi))
			{
				sd.has[s] = false;
				return;
			}
			const int before = (s + 3) % 4;
			const V2 a = cornerOk[before] ? corner[before] : foot + dir * lo;
			const V2 b = cornerOk[s] ? corner[s] : foot + dir * hi;
			if (dot(b - a, dir) <= 0)
			{
				sd.has[s] = false;
				return;
			}
			sd.middle[s] = (a + b) * 0.5;
			// How thick the border can be here: as the first pass saw it,
			// as expected, or up to an eighth of the way across.
			const int opposite = (s + 2) % 4;
			const double acrossPx = sd.has[opposite] ? std::fabs(signedDistance(sd.outer[opposite], sd.middle[s])) : 0;
			double wMin = 0.5, wMax = acrossPx > 0 ? 0.08 * acrossPx + 2 : 20;
			// A blurred edge (motion smears the border across the way the gun
			// moves) widens the border as seen by about twice its rise.
			double blur = 0;
			if (pass == 1 && sd.seen[s].width.size() >= 6)
			{
				std::vector<double>& widths = ws.values;
				widths = sd.seen[s].width;
				blur = std::max(0.0, median(widths) - 1.5);
			}
			if (pass == 1 && sd.band[s] > 0)
			{
				wMin = 0.6 * sd.band[s];
				wMax = 1.5 * sd.band[s] + 1.5;
			}
			else if (tauExpected[s] > 0 && acrossPx > 0)
			{
				wMin = 0.45 * tauExpected[s] * acrossPx;
				wMax = 1.8 * tauExpected[s] * acrossPx + 1.5 + 2 * blur;
			}
			else if (best.band > 0)
				wMax = std::min(wMax, 2.2 * best.band + 3) + 2 * blur;
			sd.wLo[s] = wMin;
			sd.wHi[s] = wMax;
			const double margin = std::max(3.0, (best.band > 0 ? std::min(best.band, wMax) : 4) + 3);
			// The border's level on this side, from the first pass: the low
			// quarter of the levels its profiles reached, since the picture
			// past a thin border can only raise them.
			float level = 0;
			if (pass == 1 && sd.seen[s].level.size() >= 6)
			{
				std::vector<float> levels(sd.seen[s].level);
				std::nth_element(levels.begin(), levels.begin() + levels.size() / 4, levels.end());
				level = levels[levels.size() / 4];
			}
			measureSide(im, lens, l, a, b, cornerOk[before] ? margin : 0, cornerOk[s] ? margin : 0, pass == 0 ? 5 : 2.5, wMin, wMax,
					o.minContrast, level, ws.profile, sd.seen[s]);
			ws.undistorted.clear();
			for (const V2& q : sd.seen[s].outer)
				ws.undistorted.push_back(lens.undistort(q));
			if (robustFit(ws.undistorted, &l, rng, ws.values, sd.outerFit[s]))
			{
				sd.outer[s] = facingAway(sd.outerFit[s].line, sd.inside);
				// Too few edge points for its length: not the border's side.
				if (sd.outerFit[s].inliers < std::max(6, sd.seen[s].profiles / 4))
					sd.outerFit[s].ok = false;
			}
			// The border's inner edge: the picture beside it can only make the
			// border look thicker (where it is as bright), never thinner, so
			// the fit starts from the thin end of the thicknesses seen.
			std::vector<double>& band = ws.values;
			band = sd.seen[s].band;
			const double thin = lowestMode(band, 0.5);
			sd.band[s] = thin;
			ws.undistorted.clear();
			for (const V2& q : sd.seen[s].inner)
				ws.undistorted.push_back(lens.undistort(q));
			Line guess = sd.outer[s];
			guess.d -= thin > 0 ? thin : 0.5 * (wMin + wMax);
			robustFit(ws.undistorted, &guess, rng, ws.values, sd.innerFit[s], thin > 0);
			if (sd.innerFit[s].ok)
			{
				sd.innerFit[s].line = facingAway(sd.innerFit[s].line, sd.inside);
				if (sd.innerFit[s].inliers < std::max(6, (int)sd.seen[s].outer.size() / 4))
					sd.innerFit[s].ok = false;
			}
		}
	};
	for (int pass = 0; pass < 2; pass++)
	{
		for (int s = 0; s < 4; s++)
			if (sd.has[s])
				measure(s, pass);
		for (int s = 0; s < 4; s++)
			if (sd.has[s] && !sd.outerFit[s].ok)
				sd.has[s] = false;
	}
	// The border is a closed band: past a corner of its outline, each side's
	// band ends (outside the screen it is dark). Where a side's band runs on
	// past a corner, the side that meets it there is an edge of the
	// picture's own that the outline took for the border.
	float in[4] = {}, out[4] = {};
	for (int s = 0; s < 4; s++)
	{
		if (!sd.has[s] || sd.seen[s].level.empty())
			continue;
		std::vector<float> v(sd.seen[s].level);
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		in[s] = v[v.size() / 2];
		v = sd.seen[s].outside;
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		out[s] = v[v.size() / 2];
	}
	// And the border stands clearly above what is outside it; an edge in the
	// picture between a bright area and a grey one does not.
	bool fake[4] = {};
	for (int s = 0; s < 4; s++)
		fake[s] = sd.has[s] && in[s] > 0 && in[s] - out[s] < 0.35f * in[s];
	for (int s = 0; s < 4; s++)
	{
		const int next = (s + 1) % 4;
		V2 corner;
		if (!sd.has[s] || !sd.has[next] || !meet(sd.outer[s], sd.outer[next], corner))
			continue;
		// Side s's band beyond the corner, and the next side's band back
		// beyond it.
		for (int which = 0; which < 2; which++)
		{
			const int side = which == 0 ? s : next;
			const V2 dir = which == 0 ? along(sd.outer[s]) : along(sd.outer[next]) * -1;
			// The band's peak across its thickness (a thin border is
			// dimmed by blur at any one depth), lit when three quarters of
			// the way from what is outside the screen there (beyond the
			// other side: the brighter of the two outsides) to the border.
			const int across = (side + 2) % 4, other = which == 0 ? next : s;
			double thick = sd.band[side];
			if (tauExpected[side] > 0 && sd.has[across])
				thick = tauExpected[side] * std::fabs(signedDistance(sd.outer[across], corner));
			const double deepest = std::clamp(thick, 1.5, 6.0);
			const float base = std::max(out[side], out[other]);
			const float bright = base + 0.75f * (in[side] - base);
			int seen = 0, lit = 0;
			// Too little between them to tell (a dim border, a lit wall).
			const bool clear = in[side] - base >= 0.5f * (in[side] - out[side]);
			for (double d = 3; d <= 10 && clear; d += 1)
			{
				float peak = 0;
				bool inside = true;
				for (double depth = 0.5; depth <= deepest && inside; depth += 0.5)
				{
					const V2 p = lens.distort(corner + dir * d - sd.outer[side].n * depth);
					inside = p.x >= 1 && p.y >= 1 && p.x <= im.w - 2 && p.y <= im.h - 2;
					if (inside)
						peak = std::max(peak, im.at(p));
				}
				if (!inside)
					continue;
				seen++;
				lit += peak >= bright;
			}
			if (seen >= 4 && 2 * lit > seen)
				fake[which == 0 ? next : s] = true;
		}
	}
	for (int s = 0; s < 4; s++)
		if (fake[s])
			sd.has[s] = false;

	// The border is the outermost band: past a side that is an edge of the
	// picture's own, the true side's band lies further out, as bright as the
	// border on the other sides, no thicker than the border can be, with the
	// dark of the bezel beyond it. Found along most of the side, the side
	// moves out to it and is measured again.
	float levels[4], darkest = 1e9f;
	int counted = 0;
	for (int s = 0; s < 4; s++)
		if (sd.has[s] && in[s] > 0)
		{
			levels[counted++] = in[s];
			darkest = std::min(darkest, out[s]);
		}
	if (counted >= 3)
	{
		for (int i = 1; i < counted; i++)	// (four at most: by hand)
			for (int j = i; j > 0 && levels[j] < levels[j - 1]; j--)
				std::swap(levels[j], levels[j - 1]);
		const float border = levels[counted / 2];
		const float bright = darkest + 0.75f * (border - darkest), dark = darkest + 0.3f * (border - darkest), half = 0.5f * (bright + dark);
		for (int s = 0; s < 4; s++)
		{
			const int before = (s + 3) % 4, opposite = (s + 2) % 4;
			V2 a, b;
			if (!sd.has[s] || !sd.has[before] || !sd.has[(s + 1) % 4] || !sd.has[opposite] || !meet(sd.outer[before], sd.outer[s], a)
					|| !meet(sd.outer[s], sd.outer[(s + 1) % 4], b))
				continue;
			const double len = length(b - a);
			const double across = std::fabs(signedDistance(sd.outer[opposite], (a + b) * 0.5));
			if (len < 16 || across < 8)
				continue;
			const double thick = tauExpected[s] > 0 ? tauExpected[s] * across : sd.band[s] > 0 ? sd.band[s] : 0.06 * across;
			const double widest = 2.5 * thick + 3, reach = std::clamp(0.3 * across, 6.0, 60.0);
			const V2 u = (b - a) * (1 / len), n = sd.outer[s].n;
			std::vector<V2>& found = ws.undistorted;
			found.clear();
			int profiles = 0;
			for (double t = 4; t <= len - 4; t += 2)
			{
				const V2 p0 = a + u * t;
				double start = -1;
				bool done = false, inFrame = true;
				for (double x = 2; x <= reach && !done; x += STEP)
				{
					const V2 q = lens.distort(p0 + n * x);
					if (q.x < 0.5 || q.y < 0.5 || q.x > im.w - 1.5 || q.y > im.h - 1.5)
					{
						inFrame = x > 4;
						break;
					}
					const float v = im.at(q);
					if (start < 0)
					{
						if (v >= bright)
							start = x;
					}
					else if (v <= dark)
					{
						// Out of the band: its outer edge is where it fell
						// through half way, a step or so back.
						const float last = im.at(lens.distort(p0 + n * (x - STEP)));
						double edge = x - STEP;
						if (last > v)
							edge = x - STEP + STEP * std::clamp((double)(last - half) / (last - v), 0.0, 1.0);
						if (x - start <= widest)
							found.push_back(p0 + n * edge);
						done = true;
					}
					else if (x - start > widest)
						done = true;	// a wide bright area: not a band
				}
				profiles += inFrame;
			}
			if ((int)found.size() < std::max(6, profiles / 2))
				continue;
			SideFit fit;
			if (!robustFit(found, nullptr, rng, ws.values, fit))
				continue;
			const Line moved = facingAway(fit.line, sd.inside);
			if (dot(moved.n, n) < std::cos(15 * PI / 180) || signedDistance(sd.outer[s], (a + b) * 0.5 - moved.n * signedDistance(moved, (a + b) * 0.5)) < 2)
				continue;
			sd.outer[s] = moved;
			measure(s, 0);
			measure(s, 1);
			if (!sd.outerFit[s].ok)
				sd.has[s] = false;
		}
	}
}

// Whether a side's inner edge agrees with its outer edge: nearly parallel,
// at a thickness the border can have, at both ends.
bool innerAgrees(const Sides& sd, int s)
{
	if (!sd.has[s] || !sd.innerFit[s].ok)
		return false;
	const Line& in = sd.innerFit[s].line;
	if (dot(in.n, sd.outer[s].n) < std::cos(3 * PI / 180))
		return false;
	const V2 dir = along(sd.outer[s]);
	for (const double t : { -40.0, 0.0, 40.0 })
	{
		const V2 p = sd.middle[s] + dir * t;
		const V2 onInner = p - in.n * signedDistance(in, p);
		const double inward = -signedDistance(sd.outer[s], onInner);
		if (inward < 0.5 * sd.wLo[s] || inward > sd.wHi[s] + 2)
			return false;
	}
	return true;
}

// The picture's four sides from the measured ones. With the whole outline
// seen, each comes from its outer edge (measured against the dark round the
// screen, the cleanest edge there is) and the border's thickness: told,
// learnt, or seen on the inner edges in this frame. Otherwise from its inner
// edge. One side out of view is placed from the other three.
struct Picture
{
	Line inner[4];
	SideSource src[4] = {};
	V2 outerCorner[4];
	bool outerOk = false;
	double tau[4] = {}, tauSeen[4] = {};
};

bool placePicture(const Image& im, const Lens& lens, const Options& o, const Sides& sd, const double told[4], const double learnt[4],
		Picture& pic)
{
	bool innerOk[4];
	for (int s = 0; s < 4; s++)
		innerOk[s] = innerAgrees(sd, s);
	pic.outerOk = sd.has[0] && sd.has[1] && sd.has[2] && sd.has[3];
	for (int s = 0; s < 4 && pic.outerOk; s++)
		pic.outerOk = meet(sd.outer[s], sd.outer[(s + 1) % 4], pic.outerCorner[s]);
	Homography outerMap, outerBack;
	if (pic.outerOk)
	{
		const V2 q[4] = { pic.outerCorner[3], pic.outerCorner[0], pic.outerCorner[1], pic.outerCorner[2] };
		pic.outerOk = squareToQuad(q, outerMap) && invert(outerMap, outerBack);
	}
	if (pic.outerOk)
	{
		// The border's thickness seen on each side, as a fraction of the
		// outline across: the same on opposite sides however the screen is
		// seen, since the homography takes the perspective out.
		for (int s = 0; s < 4; s++)
		{
			if (!innerOk[s])
				continue;
			const V2 mid = (pic.outerCorner[(s + 3) % 4] + pic.outerCorner[s]) * 0.5;
			const Line& in = sd.innerFit[s].line;
			V2 uv;
			if (!outerBack.apply(mid - in.n * signedDistance(in, mid), uv))
				continue;
			pic.tauSeen[s] = s == TOP ? uv.y : s == BOTTOM ? 1 - uv.y : s == LEFT ? uv.x : 1 - uv.x;
		}
		// A side seen much thicker or thinner than expected, or than its
		// opposite, is the picture's own edge beside the border. A clipped
		// border (the exposure too high) reads thicker than it is.
		// A blur that smears the border wider dims it as much: a side whose
		// border is dimmer than the brightest side's may be that much
		// thicker.
		float level[4] = {}, brightest = 0;
		for (int s = 0; s < 4; s++)
			if (!sd.seen[s].level.empty())
			{
				std::vector<float> v(sd.seen[s].level);
				std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
				level[s] = v[v.size() / 2];
				brightest = std::max(brightest, level[s]);
			}
		for (int s = 0; s < 4; s++)
		{
			const double ref = told[s] > 0 ? told[s] : learnt[s];
			int clipped = 0;
			for (const float l : sd.seen[s].level)
				clipped += l >= 250;
			const double dimmed = level[s] > 0 ? std::clamp(brightest / level[s], 1.0f, 3.0f) : 1.0;
			const double most = (2 * clipped > (int)sd.seen[s].level.size() ? 2.4 : 1.8) * dimmed * ref;
			if (pic.tauSeen[s] > 0 && ref > 0 && (pic.tauSeen[s] < 0.45 * ref || pic.tauSeen[s] > most))
				pic.tauSeen[s] = 0;
		}
		// Nothing told or learnt: the thinnest side seen is the border (the
		// display draws it equally thick on every side, so the two axes
		// compare through the picture's shape); a side much thicker is not.
		{
			double byOf[4] = {}, thinnest = 1e9;
			for (int s = 0; s < 4; s++)
				if (pic.tauSeen[s] > 0 && told[s] <= 0 && learnt[s] <= 0)
				{
					const double fraction = pic.tauSeen[s] / (1 - 2 * pic.tauSeen[s]);
					byOf[s] = axisOf(s) == 0 ? fraction : fraction * o.pictureAspect;
					thinnest = std::min(thinnest, byOf[s]);
				}
			for (int s = 0; s < 4; s++)
				if (byOf[s] > 1.4 * thinnest)
					pic.tauSeen[s] = 0;
		}
		// The thickness to take in from the outer edge. The outer edge less
		// the told thickness and the inner edge seen are two measures of the
		// same line: blur and clipping move them apart by the same (both of
		// the border's edges out from its middle), and the inner one also
		// scatters with the picture beside it. Weighted a third to the seen
		// one: measured on the test's frames against half (their mean,
		// which cancels the blur exactly) and none (as told), a third had
		// the least error in most conditions. The same with what the Tracker
		// learnt when nothing is told. Not seen: as learnt, or as told
		// corrected as the opposite side was, or as the opposite side.
		for (int s = 0; s < 4; s++)
		{
			const double ref = told[s] > 0 ? told[s] : learnt[s];
			if (pic.tauSeen[s] > 0)
				pic.tau[s] = ref > 0 ? ref + (pic.tauSeen[s] - ref) / 3 : pic.tauSeen[s];
		}
		for (int s = 0; s < 4; s++)
		{
			if (pic.tau[s] > 0)
				continue;
			const int opp = (s + 2) % 4;
			if (learnt[s] > 0)
				pic.tau[s] = learnt[s];
			else if (told[s] > 0)
				pic.tau[s] = pic.tauSeen[opp] > 0 && told[opp] > 0 ? told[s] * pic.tau[opp] / told[opp] : told[s];
			else if (pic.tauSeen[opp] > 0)
				pic.tau[s] = pic.tau[opp];
		}
		// An axis with nothing: the border is as thick in the display's
		// pixels on every side, so it follows from the other and the
		// picture's shape.
		const double a = o.pictureAspect;
		for (int axis = 0; axis < 2; axis++)
		{
			const int s1 = axis == 0 ? TOP : RIGHT, s2 = axis == 0 ? BOTTOM : LEFT;
			const int o1 = axis == 0 ? RIGHT : TOP, o2 = axis == 0 ? LEFT : BOTTOM;
			const double other = (pic.tau[o1] > 0 && pic.tau[o2] > 0) ? 0.5 * (pic.tau[o1] + pic.tau[o2])
					: std::max(pic.tau[o1], pic.tau[o2]);
			if (other <= 0)
				continue;
			const double fraction = other / (1 - 2 * other);	// of the picture, along the other axis
			const double mine = axis == 0 ? fraction * a : fraction / a;
			for (const int s : { s1, s2 })
				if (pic.tau[s] <= 0)
					pic.tau[s] = mine / (1 + 2 * mine);
		}
		for (int s = 0; s < 4; s++)
		{
			const double tau = pic.tau[s];
			if (tau <= 0 || tau >= 0.3)
				continue;
			const V2 a0 = s == TOP ? V2{ 0, tau } : s == BOTTOM ? V2{ 0, 1 - tau } : s == LEFT ? V2{ tau, 0 } : V2{ 1 - tau, 0 };
			const V2 a1 = s == TOP ? V2{ 1, tau } : s == BOTTOM ? V2{ 1, 1 - tau } : s == LEFT ? V2{ tau, 1 } : V2{ 1 - tau, 1 };
			V2 u0, u1;
			if (!outerMap.apply(a0, u0) || !outerMap.apply(a1, u1))
				continue;
			pic.inner[s] = facingAway(through(u0, u1), sd.inside);
			pic.src[s] = SideSource::OuterEdge;
		}
	}
	// Without the whole outline: the outer edge moved in by the border's
	// thickness seen along it (its direction is the outer edge's, cleaner).
	for (int s = 0; s < 4; s++)
		if (pic.src[s] == SideSource::Missing && innerOk[s] && sd.band[s] > 0)
		{
			pic.inner[s] = sd.outer[s];
			pic.inner[s].d -= sd.band[s];
			pic.src[s] = SideSource::InnerEdge;
		}
	int known = 0, missing = -1;
	for (int s = 0; s < 4; s++)
	{
		if (pic.src[s] != SideSource::Missing)
			known++;
		else
			missing = s;
	}
	if (known == 3 && o.placeHiddenSide)
	{
		Line placed;
		V2 a, b;
		if (placeSide(pic.inner, missing, lens.c, o.focal * im.w, o.pictureAspect, placed)
				&& meet(placed, pic.inner[(missing + 3) % 4], a) && meet(placed, pic.inner[(missing + 1) % 4], b))
		{
			// It must be out of view: a side in view is measured, not placed.
			const V2 mid = lens.distort((a + b) * 0.5);
			if (mid.x < 6 || mid.y < 6 || mid.x > im.w - 7 || mid.y > im.h - 7)
			{
				pic.inner[missing] = facingAway(placed, sd.inside);
				pic.src[missing] = SideSource::Placed;
				known = 4;
			}
		}
	}
	return known == 4;
}

Result run(const Frame& frame, const Options& o, const Calibration& cal, Workspace& ws, Hints& hints, double lensK, EdgeSet* edges)
{
	Result res;
	res.lensK = (float)lensK;
	for (double& t : hints.seen)
		t = 0;
	if (!frame.luma || frame.width < 32 || frame.height < 24 || frame.stride < frame.width)
		return res;
	Image im{ frame.luma, frame.width, frame.height, frame.stride };
	if (!o.borderBrighter)
	{
		ws.inverted.resize((size_t)im.w * im.h);
		for (int y = 0; y < im.h; y++)
		{
			const uint8_t* src = im.row(y);
			uint8_t* dst = ws.inverted.data() + (size_t)y * im.w;
			for (int x = 0; x < im.w; x++)
				dst[x] = (uint8_t)(255 - src[x]);
		}
		im = { ws.inverted.data(), im.w, im.h, im.w };
	}
	Candidate candidates[4];
	const int count = findScreen(im, o, hints, ws, candidates, 4);
	if (count == 0)
		return res;

	// The border's thickness on each side as a fraction of the outline
	// across: as Options tell it, as the Tracker learnt it, and the one the
	// profiles look for (learnt first: it holds the television's cropping).
	double told[4], tauExpected[4];
	for (int s = 0; s < 4; s++)
	{
		const double fraction = axisOf(s) == 0 ? o.borderY : o.borderX;
		told[s] = fraction > 0 ? fraction / (1 + 2 * fraction) : 0;
		tauExpected[s] = hints.tau[s] > 0 ? hints.tau[s] : told[s];
	}
	const Lens lens{ { (im.w - 1) / 2.0, (im.h - 1) / 2.0 }, im.w / 2.0, lensK };
	// The best candidate whose sides measure up as the border's; the next if not.
	// With the border's thickness known, a candidate on which no side shows
	// the border's inner edge where it should be (a window, a lamp's shade:
	// bright and filled, with no band) gives way to one that does; it is
	// kept if none does (a picture all as bright as the border).
	Sides sd;
	Picture pic;
	bool placed = false;
	int chosen = -1;
	bool knowsThickness = false;
	for (int s = 0; s < 4; s++)
		knowsThickness = knowsThickness || tauExpected[s] > 0;
	for (int c = 0; c < count; c++)
	{
		Sides tsd;
		Picture tpic;
		measureSides(im, lens, o, candidates[c], ws, tauExpected, tsd);
		if (!placePicture(im, lens, o, tsd, told, hints.tau, tpic))
			continue;
		bool banded = !knowsThickness;
		for (int s = 0; s < 4; s++)
			banded = banded || innerAgrees(tsd, s);
		// One with no band at all only where the gun points.
		if (!banded && !candidates[c].holdsMiddle)
			continue;
		if (chosen < 0 || banded)
		{
			sd = tsd;
			pic = tpic;
			chosen = c;
			placed = true;
		}
		if (banded)
			break;
	}
	if (chosen < 0)
	{
		// None: what the best showed, for the overlay.
		chosen = 0;
		sd = Sides();
		pic = Picture();
		measureSides(im, lens, o, candidates[0], ws, tauExpected, sd);
		placePicture(im, lens, o, sd, told, hints.tau, pic);
		placed = false;
	}
	const Candidate& best = candidates[chosen];
	hints.fraction = best.fraction;
	res.threshold = best.threshold;
	res.roll = (float)(best.roll * 180 / PI);
	for (int s = 0; s < 4; s++)
		res.sides[s] = pic.src[s];
	if (pic.outerOk)
	{
		res.outerFound = true;
		for (int s = 0; s < 4; s++)
		{
			const V2 p = lens.distort(pic.outerCorner[s]);
			res.outer[CORNER_OUT[s]] = { (float)p.x, (float)p.y };
		}
		const double ty = 0.5 * (pic.tau[TOP] + pic.tau[BOTTOM]), tx = 0.5 * (pic.tau[LEFT] + pic.tau[RIGHT]);
		res.borderY = (float)(ty > 0 && ty < 0.5 ? ty / (1 - 2 * ty) : 0);
		res.borderX = (float)(tx > 0 && tx < 0.5 ? tx / (1 - 2 * tx) : 0);
	}
	// The border's level, for the caller's control of the camera's exposure.
	{
		std::vector<double>& levels = ws.values;
		levels.clear();
		int clipped = 0;
		for (int s = 0; s < 4; s++)
			if (sd.has[s])
				for (const float l : sd.seen[s].level)
				{
					levels.push_back(l);
					clipped += l >= 250;
				}
		if (!levels.empty())
		{
			res.clipped = (float)clipped / levels.size();
			res.borderLevel = (float)median(levels);
		}
	}
	if (!placed)
		return res;

	// The picture's corners and the map to it; the optical centre through it.
	V2 corner[4];
	for (int s = 0; s < 4; s++)
		if (!meet(pic.inner[s], pic.inner[(s + 1) % 4], corner[s]))
			return res;
	const V2 quad[4] = { corner[3], corner[0], corner[1], corner[2] };
	for (int i = 0; i < 4; i++)
	{
		const V2 e1 = quad[(i + 1) % 4] - quad[i], e2 = quad[(i + 2) % 4] - quad[(i + 1) % 4];
		if (e1.x * e2.y - e1.y * e2.x <= 0)
			return res;	// not convex and clockwise: not a screen
	}
	Homography map, back;
	if (!squareToQuad(quad, map) || !invert(map, back))
		return res;
	V2 aim = lens.c;
	if (cal.inCamera)
		aim = lens.undistort(lens.c + V2{ cal.offsetX * 1e-4 * im.w, cal.offsetY * 1e-4 * im.h });
	V2 uv;
	if (!back.apply(aim, uv))
		return res;
	double x = 2 * uv.x - 1, y = 2 * uv.y - 1;
	if (!cal.inCamera)
	{
		x += cal.offsetX * 2e-4;
		y += cal.offsetY * 2e-4;
	}
	res.found = true;
	res.x = (float)x;
	res.y = (float)y;
	res.offScreen = !(x >= -1 && x <= 1 && y >= -1 && y <= 1);
	for (int i = 0; i < 4; i++)
	{
		const V2 p = lens.distort(quad[i]);
		res.corners[i] = { (float)p.x, (float)p.y };
	}
	// Quality: each side's share of profiles on its line, less for edges
	// that scatter, less again for a side not measured.
	double quality = 0, rms = 0;
	int fitted = 0;
	for (int s = 0; s < 4; s++)
	{
		const SideFit* fit = pic.src[s] == SideSource::OuterEdge ? &sd.outerFit[s] : pic.src[s] == SideSource::InnerEdge ? &sd.innerFit[s] : nullptr;
		if (!fit)
		{
			quality += 0.45;
			continue;
		}
		const double share = clamp01(1.15 * fit->inliers / std::max(1, sd.seen[s].profiles));
		quality += share * std::min(1.0, 0.4 / std::max(0.4, fit->rms));
		rms += fit->rms;
		fitted++;
		res.innerSeen += innerAgrees(sd, s) && (pic.tauSeen[s] > 0 || pic.src[s] == SideSource::InnerEdge);
	}
	res.quality = (float)(quality / 4);
	res.edgeRms = (float)(fitted ? rms / fitted : 0);
	hints.haveRoll = true;
	hints.roll = best.roll;
	for (int s = 0; s < 4; s++)
		hints.seen[s] = pic.tauSeen[s];
	if (pic.outerOk)
	{
		hints.haveBox = true;
		hints.box[0] = std::min({ res.outer[0].x, res.outer[3].x });
		hints.box[1] = std::min({ res.outer[0].y, res.outer[1].y });
		hints.box[2] = std::max({ res.outer[1].x, res.outer[2].x });
		hints.box[3] = std::max({ res.outer[2].y, res.outer[3].y });
	}
	if (edges)
	{
		edges->count = 0;
		for (int s = 0; s < 4; s++)
		{
			edges->points[s].clear();
			edges->points[4 + s].clear();
			if (sd.has[s] && sd.outerFit[s].ok)
				for (size_t i = 0; i < sd.seen[s].outer.size(); i++)
					if (sd.outerFit[s].inlier[i])
						edges->points[s].push_back(sd.seen[s].outer[i]);
			if (innerAgrees(sd, s))
				for (size_t i = 0; i < sd.seen[s].inner.size(); i++)
					if (sd.innerFit[s].inlier[i])
						edges->points[4 + s].push_back(sd.seen[s].inner[i]);
			edges->count += (int)(edges->points[s].size() + edges->points[4 + s].size());
		}
	}
	return res;
}

// How far the edge points are from straight lines once the distortion k is
// taken out: least at the lens's own k.
double crookedness(const EdgeSet& edges, const Lens& base, double k, std::vector<V2>& tmp)
{
	Lens lens = base;
	lens.k = k;
	double sum = 0;
	for (const std::vector<V2>& side : edges.points)
	{
		if (side.size() < 8)
			continue;
		tmp.clear();
		for (const V2& p : side)
			tmp.push_back(lens.undistort(p));
		Line l;
		if (!fitLine(tmp.data(), nullptr, (int)tmp.size(), l))
			continue;
		for (const V2& p : tmp)
		{
			const double r = signedDistance(l, p);
			sum += std::min(r * r, 4.0);
		}
	}
	return sum;
}

}

Result detect(const Frame& frame, const Options& options, const Calibration& calibration)
{
	Workspace ws;
	Hints hints;
	return run(frame, options, calibration, ws, hints, options.lensK, nullptr);
}

struct Tracker::State
{
	Workspace ws;
	Hints hints;
	EdgeSet edges;
	std::vector<V2> tmp;
	double lensK = 0;
	std::vector<double> lensSeen;	// the last frames' own measures
	std::vector<double> tauSeen[4];	// the border's thickness, the last frames' own measures
	int misses = 0;
	struct OneEuro
	{
		bool started = false;
		double x = 0, dx = 0, t = 0;
		static double alpha(double cutoff, double dt) { return 1 / (1 + 1 / (2 * PI * cutoff * dt)); }
		double filter(double value, double time, double minCutoff, double beta)
		{
			if (!started || time <= t || time - t > 0.5)
			{
				started = true;
				x = value;
				dx = 0;
				t = time;
				return value;
			}
			const double dt = time - t;
			t = time;
			dx += alpha(1.0, dt) * ((value - x) / dt - dx);
			x += alpha(minCutoff + beta * std::fabs(dx), dt) * (value - x);
			return x;
		}
	} smoothX, smoothY;
};

Tracker::Tracker(const Options& options) : options_(options), state_(new State)
{
	reset();
}

Tracker::~Tracker() = default;

void Tracker::reset()
{
	*state_ = State();
	state_->lensK = options_.lensK;
}

Result Tracker::update(const Frame& frame, const Calibration& calibration, double seconds)
{
	State& st = *state_;
	Result r = run(frame, options_, calibration, st.ws, st.hints, st.lensK, options_.measureLens ? &st.edges : nullptr);
	if (!r.found)
	{
		// Lost for a while: the screen may come back turned another way.
		if (++st.misses > 30)
		{
			st.hints.haveRoll = false;
			st.hints.haveBox = false;
		}
		st.smoothX.started = st.smoothY.started = false;
		return r;
	}
	st.misses = 0;
	// The border's thickness on each side, learnt from the last frames that
	// saw its inner edge: for when Options do not tell it, and for the sides
	// a frame does not see. Within reach of what Options tell.
	for (int s = 0; s < 4; s++)
	{
		const double seen = st.hints.seen[s];
		if (seen <= 0)
			continue;
		std::vector<double>& v = st.tauSeen[s];
		v.push_back(seen);
		if (v.size() > 31)
			v.erase(v.begin());
		if (v.size() < 9)
			continue;
		// The thinnest it is often seen at, as within a frame: the picture
		// beside the border only ever makes it look thicker.
		std::vector<double> sorted(v);
		double tau = lowestMode(sorted, 0);
		const double told = axisOf(s) == 0 ? options_.borderY : options_.borderX;
		if (told > 0)
		{
			const double expected = told / (1 + 2 * told);
			tau = std::clamp(tau, 0.7 * expected, 1.3 * expected);
		}
		st.hints.tau[s] = tau;
	}
	// The lens: from frames that saw the whole outline at least a third of
	// the frame wide (smaller, its edges bend too little to measure: one
	// frame's measure scatters by 0.016 at 120 px wide, 0.05 at 85), the
	// median of the last frames' measures.
	const double spanX = std::max(r.outer[1].x, r.outer[2].x) - std::min(r.outer[0].x, r.outer[3].x);
	if (options_.measureLens && r.outerFound && spanX > frame.width / 3.0 && st.edges.count > 150)
	{
		const Lens base{ { (frame.width - 1) / 2.0, (frame.height - 1) / 2.0 }, frame.width / 2.0, 0 };
		double lo = -0.45, hi = 0.25;
		const double g = 0.5 * (std::sqrt(5.0) - 1);
		double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
		double f1 = crookedness(st.edges, base, x1, st.tmp), f2 = crookedness(st.edges, base, x2, st.tmp);
		for (int it = 0; it < 22; it++)
		{
			if (f1 < f2)
			{
				hi = x2;
				x2 = x1;
				f2 = f1;
				x1 = hi - g * (hi - lo);
				f1 = crookedness(st.edges, base, x1, st.tmp);
			}
			else
			{
				lo = x1;
				x1 = x2;
				f1 = f2;
				x2 = lo + g * (hi - lo);
				f2 = crookedness(st.edges, base, x2, st.tmp);
			}
		}
		const double k = 0.5 * (lo + hi);
		if (k > -0.44 && k < 0.24)
		{
			st.lensSeen.push_back(k);
			if (st.lensSeen.size() > 63)
				st.lensSeen.erase(st.lensSeen.begin());
			std::vector<double> sorted(st.lensSeen);
			if (sorted.size() >= 5)
				st.lensK = median(sorted);
		}
	}
	if (options_.smoothCutoff > 0)
	{
		r.x = (float)st.smoothX.filter(r.x, seconds, options_.smoothCutoff, options_.smoothBeta);
		r.y = (float)st.smoothY.filter(r.y, seconds, options_.smoothCutoff, options_.smoothBeta);
		r.offScreen = !(r.x >= -1 && r.x <= 1 && r.y >= -1 && r.y <= 1);
	}
	return r;
}

}
