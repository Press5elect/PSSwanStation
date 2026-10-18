/* PSSwanStation - the renderer tests' program, run in place of the BIOS.
 *
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Draws sixteen scenes once into a 320x240 picture at the top left of video
 * memory, each in its own 80x60 cell, over a magenta background, then shows
 * the picture for ever. ps5/tests/gpu/run.sh reads video memory back from
 * the software renderer and from the hardware renderer and compares them,
 * and counts the background showing through where scenes cover it (the
 * gaps between polygons drawn larger).
 *
 *    0  textured grid (4-bit), corners moved off the pixel grid
 *    1  shaded grid, corners moved
 *    2  textured fan (15-bit) of thin triangles
 *    3  T-junctions: a large quad beside eight small ones
 *    4..7  overlapping semi-transparent triangles, modes 0..3, one batch
 *    8..11 textured semi-transparent quads and rectangles (palette entries
 *          with and without the semi-transparency bit), modes 0..3
 *   12  mask bit: drawn with "set mask", then drawn over with "check mask"
 *   13  dithering: shaded quads, untextured and textured
 *   14  lines: mono, shaded, semi-transparent poly-lines, every direction
 *   15  rectangles: 1x1, 8x8, 16x16, odd sizes, texture window, page edge
 *
 * Only the CPU's registers, the stack and a few words of scratchpad are
 * used (the ROM's data section is not loaded; see tests/guest/rom.ld).
 */
typedef unsigned int u32;

__attribute__((section(".params"))) const volatile u32 param_seed = 1;
__attribute__((section(".params"))) const volatile u32 param_mode = 0;

#define IO32(a) (*(volatile u32 *)(0xBF800000u + (a)))
#define GP0(v) (IO32(0x1810) = (v))
#define GP1(v) (IO32(0x1814) = (v))

static u32 rng_state;

static u32 rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

static void wait_gpu(void)
{
	u32 i;
	for (i = 0; i < 1000000u && !(IO32(0x1814) & 0x04000000u); i++)
	{
	}
}

static u32 xy(int x, int y)
{
	return ((u32)y << 16) | ((u32)x & 0xFFFFu);
}

static void fill(int x, int y, int w, int h, u32 bgr)
{
	GP0(0x02000000u | bgr);
	GP0(xy(x, y));
	GP0(xy(w, h));
	wait_gpu();
}

/* Texture page attribute for a polygon: page base x/64, y/256, mode, depth. */
static u32 tpage(int px, int py, u32 semi, u32 depth)
{
	return (u32)(px / 64) | ((u32)(py / 256) << 4) | (semi << 5) | (depth << 7);
}

static u32 clut(int x, int y)
{
	return (u32)(x / 16) | ((u32)y << 6);
}

/* A colour no texture or vertex colour here comes near: magenta. */
#define BACKGROUND 0xFF00FFu

static u32 colour(void)
{
	/* Green always well up, so nothing drawn looks like the background. */
	return (rng() & 0xFF00FFu) | 0x00600000u | (((rng() & 0x7Fu) + 0x60u) << 8);
}

/* ------------------------------------------------------------ textures */

#define TEX4_X 512 /* 64x64 4-bit, indices 1..15: no transparency */
#define TEX4B_X 576 /* 64x64 4-bit, indices 0..15 */
#define TEX15_X 640 /* 64x64 15-bit */
#define CLUT_X 512
#define CLUT_Y 256

static u32 rgb15(u32 bgr)
{
	return ((bgr >> 3) & 31u) | (((bgr >> 11) & 31u) << 5) | (((bgr >> 19) & 31u) << 10);
}

