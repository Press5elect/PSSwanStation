#version 450
// PSSwanStation - frame generation: how each part of the picture moved between
// the frame before and this one, as seen from the picture half way between.
//
// For a place x in the picture between the two, a movement v is right when
// the frame before at x - v/2 looks like this frame at x + v/2. The movements
// tried are those around what the coarser picture above found (twice as large
// here), and no movement at all; the one whose two patches differ least is
// kept, with a small preference for the coarser picture's answer, which keeps
// flat areas from choosing at random.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (set = 0, binding = 2) uniform sampler2D coarser;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over this picture's size
	vec4 search;	// x: how many steps each way, y: a step in pixels, z: 1 when there is a coarser answer, w: the preference
} pc;
layout (location = 0) out vec4 FragColor;

float difference(vec2 uv, vec2 v)
{
	float sum = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			// Two pixels apart: each tap is a mix of four pixels, and the nine
			// of them see a patch six across.
			vec2 at = uv + vec2(x, y) * 2.0 * pc.size.xy;
			sum += abs(texture(before, at - v * 0.5 * pc.size.xy).r - texture(now, at + v * 0.5 * pc.size.xy).r);
		}
	return sum;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec2 base = pc.search.z > 0.5 ? texture(coarser, uv).rg * 2.0 : vec2(0.0);
	int reach = int(pc.search.x);
	// No movement first: what stands still is the commonest thing in a picture.
	float best = difference(uv, vec2(0.0)) + pc.search.w * length(base) * 0.5;
	vec2 found = vec2(0.0);
	for (int y = -reach; y <= reach; y++)
		for (int x = -reach; x <= reach; x++)
		{
			vec2 step = vec2(x, y) * pc.search.y;
			vec2 v = base + step;
			float d = difference(uv, v) + pc.search.w * length(step);
			if (d < best)
			{
				best = d;
				found = v;
			}
		}
	FragColor = vec4(found, best / 9.0, 1.0);
}
