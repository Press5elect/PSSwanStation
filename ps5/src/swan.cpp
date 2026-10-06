/*
	PSSwanStation - the swan, drawn so that it can move.

	SPDX-License-Identifier: GPL-3.0-or-later

	The title's mark is a swan on water in a rounded box (ps5/tools/make-art.py
	draws the icon and the loading picture). In the interface the box is a
	picture and the bird is drawn here, from the same shapes, each frame: a
	body, a tail, a neck that is a curve, a head with a beak and an eye, and
	wings. So it can do what a picture cannot: turn its head to look behind it,
	dip it, open its wings, stretch its neck and fly.

	Everything is in the mark's own coordinates: a unit square whose (0, 0) is
	the bird's top left corner, as make-art.py's swan() has it. Two poses are
	kept as numbers, swimming (the mark exactly) and flying, and a pose in
	between is every number in between.
*/
#include "ui.h"

#include <algorithm>
#include <cmath>

namespace fe::ui
{
namespace
{

struct Point
{
	float x, y;
};

Point mixed(Point a, Point b, float t)
{
	return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

float mixed(float a, float b, float t)
{
	return a + (b - a) * t;
}

Point bezier(Point p0, Point p1, Point p2, Point p3, float t)
{
	const float u = 1 - t;
	return { u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
			u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y };
}

constexpr float Pi = 3.14159265f;

}

void swan(ImVec2 origin, float size, const SwanPose& pose, float alpha, ImDrawList *list)
{
	if (alpha <= 0.004f || size <= 1.f)
		return;
	if (list == nullptr)
		list = draw();
	const float fly = std::clamp(pose.fly, 0.f, 1.f);
	const float swim = 1.f - fly;
	const float cosT = std::cos(pose.tilt), sinT = std::sin(pose.tilt);
	// The body's middle is what the bird turns about.
	const Point pivot = { 0.585f, 0.640f };
	const float face = std::clamp(pose.face, -1.f, 1.f);
	const float wide = std::max(std::fabs(face), 0.06f);		// how wide a round part is, seen turning
	const auto place = [&](Point p) {
		const float dx = (p.x - pivot.x) * face, dy = p.y - pivot.y;
		return ImVec2(origin.x + (pivot.x + dx * cosT - dy * sinT) * size,
				origin.y + (pivot.y + dx * sinT + dy * cosT) * size);
	};
	const auto colour = [alpha](int r, int g, int b, float a = 1.f) {
		return IM_COL32(r, g, b, (int)(255 * std::clamp(alpha * a, 0.f, 1.f)));
	};
	const ImU32 white = colour(255, 255, 255);
	const ImU32 shade = colour(190, 214, 245);

	// A wing, open: from the shoulder to a tip that the beat moves, and back
	// to the body behind it. `beat` is 1 with the wing up, -1 down.
	const auto wing = [&](float beat, Point shift, ImU32 fill, bool edge) {
		const Point shoulder = { 0.47f + shift.x, 0.600f + shift.y };
		const Point back = { 0.72f + shift.x, 0.615f + shift.y };
		const Point tip = { 0.640f + 0.060f * beat + shift.x, 0.600f - 0.500f * beat + shift.y };
		const Point lead1 = { shoulder.x - 0.070f, shoulder.y - 0.260f * beat };
		const Point lead2 = { tip.x - 0.120f, tip.y + 0.100f * beat };
		const Point trail1 = { tip.x + 0.150f, tip.y + 0.130f * beat };
		const Point trail2 = { back.x + 0.110f, back.y - 0.170f * beat };
		ImVec2 points[26];
		int count = 0;
		for (int i = 0; i <= 12; i++)
			points[count++] = place(bezier(shoulder, lead1, lead2, tip, (float)i / 12.f));
		for (int i = 1; i <= 12; i++)
			points[count++] = place(bezier(tip, trail1, trail2, back, (float)i / 12.f));
		list->AddConcavePolyFilled(points, count, fill);
		if (edge)
		{
			// Its trailing edge, so a white wing shows against the white body.
			for (int i = 0; i <= 12; i++)
				list->PathLineTo(place(bezier(tip, trail1, trail2, back, (float)i / 12.f)));
			list->PathStroke(shade, std::max(size * 0.012f, 1.f));
		}
	};

	// The far wing, behind everything, a beat ahead of the near one.
	if (fly > 0.02f)
		wing(std::sin(pose.beat + 0.55f), { 0.035f, -0.012f }, colour(190, 214, 245, fly), false);

	// The tail: raised on the water, flat behind in the air.
	{
		const Point swimTail[4] = { { 0.700f, 0.560f }, { 0.905f, 0.435f }, { 0.835f, 0.660f }, { 0.740f, 0.740f } };
		const Point flyTail[4] = { { 0.740f, 0.575f }, { 0.975f, 0.605f }, { 0.900f, 0.668f }, { 0.760f, 0.690f } };
		ImVec2 points[4];
		for (int i = 0; i < 4; i++)
			points[i] = place(mixed(swimTail[i], flyTail[i], fly));
		list->AddConvexPolyFilled(points, 4, white);
		list->AddCircleFilled(place(mixed(Point{ 0.800f, 0.585f }, Point{ 0.800f, 0.622f }, fly)),
				size * mixed(0.075f, 0.046f, fly), white, 32);
	}

	// The body.
	{
		const Point centre = mixed(Point{ 0.585f, 0.640f }, Point{ 0.565f, 0.625f }, fly);
		list->AddEllipseFilled(place(centre),
				ImVec2(size * mixed(0.250f, 0.262f, fly) * wide, size * mixed(0.135f, 0.098f, fly)), white, pose.tilt, 48);
	}

	// The head's turn: 0 looks ahead, as the mark does; towards 1 it turns to
	// look behind (the beak shortens, points at the viewer, then the other
	// way); below 0 it dips forward.
	const float back = std::clamp(pose.look, 0.f, 1.f) * swim;
	const float dip = std::clamp(-pose.look, 0.f, 1.f) * swim;
	const float turn = back * 0.86f * Pi;
	const float facing = std::cos(turn);		// 1 ahead, 0 at the viewer, -1 behind

	// The neck: an S from the chest to the head; stretched out ahead in flight.
	Point n0 = mixed(Point{ 0.415f, 0.640f }, Point{ 0.400f, 0.612f }, fly);
	Point n1 = mixed(Point{ 0.200f, 0.560f }, Point{ 0.300f, 0.598f }, fly);
	Point n2 = mixed(Point{ 0.520f, 0.360f }, Point{ 0.200f, 0.584f }, fly);
	Point n3 = mixed(Point{ 0.345f, 0.235f }, Point{ 0.095f, 0.566f }, fly);
	// Looking behind brings the head back over the body; a dip lowers it.
	const float over = std::sin(turn * 0.5f);
	n3.x += 0.085f * over * over - 0.035f * dip;
	n3.y += -0.012f * over + 0.070f * dip;
	n2.x += -0.030f * over - 0.030f * dip;
	n2.y += 0.030f * dip;
	n1.x += -0.012f * dip;
	const float thick0 = mixed(0.062f, 0.056f, fly), thick1 = mixed(0.042f, 0.037f, fly);
	constexpr int Links = 30;
	for (int i = 0; i <= Links; i++)
	{
		const float t = (float)i / Links;
		list->AddCircleFilled(place(bezier(n0, n1, n2, n3, t)), size * mixed(thick0, thick1, t), white, 20);
	}

	// The head, the beak and the eye.
	{
		const Point head = { n3.x - 0.010f * facing, n3.y - 0.003f };
		list->AddEllipseFilled(place(head), ImVec2(size * 0.052f, size * 0.047f), white, pose.tilt, 28);
		const ImU32 beak = colour(255, 170, 60);
		const float away = 1.f - std::fabs(facing);		// 1 when the beak points at the viewer
		if (std::fabs(facing) > 0.04f)
		{
			const float base = head.x - 0.035f * facing;
			const ImVec2 points[3] = { place({ base, head.y - 0.020f }), place({ head.x - 0.140f * facing, head.y + 0.036f }),
					place({ base, head.y + 0.036f }) };
			list->AddTriangleFilled(points[0], points[1], points[2], beak);
		}
		// Seen from the front the beak is a small oval under the eyes.
		const float front = std::clamp((away - 0.45f) / 0.55f, 0.f, 1.f);
		if (front > 0.02f)
			list->AddEllipseFilled(place({ head.x - 0.018f * facing, head.y + 0.020f }),
					ImVec2(size * 0.022f * front, size * 0.026f * front), beak, pose.tilt, 16);
		const ImU32 eye = colour(20, 30, 56);
		const float eyeY = head.y - 0.010f;
		const float spread = 0.024f * front;		// both eyes show when it faces the viewer
		list->AddCircleFilled(place({ head.x - 0.008f * facing - spread, eyeY }), std::max(size * 0.0095f, 0.8f), eye, 12);
		if (front > 0.02f)
			list->AddCircleFilled(place({ head.x - 0.008f * facing + spread, eyeY }), std::max(size * 0.0095f, 0.8f),
					colour(20, 30, 56, front), 12);
	}

	// The folded wing: a crescent of feathers along the back, on the water only.
	if (swim > 0.02f)
	{
		const Point centre = { 0.660f, 0.598f };
		constexpr int Steps = 18;
		ImVec2 points[(Steps + 1) * 2];
		for (int i = 0; i <= Steps; i++)
		{
			const float along = (float)i / Steps;
			const float angle = (0.10f + 0.74f * along) * Pi;
			const Point outer = { centre.x + 0.200f * std::cos(angle), centre.y + 0.124f * std::sin(angle) };
			// Thick in the middle, to nothing at its ends.
			const float thick = 0.052f * std::pow(std::sin(Pi * along), 0.75f);
			const float dx = centre.x - outer.x, dy = centre.y - 0.060f - outer.y;
			const float length = std::max(std::sqrt(dx * dx + dy * dy), 0.001f);
			points[i] = place(outer);
			points[(Steps + 1) * 2 - 1 - i] = place({ outer.x + dx / length * thick, outer.y + dy / length * thick });
		}
		list->AddConcavePolyFilled(points, (Steps + 1) * 2, colour(190, 214, 245, swim));
	}

	// The near wing, over the body.
	if (fly > 0.02f)
		wing(std::sin(pose.beat), { 0, 0 }, colour(255, 255, 255, fly), true);
}

// What the swan does while it only sits in its box: now and then it looks
// behind it, and once in a while dips its head. A slow cycle, by the clock.
SwanPose swanIdle(double time)
{
	SwanPose pose;
	const auto ramp = [](double t, double from, double to) {
		const float x = std::clamp((float)((t - from) / (to - from)), 0.f, 1.f);
		return x * x * (3.f - 2.f * x);
	};
	const double t = std::fmod(time, 13.0);
	// Looks behind, twice: a glance, then a longer look.
	const float look = ramp(t, 3.2, 3.75) * (1.f - ramp(t, 4.9, 5.45))
			+ ramp(t, 6.3, 6.8) * (1.f - ramp(t, 8.6, 9.2));
	const float dip = ramp(t, 10.6, 11.0) * (1.f - ramp(t, 11.5, 12.0));
	pose.look = look - dip * 0.8f;
	// It is on water: it rocks a little.
	pose.tilt = 0.018f * (float)std::sin(time * 1.3);
	return pose;
}

}
