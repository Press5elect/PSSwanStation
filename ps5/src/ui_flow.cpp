/*
	PSSwanStation - the library's views in space: covers as cases in a row, a
	flow, a wall, a cascade, a wheel.

	SPDX-License-Identifier: GPL-3.0-or-later

	These are the kind of view Aurora, the Xbox 360's homebrew dashboard, shows
	its games in, and they are described the way Aurora describes its cover
	layouts: where the case under the cursor stands, where the first one to its
	left and to its right stands, how far each further one is from the one
	before (a step in place and a step in angle), the same for the rows above
	and below in a view with several, and where the camera is. A view is those
	few numbers; the ones built in are this title's own, made for the nearly
	square covers PlayStation games have.

	An Aurora layout file (.cfljson) in <root>layouts/ is read into the same
	numbers and offered beside them. Aurora's own code is not public: what a
	number means was worked out from layouts and how they are known to look
	(a rotation is about x, y and z in radians, applied as Direct3D's yaw, pitch
	and roll are; a case is one unit high and stands with its spine to the
	left), so a layout may not look quite as it does on the Xbox. Its "Normal"
	mode is the view, its "Initial" mode where the cases come from when the
	view opens; lights, the floor's grid and the other modes are not used.

	Everything is drawn through ImGui's draw list, which knows no depth: the
	cases are sorted from the farthest to the nearest, the faces turned away
	are left out, and a cover is a small mesh rather than one quad, so that it
	narrows towards its far edge as a picture in space does.
*/
#include "ui_internal.h"
#include "display.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>