static void upload_textures(void)
{
	u32 i, j;


	/* Two palettes: row 0 opaque, row 1 with the semi bit on odd entries
	 * and entry 0 transparent. */
	GP0(0xA0000000u);
	GP0(xy(CLUT_X, CLUT_Y));
	GP0(xy(16, 2));
	rng_state = 77;
	for (j = 0; j < 2; j++)
	{
		for (i = 0; i < 16; i += 2)
		{
			u32 lo = rgb15(colour()), hi = rgb15(colour());
			if (j == 0 && i == 14)
				hi = 0x001Fu; /* entry 15 of the first palette: the atlas's red border */
			if (j == 1)
			{
				hi |= 0x8000u;
				if (i == 0)
					lo = 0;
			}
			GP0(lo | (hi << 16));
		}
	}
	wait_gpu();

	/* 4-bit textures: 64 texels a row are 16 halfwords, 8 words. The
	 * first is an atlas of sixteen 16x16 tiles, each a pattern of two
	 * colours inside a one-texel border of entry 15, red: a polygon that
	 * shows red has sampled outside its tile (a seam). */
	GP0(0xA0000000u);
	GP0(xy(TEX4_X, 0));
	GP0(xy(16, 64));
	for (j = 0; j < 64u; j++)
	{
		for (i = 0; i < 64u; i += 8)
		{
			u32 w = 0, k;
			for (k = 0; k < 8; k++)
			{
				const u32 t = i + k, lt = t & 15u, lr = j & 15u, tile = (t >> 4) + (j >> 4) * 4u;
				const u32 border = lt == 0 || lr == 0 || lt == 15 || lr == 15;
				const u32 index = border ? 15u : 1u + ((tile * 3u + (((lt ^ lr) >> 1) & 1u)) % 14u);
				w |= index << (k * 4);
			}
			GP0(w);
		}
	}
	wait_gpu();
	rng_state = 1234;
	GP0(0xA0000000u);
	GP0(xy(TEX4B_X, 0));
	GP0(xy(16, 64));
	for (i = 0; i < 64u * 8u; i++)
		GP0(rng());
	wait_gpu();

	/* 15-bit: random colours, the semi bit on a quarter, never 0000h. */
	GP0(0xA0000000u);
	GP0(xy(TEX15_X, 0));
	GP0(xy(64, 64));
	for (i = 0; i < 64u * 32u; i++)
	{
		u32 a = rgb15(colour()) | ((rng() & 3u) == 0 ? 0x8000u : 0);
		u32 b = rgb15(colour()) | ((rng() & 3u) == 0 ? 0x8000u : 0);
		GP0(a | (b << 16));
	}
	wait_gpu();
}

/* -------------------------------------------------------------- scenes */

/* A grid of polygons over the cell, inner corners moved by up to 0.9 of a
 * cell's step off the pixel grid (still integer pixels: the console has no
 * other), textured or shaded. */
static void scene_grid(int cx, int cy, int textured)
{
	int gx[6][6], gy[6][6];
	int i, j;
	for (j = 0; j < 6; j++)
	{
		for (i = 0; i < 6; i++)
		{
			int x = cx + 2 + i * 15, y = cy + 2 + j * 11;
			if (i > 0 && i < 5)
				x += (int)(rng() % 9u) - 4;
			if (j > 0 && j < 5)
				y += (int)(rng() % 7u) - 3;
			gx[j][i] = x;
			gy[j][i] = y;
		}
	}
	for (j = 0; j < 5; j++)
	{
		for (i = 0; i < 5; i++)
		{
			if (textured)
			{
				/* Each quad shows the inside of one atlas tile, texels 1..14. */
				const u32 tile = (u32)(i + j * 5) & 15u;
				const u32 u0 = (tile & 3u) * 16u + 1u, v0 = (tile >> 2) * 16u + 1u, u1 = u0 + 14u, v1 = v0 + 14u;
				GP0(0x2C808080u); /* textured quad, modulated by grey 80h: unchanged */
				GP0(xy(gx[j][i], gy[j][i]));
				GP0((clut(CLUT_X, CLUT_Y) << 16) | (v0 << 8) | u0);
				GP0(xy(gx[j][i + 1], gy[j][i + 1]));
				GP0((tpage(TEX4_X, 0, 0, 0) << 16) | (v0 << 8) | u1);
				GP0(xy(gx[j + 1][i], gy[j + 1][i]));
				GP0((v1 << 8) | u0);
				GP0(xy(gx[j + 1][i + 1], gy[j + 1][i + 1]));
				GP0((v1 << 8) | u1);
			}
			else
			{
				GP0(0x38000000u | colour());
				GP0(xy(gx[j][i], gy[j][i]));
				GP0(colour());
				GP0(xy(gx[j][i + 1], gy[j][i + 1]));
				GP0(colour());
				GP0(xy(gx[j + 1][i], gy[j + 1][i]));
				GP0(colour());
				GP0(xy(gx[j + 1][i + 1], gy[j + 1][i + 1]));
			}
		}
		wait_gpu();
	}
}

