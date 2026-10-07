#version 450
// PSSwanStation - frame generation: which movement each part of the picture
// has, decided on the picture itself.
//
// The movement was found for patches of a small picture (fg_search.frag,
// fg_tidy.frag); where two things meet, a patch has the one's movement or the
// other's, and their mix is neither's. Here, on a picture half the game's
// size, and for the moment between the two frames that is being drawn, each
// place tries the movements of the four patches round it, of four further off, their
// mix (right where things move evenly), and no movement at all (which is
// what writing over the game's picture has, whatever moves behind it), and
// keeps the one for which the frame before, at where this place came from,
// looks most like this frame at where it is going. Looks like: at five
// points, not one, so that a striped or dotted thing is not taken for
// another part of itself. The movement kept is then tried a pixel to each
// side as well, which sets thin lines right. Last, how far the two frames
// agree under it is kept beside it: fg_blend.frag believes it that far.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (set = 0, binding = 2) uniform sampler2D movement;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: one over this picture's size; zw: one over the movement picture's size
	vec4 more;		// xy: one over the game's picture's size; z: 1 to try a pixel to each side; w: the phase, 0 the frame before, 1 this one
} pc;
layout (location = 0) out vec4 FragColor;

float most(vec3 v)
{
	return max(v.r, max(v.g, v.b));
}

// How unlike the frame before and this one are round `uv` under a movement
// (in parts of the picture): 0 the same, 1 nothing alike.
float unlike(vec2 uv, vec2 v)
{
	// From the frame before, the place has come `phase` of its way; to this
	// frame it has the rest to go.
	vec2 back = v * pc.more.w, on = v * (1.0 - pc.more.w);
	vec2 low = min(uv - back, uv + on), high = max(uv - back, uv + on);
	// Taken from beyond the picture's edge, it is no picture.
	if (low.x < 0.0 || low.y < 0.0 || high.x > 1.0 || high.y > 1.0)
		return 1.0;
	vec2 step = pc.size.xy * 1.5;
	float sum = most(abs(texture(before, uv - back).rgb - texture(now, uv + on).rgb)) * 2.0;
	for (int i = 0; i < 4; i++)
	{
		vec2 at = uv + vec2(i == 0 ? step.x : i == 1 ? -step.x : 0.0, i == 2 ? step.y : i == 3 ? -step.y : 0.0);
		sum += most(abs(texture(before, at - back).rgb - texture(now, at + on).rgb));
	}
	return sum / 6.0;
}

void main()
{
	vec2 uv = gl_FragCoord.xy * pc.size.xy;
	vec2 corner = (floor(uv / pc.size.zw - 0.5) + 0.5) * pc.size.zw;
	float least = 1e9, apart = 1.0, patches = 0.0;
	vec2 kept = vec2(0.0);
	for (int i = 0; i < 10; i++)
	{
		// xy: the movement, in the movement picture's pixels; z: how far apart
		// the two patches it was found for were.
		vec3 found = vec3(0.0);
		if (i == 0)
			found = texture(movement, uv).rgb;
		else if (i < 5)
			found = texture(movement, corner + vec2((i - 1) & 1, (i - 1) >> 1) * pc.size.zw).rgb;
		else if (i < 9)
		{
			// And of the patches a little further off, each way: at the very
			// edge of a thing, the patches nearest may all have gone with
			// what is behind it.
			int k = i - 5;
			found = texture(movement, corner + (vec2(0.5) + vec2(k == 0 ? 5.0 : k == 1 ? -5.0 : 0.0, k == 2 ? 5.0 : k == 3 ? -5.0 : 0.0)) * pc.size.zw).rgb;
		}
		vec2 v = found.xy * pc.size.zw;
		float differ = unlike(uv, v);
		// A patch's own movement has to be clearly the better one, one
		// further off more clearly, and standing still clearly better than
		// moving.
		float counted = differ + (i == 0 ? 0.0 : i < 5 ? 0.008 : i < 9 ? 0.012 : 0.02);
		if (counted < least)
		{
			least = counted;
			apart = differ;
			patches = found.z;
			kept = v;
		}
	}
	if (pc.more.z > 0.5 && kept != vec2(0.0))
	{
		// A pixel of the game's picture more or less, each way.
		vec2 around = kept;
		for (int i = 0; i < 4; i++)
		{
			vec2 v = around + vec2(i == 0 ? pc.more.x : i == 1 ? -pc.more.x : 0.0, i == 2 ? pc.more.y : i == 3 ? -pc.more.y : 0.0);
			float differ = unlike(uv, v);
			if (differ + 0.002 < apart)
			{
				apart = differ;
				kept = v;
			}
		}
	}
	// Believed when the two frames agree under it, here and in the patches it
	// was found for (soft, small pictures, which grain does not disturb).
	float alike = (1.0 - smoothstep(0.10, 0.26, apart)) * (1.0 - smoothstep(0.05, 0.12, patches));
	FragColor = vec4(kept, alike, 1.0);
}
