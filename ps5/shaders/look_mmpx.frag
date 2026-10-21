#version 450
// PSSwanStation - MMPX: the picture made twice its size the way pixel art is
// drawn larger by hand, keeping its lines one colour and its corners sharp.
// After Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving Pixel Art
// Magnification", Journal of Computer Graphics Techniques 10(2), 2021, whose
// reference implementation is MIT-licensed (ps5/licenses/README.txt). Each
// texel E becomes four, J K / L M, from its neighbours:
//
//              P
//          A   B   C
//      Q   D   E   F   R
//          G   H   I
//              S
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
layout (set = 0, binding = 0) uniform sampler2D source;
layout (push_constant) uniform pushBlock
{
	vec4 picture;	// xy: the picture's size in texels
} pc;
layout (location = 0) out vec4 FragColor;

ivec2 size;

vec3 src(ivec2 p)
{
	return texelFetch(source, clamp(p, ivec2(0), size - 1), 0).rgb;
}

bool eq(vec3 a, vec3 b) { return all(equal(a, b)); }
bool ne(vec3 a, vec3 b) { return any(notEqual(a, b)); }
float luma(vec3 c) { return c.r + c.g + c.b; }
bool allEq2(vec3 b, vec3 a0, vec3 a1) { return eq(b, a0) && eq(b, a1); }
bool allEq3(vec3 b, vec3 a0, vec3 a1, vec3 a2) { return eq(b, a0) && eq(b, a1) && eq(b, a2); }
bool allEq4(vec3 b, vec3 a0, vec3 a1, vec3 a2, vec3 a3) { return eq(b, a0) && eq(b, a1) && eq(b, a2) && eq(b, a3); }
bool anyEq3(vec3 b, vec3 a0, vec3 a1, vec3 a2) { return eq(b, a0) || eq(b, a1) || eq(b, a2); }
bool noneEq2(vec3 b, vec3 a0, vec3 a1) { return ne(b, a0) && ne(b, a1); }
bool noneEq4(vec3 b, vec3 a0, vec3 a1, vec3 a2, vec3 a3) { return ne(b, a0) && ne(b, a1) && ne(b, a2) && ne(b, a3); }

void main()
{
	size = ivec2(pc.picture.xy);
	const ivec2 out_ = ivec2(gl_FragCoord.xy);
	const ivec2 p = out_ / 2;
	const ivec2 quadrant = out_ - p * 2;
	const int x = p.x, y = p.y;

	const vec3 A = src(ivec2(x - 1, y - 1)), B = src(ivec2(x, y - 1)), C = src(ivec2(x + 1, y - 1));
	const vec3 D = src(ivec2(x - 1, y)), E = src(ivec2(x, y)), F = src(ivec2(x + 1, y));
	const vec3 G = src(ivec2(x - 1, y + 1)), H = src(ivec2(x, y + 1)), I = src(ivec2(x + 1, y + 1));
	vec3 J = E, K = E, L = E, M = E;

	if (ne(A, E) || ne(B, E) || ne(C, E) || ne(D, E) || ne(F, E) || ne(G, E) || ne(H, E) || ne(I, E))
	{
		const vec3 P = src(ivec2(x, y - 2)), S = src(ivec2(x, y + 2));
		const vec3 Q = src(ivec2(x - 2, y)), R = src(ivec2(x + 2, y));
		const float Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

		// Lines at 1:1: a corner takes the colour of the two neighbours it is between.
		if ((eq(D, B) && ne(D, H) && ne(D, F)) && (El >= Dl || eq(E, A)) && anyEq3(E, A, C, G)
				&& ((El < Dl) || ne(A, D) || ne(E, P) || ne(E, Q)))
			J = D;
		if ((eq(B, F) && ne(B, D) && ne(B, H)) && (El >= Bl || eq(E, C)) && anyEq3(E, A, C, I)
				&& ((El < Bl) || ne(C, B) || ne(E, P) || ne(E, R)))
			K = B;
		if ((eq(H, D) && ne(H, F) && ne(H, B)) && (El >= Hl || eq(E, G)) && anyEq3(E, A, G, I)
				&& ((El < Hl) || ne(G, H) || ne(E, S) || ne(E, Q)))
			L = H;
		if ((eq(F, H) && ne(F, B) && ne(F, D)) && (El >= Fl || eq(E, I)) && anyEq3(E, C, G, I)
				&& ((El < Fl) || ne(I, H) || ne(E, R) || ne(E, S)))
			M = F;

		// Where a line crosses another.
		if ((ne(E, F) && allEq4(E, C, I, D, Q) && allEq2(F, B, H)) && ne(F, src(ivec2(x + 3, y))))
			K = M = F;
		if ((ne(E, D) && allEq4(E, A, G, F, R) && allEq2(D, B, H)) && ne(D, src(ivec2(x - 3, y))))
			J = L = D;
		if ((ne(E, H) && allEq4(E, G, I, B, P) && allEq2(H, D, F)) && ne(H, src(ivec2(x, y + 3))))
			L = M = H;
		if ((ne(E, B) && allEq4(E, A, C, H, S) && allEq2(B, D, F)) && ne(B, src(ivec2(x, y - 3))))
			J = K = B;

		// The tip of a triangle.
		if (Bl < El && allEq4(E, G, H, I, S) && noneEq4(E, A, D, C, F))
			J = K = B;
		if (Hl < El && allEq4(E, A, B, C, P) && noneEq4(E, D, G, I, F))
			L = M = H;
		if (Fl < El && allEq4(E, A, D, G, Q) && noneEq4(E, B, C, I, H))
			K = M = F;
		if (Dl < El && allEq4(E, C, F, I, R) && noneEq4(E, B, A, G, H))
			J = L = D;

		// Lines at 2:1.
		if (ne(H, B))
		{
			if (ne(H, A) && ne(H, E) && ne(H, C))
			{
				if (allEq3(H, G, F, R) && noneEq2(H, D, src(ivec2(x + 2, y - 1))))
					L = M;
				if (allEq3(H, I, D, Q) && noneEq2(H, F, src(ivec2(x - 2, y - 1))))
					M = L;
			}
			if (ne(B, I) && ne(B, G) && ne(B, E))
			{
				if (allEq3(B, A, F, R) && noneEq2(B, D, src(ivec2(x + 2, y + 1))))
					J = K;
				if (allEq3(B, C, D, Q) && noneEq2(B, F, src(ivec2(x - 2, y + 1))))
					K = J;
			}
		}
		if (ne(F, D))
		{
			if (ne(D, I) && ne(D, E) && ne(D, C))
			{
				if (allEq3(D, A, H, S) && noneEq2(D, B, src(ivec2(x + 1, y + 2))))
					J = L;
				if (allEq3(D, G, B, P) && noneEq2(D, H, src(ivec2(x + 1, y - 2))))
					L = J;
			}
			if (ne(F, E) && ne(F, A) && ne(F, G))
			{
				if (allEq3(F, C, H, S) && noneEq2(F, B, src(ivec2(x - 1, y + 2))))
					K = M;
				if (allEq3(F, I, B, P) && noneEq2(F, H, src(ivec2(x - 1, y - 2))))
					M = K;
			}
		}
	}
	const vec3 colour = quadrant.y == 0 ? (quadrant.x == 0 ? J : K) : (quadrant.x == 0 ? L : M);
	FragColor = vec4(colour, 1.0);
}