/* Thin triangles around a centre, sharing their edges, 15-bit textured. */
static void scene_fan(int cx, int cy)
{
	static const signed char ring[24][2] = {
		{ 38, 0 }, { 37, 8 }, { 33, 15 }, { 27, 21 }, { 19, 25 }, { 10, 27 }, { 0, 28 }, { -10, 27 },
		{ -19, 25 }, { -27, 21 }, { -33, 15 }, { -37, 8 }, { -38, 0 }, { -37, -8 }, { -33, -15 },
		{ -27, -21 }, { -19, -25 }, { -10, -27 }, { 0, -28 }, { 10, -27 }, { 19, -25 }, { 27, -21 },
		{ 33, -15 }, { 37, -8 },
	};
	const int mx = cx + 40, my = cy + 30;
	int k;
	/* Background square fully inside the fan's reach is not drawn: the fan
	 * itself must cover its disc. */
	for (k = 0; k < 24; k++)
	{
		const int n = (k + 1) % 24;
		GP0(0x24808080u);
		GP0(xy(mx, my));
		GP0((clut(CLUT_X, CLUT_Y) << 16) | (32u << 8) | 32u);
		GP0(xy(mx + ring[k][0], my + ring[k][1]));
		GP0((tpage(TEX15_X, 0, 0, 2) << 16) | ((u32)(32 + ring[k][1]) << 8) | (u32)(32 + ring[k][0] / 2 + 12));
		GP0(xy(mx + ring[n][0], my + ring[n][1]));
		GP0(((u32)(32 + ring[n][1]) << 8) | (u32)(32 + ring[n][0] / 2 + 12));
	}
	wait_gpu();
}

/* A large quad, and eight small quads along its right edge whose corners
 * lie on that edge where the large one has none (T-junctions). */
static void scene_tjunction(int cx, int cy)
{
	int k;
	const int x0 = cx + 3, x1 = cx + 41, y0 = cy + 3, y1 = cy + 57;
	GP0(0x28000000u | colour());
	GP0(xy(x0, y0));
	GP0(xy(x1, y0 + 2));
	GP0(xy(x0, y1));
	GP0(xy(x1 - 3, y1));
	for (k = 0; k < 8; k++)
	{
		/* Points along the large quad's right edge, x1,y0+2 to x1-3,y1. */
		const int ya = y0 + 2 + k * 7, yb = ya + 7;
		const int xa = x1 - (3 * (ya - y0 - 2)) / 52, xb = x1 - (3 * (yb - y0 - 2)) / 52;
		GP0(0x38000000u | colour());
		GP0(xy(xa, ya));
		GP0(colour());
		GP0(xy(cx + 77, ya + 1));
		GP0(colour());
		GP0(xy(xb, yb));
		GP0(colour());
		GP0(xy(cx + 77, yb + 1));
	}
	wait_gpu();
}

/* Many overlapping semi-transparent triangles, one blend mode. */
static void scene_blend(int cx, int cy, u32 mode)
{
	int k;
	fill(cx, cy, 80, 60, 0x404040u);
	GP0(0xE1000400u | (mode << 5));
	for (k = 0; k < 24; k++)
	{
		const int x = cx + (int)(rng() % 60u), y = cy + (int)(rng() % 40u);
		GP0(0x22000000u | colour()); /* flat semi-transparent triangle */
		GP0(xy(x, y));
		GP0(xy(x + 6 + (int)(rng() % 20u), y + (int)(rng() % 10u)));
		GP0(xy(x + (int)(rng() % 10u), y + 6 + (int)(rng() % 18u)));
	}
	wait_gpu();
	GP0(0xE1000400u);
}

/* Textured semi-transparent quads (4-bit palette with semi bits, and
 * 15-bit), shaded too, and rectangles, one blend mode. */
