#version 450
// PSSwanStation - a picture drawn into a rectangle of the screen: sampled as
// the bound sampler says (nearest for square pixels), times the tint.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D picture;
layout (push_constant) uniform pushBlock
{
	vec4 rect;
	vec4 uv;
	vec4 tint;
} pc;
layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 colour;
void main()
{
	colour = vec4(texture(picture, uv).rgb, 1.0) * pc.tint;
}
