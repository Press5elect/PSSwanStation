#version 450
// PSSwanStation - HDR output: OGBox's hdr.frag (Press5elect/OGBox shaders/glsl/ogbox/hdr.frag, OGBox
// 0.0.9), unchanged but for this header and the input it never read (v_uv: PSSwanStation's
// full-screen triangle, fsr_vertex.vert, has no outputs).
// Copyright (C) 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect"; see NOTICE.
//
// The frame drawn as for an SDR screen (the picture, and the menus over it in a layer of their
// own) made into HDR10's pixels for the framebuffers VideoOut reads as PQ with BT.2020 primaries
// (ps5/patches/PS5_Mesa: wsi/videoout HDR10 framebuffers on request), as VLC-PS5 does for its own
// SDR menus (shaders/ui_pq.frag: the PQ curve of SMPTE ST 2084, BT.709's colours in BT.2020's,
// the graphics white of ITU-R BT.2408 at 203 nits).
//  - the SDR values decoded with a 2.2 gamma (as an SDR TV shows them), then BT.709 to BT.2020;
//  - white at metrics.x nits (the setting; BT.2408's 203 by default), the menus exactly so;
//  - the game's picture (metrics.w 1) with its highlights raised: linear luminance above 0.5 rises
//    on t^2 to metrics.y nits at full white, with the slope and value of the straight line at the
//    knee (no step), and every colour scaled by its luminance's gain, so hues keep and no channel
//    passes the peak; metrics.y at or below the white: none.
// metrics.z 1: the layers are sampled through sRGB views (already linear).
// cal (Settings, Picture, Adjust HDR brightness): .x 1 the highlights' test: a square in the middle
// at PQ's top (10000 nits, which a TV shows at its brightest) with an X in it at .y nits; where .y
// reaches the TV's peak the X can no longer be seen. .x 2 the white level's: a grey scale of SDR's
// eleven tenths and a white patch below it, at the white level .y. Over the menus.
layout (set = 0, binding = 0) uniform sampler2D u_picture;
layout (set = 0, binding = 1) uniform sampler2D u_ui;     // premultiplied (ImGui's blending over clear 0)
layout (push_constant) uniform PC { vec4 metrics; vec4 cal; } pc;
layout (location = 0) out vec4 o;

const float M1 = 0.1593017578125, M2 = 78.84375;
const float C1 = 0.8359375, C2 = 18.8515625, C3 = 18.6875;
const float KNEE = 0.5;

/* BT.709 to BT.2020 (ITU-R BT.2087), columns as GLSL takes them */
const mat3 BT709_TO_2020 = mat3(
	0.6274, 0.0691, 0.0164,
	0.3293, 0.9195, 0.0880,
	0.0433, 0.0114, 0.8956);

vec3 pq(vec3 nits)
{
	vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(M1));
	return pow((C1 + C2 * y) / (1.0 + C3 * y), vec3(M2));
}

vec3 decode(vec3 c)
{
	return pc.metrics.z > 0.5 ? c : pow(max(c, vec3(0.0)), vec3(2.2));
}

/* the test screen's pattern at p (screen pixels) for a screen of size s: true, and its nits, inside it */
bool pattern(vec2 p, vec2 s, out float nits)
{
	vec2 c = s * 0.5;
	if (pc.cal.x < 1.5) {
		float side = 0.40 * s.y;
		vec2 d = (p - c) / side;                     // -0.5 .. 0.5 across the square
		if (max(abs(d.x), abs(d.y)) >= 0.5) {
			return false;
		}
		bool x = max(abs(d.x), abs(d.y)) < 0.3 && min(abs(d.x - d.y), abs(d.x + d.y)) < 0.07;
		nits = x ? pc.cal.y : 10000.0;
		return true;
	}
	vec2 size = vec2(0.64 * s.x, 0.40 * s.y);
	vec2 d = (p - (c - size * 0.5)) / size;          // 0 .. 1 across the chart
	if (any(lessThan(d, vec2(0.0))) || any(greaterThanEqual(d, vec2(1.0)))) {
		return false;
	}
	float level = 1.0;
	if (d.y < 0.42) {
		float k = d.x * 11.0;
		level = fract(k) < 0.06 || d.y > 0.38 ? 0.0 : floor(k) / 10.0;   // eleven patches, black between
	}
	nits = pow(level, 2.2) * pc.cal.y;
	return true;
}

void main()
{
	ivec2 at = ivec2(gl_FragCoord.xy);
	float test;
	if (pc.cal.x > 0.5 && pattern(gl_FragCoord.xy, vec2(textureSize(u_ui, 0)), test)) {
		o = vec4(pq(vec3(test)), 1.0);   // grey: the same in BT.2020
		return;
	}
	vec3 picture = texelFetch(u_picture, at, 0).rgb;
	vec4 ui = texelFetch(u_ui, at, 0);
	float white = max(pc.metrics.x, 1.0);
	float gain = 1.0;
	if (pc.metrics.w > 0.5 && pc.metrics.y > white) {
		float l = dot(decode(picture), vec3(0.2126, 0.7152, 0.0722));
		if (l > KNEE) {
			float t = (min(l, 1.0) - KNEE) / (1.0 - KNEE);
			gain = (l + (pc.metrics.y / white - 1.0) * t * t) / l;
		}
	}
	/* the menus over the picture as an SDR screen blends them (in its encoded values), the gain
	 * kept off them in proportion to their cover */
	vec3 sdr = ui.rgb + picture * (1.0 - ui.a);
	vec3 light = decode(sdr) * mix(gain, 1.0, ui.a);
	o = vec4(pq(max(BT709_TO_2020 * light, vec3(0.0)) * white), 1.0);
}