static void scene_blend_textured(int cx, int cy, u32 mode)
{
	int k;
	fill(cx, cy, 80, 60, 0x406020u);
	for (k = 0; k < 10; k++)
	{
		const int x = cx + (int)(rng() % 50u), y = cy + (int)(rng() % 34u);
		const int fifteen = k & 1;
		const u32 page = fifteen ? tpage(TEX15_X, 0, mode, 2) : tpage(TEX4B_X, 0, mode, 0);
		GP0(0x3E000000u | colour()); /* shaded textured semi-transparent quad */
		GP0(xy(x, y));
		GP0((clut(CLUT_X, CLUT_Y + 1) << 16) | (4u << 8) | 4u);
		GP0(colour());
		GP0(xy(x + 26, y + 2));
		GP0((page << 16) | (6u << 8) | 40u);
		GP0(colour());
		GP0(xy(x + 1, y + 24));
		GP0((40u << 8) | 3u);
		GP0(colour());
		GP0(xy(x + 28, y + 26));
		GP0((42u << 8) | 42u);
	}
	wait_gpu();
	GP0(0xE1000400u | (mode << 5) | (u32)(TEX4B_X / 64)); /* the page rectangles use */
	for (k = 0; k < 6; k++)
	{
		const int x = cx + (int)(rng() % 56u), y = cy + (int)(rng() % 40u);
		GP0(0x66000000u | colour()); /* textured semi-transparent rectangle, modulated */
		GP0(xy(x, y));
		GP0((clut(CLUT_X, CLUT_Y + 1) << 16) | ((u32)(rng() & 31u) << 8) | (u32)(rng() & 31u));
		GP0(xy(9 + (int)(rng() % 14u), 7 + (int)(rng() % 12u)));
	}
	wait_gpu();
	GP0(0xE1000400u);
}

/* Mask bit: quads drawn with "set mask", then others over them with
 * "check mask" (only where the mask is clear), semi-transparent too. */
static void scene_mask(int cx, int cy)
{
	int k;
	fill(cx, cy, 80, 60, 0x202020u);
	GP0(0xE6000001u);
	for (k = 0; k < 4; k++)
	{
		const int x = cx + 4 + k * 18, y = cy + 4 + (k & 1) * 10;
		GP0(0x28000000u | colour());
		GP0(xy(x, y));
		GP0(xy(x + 14, y));
		GP0(xy(x, y + 30));
		GP0(xy(x + 14, y + 30));
	}
	wait_gpu();
	GP0(0xE6000002u);
	GP0(0xE1000400u | (1u << 5));
	for (k = 0; k < 6; k++)
	{
		const int x = cx + (int)(rng() % 50u), y = cy + (int)(rng() % 30u);
		GP0(((k & 1) ? 0x2A000000u : 0x28000000u) | colour());
		GP0(xy(x, y));
		GP0(xy(x + 28, y + 3));
		GP0(xy(x + 2, y + 26));
		GP0(xy(x + 27, y + 28));
	}
	/* Textured with the semi bit from the texels: set and check together. */
	GP0(0xE6000003u);
	GP0(0x2E808080u);
	GP0(xy(cx + 10, cy + 34));
	GP0((clut(CLUT_X, CLUT_Y + 1) << 16) | 0u);
	GP0(xy(cx + 70, cy + 34));
	GP0((tpage(TEX4B_X, 0, 1, 0) << 16) | 60u);
	GP0(xy(cx + 10, cy + 58));
	GP0(24u << 8);
	GP0(xy(cx + 70, cy + 58));
	GP0((24u << 8) | 60u);
	wait_gpu();
	GP0(0xE6000000u);
	GP0(0xE1000400u);
}

/* Shaded quads with dithering on: untextured, and textured modulated. */
static void scene_dither(int cx, int cy)
{
	GP0(0xE1000600u);
	GP0(0x38102030u);
	GP0(xy(cx + 2, cy + 2));
	GP0(0x00F0E0D0u);
	GP0(xy(cx + 78, cy + 2));
	GP0(0x00305070u);
	GP0(xy(cx + 2, cy + 29));
	GP0(0x00A08060u);
	GP0(xy(cx + 78, cy + 29));
	GP0(0x3C406080u);
	GP0(xy(cx + 2, cy + 31));
	GP0((clut(CLUT_X, CLUT_Y) << 16) | 0u);
	GP0(0x00C0A080u);
	GP0(xy(cx + 78, cy + 31));
	GP0((tpage(TEX4_X, 0, 0, 0) << 16 | 0x0200u << 16) | 63u);
	GP0(0x00204060u);
	GP0(xy(cx + 2, cy + 58));
	GP0(27u << 8);
	GP0(0x00FFFFFFu);
	GP0(xy(cx + 78, cy + 58));
	GP0((27u << 8) | 63u);
	wait_gpu();
	GP0(0xE1000400u);
}

