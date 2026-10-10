#version 450
// PSSwanStation - a 3D scene, from premultiplied to straight alpha, which the
// interface kit draws its pictures with.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D scene;
layout (location = 0) out vec4 colour;
void main()
{
	const vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
	colour = c.a > 0.002 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