namespace fe::ui
{
using namespace platform;

namespace
{

// ------------------------------------------------------------------ vectors

struct V3
{
	float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
V3 mix3(V3 a, V3 b, float t) { return a + (b - a) * t; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 unit(V3 a)
{
	const float length = std::sqrt(dot(a, a));
	return length > 1e-6f ? a * (1.f / length) : V3{ 0, 0, 1 };
}

// Turned about z, then x, then y (roll, pitch, yaw), as Direct3D turns.
V3 rotate(V3 p, V3 angle)
{
	float c = std::cos(angle.z), s = std::sin(angle.z);
	p = { p.x * c - p.y * s, p.x * s + p.y * c, p.z };
	c = std::cos(angle.x), s = std::sin(angle.x);
	p = { p.x, p.y * c - p.z * s, p.y * s + p.z * c };
	c = std::cos(angle.y), s = std::sin(angle.y);
	p = { p.x * c + p.z * s, p.y, -p.x * s + p.z * c };
	return p;
}

// ------------------------------------------------------------------ layouts

struct Side
{
	V3 pos, angle, scale{ 1, 1, 1 }, spacer, delta;
};

struct Mode
{
	V3 centerPos, centerAngle, centerScale{ 1, 1, 1 };
	// The middle column in the rows the cursor is not in.
	V3 rowPos, rowAngle, rowScale{ 1, 1, 1 };
	Side left, right;
	V3 topSpacer, bottomSpacer, topAngle, bottomAngle, topDelta, bottomDelta;
	V3 swingAngleSpeed, swingAngle, swingPosSpeed, swingPos;
	V3 eye{ 0, 0, -7 }, aim{ 0, 0, 0 };
	bool mirror = false;
	V3 mirrorOffset{ 0, 1, 0 };
};

struct Layout
{
	std::string name;
	int rows = 1, columns = 21;
	Mode normal, initial;
	bool hasInitial = false;
	// One of this title's own (made for square covers) or an Aurora file.
	bool own = true;
	// The row runs down the screen: Up and Down move along it, and what is
	// known about the game is beside it.
	bool vertical = false;
	// Where the game's name is: 0 under the middle, 1 at the left.
	int caption = 0;
	// The rows stay where they are and the cursor goes from one to the next
	// (a wall); otherwise the cursor's row is always the middle one.
	bool fixedRows = false;
	// How much of the bottom of the screen is the name's (in units): the view
	// is centred in what is left.
	float foot = 0;
};

Side side(V3 pos, V3 angle, V3 spacer, V3 delta = {}, V3 scale = { 1, 1, 1 })
{
	Side s;
	s.pos = pos;
	s.angle = angle;
	s.spacer = spacer;
	s.delta = delta;
	s.scale = scale;
	return s;
}

// This title's own views.
std::vector<Layout> ownLayouts()
{
	std::vector<Layout> list;
	{
		// The case under the cursor faces the screen; the others lean towards
		// it from both sides, one over the next.
		Layout l;
		l.name = "Flow";
		l.columns = 25;
		Mode& m = l.normal;
		l.foot = 60;
		m.centerPos = { 0, 0, -4.5f };
		m.left = side({ -1.36f, 0, -3.3f }, { 0, -1.05f, 0 }, { -0.38f, 0, 0.02f });
		m.right = side({ 1.36f, 0, -3.3f }, { 0, 1.05f, 0 }, { 0.38f, 0, 0.02f });
		m.eye = { 0, 0.18f, -7 };
		m.aim = { 0, -0.05f, 0 };
		m.mirror = true;
		list.push_back(l);
	}
	{
		// A flat row on a floor, the one under the cursor nearer.
		Layout l;
		l.name = "Row";
		l.columns = 15;
		Mode& m = l.normal;
		l.foot = 60;
		m.centerPos = { 0, 0.03f, -4.35f };
		m.left = side({ -1.42f, 0, -3.2f }, {}, { -1.10f, 0, 0 });
		m.right = side({ 1.42f, 0, -3.2f }, {}, { 1.10f, 0, 0 });
		m.eye = { 0, 0.22f, -7 };
		m.aim = { 0, -0.08f, 0 };
		m.mirror = true;
		list.push_back(l);
	}
	{
		// Three rows that bend away at both ends.
		Layout l;
		l.name = "Wall";
		l.rows = 3;
		l.columns = 17;
		l.fixedRows = true;
		l.foot = 104;
		Mode& m = l.normal;
		m.centerPos = { 0, 0, -3.5f };
		m.rowPos = { 0, 0, -3.0f };
		m.left = side({ -1.10f, 0, -2.96f }, { 0, 0.10f, 0 }, { -1.06f, 0, 0.14f }, { 0, 0.06f, 0 });
		m.right = side({ 1.10f, 0, -2.96f }, { 0, -0.10f, 0 }, { 1.06f, 0, 0.14f }, { 0, -0.06f, 0 });
		m.topSpacer = { 0, 1.10f, 0 };
		m.bottomSpacer = { 0, -1.10f, 0 };
		m.eye = { 0, 0, -8.75f };
		list.push_back(l);
	}
	{
		// The case under the cursor at the left; the ones to come stand behind
		// one another, away to the right.
		Layout l;
		l.name = "Cascade";
		l.columns = 23;
		l.caption = 1;
		Mode& m = l.normal;
		m.centerPos = { -0.55f, -0.02f, -4.2f };
		m.centerAngle = { 0, 0.12f, 0 };
		m.left = side({ -2.75f, -0.02f, -3.2f }, { 0, -1.15f, 0 }, { -0.34f, 0, 0.1f });
		m.right = side({ 0.72f, 0.03f, -3.45f }, { 0, 0.62f, 0 }, { 0.50f, 0.035f, 0.52f });
		m.eye = { 0, 0.3f, -7 };
		m.aim = { 0, -0.05f, 0 };
		m.mirror = true;
		list.push_back(l);
	}
	{
		// A wheel at the right, turning past the one under the cursor.
		Layout l;
		l.name = "Wheel";
		l.columns = 13;
		l.vertical = true;
		l.caption = 1;
		Mode& m = l.normal;
		m.centerPos = { 1.05f, 0, -3.6f };
		m.centerAngle = { 0, 0.20f, 0 };
		m.left = side({ 1.52f, 1.13f, -2.55f }, { 0, 0.30f, 0.07f }, { 0.20f, 0.84f, 0.55f }, { 0, 0, 0.07f });
		m.right = side({ 1.52f, -1.13f, -2.55f }, { 0, 0.30f, -0.07f }, { 0.20f, -0.84f, 0.55f }, { 0, 0, -0.07f });
		m.eye = { 0, 0, -7 };
		list.push_back(l);
	}
	return list;
}

// ------------------------------------------------------ an Aurora layout file

// As much of JSON as a layout file has.
struct Json
{
	enum Kind { Null, Number, Text, Flag, List, Object } kind = Null;
	double number = 0;
	std::string text;
	std::vector<Json> items;
	// An object's members: their names, and their values in the same order.
	std::vector<std::string> names;
	std::vector<Json> values;
	const Json& operator[](const char *key) const;
};

const Json& Json::operator[](const char *key) const
{
	static const Json none;
	for (size_t i = 0; i < names.size() && i < values.size(); i++)
		if (names[i] == key)
			return values[i];
	return none;
}

struct JsonReader
{
	const char *at, *end;
	int depth = 0;
	bool failed = false;

	void space()
	{
		while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n'))
			at++;
	}
	std::string string()
	{
		std::string out;
		at++;
		while (at < end && *at != '"')
		{
			if (*at == '\\' && at + 1 < end)
			{
				at++;
				// A layout's names are plain: an escape is kept as its letter.
				out += *at == 'n' ? '\n' : *at == 't' ? '\t' : *at;
			}
			else
				out += *at;
			at++;
		}
		if (at < end)
			at++;
		else
			failed = true;
		return out;
	}
	Json value()
	{
		Json json;
		space();
		if (at >= end || ++depth > 24)
		{
			failed = true;
			return json;
		}
		if (*at == '{')
		{
			json.kind = Json::Object;
			at++;
			space();
			while (at < end && *at != '}' && !failed)
			{
				space();
				if (at >= end || *at != '"')
				{
					failed = true;
					break;
				}
				std::string key = string();
				space();
				if (at >= end || *at != ':')
				{
					failed = true;
					break;
				}
				at++;
				Json member = value();
				json.names.push_back(std::move(key));
				json.values.push_back(std::move(member));
				space();
				if (at < end && *at == ',')
					at++;
				space();
			}
			if (at < end)
				at++;
		}
		else if (*at == '[')
		{
			json.kind = Json::List;
			at++;
			space();
			while (at < end && *at != ']' && !failed)
			{
				json.items.push_back(value());
				space();
				if (at < end && *at == ',')
					at++;
				space();
			}
			if (at < end)
				at++;
		}
		else if (*at == '"')
		{
			json.kind = Json::Text;
			json.text = string();
		}
		else if (end - at >= 4 && !strncmp(at, "true", 4))
		{
			json.kind = Json::Flag;
			json.number = 1;
			at += 4;
		}
		else if (end - at >= 5 && !strncmp(at, "false", 5))
		{
			json.kind = Json::Flag;
			at += 5;
		}
		else if (end - at >= 4 && !strncmp(at, "null", 4))
			at += 4;
		else
		{
			char *after = nullptr;
			const std::string digits(at, std::min<size_t>((size_t)(end - at), 40));
			json.number = strtod(digits.c_str(), &after);
			if (after == digits.c_str())
				failed = true;
			else
			{
				json.kind = Json::Number;
				at += after - digits.c_str();
			}
		}
		depth--;
		return json;
	}
};

V3 v3(const Json& json, V3 otherwise = {})
{
	if (json.kind != Json::List || json.items.size() < 3)
		return otherwise;
	V3 v{ (float)json.items[0].number, (float)json.items[1].number, (float)json.items[2].number };
	// A number no view is made of.
	for (float f : { v.x, v.y, v.z })
		if (!(std::fabs(f) < 1000.f))
			return otherwise;
	return v;
}

Mode modeFrom(const Json& json)
{
	Mode m;
	const Json& covers = json["covers"];
	const Json& position = covers["position"], &angle = covers["angle"], &scale = covers["scale"];
	const Json& spacer = covers["spacer"], &delta = covers["deltaangle"], &swing = covers["oscillation"];
	m.centerPos = v3(position["center"]);
	m.centerAngle = v3(angle["center"]);
	m.centerScale = v3(scale["center"], { 1, 1, 1 });
	m.rowPos = v3(position["rowcenter"]);
	m.rowAngle = v3(angle["rowcenter"]);
	m.rowScale = v3(scale["rowcenter"], { 1, 1, 1 });
	m.left = side(v3(position["left"]), v3(angle["left"]), v3(spacer["left"]), v3(delta["left"]), v3(scale["left"], { 1, 1, 1 }));
	m.right = side(v3(position["right"]), v3(angle["right"]), v3(spacer["right"]), v3(delta["right"]),
			v3(scale["right"], { 1, 1, 1 }));
	m.topSpacer = v3(spacer["top"]);
	m.bottomSpacer = v3(spacer["bottom"]);
	m.topAngle = v3(angle["top"]);
	m.bottomAngle = v3(angle["bottom"]);
	m.topDelta = v3(delta["top"]);
	m.bottomDelta = v3(delta["bottom"]);
	m.swingAngleSpeed = v3(swing["angle"]["speed"]);
	m.swingAngle = v3(swing["angle"]["amplitude"]);
	m.swingPosSpeed = v3(swing["position"]["speed"]);
	m.swingPos = v3(swing["position"]["amplitude"]);
	m.eye = v3(json["camera"]["position"], { 0, 0, -7 });
	m.aim = v3(json["camera"]["aim"]);
	m.mirror = json["mirror"]["enabled"].number != 0;
	m.mirrorOffset = v3(json["mirror"]["offset"], { 0, 1, 0 });
	return m;
}

bool layoutFromFile(const std::string& path, Layout& layout)
{
	std::vector<uint8_t> bytes;
	if (!readFile(path, bytes) || bytes.size() < 16 || bytes.size() > (1u << 20))
		return false;
	size_t start = 0;
	if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
		start = 3;
	JsonReader reader{ reinterpret_cast<const char *>(bytes.data()) + start, reinterpret_cast<const char *>(bytes.data()) + bytes.size() };
	const Json root = reader.value();
	if (reader.failed || root.kind != Json::Object || root["modes"].kind != Json::List)
		return false;
	const Json *normal = nullptr, *initial = nullptr;
	for (const Json& mode : root["modes"].items)
	{
		if (mode["name"].text == "Normal")
			normal = &mode;
		else if (mode["name"].text == "Initial")
			initial = &mode;
	}
	if (normal == nullptr)
		return false;
	layout = Layout();
	layout.own = false;
	layout.name = root["info"]["name"].text;
	if (layout.name.empty())
		layout.name = fileTitle(path);
	if (layout.name.size() > 40)
		layout.name.resize(40);
	layout.rows = std::clamp((int)root["properties"]["rows"].number, 1, 9);
	layout.columns = std::clamp((int)root["properties"]["columns"].number, 3, 41);
	layout.normal = modeFrom(*normal);
	if (initial != nullptr)
	{
		layout.initial = modeFrom(*initial);
		layout.hasInitial = true;
	}
	return true;
}

std::vector<Layout> layouts;
bool layoutsRead;

void readLayouts()
{
	layouts = ownLayouts();
	layoutsRead = true;
	const std::string folder = rootDir + "layouts";
	std::vector<std::string> files;
	if (DIR *dir = opendir(folder.c_str()))
	{
		while (const dirent *entry = readdir(dir))
			if (entry->d_name[0] != '.')
				files.push_back(entry->d_name);
		closedir(dir);
	}
	std::sort(files.begin(), files.end());
	int read = 0;
	for (const std::string& name : files)
	{
		if (lowercase(extension(name)) != ".cfljson" || layouts.size() >= 60)
			continue;
		Layout layout;
		if (layoutFromFile(folder + "/" + name, layout))
		{
			// Two of one name are told apart by their files.
			for (const Layout& other : layouts)
				if (other.name == layout.name)
				{
					layout.name = fileTitle(name);
					break;
				}
			layouts.push_back(layout);
			read++;
		}
		else
			diag::mark("layouts: %s is not a layout this title can read", name.c_str());
	}
	if (read > 0)
		diag::mark("layouts: %d read from %s", read, folder.c_str());
}

// ------------------------------------------------------------------- placing

struct Placement
{
	V3 pos, angle, scale{ 1, 1, 1 };
};

V3 swing(V3 speed, V3 amount, double time)
{
	return { amount.x * (float)std::sin(speed.x * time), amount.y * (float)std::sin(speed.y * time),
			amount.z * (float)std::sin(speed.z * time) };
}

// Where the case `k` columns to the right of the cursor and `j` rows below it
// is; both may be between whole numbers, while the view moves.
// `rowAway`: how many rows the cursor's is from this one.
Placement place(const Mode& m, float k, float j, float rowAway, double time)
{
	const float ak = std::fabs(k), aj = std::fabs(j);
	const Side& s = k < 0 ? m.left : m.right;
	Placement out;
	if (ak >= 1.f)
	{
		out.pos = s.pos + s.spacer * (ak - 1.f);
		out.angle = s.angle + s.delta * (ak - 1.f);
		out.scale = s.scale;
	}
	else
	{
		const float row = std::min(rowAway, 1.f);
		const float t = ak * ak * (3.f - 2.f * ak);
		out.pos = mix3(mix3(m.centerPos, m.rowPos, row), s.pos, t);
		out.angle = mix3(mix3(m.centerAngle, m.rowAngle, row), s.angle, t);
		out.scale = mix3(mix3(m.centerScale, m.rowScale, row), s.scale, t);
	}
	if (aj > 0)
	{
		const bool below = j > 0;
		out.pos = out.pos + (below ? m.bottomSpacer : m.topSpacer) * aj;
		out.angle = out.angle + (below ? m.bottomAngle : m.topAngle) * std::min(aj, 1.f)
				+ (below ? m.bottomDelta : m.topDelta) * std::max(aj - 1.f, 0.f);
	}
	out.pos = out.pos + swing(m.swingPosSpeed, m.swingPos, time);
	out.angle = out.angle + swing(m.swingAngleSpeed, m.swingAngle, time);
	return out;
}

Placement between(const Placement& a, const Placement& b, float t)
{
	return { mix3(a.pos, b.pos, t), mix3(a.angle, b.angle, t), mix3(a.scale, b.scale, t) };
}

// ------------------------------------------------------------------- drawing

struct Camera
{
	V3 eye, right, up, forward;
	float focal = 1, cx = 0, cy = 0;
};

Camera cameraFor(V3 eye, V3 aim, float top, float bottom)
{
	// (`bottom` is above what the game's name has of the screen.)
	Camera c;
	c.eye = eye;
	c.forward = unit(aim - eye);
	// Looking straight up or down has no "up" of its own.
	const V3 worldUp = std::fabs(c.forward.y) > 0.999f ? V3{ 0, 0, 1 } : V3{ 0, 1, 0 };
	c.right = unit(cross(worldUp, c.forward));
	c.up = cross(c.forward, c.right);
	// 45 degrees from the top of the screen to its bottom, centred on the part
	// of the screen the library has.
	c.focal = height() * 0.5f / std::tan(0.3927f);
	c.cx = width() * 0.5f;
	c.cy = px((top + bottom) * 0.5f);
	return c;
}

bool project(const Camera& c, V3 p, ImVec2& out, float& depth)
{
	const V3 d = p - c.eye;
	depth = dot(d, c.forward);
	if (depth < 0.25f)
		return false;
	out.x = c.cx + dot(d, c.right) / depth * c.focal;
	out.y = c.cy - dot(d, c.up) / depth * c.focal;
	return true;
}

ImU32 shadeColour(float shade, float alpha, ImU32 base = IM_COL32_WHITE)
{
	shade = std::clamp(shade, 0.f, 1.f);
	alpha = std::clamp(alpha, 0.f, 1.f);
	return IM_COL32((int)((base & 0xff) * shade), (int)(((base >> 8) & 0xff) * shade), (int)(((base >> 16) & 0xff) * shade),
			(int)(((base >> 24) & 0xff) * alpha));
}

// One case to draw.
struct Case
{
	int index = 0;
	Placement at;
	float w = 1, h = 1;		// its size, before its scale
	float depth = 0;		// of its middle, from the camera
	float light = 1, alpha = 1;
	bool focused = false;
};

constexpr float Thickness = 0.07f;

V3 worldOf(const Case& c, V3 model, bool mirrored, V3 mirrorOffset)
{
	V3 p = { model.x * c.at.scale.x, model.y * c.at.scale.y, model.z * c.at.scale.z };
	p = rotate(p, c.at.angle) + c.at.pos;
	if (mirrored)
	{
		// What a floor under the case shows: the case upside down, one case lower.
		const float floor = c.at.pos.y - mirrorOffset.y * c.h * c.at.scale.y * 0.5f;
		p.y = 2.f * floor - p.y;
		p.x -= mirrorOffset.x * c.w;
		p.z -= mirrorOffset.z;
	}
	return p;
}

// A flat side of a case.
void flatFace(const Camera& camera, const Case& c, const V3 corners[4], V3 normal, ImU32 colour)
{
	const V3 centre = (worldOf(c, corners[0], false, {}) + worldOf(c, corners[2], false, {})) * 0.5f;
	const V3 n = rotate(normal, c.at.angle);
	const float facing = dot(n, unit(camera.eye - centre));
	if (facing <= 0.02f)
		return;
	ImVec2 p[4];
	float depth;
	for (int i = 0; i < 4; i++)
		if (!project(camera, worldOf(c, corners[i], false, {}), p[i], depth))
			return;
	draw()->AddQuadFilled(p[0], p[1], p[2], p[3], shadeColour(c.light * (0.45f + 0.55f * facing), c.alpha, colour));
}

// The front of a case: its cover, as a mesh, or a card with the game's name.
// False when it is turned away or behind the camera.
bool frontFace(const Camera& camera, const Case& c, const Image& cover, const library::Game& game, bool mirrored,
		V3 mirrorOffset, ImVec2 outline[4])
{
	const float hw = c.w * 0.5f, hh = c.h * 0.5f, z = -Thickness * 0.5f;
	const V3 centre = worldOf(c, { 0, 0, z }, mirrored, mirrorOffset);
	V3 n = rotate({ 0, 0, -1 }, c.at.angle);
	if (mirrored)
		n.y = -n.y;
	if (dot(n, unit(camera.eye - centre)) <= 0.02f)
		return false;
	const V3 corners[4] = { { -hw, hh, z }, { hw, hh, z }, { hw, -hh, z }, { -hw, -hh, z } };
	float nearest = 1e9f, farthest = 0;
	for (int i = 0; i < 4; i++)
	{
		float depth;
		if (!project(camera, worldOf(c, corners[i], mirrored, mirrorOffset), outline[i], depth))
			return false;
		nearest = std::min(nearest, depth);
		farthest = std::max(farthest, depth);
	}
	// Under the reflection the picture fades towards its far end.
	const auto colourAt = [&c, mirrored](float v) {
		if (!mirrored)
			return shadeColour(c.light, c.alpha);
		return shadeColour(c.light * 0.8f, c.alpha * 0.34f * std::clamp(1.f - (1.f - v) * 1.7f, 0.f, 1.f));
	};
	ImDrawList *list = draw();
	// A game with no cover shows the card the grid shows, as a picture.
	const Image shown = cover.id != nullptr && cover.width > 0 && cover.height > 0 ? cover
			: placeholderImage(game.name, game.region, c.w / std::max(c.h, 0.01f));
	if (shown.id != nullptr && shown.width > 0 && shown.height > 0)
	{
		// The part of the picture the case shows, when it is not the picture's shape.
		float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
		const float pictureAspect = (float)shown.width / (float)shown.height, caseAspect = c.w / c.h;
		if (pictureAspect > caseAspect)
		{
			const float part = caseAspect / pictureAspect;
			u0 = 0.5f - part * 0.5f;
			u1 = 0.5f + part * 0.5f;
		}
		else if (pictureAspect < caseAspect)
		{
			const float part = pictureAspect / caseAspect;
			v0 = 0.5f - part * 0.5f;
			v1 = 0.5f + part * 0.5f;
		}
		// The more it leans away, the finer the mesh: each cell is drawn flat.
		const int cells = farthest / nearest > 1.12f ? 8 : farthest / nearest > 1.03f ? 4 : 1;
		list->PushTexture((ImTextureID)shown.id);
		list->PrimReserve(cells * cells * 6, (cells + 1) * (cells + 1));
		const unsigned first = list->_VtxCurrentIdx;
		for (int row = 0; row <= cells; row++)
			for (int column = 0; column <= cells; column++)
			{
				const float u = (float)column / (float)cells, v = (float)row / (float)cells;
				ImVec2 p;
				float depth;
				project(camera, worldOf(c, { -hw + c.w * u, hh - c.h * v, z }, mirrored, mirrorOffset), p, depth);
				list->PrimWriteVtx(p, ImVec2(u0 + (u1 - u0) * u, v0 + (v1 - v0) * v), colourAt(v));
			}
		for (int row = 0; row < cells; row++)
			for (int column = 0; column < cells; column++)
			{
				const unsigned a = first + (unsigned)(row * (cells + 1) + column), b = a + 1;
				const unsigned d = a + (unsigned)(cells + 1), e = d + 1;
				list->PrimWriteIdx((ImDrawIdx)a);
				list->PrimWriteIdx((ImDrawIdx)b);
				list->PrimWriteIdx((ImDrawIdx)e);
				list->PrimWriteIdx((ImDrawIdx)a);
				list->PrimWriteIdx((ImDrawIdx)e);
				list->PrimWriteIdx((ImDrawIdx)d);
			}
		list->PopTexture();
	}
	return true;
}

// Where the focused case's heart goes, once the scene is drawn.
ImVec2 heartAt;
bool heartShown = false;

void drawCase(const Camera& camera, const Case& c, const Image& cover, const library::Game& game)
{
	const Theme& t = theme();
	const float hw = c.w * 0.5f, hh = c.h * 0.5f, hd = Thickness * 0.5f;
	const ImU32 plastic = IM_COL32(44, 47, 58, 255), edge = IM_COL32(70, 74, 88, 255);
	// The sides that face the camera: no two of a box's cover one another.
	const V3 back[4] = { { hw, hh, hd }, { -hw, hh, hd }, { -hw, -hh, hd }, { hw, -hh, hd } };
	const V3 left[4] = { { -hw, hh, hd }, { -hw, hh, -hd }, { -hw, -hh, -hd }, { -hw, -hh, hd } };
	const V3 right[4] = { { hw, hh, -hd }, { hw, hh, hd }, { hw, -hh, hd }, { hw, -hh, -hd } };
	const V3 top[4] = { { -hw, hh, hd }, { hw, hh, hd }, { hw, hh, -hd }, { -hw, hh, -hd } };
	const V3 bottom[4] = { { -hw, -hh, -hd }, { hw, -hh, -hd }, { hw, -hh, hd }, { -hw, -hh, hd } };
	flatFace(camera, c, back, { 0, 0, 1 }, plastic);
	flatFace(camera, c, left, { -1, 0, 0 }, edge);
	flatFace(camera, c, right, { 1, 0, 0 }, edge);
	flatFace(camera, c, top, { 0, 1, 0 }, edge);
	flatFace(camera, c, bottom, { 0, -1, 0 }, edge);
	ImVec2 outline[4];
	if (!frontFace(camera, c, cover, game, false, {}, outline))
		return;
	// A soft line round the front: the mesh's own edge is a hard one.
	draw()->AddPolyline(outline, 4, shadeColour(c.light, c.alpha * 0.55f, IM_COL32(12, 14, 20, 255)), px(1.5f), ImDrawFlags_Closed);
	if (c.focused)
	{
		const float pulse = motion() == MotionFull ? 0.5f + 0.5f * (float)std::sin(clock() * 3.0) : 1.f;
		draw()->AddPolyline(outline, 4, withAlpha(t.accent, 0.25f + 0.15f * pulse), px(11), ImDrawFlags_Closed);
		draw()->AddPolyline(outline, 4, t.accent, px(4), ImDrawFlags_Closed);
		if (library::favourite(game.path))
		{
			// Drawn flat over the scene (its words are not part of its mesh).
			heartAt = ImVec2(outline[0].x + px(12), outline[0].y + px(12));
			heartShown = true;
		}
	}
}

// What the view remembers between frames.
struct State
{
	int layout = -1;
	// The column in the middle of the view and, apart from it where a wall
	// stays put, the cursor's; the cursor's row.
	float column = 0, cursorColumn = 0, row = 0;
	double opened = -10;
} state;

} // namespace

std::vector<std::string> flowNames()
{
	if (!layoutsRead)
		readLayouts();
	std::vector<std::string> names;
	for (const Layout& layout : layouts)
		names.push_back(layout.name);
	return names;
}

std::vector<std::string> libraryViewNames()
{
	std::vector<std::string> names = { "Shelves", "Covers", "List" };
	for (const std::string& name : flowNames())
		names.push_back(name);
	return names;
}

bool flowIsOwn(int index)
{
	if (!layoutsRead)
		readLayouts();
	return index >= 0 && index < (int)layouts.size() && layouts[(size_t)index].own;
}

void flowRescan()
{
	layoutsRead = false;
}

bool flowView(int index, const FlowGames& games, int& cursor, bool active, bool fresh, float top, float bottom)
{
	if (!layoutsRead)
		readLayouts();
	if (index < 0 || index >= (int)layouts.size() || games.count <= 0)
		return false;
	const Layout& layout = layouts[(size_t)index];
	const Theme& t = theme();
	const float W = unitsWide();
	const int rows = layout.rows, count = games.count;
	const int columnCount = (count + rows - 1) / rows;
	cursor = std::clamp(cursor, 0, count - 1);

	// Games fill a column from the top, then the next: the long way is across.
	if (active)
	{
		const bool along = layout.vertical && rows == 1;
		const bool next = nav(Right) || (along && nav(Down)), previous = nav(Left) || (along && nav(Up));
		if (next)
			cursor = std::min(cursor + rows, count - 1);
		if (previous && cursor - rows >= 0)
			cursor -= rows;
		if (rows > 1)
		{
			if (nav(Down) && cursor % rows < rows - 1 && cursor + 1 < count)
				cursor++;
			if (nav(Up) && cursor % rows > 0)
				cursor--;
		}
	}
	const float cursorColumn = (float)(cursor / rows), wantRow = (float)(cursor % rows);
	// A wall fills the screen from its first column to its last: it moves only
	// when the cursor nears an edge, and a short one stands in the middle.
	float wantColumn = cursorColumn;
	if (layout.fixedRows)
	{
		const float inner = 2.f;
		const float last = (float)(columnCount - 1);
		wantColumn = last <= inner * 2.f ? last * 0.5f : std::clamp(cursorColumn, inner, last - inner);
	}
	if (fresh || state.layout != index)
	{
		state.layout = index;
		state.column = wantColumn;
		state.cursorColumn = cursorColumn;
		state.row = wantRow;
		state.opened = clock();
	}
	if (motion() == MotionOff)
	{
		state.column = wantColumn;
		state.cursorColumn = cursorColumn;
		state.row = wantRow;
	}
	else
	{
		state.column = approach(state.column, wantColumn, 11.f);
		state.cursorColumn = approach(state.cursorColumn, cursorColumn, 14.f);
		state.row = approach(state.row, wantRow, layout.fixedRows ? 14.f : 11.f);
		// A jump across the library is not flown through case by case.
		if (std::fabs(state.column - wantColumn) > 12.f)
			state.column = wantColumn + (state.column > wantColumn ? 12.f : -12.f);
		if (std::fabs(state.cursorColumn - cursorColumn) > 3.f)
			state.cursorColumn = cursorColumn;
	}
	// The cases come from where the layout says they start.
	float arrived = 1;
	if (motion() == MotionFull)
		arrived = glide(std::clamp((float)(clock() - state.opened) / 0.7f, 0.f, 1.f));
	const double time = clock();
	const Mode& mode = layout.normal;
	const V3 eye = layout.hasInitial ? mix3(layout.initial.eye, mode.eye, arrived) : mode.eye;
	const V3 aim = layout.hasInitial ? mix3(layout.initial.aim, mode.aim, arrived) : mode.aim;
	const Camera camera = cameraFor(eye, aim, top, bottom - layout.foot);

	std::vector<Case> cases;
	const int reach = std::max(layout.columns / 2, 1);
	const int firstColumn = std::max((int)std::floor(state.column) - reach, 0);
	const int lastColumn = std::min((int)std::ceil(state.column) + reach, columnCount - 1);
	for (int column = firstColumn; column <= lastColumn; column++)
		for (int row = 0; row < rows; row++)
		{
			const int game = column * rows + row;
			if (game >= count)
				break;
			const float k = (float)column - state.column, rowAway = std::fabs((float)row - state.row);
			const float j = layout.fixedRows ? (float)row - (float)(rows - 1) * 0.5f : (float)row - state.row;
			Case c;
			c.index = game;
			// How far the cursor is, in cases: what the light goes by.
			float away = std::max(std::fabs(k), rowAway * 1.5f);
			if (layout.fixedRows)
			{
				// Every case is in its place on the wall; the one under the
				// cursor comes forward from it.
				c.at = place(mode, k, j, 1.f, time);
				const float columnAway = std::fabs((float)column - state.cursorColumn);
				const float near = std::clamp(1.f - std::max(columnAway, rowAway), 0.f, 1.f);
				c.at.pos = c.at.pos + (mode.centerPos - mode.rowPos) * near;
				c.at.angle = c.at.angle + (mode.centerAngle - mode.rowAngle) * near;
				away = std::max(columnAway, rowAway);
			}
			else
				c.at = place(mode, k, j, rowAway, time);
			if (arrived < 1.f)
			{
				// With no start of its own, a layout's cases rise from under it.
				Placement from = c.at;
				if (layout.hasInitial)
					from = place(layout.initial, k, j, rowAway, time);
				else
				{
					from.pos.y -= 2.6f + 0.25f * std::fabs(k);
					from.pos.z += 1.5f;
				}
				c.at = between(from, c.at, arrived);
			}
			c.focused = game == cursor;
			// A case is one unit high; a cover that is not a case's shape makes
			// it wider or narrower, within reason. An Aurora layout's distances
			// are for cases 0.71 wide: there a cover keeps that much room.
			const Image cover = games.cover(game);
			float aspect = cover.id != nullptr && cover.height > 0 ? (float)cover.width / (float)cover.height : 1.f;
			aspect = std::clamp(aspect, 0.62f, 1.3f);
			if (layout.own)
			{
				c.h = 1.f;
				c.w = aspect;
			}
			else
			{
				c.w = std::sqrt(0.71f * aspect);
				c.h = std::min(c.w / aspect, 1.f);
				c.w = c.h * aspect;
			}
			ImVec2 unused;
			if (!project(camera, c.at.pos, unused, c.depth))
				continue;
			// Dimmer away from the cursor, and gone softly at the ends.
			c.light = c.focused ? 1.f : std::clamp(0.92f - 0.045f * away, 0.38f, 0.92f);
			c.alpha = std::clamp(((float)reach + 0.5f - std::fabs(k)) / 1.5f, 0.f, 1.f) * std::clamp(arrived * 1.6f, 0.f, 1.f);
			if (c.alpha > 0.01f)
				cases.push_back(c);
		}
	// The farthest first; of two as far, the one away from the cursor first.
	std::stable_sort(cases.begin(), cases.end(), [](const Case& a, const Case& b) {
		if (std::fabs(a.depth - b.depth) > 1e-4f)
			return a.depth > b.depth;
		return a.focused < b.focused;
	});

	// The cases are a 3D scene: a mesh, drawn by its own pass into a picture
	// the size of the screen, which the page then shows where it belongs.
	heartShown = false;
	beginScene();
	if (mode.mirror)
	{
		ImVec2 outline[4];
		for (const Case& c : cases)
			frontFace(camera, c, games.cover(c.index), games.game(c.index), true, mode.mirrorOffset, outline);
	}
	for (const Case& c : cases)
		drawCase(camera, c, games.cover(c.index), games.game(c.index));
	void *scene = display::scene(endScene());
	draw()->PushClipRect(at(0, top - 8), at(W, bottom), true);
	if (scene != nullptr)
		draw()->AddImage((ImTextureID)scene, ImVec2(0, 0), ImVec2(width(), height()));
	if (heartShown)
	{
		draw()->AddCircleFilled(ImVec2(heartAt.x + px(16), heartAt.y + px(16)), px(19), IM_COL32(0, 0, 0, 170));
		text(ImVec2(heartAt.x + px(6), heartAt.y + px(6)), IM_COL32(255, 96, 128, 255), icon::Heart, Body, 20);
	}
	draw()->PopClipRect();

	// The game under the cursor, in words.
	const library::Game& game = games.game(cursor);
	const std::string facts = games.facts ? games.facts(cursor) : std::string();
	const float fade = std::clamp(arrived * 1.4f - 0.4f, 0.f, 1.f);
	if (layout.caption == 1)
	{
		const float x = 96, wide = W * 0.30f;
		float y = top + 96;
		y += toUnits(textWrapped(at(x, y), px(wide), withAlpha(t.text, fade), game.name, Bold, 40, px(150))) + 14;
		text(at(x, y), withAlpha(t.accent, fade), facts, Bold, 21);
		y += 46;
		const std::string about = games.about ? games.about(cursor) : std::string();
		if (!about.empty())
			textWrapped(at(x, y), px(wide), withAlpha(t.dim, fade), about, Body, 22, px(bottom - 60 - y));
	}
	else
	{
		// A darker foot for the words to stand on.
		const float foot = bottom - 150;
		draw()->AddRectFilledMultiColor(at(0, foot - 60), at(W, bottom), IM_COL32(8, 10, 18, 0), IM_COL32(8, 10, 18, 0),
				IM_COL32(8, 10, 18, (int)(215 * fade)), IM_COL32(8, 10, 18, (int)(215 * fade)));
		const float nameWide = std::min(toUnits(measure(game.name, Bold, 36).x), W - 240);
		textFit(at((W - nameWide) * 0.5f, foot + 44), px(nameWide + 4), withAlpha(t.text, fade), game.name, Bold, 36);
		textCentred(at(W * 0.5f, foot + 96), withAlpha(t.accent, fade), facts, Bold, 20);
	}
	return true;
}

}
