// PSSwanStation - where a Sinden Lightgun points, found from its camera's
// picture of the screen and the plain border drawn around the game's picture.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gun is a camera. The title draws a plain border (white by default)
// round the game's picture; in each camera frame this module finds that border
// and maps the camera's optical centre through the homography from its corners
// to the picture. The gun points where the optical centre falls, plus the
// gun's own calibration offset.
//
// The method, in short (docs/dev/sinden-lightgun.md, "The tracker"):
// 1. Thresholds between the frame's dark and bright levels (they follow the
//    exposure); the bright parts; the one that is the border is a large ring
//    whose outer outline is four straight lines.
// 2. Along profiles across each side, the border's outer edge (against the
//    darker bezel or room) and inner edge (against the picture) to a fraction
//    of a pixel; robust line fits with the lens's barrel distortion taken out.
// 3. Checks that each side is the border's and not an edge in the picture: it
//    stands clear of its outside, its band ends at its corners, and no band
//    lies further out.
// 4. Each side of the picture is its outer edge (the cleanest) moved in by the
//    border's thickness through the outline's homography: told, learnt, and
//    as seen on the inner edges. A side out of the camera's view is placed from
//    the other three, the camera's focal length and the picture's shape.
//
// Pure: no globals, no threads; detect() keeps nothing, and a Tracker keeps
// its state in itself. Standard C++17 only, so tests/sinden runs it on a PC.
#pragma once

#include <cstdint>
#include <memory>

namespace fe::sinden
{

struct Point
{
	float x = 0, y = 0;
};

// One camera frame's brightness (luma), 8 bits a pixel, rows stride bytes apart.
struct Frame
{
	const uint8_t* luma = nullptr;
	int width = 0, height = 0, stride = 0;
};

// The gun's calibration as its serial port gives it (commands 104 and 105):
// the value minus 10000, in hundredths of a per cent. Sinden's software is
// closed, so what the per cent is of is not known for certain: by default it
// is taken as of the picture's width and height, added to the aim (positive to
// the right and down); with inCamera it is of the camera frame's width and
// height, moving the point of the frame that is mapped to the screen.
struct Calibration
{
	int offsetX = 0;
	int offsetY = 0;
	bool inCamera = false;
};

struct Options
{
	// The border is brighter than what is round it outside (the bezel, the
	// room). False for a dark border on a bright surround: the frame is then
	// read inverted.
	bool borderBrighter = true;
	// The picture inside the border: its width over its height as the screen
	// shows it. Used to place a side out of the camera's view.
	float pictureAspect = 4.0f / 3.0f;
	// The border's thickness as a fraction of the picture's width (borderX: the
	// left and right sides) and height (borderY: top and bottom), as the title
	// draws it: each side of the picture is the border's outer edge moved in by
	// it. 0 when not known: then as the frame (or the Tracker) sees it, which a
	// white area of the picture beside the border can make look thicker.
	float borderX = 0;
	float borderY = 0;
	// The camera's focal length in pixels over the frame's width (0.8: about
	// 64 degrees across). Used only to place a side out of view.
	float focal = 0.8f;
	// Barrel distortion of the lens, Fitzgibbon's division model about the
	// frame's centre, the radius in half frame widths: a point seen at radius
	// r is at r / (1 + k r^2) without it. Barrel is negative; 0 for none.
	float lensK = 0;
	// The Tracker measures lensK from how straight the border's edges come out
	// (the median of its last frames' measures, from the fifth on), starting
	// from lensK.
	bool measureLens = true;
	// The least difference in levels (of 255) between the border and what is
	// either side of it for an edge to count.
	float minContrast = 16;
	// Place one side out of the camera's view from the other three; false: the
	// screen counts as not found unless all four are seen.
	bool placeHiddenSide = true;
	// The Tracker's smoothing of the aim (a One Euro filter: smoothing grows as
	// the aim stops, so a still gun is steady and a moving one is not slowed).
	// smoothCutoff in hertz at rest (0: off), smoothBeta how fast it opens up
	// with speed (per unit of the aim's range a second).
	float smoothCutoff = 0;
	float smoothBeta = 40.0f;
};

// Where each side of the picture came from.
enum class SideSource : uint8_t
{
	Missing,	// not found
	OuterEdge,	// the border's outer edge and its thickness (the usual)
	InnerEdge,	// the border's inner edge (part of the outline out of view)
	Placed,		// out of the camera's view: from the other three sides
};

struct Result
{
	bool found = false;
	// The aim in the picture inside the border: -1 at its left and top edges,
	// +1 at its right and bottom edges (libretro's light gun screen
	// coordinates). Off the screen: outside that range (or not found).
	float x = 0, y = 0;
	bool offScreen = true;
	// 0 to 1: about 0.9 and above with all four sides measured cleanly; lower
	// with outliers along them, or a side placed.
	float quality = 0;
	// The picture's corners (the border's inner corners) and the border's
	// outer corners, in the camera frame's pixels as seen (with the lens's
	// distortion): top-left, top-right, bottom-right, bottom-left of the
	// picture. A corner of a side out of view may be outside the frame.
	Point corners[4];
	Point outer[4];
	bool outerFound = false;
	SideSource sides[4] = {};	// top, right, bottom, left
	// Measured on the way, for the boot log and the overlay.
	float roll = 0;			// the screen's turn in the frame, degrees, clockwise
	float edgeRms = 0;		// pixels: how far the edge points are from the lines fitted to them
	float borderX = 0, borderY = 0;	// the border's thickness used, as in Options
	float lensK = 0;		// the lens's distortion used
	int threshold = 0;		// the level the border was found at
	int innerSeen = 0;		// sides whose inner edge was seen and agreed
	// The border's level in the frame (median, of 255) and the share of it
	// clipped at the top: for the caller to set the camera's exposure. A
	// clipped border reads larger than it is (up to a pixel at two to three
	// times the exposure that clips): keep it under about 230.
	float borderLevel = 0;
	float clipped = 0;
};

// One frame, from nothing: what it shows alone.
Result detect(const Frame& frame, const Options& options = Options(), const Calibration& calibration = Calibration());

// Frame after frame: remembers the level the border was found at, the
// screen's turn, the border's thickness and the lens's distortion seen so far,
// and smooths the aim when asked to. One per gun; not shared between threads.
class Tracker
{
public:
	explicit Tracker(const Options& options = Options());
	~Tracker();
	Tracker(const Tracker&) = delete;
	Tracker& operator=(const Tracker&) = delete;

	// seconds: when the frame was taken (any clock that only goes forward).
	Result update(const Frame& frame, const Calibration& calibration, double seconds);
	void reset();
	const Options& options() const { return options_; }

	struct State;

private:
	Options options_;
	std::unique_ptr<State> state_;
};

}
