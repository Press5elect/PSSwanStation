#version 450
// PSSwanStation - frame generation: the movement found (fg_search.frag), put
// in order. A patch that found a movement of its own, unlike its neighbours',
// is usually wrong, and a wrong one is a smear in the picture made. Each
// place tries the movements of the eight around it and its own, and keeps the
// one whose two patches differ least, counting against it how far it is from
// what the others say.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (set = 0, binding = 2) uniform sampler2D movement;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over this picture's size
	vec4 tidy;		// x: what a pixel's distance from the neighbours' movement counts
} pc;
layout (location = 0) out vec4 FragColor;

float difference(vec2 uv, vec2 v)
{
	float sum = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			vec2 at = uv + vec2(x, y) * 2.0 * pc.size.xy;
			sum += abs(texture(before, at - v * 0.5 * pc.size.xy).r - texture(now, at + v * 0.5 * pc.size.xy).r);
		}
	return sum;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec2 around[9];
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
			around[(y + 1) * 3 + (x + 1)] = texture(movement, uv + vec2(x, y) * pc.size.xy).rg;
	float best = 1e9;
	vec2 found = around[4];
	for (int i = 0; i < 9; i++)
	{
		vec2 v = around[i];
		float apart = 0.0;
		for (int j = 0; j < 9; j++)
			apart += min(length(v - around[j]), 4.0);
		float d = difference(uv, v) + pc.tidy.x * apart / 8.0;
		if (d < best)
		{
			best = d;
			found = v;
		}
	}
	// Finer than the half pixels it was found in: the difference a half pixel
	// to either side says where between them the least is.
	float here = difference(uv, found);
	float left = difference(uv, found - vec2(0.5, 0.0)), right = difference(uv, found + vec2(0.5, 0.0));
	float up = difference(uv, found - vec2(0.0, 0.5)), down = difference(uv, found + vec2(0.0, 0.5));
	vec2 curve = vec2(left - 2.0 * here + right, up - 2.0 * here + down);
	vec2 finer = vec2(0.0);
	if (curve.x > 1e-4 && here <= left && here <= right)
		finer.x = clamp(0.25 * (left - right) / curve.x, -0.25, 0.25);
	if (curve.y > 1e-4 && here <= up && here <= down)
		finer.y = clamp(0.25 * (up - down) / curve.y, -0.25, 0.25);
	FragColor = vec4(found + finer, best / 9.0, 1.0);
}
