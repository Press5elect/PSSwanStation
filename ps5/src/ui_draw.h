/*
	PSSwanStation - what the interface draws with: points, colours, and a
	draw list that turns each call into the interface kit's shapes.

	SPDX-License-Identifier: GPL-3.0-or-later

	The pages were written against Dear ImGui's draw lists, and keep their
	names (ImVec2, ImU32, IM_COL32, ImDrawList's Add... calls), which the
	interface kit (PS5_VKHomebrewUI) now draws: the shapes as signed distance
	fields, the pictures as its images, in the frame's kit layers. A 3D scene
	(the library's views in space) is drawn into a mesh instead, with its own
	pass (display::meshScene), since the kit draws pictures only flat.

	Points are in the screen's pixels, as before; colours are 0xAABBGGRR.
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hui::gfx { class DrawList; }

struct ImVec2
{
	float x = 0, y = 0;
	constexpr ImVec2() = default;
	constexpr ImVec2(float x, float y) : x(x), y(y) {}
};

struct ImVec4
{
	float x = 0, y = 0, z = 0, w = 0;
	constexpr ImVec4() = default;
	constexpr ImVec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};

typedef unsigned int ImU32;
typedef void *ImTextureID;
typedef unsigned int ImDrawIdx;

#define IM_COL32(R, G, B, A) (((ImU32)(A) << 24) | ((ImU32)(B) << 16) | ((ImU32)(G) << 8) | ((ImU32)(R)))
#define IM_COL32_WHITE IM_COL32(255, 255, 255, 255)
#define IM_COL32_BLACK IM_COL32(0, 0, 0, 255)
#define IM_COL32_BLACK_TRANS IM_COL32(0, 0, 0, 0)

enum ImDrawFlags_
{
	ImDrawFlags_None = 0,
	ImDrawFlags_Closed = 1 << 0,
	ImDrawFlags_RoundCornersTopLeft = 1 << 4,
	ImDrawFlags_RoundCornersTopRight = 1 << 5,
	ImDrawFlags_RoundCornersBottomLeft = 1 << 6,
	ImDrawFlags_RoundCornersBottomRight = 1 << 7,
	ImDrawFlags_RoundCornersTop = ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersTopRight,
	ImDrawFlags_RoundCornersBottom = ImDrawFlags_RoundCornersBottomLeft | ImDrawFlags_RoundCornersBottomRight,
	ImDrawFlags_RoundCornersLeft = ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft,
	ImDrawFlags_RoundCornersRight = ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight,
	ImDrawFlags_RoundCornersAll = ImDrawFlags_RoundCornersTop | ImDrawFlags_RoundCornersBottom,
};
typedef int ImDrawFlags;

// One vertex of a 3D scene's mesh, in the screen's pixels.
struct ImDrawVert
{
	ImVec2 pos;
	ImVec2 uv;
	ImU32 col;
};

// A draw list: a kit DrawList (the interface), or, while a 3D scene is being
// drawn, the scene's mesh.
class ImDrawList
{
public:
	// The kit list the calls go into; null for a scene.
	hui::gfx::DrawList *kit = nullptr;

	void AddRectFilled(ImVec2 a, ImVec2 b, ImU32 col, float rounding = 0.f, ImDrawFlags flags = 0);
	void AddRect(ImVec2 a, ImVec2 b, ImU32 col, float rounding = 0.f, ImDrawFlags flags = 0, float thickness = 1.f);
	// Corners: upper left, upper right, lower right, lower left.
	void AddRectFilledMultiColor(ImVec2 a, ImVec2 b, ImU32 ul, ImU32 ur, ImU32 br, ImU32 bl);
	void AddLine(ImVec2 a, ImVec2 b, ImU32 col, float thickness = 1.f);
	void AddPolyline(const ImVec2 *points, int count, ImU32 col, ImDrawFlags flags, float thickness);
	void AddCircle(ImVec2 centre, float radius, ImU32 col, int segments = 0, float thickness = 1.f);
	void AddCircleFilled(ImVec2 centre, float radius, ImU32 col, int segments = 0);
	void AddTriangle(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col, float thickness = 1.f);
	void AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col);
	void AddQuadFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 col);
	void AddConvexPolyFilled(const ImVec2 *points, int count, ImU32 col);
	void AddConcavePolyFilled(const ImVec2 *points, int count, ImU32 col);
	// radius: the half widths; rotation in radians.
	void AddEllipseFilled(ImVec2 centre, ImVec2 radius, ImU32 col, float rotation = 0.f, int segments = 0);
	void AddImage(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
			ImU32 col = IM_COL32_WHITE);
	void AddImageRounded(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 col, float rounding,
			ImDrawFlags flags = 0);
	void PushClipRect(ImVec2 a, ImVec2 b, bool intersect = true);
	void PopClipRect();
	void PathLineTo(ImVec2 point);
	void PathStroke(ImU32 col, float thickness = 1.f, ImDrawFlags flags = 0);

	// A 3D scene's textured mesh, as Dear ImGui's PrimReserve and PrimWrite
	// made it: the texture pushed, then vertices and indices.
	void PushTexture(ImTextureID texture);
	void PopTexture();
	void PrimReserve(int indices, int vertices);
	void PrimWriteVtx(ImVec2 pos, ImVec2 uv, ImU32 col);
	void PrimWriteIdx(ImDrawIdx index);
	unsigned _VtxCurrentIdx = 0;
	// The scene's vertices so far (a card drawn flat is carried into place by
	// moving them); empty for a kit list.
	struct Vertices
	{
		std::vector<ImDrawVert> *v = nullptr;
		int Size = 0;
		ImDrawVert& operator[](int i) { return (*v)[(size_t)i]; }
	} VtxBuffer;

	// The scene being drawn into, if this list is one.
	struct Scene *scene = nullptr;

private:
	std::vector<ImVec2> path_;
	void sceneTriangle(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col);
};

// A 3D scene's mesh: triangles, each run with the texture it samples (null:
// plain colour).
struct Scene
{
	struct Run
	{
		void *texture = nullptr;
		uint32_t first = 0, count = 0;	// indices
	};
	std::vector<ImDrawVert> vertices;
	std::vector<uint32_t> indices;
	std::vector<Run> runs;
	std::vector<void *> textures;	// the pushed ones
	void clear()
	{
		vertices.clear();
		indices.clear();
		runs.clear();
		textures.clear();
	}
};
