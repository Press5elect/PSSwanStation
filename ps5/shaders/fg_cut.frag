#version 450
// PSSwanStation - frame generation: was there a cut between the picture
// before and this one? Each of nine parts of the screen compares how its
// brightness is spread in the two pictures, in 16 steps: as it is, and after
// taking away the part's average and dividing by its spread (which sees a
// changed scene of the same brightness, where the part has some contrast). A
// part changed when either changed for most of it. Where most of the parts changed, the pictures are of two
// different scenes, and nothing is made between them: a made picture would be
// the old scene smeared into the new one. (A fade counts too: nothing is lost,
// since a fade gives frame generation nothing to move.)
// One pixel: r is 1 for a cut, g how many parts changed (of 9).
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D before;
layout (set = 0, binding = 1) uniform sampler2D now;
layout (push_constant) uniform pushBlock
{
	vec4 size;		// xy: the brightness pictures' size, in texels
	vec4 more;		// x: how much of a part has to change; y: how many parts
} pc;
layout (location = 0) out vec4 FragColor;

const int Bins = 16;

void spread(sampler2D picture, ivec2 a, ivec2 b, out float mean, out float deviation)
{
	float sum = 0.0, squares = 0.0, n = 0.0;
	for (int y = a.y; y < b.y; y++)
		for (int x = a.x; x < b.x; x++)
		{
			const float v = texelFetch(picture, ivec2(x, y), 0).r;
			sum += v;
			squares += v * v;
			n += 1.0;
		}
	mean = sum / max(n, 1.0);
	deviation = sqrt(max(squares / max(n, 1.0) - mean * mean, 0.0));
}

int plainBin(float v)
{
	return clamp(int(floor(v * float(Bins))), 0, Bins - 1);
}

int bin(float v, float mean, float deviation)
{
	// Two deviations either side of the average over the 16 steps; a part
	// of one brightness all falls in the middle.
	const float z = deviation > 0.004 ? (v - mean) / deviation : 0.0;
	return clamp(int(floor((z + 2.0) * float(Bins) / 4.0)), 0, Bins - 1);
}

void main()
{
	const ivec2 n = ivec2(pc.size.xy);
	int changed = 0;
	for (int ry = 0; ry < 3; ry++)
		for (int rx = 0; rx < 3; rx++)
		{
			const ivec2 a = ivec2(rx * n.x / 3, ry * n.y / 3);
			const ivec2 b = ivec2((rx + 1) * n.x / 3, (ry + 1) * n.y / 3);
			float meanA, devA, meanB, devB;
			spread(before, a, b, meanA, devA);
			spread(now, a, b, meanB, devB);
			float h[Bins], p[Bins];
			for (int i = 0; i < Bins; i++)
				h[i] = p[i] = 0.0;
			float count = 0.0;
			for (int y = a.y; y < b.y; y++)
				for (int x = a.x; x < b.x; x++)
				{
					const float va = texelFetch(before, ivec2(x, y), 0).r;
					const float vb = texelFetch(now, ivec2(x, y), 0).r;
					h[bin(va, meanA, devA)] += 1.0;
					h[bin(vb, meanB, devB)] -= 1.0;
					p[plainBin(va)] += 1.0;
					p[plainBin(vb)] -= 1.0;
					count += 1.0;
				}
			float difference = 0.0, plain = 0.0;
			for (int i = 0; i < Bins; i++)
			{
				difference += abs(h[i]);
				plain += abs(p[i]);
			}
			// Half of a sum is the share of the part that changed its step. The
			// spread-divided steps only where both parts have some contrast:
			// in a part of nearly one colour any small thing coming into it
			// would move every pixel's step.
			const bool contrast = devA > 0.04 && devB > 0.04;
			if (max(contrast ? difference : 0.0, plain) * 0.5 / max(count, 1.0) > pc.more.x)
				changed++;
		}
	FragColor = vec4(changed >= int(pc.more.y) ? 1.0 : 0.0, float(changed) / 9.0, 0.0, 1.0);
}