/* Lines in every direction from a centre, mono and shaded, then
 * semi-transparent poly-lines over them. */
static void scene_lines(int cx, int cy)
{
	int k;
	const int mx = cx + 40, my = cy + 30;
	fill(cx, cy, 80, 60, 0x101010u);
	for (k = 0; k < 16; k++)
	{
		const int ex = cx + (int)(rng() % 80u), ey = cy + (int)(rng() % 60u);
		if (k & 1)
		{
			GP0(0x50000000u | colour());
			GP0(xy(mx, my));
			GP0(colour());
			GP0(xy(ex, ey));
		}
		else
		{
			GP0(0x40000000u | colour());
			GP0(xy(mx, my));
			GP0(xy(ex, ey));
		}
	}
	GP0(0xE1000400u | (1u << 5));
	GP0(0x5A000000u | colour());
	GP0(xy(cx + 2, cy + 2));
	for (k = 0; k < 12; k++)
	{
		GP0(colour());
		GP0(xy(cx + (int)(rng() % 80u), cy + (int)(rng() % 60u)));
	}
	GP0(0x55555555u);
	wait_gpu();
	GP0(0xE1000400u);
}

/* Rectangles: the fixed sizes, odd sizes, texture window, page edge. */
static void scene_rects(int cx, int cy)
{
	int k;
	fill(cx, cy, 80, 60, 0x303030u);
	GP0(0xE1000400u | (u32)(TEX4_X / 64));
	for (k = 0; k < 6; k++)
	{
		GP0(0x68000000u | colour()); /* 1x1 */
		GP0(xy(cx + 2 + k * 3, cy + 2));
	}
	GP0(0x74808080u); /* 8x8 textured */
	GP0(xy(cx + 2, cy + 6));
	GP0((clut(CLUT_X, CLUT_Y) << 16) | (5u << 8) | 3u);
	GP0(0x7C808080u); /* 16x16 textured */
	GP0(xy(cx + 12, cy + 6));
	GP0((clut(CLUT_X, CLUT_Y) << 16) | (60u << 8) | 250u); /* runs over the page's right edge */
	GP0(0x64808080u); /* odd sized, textured */
	GP0(xy(cx + 30, cy + 4));
	GP0((clut(CLUT_X, CLUT_Y) << 16) | (17u << 8) | 9u);
	GP0(xy(23, 19));
	/* Texture window: 16x16 repeats from (16, 8). */
	GP0(0xE2000000u | (30u) | (30u << 5) | (2u << 10) | (1u << 15));
	GP0(0x64808080u);
	GP0(xy(cx + 2, cy + 26));
	GP0((clut(CLUT_X, CLUT_Y) << 16) | 0u);
	GP0(xy(50, 32));
	GP0(0xE2000000u);
	/* Raw-textured semi-transparent, 15-bit. */
	GP0(0xE1000400u | (u32)(TEX15_X / 64) | (2u << 7) | (0u << 5));
	GP0(0x67000000u);
	GP0(xy(cx + 56, cy + 26));
	GP0((7u << 8) | 13u);
	GP0(xy(21, 31));
	wait_gpu();
	GP0(0xE1000400u);
}

/* ------------------------------------------------------- 3D (mode 1) */

/* The GTE projects a tilted plane of quads; their corners are stored by
 * SWC2 straight into a packet in RAM and sent by DMA, so PGXP knows them to
 * a fraction of a pixel (as in a game). */

#define PACKET ((volatile u32 *)0xA0040000u)

static void gte_on(void)
{
	u32 sr;
	__asm__ volatile("mfc0 %0, $12\n\tnop" : "=r"(sr));
	sr |= 0x40000000u;
	__asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" : : "r"(sr));
}

