#version 450
// PSSwanStation - a picture drawn into a rectangle of the screen (the game's,
// under the interface): two triangles, the corners from the push constants.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (push_constant) uniform pushBlock
{
	vec4 rect;	// x0 y0 x1 y1, in clip space
	vec4 uv;	// u0 v0 u1 v1
	vec4 tint;	// multiplied in: alpha for a dimmed picture
} pc;
layout (location = 0) out vec2 uv;
void main()
{
	const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
	const vec2 c = corners[gl_VertexIndex];
	gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c), 0.0, 1.0);
	uv = mix(pc.uv.xy, pc.uv.zw, c);
}
