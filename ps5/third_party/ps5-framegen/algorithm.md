# The algorithm

A generated frame is the picture at the moment halfway between two frames of the game: the previous one (A) and the current one (B).
For a pixel `x` of it and a motion `v` (half of the way the content moves from A to B), the content at `x` is where A shows it at
`x + v` and where B shows it at `x - v`. If `v` is right, those two samples agree, and their blend is the pixel. Everything below is
about finding a `v` that is right, and about what to show where there is none.

All of it runs in three groups of compute passes a frame, recorded by `interpolator::record()` (`src/ps5_framegen.cpp`).

## 1. Optical flow (FSR3's, unchanged)

The passes `of_*`: the luma of each frame, a 7-level pyramid of it, a scene change detection from luma histograms, and a block search
from the coarsest level down with a filter and an upscale at each level (FSR3's "v5" optical flow). The result is one motion per 8x8
block of B, in whole pixels, pointing from B to A.

Two changes, both in the callback headers:

- **Scene changes:** FSR3 counted the first 5 frames after a reset, and the 4 frames after a detected scene change, as scene
  changes, with no flow and no interpolation. Now only the frame of the change itself (and the first frame after a reset) is.
  The motion comes back a frame after a cut instead of four, so there is no judder after every cut.
- **Portability:** the search's reductions run in shared memory (`FFX_FG_PORTABLE_WAVE`), not in wave operations that assume
  32 or 64 lanes. Software devices have 4.

## 2. The vector field (FSR3's)

`fi_setup` and `fi_vector_field` push each block's motion, halved, to where the block is halfway. Where two blocks land on the same
place, the faster one wins. FSR3 keeps this field to sample a motion at any pixel. It is one candidate here among several.

## 3. The interpolation (`computeFrameinterpolationQuad`, one 2x2 quad a thread)

### Candidates

| Index | Candidate | Bias | Why |
|---|---|---|---|
| (zero) | no motion | 0.008 | the HUD, text, still scenery, a character the camera follows |
| 0 | the vector field, blended | 0 | FSR3's own answer |
| 1-4 | the field's 4 nearest cells, one by one | 0.004 | a blend of two motions at an edge is neither |
| 5 | the optical flow of the quad's block | 0.004 | the field may have put another block's motion here |
| 6-9 | the optical flow of the 4 blocks around it | 0.006 | at the edge of something moving, its own motion and the background's are both nearby |
| 10 | the quad's motion in the last generated frame | 0.004 | motion goes on; the choice stays the same from frame to frame |
| 11 | the quad's best one, refined | as its source | sub-pixel precision, see below |

A motion found twice, within a quarter of a pixel, is tried once. A motion under half a pixel counts as no motion.

### Score

For each candidate and pixel, the error is the sum of the three channels' differences between the two samples, from 0 to 3. The
score adds half of the quad's mean error, so that the quad decides together where its pixels cannot tell, and the candidate's bias.
Ties go to motion: a moving pattern can match "no motion" for a frame or two.

Penalties added to the bias:

- **Consistency (`FgUnlike`, 0.12):** what the candidate takes from B at `x - v`, and from A at `x + v`, should have moved by
  `2v` according to the optical flow there. The most alike of the 2x2 nearest blocks counts, which is lenient where two motions
  meet. Tolerance: 3 px plus 15% of the motion.
  - The reason: two dark samples agree just as well as the right ones. In GTA IV a pixel of Niko's ear took the background's
    motion from the block beside it, because A's dark wall and B's dark hair agreed better than the ear under the head's motion
    (the ear's shading changes as the head moves). The ear came out with a black speck.
- **Off screen (`FG_BORDER`, 0.06):** when one of the two samples falls off the screen, there is nothing to compare, so the
  sample on the screen is used alone at a middling score.
  - This is the strip that comes in or goes out at the edge of the screen as the camera turns. Before, that strip filled with
    whatever else happened to agree.

### Sub-pixel refinement

The flow is in whole pixels, so half vectors come in steps of half a pixel. Along a sharp edge moving a fraction of a pixel, the two
samples miss each other by up to a quarter of a pixel each, and the graffiti on GTA IV's walls shimmered along every line.

The quad's best candidate is tried half a pixel to each side on each axis. A parabola through the three errors gives the best
fraction, and that motion becomes one more candidate. Quads that already agree within 0.03 are left as they are.

### Choosing, per pixel

- **The best candidate:** each pixel takes its own best (the lowest score). If it agrees (error under `FG_MATCH`, 0.12), the pixel
  is the blend of its two samples.
- **Own match (m73f):** the quad's choice may not agree at one of its pixels while another motion does, at that pixel alone. That
  pixel belongs to what that motion moves, for example an ear in a quad that is mostly background, so it takes that motion. It is
  not an edge.
- **Edges:** where nothing agrees, something in front covers or uncovers what moves behind it. Of the best moving candidate's two
  samples, the one that another motion explains belongs to what is in front, so the other one is shown. "Another motion" means no
  motion, or the best moving candidate that differs by more than 2 px. When neither or both are explained, the pixel is the blend
  (FSR3's own fallback). A soft ramp between `FG_MATCH` and `FG_MATCH + FG_MATCH_SOFT` keeps this from flickering.
- **Still history:** a pixel that has not changed for 8 frames in a row (`rw_static_history`) shows no motion, whatever the
  candidates say.
  - The reason: inside a thin letter of a subtitle over a moving background, a moving candidate's two samples both land on the
    background and agree just as well. A single pair of frames cannot tell that from a pattern moving by its own period (a fence,
    bricks). The history can, because that coincidence does not last.
  - The history is cleared when the interpolator starts. Before m74 it started with whatever the memory held, and random pixels
    of moving things were held still.

### Other moments than halfway

With several frames generated (`set_generated`), the frame at moment `t` (0 is the previous frame, 1 the current one) takes A at
`x + 2t v` and B at `x - 2(1 - t) v`. At `t = 0.5` that is exactly `x + v` and `x - v`. The blend weighs the nearer frame more. The
vector field stays FSR3's halfway one for every moment: its candidates are a little off away from the middle, and the other
candidates are not tied to a moment. The still history and the quads' motions are updated by the first frame of the group only.

### Debug view

With `dispatchFlags` bit 2 (`set_debug_view(true)`), the output shows the decisions instead of the picture:

| Colour | Decision |
|---|---|
| grey | no motion |
| white | still by the history |
| green | a motion agreed |
| red / blue | an edge where A's / B's sample was kept |
| magenta | an edge, blended |

Built with `-DFG_DEBUG_VECTORS`, it shows the chosen motion in red and green and the candidate index in blue, for scripts.

## What was tried and dropped

| Tried | Result |
|---|---|
| A median of the edge pixels (m73f's cleanup pass) | The specks it was for were not edge pixels; it changed little else and cost a pass |
| A local search for quads where nothing agrees (for hands swinging as a character walks) | Spurious matches elsewhere made the whole frame worse, and the hands, which also rotate, were not recovered |
| Skipping the candidates for quads that did not change | A pattern moving by its own period looks unchanged too: the moving checkerboard of the "all" scene came out three times worse |
| Stopping after the vector field when it agrees | Cost precision, saved little |

They are still in the source behind test defines (`FG_QUIET_SKIP`, `FG_FIELD_EARLY`), or described in the history of RPCS3-PS5.

## Known limits

- **Small fast things that turn as they move** (hands, a thrown object): the flow's 8x8 blocks follow what is around them, and no
  single motion fits a rotating hand. They come out fragmented or faded in the generated frame. The real frames before and after
  are untouched, so at 60 fps or more it is hard to see.
- **What a camera turn uncovers at the screen's edge:** the flow has nothing there (the content was off screen in A). The on-screen
  sample is used when the right motion is among the candidates.
- **Without depth there is no order between two motions:** where two layers move differently, the edge test guesses which one is in
  front from which sample another motion explains.
