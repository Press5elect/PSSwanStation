#version 450
// PSSwanStation - the picture's look, at the game's own size: the part of the
// texture that is picture, as a television's input would carry it (the
// PlayStation's dither undone, S-Video or composite), then its colours.
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 picture;	// xy: the picture's size in texels; zw: how much of the texture is picture
	vec4 signal;	// x: 0 as it is, 1 dither undone, 2 S-Video, 3 composite; y: texels to a
					// PlayStation pixel; z: the frame's half (0, 1); w: colour carrier cycles a
					// PlayStation pixel
	vec4 colour;	// brightness, contrast, saturation, gamma (1 1 1 1: unchanged)
	vec4 more;		// x: how far apart two colours may be and still be one under dither
} pc;
layout (location = 0) out vec4 FragColor;

vec3 at(vec2 texel)
{
	texel = clamp(floor(texel) + 0.5, vec2(0.5), pc.picture.xy - 0.5);
	return textureLod(source, texel / pc.picture.xy * pc.picture.zw, 0.0).rgb;
}

const mat3 toYiq = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
const mat3 toRgb = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);

// The PlayStation adds a 4 x 4 pattern to its colours before it drops them to
// five bits; a tent over five of its pixels each way weighs every place of the
// pattern alike. A neighbour too far from this colour is an edge, not dither.
vec3 undither(vec2 p, float cell)
{
	const vec3 c = at(p);
	vec3 sum = c;
	float weight = 1.0;
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++)
		{
			if (dx == 0 && dy == 0)
				continue;
			const vec3 n = at(p + vec2(dx, dy) * cell);
			const vec3 d = abs(n - c);
			if (max(d.r, max(d.g, d.b)) > pc.more.x)
				continue;
			const float w = (abs(dx) == 2 ? 0.5 : 1.0) * (abs(dy) == 2 ? 0.5 : 1.0);
			sum += n * w;
			weight += w;
		}
	return sum / weight;
}

// One line of the picture as a television's signal: brightness and colour
// (YIQ) apart (S-Video), or the colour riding on the brightness at the colour
// carrier's frequency (composite), each read back through its filter.
vec3 analogue(vec2 p, float cell, bool composite)
{
	const float cycles = pc.signal.w;
	const float period = 1.0 / max(cycles, 0.05);
	const float line = floor(p.y / cell);
	// The carrier's phase turns half a cycle each line and each frame.
	const float phase = 3.14159265 * mod(line + pc.signal.z, 2.0);
	const float sigmaY = composite ? 0.45 * period : 0.25;
	const float sigmaC = 1.6 * period;
	float y = 0.0, i = 0.0, q = 0.0, wy = 0.0, wc = 0.0;
	for (int j = -24; j <= 24; j++)
	{
		const float d = float(j) / 3.0;			// in PlayStation pixels
		const vec3 yiq = toYiq * at(vec2(p.x + d * cell, p.y));
		const float ky = exp(-0.5 * d * d / (sigmaY * sigmaY));
		const float kc = exp(-0.5 * d * d / (sigmaC * sigmaC));
		if (composite)
		{
			const float theta = 6.2831853 * cycles * (floor(p.x / cell) + d) + phase;
			const float s = yiq.x + yiq.y * cos(theta) + yiq.z * sin(theta);
			y += ky * s;
			i += kc * s * cos(theta);
			q += kc * s * sin(theta);
		}
		else
		{
			y += ky * yiq.x;
			i += kc * yiq.y;
			q += kc * yiq.z;
		}
		wy += ky;
		wc += kc;
	}
	const float gain = composite ? 2.0 : 1.0;
	return toRgb * vec3(y / wy, gain * i / wc, gain * q / wc);
}

void main()
{
	const vec2 p = gl_FragCoord.xy;
	const float cell = max(pc.signal.y, 1.0);
	vec3 c;
	if (pc.signal.x > 2.5)
		c = analogue(p, cell, true);
	else if (pc.signal.x > 1.5)
		c = analogue(p, cell, false);
	else if (pc.signal.x > 0.5)
		c = undither(p, cell);
	else
		c = at(p);
	c = (c - 0.5) * pc.colour.y + 0.5;
	c *= pc.colour.x;
	c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, pc.colour.z);
	c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / pc.colour.w));
	FragColor = vec4(c, 1.0);
}