/* clang's assembler has no COP2 instructions for MIPS I: they are written as
 * words, the value in $t0 (register 8), an address in $t1 (9). */
#define STR_(x) #x
#define STR(x) STR_(x)
#define CTC2(reg, v)                                                                                  \
	do                                                                                                \
	{                                                                                                 \
		register u32 val_ __asm__("t0") = (v);                                                        \
		__asm__ volatile(".word " STR(0x48C80000 | ((reg) << 11)) "\n\tnop\n\tnop" : : "r"(val_)); \
	} while (0)
#define MTC2(reg, v)                                                                                  \
	do                                                                                                \
	{                                                                                                 \
		register u32 val_ __asm__("t0") = (v);                                                        \
		__asm__ volatile(".word " STR(0x48880000 | ((reg) << 11)) "\n\tnop\n\tnop" : : "r"(val_)); \
	} while (0)

/* Projects (x, y, z) and stores the screen position at p. */
static void project(int x, int y, int z, volatile u32 *p)
{
	MTC2(0, ((u32)(y & 0xFFFF) << 16) | (u32)(x & 0xFFFF));
	MTC2(1, (u32)(z & 0xFFFF));
	__asm__ volatile("nop\n\tnop\n\t.word 0x4A180001\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory"); /* RTPS */
	{
		register volatile u32 *addr_ __asm__("t1") = p;
		/* SWC2 SXY2 (data register 14), 0($t1) */
		__asm__ volatile(".word 0xE92E0000\n\tnop" : : "r"(addr_) : "memory");
	}
}

static void dma_packet(u32 words)
{
	u32 i;
	GP1(0x04000002u); /* DMA: CPU to GP0 */
	IO32(0x10F0) |= 0x00000800u; /* DPCR: channel 2 on */
	IO32(0x10A0) = 0x00040000u; /* MADR */
	IO32(0x10A4) = words; /* BCR: one block of this many words */
	IO32(0x10A8) = 0x11000001u; /* CHCR: from RAM, manual, start */
	for (i = 0; i < 1000000u && (IO32(0x10A8) & 0x01000000u); i++)
	{
	}
	wait_gpu();
	GP1(0x04000000u);
}

static void gte_setup(int cx, int cy, int pitch)
{
	/* Rotation about X: cos and sin of the tilt, 4096 being one. */
	static const short cs[3][2] = { { 3547, 2048 }, { 2896, 2896 }, { 2048, 3547 } };
	const int c = cs[pitch][0], sn = cs[pitch][1];
	CTC2(0, 4096u);                                             /* R11 = 1, R12 = 0 */
	CTC2(1, 0u);                                                /* R13, R21 */
	CTC2(2, ((u32)((-sn) & 0xFFFF) << 16) | (u32)(c & 0xFFFF)); /* R22 = c, R23 = -s */
	CTC2(3, (u32)(sn & 0xFFFF) << 16);                          /* R31 = 0, R32 = s */
	CTC2(4, (u32)(c & 0xFFFF));                                 /* R33 = c */
	CTC2(5, 0u);                                     /* TRX */
	CTC2(6, 0u);                                     /* TRY */
	CTC2(7, 700u);                                   /* TRZ */
	CTC2(24, (u32)(cx + 40) << 16);                  /* OFX */
	CTC2(25, (u32)(cy + 30) << 16);                  /* OFY */
	CTC2(26, 300u);                                  /* H */
}

/* A plane of n x n quads, 24 units a side each; mode 0 textured (atlas
 * tiles), 1 shaded, 2 with every second column of quads split in two
 * along their length (T-junctions on the long edges). */
