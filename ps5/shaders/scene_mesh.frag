#version 450
// PSSwanStation - a 3D scene's mesh: the picture times the vertex colour,
// written premultiplied (scene_resolve.frag makes it straight again).
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D picture;
layout (location = 0) in vec2 uv;
layout (location = 1) in vec4 tint;
layout (location = 0) out vec4 colour;
void main()
{
	const vec4 c = texture(picture, uv) * tint;
	colour = vec4(c.rgb * c.a, c.a);
}
