#version 450
// PSSwanStation - a 3D scene of the interface (the library's views in space):
// its mesh, laid out in the screen's pixels.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (location = 0) in vec2 position;
layout (location = 1) in vec2 coordinate;
layout (location = 2) in vec4 colour;
layout (push_constant) uniform pushBlock
{
	vec2 screen;	// the target's size in pixels
} pc;
layout (location = 0) out vec2 uv;
layout (location = 1) out vec4 tint;
void main()
{
	gl_Position = vec4(position / pc.screen * 2.0 - 1.0, 0.0, 1.0);
	uv = coordinate;
	tint = colour;
}