static void scene_plane(int cx, int cy, int pitch, int kind)
{
	const int n = 7, size = 24, half = n * size / 2;
	int i, j;
	u32 w = 0;
	gte_setup(cx, cy, pitch);
	for (j = 0; j < n; j++)
	{
		for (i = 0; i < n; i++)
		{
			const int x0 = i * size - half, z0 = j * size - half, x1 = x0 + size, z1 = z0 + size;
			if (kind == 0)
			{
				const u32 tile = (u32)(i * 5 + j * 3) & 15u;
				const u32 u0 = (tile & 3u) * 16u + 1u, v0 = (tile >> 2) * 16u + 1u, u1 = u0 + 14u, v1 = v0 + 14u;
				PACKET[w++] = 0x2C808080u;
				project(x0, 0, z1, &PACKET[w++]);
				PACKET[w++] = (clut(CLUT_X, CLUT_Y) << 16) | (v0 << 8) | u0;
				project(x1, 0, z1, &PACKET[w++]);
				PACKET[w++] = (tpage(TEX4_X, 0, 0, 0) << 16) | (v0 << 8) | u1;
				project(x0, 0, z0, &PACKET[w++]);
				PACKET[w++] = (v1 << 8) | u0;
				project(x1, 0, z0, &PACKET[w++]);
				PACKET[w++] = (v1 << 8) | u1;
			}
			else if (kind == 1 || (i & 1) == 0)
			{
				PACKET[w++] = 0x38000000u | colour();
				project(x0, 0, z1, &PACKET[w++]);
				PACKET[w++] = colour();
				project(x1, 0, z1, &PACKET[w++]);
				PACKET[w++] = colour();
				project(x0, 0, z0, &PACKET[w++]);
				PACKET[w++] = colour();
				project(x1, 0, z0, &PACKET[w++]);
			}
			else
			{
				/* Two halves across the quad's depth: a corner in the middle of
				 * each long edge, which the neighbours do not have. */
				const int zm = (z0 + z1) / 2 + 3;
				int h;
				for (h = 0; h < 2; h++)
				{
					const int za = h ? zm : z0, zb = h ? z1 : zm;
					PACKET[w++] = 0x28000000u | colour();
					project(x0, 0, zb, &PACKET[w++]);
					project(x1, 0, zb, &PACKET[w++]);
					project(x0, 0, za, &PACKET[w++]);
					project(x1, 0, za, &PACKET[w++]);
				}
			}
		}
		dma_packet(w);
		w = 0;
	}
}

void test_main(void)
{
	int k;
	rng_state = param_seed;

	GP1(0x00000000u); /* reset */
	GP1(0x08000001u); /* 320x240, NTSC, 15-bit */
	GP1(0x05000000u); /* display from 0,0 */
	GP1(0x06C60260u); /* horizontal range */
	GP1(0x07040010u); /* vertical range */
	GP1(0x03000000u); /* display on */
	GP0(0xE1000400u); /* drawing to the display allowed */
	GP0(0xE3000000u);
	GP0(0xE4000000u | (511u << 10) | 1023u);
	GP0(0xE5000000u);
	GP0(0xE6000000u);

	upload_textures();
	fill(0, 0, 320, 240, BACKGROUND);

	GP0(0xE4000000u | (239u << 10) | 319u);
	for (k = 0; k < 16; k++)
	{
		const int cx = (k & 3) * 80, cy = (k >> 2) * 60;
		rng_state = param_seed * 1000u + (u32)k;
		/* Each cell is clipped to itself, so one scene cannot spill into the next. */
		GP0(0xE3000000u | ((u32)cy << 10) | (u32)cx);
		GP0(0xE4000000u | ((u32)(cy + 59) << 10) | (u32)(cx + 79));
		if (param_mode == 1)
		{
			/* 3D: planes at three tilts, textured, shaded, T-junctions; the
			 * other cells as in mode 0's second row on. */
			if (k < 9)
			{
				if (k == 0)
					gte_on();
				scene_plane(cx, cy, k / 3, k % 3);
				continue;
			}
		}
		switch (k)
		{
		case 0: scene_grid(cx, cy, 1); break;
		case 1: scene_grid(cx, cy, 0); break;
		case 2: scene_fan(cx, cy); break;
		case 3: scene_tjunction(cx, cy); break;
		case 4: case 5: case 6: case 7: scene_blend(cx, cy, (u32)(k - 4)); break;
		case 8: case 9: case 10: case 11: scene_blend_textured(cx, cy, (u32)(k - 8)); break;
		case 12: scene_mask(cx, cy); break;
		case 13: scene_dither(cx, cy); break;
		case 14: scene_lines(cx, cy); break;
		case 15: scene_rects(cx, cy); break;
		}
	}
	GP0(0xE3000000u);
	GP0(0xE4000000u | (511u << 10) | 1023u);

	for (;;)
	{
	}
}
