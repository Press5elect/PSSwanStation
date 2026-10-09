#version 450
// PSSwanStation - the picture made smaller for the screen: each pixel of the
// screen is the average of the picture's texels under it, each weighted by how
// much of it the pixel covers. A game drawn at 8x and shown on a 1080p screen
// keeps everything that was drawn (its edges smooth, nothing shimmers), where
// a bilinear sample takes four texels of the nine or more under a pixel.
// Along an axis that is enlarged rather than reduced it is bilinear.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 picture;	// xy: the picture's size in texels
	vec4 target;	// xy: the size it is made
} pc;
layout (location = 0) out vec4 FragColor;

// How much of texel i (from i to i + 1) lies within [lo, hi].
float cover(float i, float lo, float hi)
{
	return max(min(i + 1.0, hi) - max(i, lo), 0.0);
}

void main()
{
	const vec2 ratio = pc.picture.xy / pc.target.xy;
	// The pixel's footprint on the picture, at least a texel wide (which is
	// bilinear where the picture is enlarged).
	const vec2 width = max(ratio, vec2(1.0));
	const vec2 centre = gl_FragCoord.xy * ratio;
	const vec2 lo = centre - 0.5 * width;
	const vec2 hi = centre + 0.5 * width;
	const ivec2 size = ivec2(pc.picture.xy);
	const ivec2 first = ivec2(floor(lo));
	const ivec2 last = ivec2(ceil(hi)) - 1;
	vec3 sum = vec3(0.0);
	float total = 0.0;
	// At most 9 texels each way (a picture up to eight times the screen's).
	for (int y = first.y; y <= min(last.y, first.y + 8); y++)
	{
		const float wy = cover(float(y), lo.y, hi.y);
		for (int x = first.x; x <= min(last.x, first.x + 8); x++)
		{
			const float w = wy * cover(float(x), lo.x, hi.x);
			sum += w * texelFetch(source, clamp(ivec2(x, y), ivec2(0), size - 1), 0).rgb;
			total += w;
		}
	}
	FragColor = vec4(sum / max(total, 1e-6), 1.0);
}
