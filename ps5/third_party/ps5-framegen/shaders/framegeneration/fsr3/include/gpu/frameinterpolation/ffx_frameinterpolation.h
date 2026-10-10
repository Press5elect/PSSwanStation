// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef FFX_FRAMEINTERPOLATION_H
#define FFX_FRAMEINTERPOLATION_H

struct InterpolationSourceColor
{
    FfxFloat32x3 fRaw;
    FfxFloat32x3 fLinear;
    FfxFloat32   fBilinearWeightSum;
};

InterpolationSourceColor NewInterpolationSourceColor()
{
    InterpolationSourceColor c;
    c.fRaw = FfxFloat32x3(0.0, 0.0, 0.0);
    c.fLinear = FfxFloat32x3(0.0, 0.0, 0.0);
    c.fBilinearWeightSum = 0.0;
    return c;
}

InterpolationSourceColor SampleTextureBilinear(FfxBoolean isCurrent, FfxFloat32x2 fUv, FfxFloat32x2 fMotionVector, FfxInt32x2 texSize)
{
    InterpolationSourceColor result = NewInterpolationSourceColor();

    FfxFloat32x2 fReprojectedUv = fUv + fMotionVector;
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, texSize);

    FfxFloat32x3 fColor = FfxFloat32x3(0.0, 0.0, 0.0);
    FfxFloat32 fWeightSum = 0.0f;
    for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {

        const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        const FfxInt32x2 iSamplePos = bilinearInfo.iBasePos + iOffset;

        if (IsInRect(iSamplePos, InterpolationRectBase(), InterpolationRectSize()))
        {
            FfxFloat32 fWeight = bilinearInfo.fWeights[iSampleIndex];

            if (isCurrent)
                fColor += LoadCurrentBackbuffer(iSamplePos).rgb * fWeight;
            else
                fColor += LoadPreviousBackbuffer(iSamplePos).rgb * fWeight;
            fWeightSum += fWeight;
        }
    }

    //normalize colors
    fColor = (fWeightSum != 0.0f) ? fColor / fWeightSum : FfxFloat32x3(0.0f, 0.0f, 0.0f);

    result.fRaw               = fColor;
    result.fLinear            = RawRGBToLinear(fColor);
    result.fBilinearWeightSum = fWeightSum;

    return result;
}

void updateInPaintingWeight(inout FfxFloat32 fInPaintingWeight, FfxFloat32 fFactor)
{
    fInPaintingWeight = ffxSaturate(ffxMax(fInPaintingWeight, fFactor));
}

#if defined(FFX_FG_OF_ONLY)
// RPCS3-PS5 (m73b): the interpolation of the optical flow path, a 2x2 quad of pixels a thread. A PS3 game gives no motion vectors or
// depth, so FSR3's game vector path (vectors ordered by depth, the disocclusion mask, the inpainting) cannot run, and the optical
// flow's 8x8 blocks alone drag what stays still on screen (the HUD, text, the character the camera follows) along with what moves
// behind it. Instead each pixel tries several motions and keeps the one under which the two frames agree:
//   candidates   no motion; the vector field blended (FSR3's); the field's 4 nearest cells one by one; the optical flow of the
//                quad's block and (m73f) of the 4 blocks around it, so that near the edge of something moving the background's motion
//                and its own are both there (the field keeps the faster one where they meet); (m74) the motion the quad took in the
//                last generated frame; (m74) the quad's best one refined to a fraction of a pixel; the same motion found twice is
//                tried once
//   score        how far apart the previous frame half a vector back and the current frame half a vector ahead are (the sum of
//                the 3 channels' differences) at the pixel, plus half of the quad's mean, so a quad decides together where its
//                pixels can't tell; plus (m73g, FgUnlike) a penalty for a motion that is not the optical flow's own where it takes
//                its samples (two dark samples agree by chance); (m74) a sample off the screen leaves the other one alone, at a
//                middling score
//   agreement    the best candidate's two samples, blended
//   edges        where nothing agrees, something in front covers or uncovers what moves behind it: of the best moving
//                candidate's two samples, the one that another motion (none, or the next best moving one) explains belongs to
//                what is in front, so the other one is shown; when neither or both are explained, the best candidate's blend
//                (FSR3's own fallback)
//   still        a pixel that has not changed for FG_STILL_FRAMES frames in a row is HUD or still scenery: no motion, whatever
//                the candidates say. Inside a thin letter of a subtitle over a moving background, a moving candidate's two samples
//                both land on the background and agree just as well; a single pair of frames cannot tell that from a moving
//                pattern whose period matches the motion (a fence, bricks), which the history can (the coincidence does not last).

FFX_STATIC const FfxFloat32 FG_MATCH      = 0.12f; // two samples agree under this (the sum of the 3 channels' differences, 0..3)
FFX_STATIC const FfxFloat32 FG_MATCH_SOFT = 0.18f; // and disagree fully from FG_MATCH + this
FFX_STATIC const FfxFloat32 FG_OWN        = 0.20f; // a sample is explained by another motion under this
FFX_STATIC const FfxFloat32 FG_NONE       = 1e9f;  // no candidate
#define FG_CANDIDATES 12                             // the moving candidates (see computeFrameinterpolationQuad; m74: 10 the last frame's, 11 the refined one)
FFX_STATIC const FfxFloat32 FG_STILL      = 0.02f; // a pixel under this between the frames has not changed
FFX_STATIC const FfxUInt32  FG_STILL_FRAMES = 8;   // frames in a row unchanged that make a pixel still (HUD text that just appeared waits that long)
#if !defined(FG_UNLIKE_SCORE)
#define FG_UNLIKE_SCORE 0.12f
#endif
FFX_STATIC const FfxFloat32 FG_UNLIKE     = FG_UNLIKE_SCORE; // m73g: the score added to a motion that is not the motion of what it takes (FgUnlike)
#if !defined(FG_REFINE_STEP)
#define FG_REFINE_STEP 0.5f
#endif
#if !defined(FG_REFINE_MIN)
#define FG_REFINE_MIN 0.03f                          // m74: a quad that agrees within this under its best candidate is not refined
#endif
#if !defined(FG_BORDER)
#define FG_BORDER 0.06f                              // m74: the score of a motion one of whose samples is off the screen
#endif
#if !defined(FG_TEMPORAL_BIAS)
#define FG_TEMPORAL_BIAS 0.004f                      // m74: the bias of the last frame's motion (as the flow's own blocks)
#endif
#if !defined(FG_QUIET)
#define FG_QUIET 0.006f                              // tests (FG_QUIET_SKIP): a quad whose pixels all changed less than this is left still
#endif
#if !defined(FG_EARLY)
#define FG_EARLY 0.03f                               // tests (FG_FIELD_EARLY): the field's motion agrees this well at a quad: nothing else
#endif

FfxFloat32 FgDiff(FfxFloat32x3 a, FfxFloat32x3 b)
{
    const FfxFloat32x3 d = abs(a - b);
    return d.x + d.y + d.z;
}

// m74: more than one frame generated between two of the game's (ps5_framegen.h, set_generated): the moment t of this one, from 0 (the
// previous frame) to 1 (the current one). The motions are half vectors v (the way from the current frame to the previous one,
// halved), so what is at uv at moment t is in the previous frame at uv + 2t v and in the current one at uv - 2(1 - t) v (at t = 0.5,
// uv + v and uv - v exactly), and the blend weighs the nearer frame more. Only the first frame generated keeps the still history and
// the quads' motions (FG_DISPATCH_KEEP_HISTORY on the others). The vector field is FSR3's halfway one for every moment.
#define FG_DISPATCH_KEEP_HISTORY (1u << 8)

FfxFloat32 FgT()
{
    FfxFloat32 t = InterpolationT(); // (not const: glslang refuses a const local from a const-returning function)
    return (t > 0.0f && t < 1.0f) ? t : 0.5f;
}

FfxFloat32 FgTP()
{
    return 2.0f * FgT();
}

FfxFloat32 FgTC()
{
    return 2.0f * (1.0f - FgT());
}

FfxBoolean FgKeepHistory()
{
    return (GetDispatchFlags() & FG_DISPATCH_KEEP_HISTORY) != 0;
}

FfxFloat32x3 FgBlend(FfxFloat32x3 p, FfxFloat32x3 c)
{
    const FfxFloat32 t = FgT();
    return (t == 0.5f) ? 0.5f * (p + c) : p * (1.0f - t) + c * t;
}

struct FgPixel
{
    FfxFloat32x2 fUv;
    FfxFloat32x3 fPrev0, fCurr0; // no motion
    FfxFloat32   fErr0;
    FfxFloat32   fScoreA;        // the best moving candidate
    FfxFloat32   fErrA;
    FfxFloat32x2 fMvA;
    FfxFloat32x3 fPrevA, fCurrA;
    FfxFloat32   fScore[FG_CANDIDATES]; // each moving candidate's score (FG_NONE: not tried)
    FfxFloat32   fErrM;          // m73f: the moving candidate that agrees best at this pixel alone (no quad mean), and its samples
    FfxFloat32x3 fPrevM, fCurrM;
    FfxInt32     iA;             // tests (FG_DEBUG_VECTORS): the best moving candidate's index
};

void FgTryCandidate(inout FgPixel px[4], FfxInt32 k, FfxFloat32x2 fMv, FfxFloat32 fBias)
{
    FfxFloat32   e[4];
    FfxFloat32x3 p[4];
    FfxFloat32x3 c[4];
    FfxFloat32   fMean = 0.0f;

    FFX_UNROLL
    for (FfxInt32 i = 0; i < 4; i++)
    {
        const FfxFloat32x2 fUvP = px[i].fUv + fMv * FgTP();
        const FfxFloat32x2 fUvC = px[i].fUv - fMv * FgTC();
        p[i] = SamplePreviousBackbuffer(fUvP);
        c[i] = SampleCurrentBackbuffer(fUvC);
        const FfxBoolean bInP = IsUvInside(fUvP);
        const FfxBoolean bInC = IsUvInside(fUvC);
        e[i] = (bInP && bInC) ? FgDiff(p[i], c[i]) : 3.0f;
#if !defined(FG_NO_BORDER)
        // m74: at the edge of the screen, where the picture comes in or goes out as the camera turns, one of the two samples is off the
        // screen: nothing to compare with, so the sample on the screen alone, at a middling score (a motion that agrees elsewhere wins
        // over it, a stray one that does not fit the flow loses to it: FgUnlike). m73 scored such a motion as failing, and the strip
        // along the edge filled with whatever else happened to agree (patches of the wrong texture, GTA IV's left edge when turning).
        if (bInP != bInC)
        {
            e[i] = FG_BORDER;
            if (bInP) c[i] = p[i]; else p[i] = c[i];
        }
#endif
        fMean += e[i];
    }
    fMean *= 0.25f;

    FFX_UNROLL
    for (FfxInt32 j = 0; j < 4; j++)
    {
        const FfxFloat32 fScore = e[j] + 0.5f * fMean + fBias;
        px[j].fScore[k] = fScore;
        if (fScore < px[j].fScoreA)
        {
            px[j].fScoreA = fScore;
            px[j].fErrA   = e[j];
            px[j].fMvA    = fMv;
            px[j].fPrevA  = p[j];
            px[j].fCurrA  = c[j];
            px[j].iA      = k;
        }
        if (e[j] + fBias < px[j].fErrM)
        {
            px[j].fErrM  = e[j] + fBias;
            px[j].fPrevM = p[j];
            px[j].fCurrM = c[j];
        }
    }
}

// m73g: how unlike the motion of what it takes a moving candidate is, 0 (alike) to 1. The candidate takes the current frame at uv - v
// and the previous one at uv + v, so what is at each of those places should have moved by 2v (the optical flow is "previous - current"
// for the current frame's blocks); of the 2x2 blocks nearest each place the most alike counts (lenient where two motions meet), and
// the worse of the two places. Two dark samples agree just as well as the right ones: in GTA IV a pixel of Niko's ear took the
// background's motion from the block beside it, the previous frame's dark wall and the current frame's dark hair agreed better than
// the ear under the head's motion (its shading changes as it moves), and it came out a black speck (frame dumps of 2026-10-06). The
// previous frame's side is judged by the current frame's blocks at the same place: right where the motion is smooth, lenient near an
// edge.
FfxFloat32 FgUnlike(FfxFloat32x2 fQuadUv, FfxFloat32x2 fMv, FfxInt32x2 iFieldSize, FfxFloat32x2 fSize)
{
    const FfxFloat32x2 fWant  = 2.0f * fMv;
    const FfxFloat32x2 fBlock = fSize / FfxFloat32(GetOpticalFlowBlockSize());
    FfxFloat32 fWorst = 0.0f;

    FFX_UNROLL
    for (FfxInt32 side = 0; side < 2; side++)
    {
        const FfxFloat32x2 fAt   = (side == 0) ? fQuadUv - fMv * FgTC() : fQuadUv + fMv * FgTP();
        const FfxInt32x2   iNear = FfxInt32x2(floor(fAt * fBlock - 0.5f));
        FfxFloat32 fBest = 1e9f;

        FFX_UNROLL
        for (FfxInt32 n = 0; n < 4; n++)
        {
            const FfxInt32x2 iAt = clamp(iNear + FfxInt32x2(n & 1, n >> 1), FfxInt32x2(0, 0), iFieldSize - 1);
            fBest = ffxMin(fBest, length((LoadOpticalFlow(iAt) - fWant) * fSize));
        }
        fWorst = ffxMax(fWorst, fBest);
    }

    const FfxFloat32 fTolerance = 3.0f + 0.15f * length(fWant * fSize);
    return ffxSaturate((fWorst - fTolerance) / fTolerance);
}

// m74: how far apart the previous frame at uv + v and the current one at uv - v are, over the quad (the mean of its 4 pixels).
FfxFloat32 FgQuadError(FgPixel px[4], FfxFloat32x2 fMv)
{
    FfxFloat32 fSum = 0.0f;

    FFX_UNROLL
    for (FfxInt32 i = 0; i < 4; i++)
    {
        const FfxFloat32x2 fUvP = px[i].fUv + fMv * FgTP();
        const FfxFloat32x2 fUvC = px[i].fUv - fMv * FgTC();
        const FfxBoolean bInP = IsUvInside(fUvP);
        const FfxBoolean bInC = IsUvInside(fUvC);
#if !defined(FG_NO_BORDER)
        // a sample off the screen as FgTryCandidate scores it
        fSum += (bInP && bInC) ? FgDiff(SamplePreviousBackbuffer(fUvP), SampleCurrentBackbuffer(fUvC)) : (bInP != bInC) ? FG_BORDER : 3.0f;
#else
        fSum += (bInP && bInC) ? FgDiff(SamplePreviousBackbuffer(fUvP), SampleCurrentBackbuffer(fUvC)) : 3.0f;
#endif
    }

    return 0.25f * fSum;
}

void computeFrameinterpolationQuad(FfxInt32x2 iQuad)
{
    const FfxInt32x2 iBase = iQuad * 2;
    FfxInt32x2 iSize = DisplaySize();

    if (FrameIndexSinceLastReset() == 0)
    {
        // just reset: the current frame, not interpolated, and no pixel still yet
        FFX_UNROLL
        for (FfxInt32 i = 0; i < 4; i++)
        {
            const FfxInt32x2 iPos = iBase + FfxInt32x2(i & 1, i >> 1);
            if (IsOnScreen(iPos, iSize))
            {
                StoreFrameinterpolationOutput(iPos, FfxFloat32x4(LoadCurrentBackbuffer(iPos), 0.0f));
                StoreStaticHistory(iPos, 0);
            }
        }
#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_QUAD_MOTION)
        StoreQuadMotion(iQuad, FfxFloat32x2(0.0f, 0.0f)); // m74: the motions of the scene before a cut are no candidates
#endif
        return;
    }

    const FfxFloat32x2 fPx = 1.0f / FfxFloat32x2(iSize);
    FgPixel px[4];

    FFX_UNROLL
    for (FfxInt32 i = 0; i < 4; i++)
    {
        const FfxInt32x2 iPos = ffxMin(iBase + FfxInt32x2(i & 1, i >> 1), iSize - 1);
        px[i].fUv     = (FfxFloat32x2(iPos) + 0.5f) * fPx;
        px[i].fPrev0  = LoadPreviousBackbuffer(iPos);
        px[i].fCurr0  = LoadCurrentBackbuffer(iPos);
        px[i].fErr0   = FgDiff(px[i].fPrev0, px[i].fCurr0);
        px[i].fScoreA = FG_NONE;
        px[i].fErrA   = 3.0f;
        px[i].fMvA    = FfxFloat32x2(0.0f, 0.0f);
        px[i].fPrevA  = px[i].fPrev0;
        px[i].fCurrA  = px[i].fCurr0;
        px[i].fErrM   = 3.0f;
        px[i].fPrevM  = px[i].fPrev0;
        px[i].fCurrM  = px[i].fCurr0;
        px[i].iA      = -1;
        FFX_UNROLL
        for (FfxInt32 k = 0; k < FG_CANDIDATES; k++)
            px[i].fScore[k] = FG_NONE;
    }

    FfxFloat32 fMean0 = 0.25f * (px[0].fErr0 + px[1].fErr0 + px[2].fErr0 + px[3].fErr0);

    // The moving candidates (half vectors in uv: the previous frame at uv + v, the current at uv - v).
    const FfxFloat32x2 fQuadUv    = FfxFloat32x2(iBase + 1) * fPx;
    FfxInt32x2         iFieldSize = GetOpticalFlowSize2();
    const FfxFloat32x2 fSize      = FfxFloat32x2(iSize);

    FfxFloat32x2 fCand[FG_CANDIDATES];
    FfxBoolean   bCand[FG_CANDIDATES];
    FfxFloat32   fBias[FG_CANDIDATES];
    {
        VectorFieldEntry blended;
        SampleOpticalFlowMotionVectorField(fQuadUv, blended);
        fCand[0] = blended.fMotionVector;
        bCand[0] = true;
        fBias[0] = 0.0f;

        const BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fQuadUv, iFieldSize);
        FFX_UNROLL
        for (FfxInt32 s = 0; s < 4; s++)
        {
            const FfxInt32x2 iCell = bilinearInfo.iBasePos + bilinearInfo.iOffsets[s];
            VectorFieldEntry cell = NewVectorFieldEntry();
            if (IsOnScreen(iCell, iFieldSize))
                UnpackVectorFieldEntries(LoadOpticalFlowFieldMv(iCell), cell);
            fCand[1 + s] = cell.fMotionVector;
            bCand[1 + s] = cell.bValid;
            fBias[1 + s] = 0.004f;
        }

        const FfxInt32x2 iBlock = ffxMin(iBase / GetOpticalFlowBlockSize(), iFieldSize - 1);
        fCand[5] = LoadOpticalFlow(iBlock) * 0.5f;
        bCand[5] = true;
        fBias[5] = 0.004f;

        // m74: the motion this quad took in the last generated frame (motion goes on: a candidate the flow of a single pair can miss,
        // and the choice stays the same from frame to frame instead of flickering between two that agree about as well)
#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_QUAD_MOTION)
        fCand[10] = LoadQuadMotion(iQuad) / fSize;
        bCand[10] = true;
#else
        fCand[10] = FfxFloat32x2(0.0f, 0.0f);
        bCand[10] = false;
#endif
        fBias[10] = FG_TEMPORAL_BIAS;

        // m74: the refined one, filled in after the others
        fCand[11] = FfxFloat32x2(0.0f, 0.0f);
        bCand[11] = false;
        fBias[11] = 0.0f;

        // m73f: the blocks around it
        FFX_UNROLL
        for (FfxInt32 n = 0; n < 4; n++)
        {
            const FfxInt32x2 iOffset = (n == 0) ? FfxInt32x2(-1, 0) : (n == 1) ? FfxInt32x2(1, 0) : (n == 2) ? FfxInt32x2(0, -1) : FfxInt32x2(0, 1);
            const FfxInt32x2 iNear   = iBlock + iOffset;
#if defined(FG_NO_NEIGHBOURS)
            const FfxBoolean bInside = false; // tests: m73d's candidates
#else
            const FfxBoolean bInside = IsOnScreen(iNear, iFieldSize);
#endif
            fCand[6 + n] = bInside ? LoadOpticalFlow(iNear) * 0.5f : FfxFloat32x2(0.0f, 0.0f);
            bCand[6 + n] = bInside;
            fBias[6 + n] = 0.006f;
        }
#if defined(FG_TEST_EXTRA_MV)
        // tests: one more candidate everywhere (a half motion in pixels) in the last one's place, as if the field or a block had it
        fCand[9] = FfxFloat32x2(FG_TEST_EXTRA_MV) / fSize;
        bCand[9] = true;
        fBias[9] = 0.0f;
#endif
    }

    // m74, tests only: skipping the candidates where the work seemed not to matter cost more than it saved. FG_QUIET_SKIP: a quad whose
    // 4 pixels did not change between the frames (by less than FG_QUIET) kept still; but a pattern that moves by its own period (a
    // fence, a checkerboard) looks unchanged too, and the moving square of fg-test's "all" scene came out three times worse (with
    // FG_STILL's 0.02, also the faint dark textures of GTA IV's moving walls: 10% more error). FG_FIELD_EARLY: once the vector
    // field's motion agrees at all 4 pixels within FG_EARLY, nothing else is tried (it cost precision).
#if defined(FG_QUIET_SKIP)
    const FfxBoolean bQuietQuad = ffxMax(ffxMax(px[0].fErr0, px[1].fErr0), ffxMax(px[2].fErr0, px[3].fErr0)) < FG_QUIET;
#else
    const FfxBoolean bQuietQuad = false;
#endif
    FfxBoolean bSettled = bQuietQuad;

    FFX_UNROLL
    for (FfxInt32 k = 0; k < FG_CANDIDATES; k++)
    {
        // moving (more than half a pixel), and not a motion tried already
        FfxBoolean bTry = bCand[k] && length(fCand[k] * fSize) > 0.5f && !bSettled;
        FFX_UNROLL
        for (FfxInt32 j = 0; j < k; j++)
            bTry = bTry && !(bCand[j] && length((fCand[k] - fCand[j]) * fSize) < 0.25f);
        bCand[k] = bTry;
#if !defined(FG_NO_UNLIKE)
        if (bTry)
            fBias[k] += FG_UNLIKE * FgUnlike(fQuadUv, fCand[k], iFieldSize, fSize);
#endif
        if (bTry)
            FgTryCandidate(px, k, fCand[k], fBias[k]);
#if !defined(FG_NO_EARLY) && defined(FG_FIELD_EARLY)
        if (k == 0 && bTry && fBias[0] < 0.001f &&
            ffxMax(ffxMax(px[0].fErrA, px[1].fErrA), ffxMax(px[2].fErrA, px[3].fErrA)) < FG_EARLY)
            bSettled = true;
#endif
    }

#if !defined(FG_NO_REFINE)
    // m74: sub-pixel precision. The optical flow's vectors are whole pixels (half vectors in steps of half a pixel), so along a sharp edge
    // moving a fraction of a pixel the two frames' samples miss each other by up to a quarter of a pixel each: the graffiti on GTA IV's
    // walls shimmered along every line. The quad's best moving candidate (by the quad's summed score) is tried half a pixel to each
    // side on each axis, a parabola through the three errors gives the best fraction, and that motion is one more candidate (with the
    // first one's bias), which each pixel takes where it agrees better.
    {
        FfxInt32   kBest = -1;
        FfxFloat32 fBest = FG_NONE;

        FFX_UNROLL
        for (FfxInt32 k = 0; k < 11; k++)
        {
            const FfxFloat32 fQuad = px[0].fScore[k] + px[1].fScore[k] + px[2].fScore[k] + px[3].fScore[k];
            if (bCand[k] && fQuad < fBest)
            {
                fBest = fQuad;
                kBest = k;
            }
        }

        if (kBest >= 0 && !bSettled)
        {
            FfxFloat32x2 fV = FfxFloat32x2(0.0f, 0.0f);
            FfxFloat32   fB = 0.0f;
            FFX_UNROLL
            for (FfxInt32 k = 0; k < 11; k++)
            {
                if (k == kBest)
                {
                    fV = fCand[k];
                    fB = fBias[k];
                }
            }

            const FfxFloat32x2 fStep = FG_REFINE_STEP * fPx;
            const FfxFloat32   e0    = FgQuadError(px, fV);

            // where it already agrees within FG_REFINE_MIN, it stays as it is (and the work is saved)
            if (e0 > FG_REFINE_MIN)
            {
            const FfxFloat32   eXm   = FgQuadError(px, fV - FfxFloat32x2(fStep.x, 0.0f));
            const FfxFloat32   eXp   = FgQuadError(px, fV + FfxFloat32x2(fStep.x, 0.0f));
            const FfxFloat32   eYm   = FgQuadError(px, fV - FfxFloat32x2(0.0f, fStep.y));
            const FfxFloat32   eYp   = FgQuadError(px, fV + FfxFloat32x2(0.0f, fStep.y));

            // the parabola's lowest point on each axis, within a step; where the three do not curve up, the lowest of them
            const FfxFloat32 dX = eXm - 2.0f * e0 + eXp;
            const FfxFloat32 dY = eYm - 2.0f * e0 + eYp;
            FfxFloat32x2 fOff;
            fOff.x = (dX > 1e-5f) ? clamp(0.5f * (eXm - eXp) / dX, -1.0f, 1.0f) : ((ffxMin(eXm, eXp) < e0) ? ((eXm < eXp) ? -1.0f : 1.0f) : 0.0f);
            fOff.y = (dY > 1e-5f) ? clamp(0.5f * (eYm - eYp) / dY, -1.0f, 1.0f) : ((ffxMin(eYm, eYp) < e0) ? ((eYm < eYp) ? -1.0f : 1.0f) : 0.0f);

            fCand[11] = fV + fOff * fStep;
            bCand[11] = length(fOff) > 0.05f;
            fBias[11] = fB;

            if (bCand[11])
                FgTryCandidate(px, 11, fCand[11], fBias[11]);
            }
        }
    }
#endif


#if defined(FFX_FRAMEINTERPOLATION_BIND_UAV_QUAD_MOTION)
    // m74: the quad's motion for the next frame's candidate: the moving candidate it scored best with as a whole (the refined one
    // included), or none where staying still scores better
    {
        FfxFloat32   fBestQ = FG_NONE;
        FfxFloat32x2 fBestV = FfxFloat32x2(0.0f, 0.0f);

        FFX_UNROLL
        for (FfxInt32 k = 0; k < FG_CANDIDATES; k++)
        {
            const FfxFloat32 fQ = px[0].fScore[k] + px[1].fScore[k] + px[2].fScore[k] + px[3].fScore[k];
            if (bCand[k] && fQ < fBestQ)
            {
                fBestQ = fQ;
                fBestV = fCand[k];
            }
        }

        const FfxFloat32 fStillQ = px[0].fErr0 + px[1].fErr0 + px[2].fErr0 + px[3].fErr0 + 4.0f * (0.5f * fMean0 + 0.008f);
        if (!FgKeepHistory())
            StoreQuadMotion(iQuad, (fBestQ < fStillQ) ? fBestV * fSize : FfxFloat32x2(0.0f, 0.0f));
    }
#endif

    FFX_UNROLL
    for (FfxInt32 i = 0; i < 4; i++)
    {
        const FfxInt32x2 iPos = iBase + FfxInt32x2(i & 1, i >> 1);
        if (!IsOnScreen(iPos, iSize))
            continue;

        // still for FG_STILL_FRAMES frames: no motion
        // (m74: a second or third frame generated between the same two reads what the first one stored)
        const FfxUInt32  uStill  = (px[i].fErr0 < FG_STILL) ? (FgKeepHistory() ? LoadStaticHistory(iPos) : ffxMin(LoadStaticHistory(iPos) + 1, 255u)) : 0u;
        if (!FgKeepHistory())
            StoreStaticHistory(iPos, uStill);
        const FfxBoolean bForced = uStill >= FG_STILL_FRAMES;

        // the best candidate, and its blend; a tie goes to a motion (a moving pattern can match "no motion" for a frame or two)
        const FfxFloat32 fScore0 = px[i].fErr0 + 0.5f * fMean0 + 0.008f;
        const FfxBoolean bStill  = bForced || fScore0 <= px[i].fScoreA;
        FfxFloat32       fErr    = bStill ? px[i].fErr0 : px[i].fErrA;
        FfxFloat32x3     fColor  = bStill ? FgBlend(px[i].fPrev0, px[i].fCurr0) : FgBlend(px[i].fPrevA, px[i].fCurrA);

#if !defined(FG_NO_OWN_MATCH)
        // m73f: the quad's choice does not agree at this pixel, but a motion (or none) does, at this pixel alone: the pixel belongs to
        // what that motion moves (an ear of a character moving a little, in a quad that is mostly background), not to an edge
        // where something is uncovered. m73d sent it to the edge handling, which took the background's sample: dark specks along
        // Niko's ears in GTA IV.
        if (fErr > FG_MATCH)
        {
            const FfxBoolean bOwnStill = px[i].fErr0 <= px[i].fErrM;
            const FfxFloat32 fOwnErr   = bOwnStill ? px[i].fErr0 : px[i].fErrM;

            if (fOwnErr < FG_MATCH)
            {
                fErr   = fOwnErr;
                fColor = bOwnStill ? FgBlend(px[i].fPrev0, px[i].fCurr0) : FgBlend(px[i].fPrevM, px[i].fCurrM);
            }
        }
#endif

        const FfxFloat32 fEdge = ffxSaturate((fErr - FG_MATCH) / FG_MATCH_SOFT);
        // white: still (the history), grey: no motion, green: a motion
        FfxFloat32x3 fDebug = bForced ? FfxFloat32x3(1.0f, 1.0f, 1.0f) : (bStill ? FfxFloat32x3(0.5f, 0.5f, 0.5f) : FfxFloat32x3(0.0f, 0.8f, 0.0f));
        if (fEdge > 0.0f && px[i].fScoreA < FG_NONE)
        {
            // Which of the best moving candidate's samples does another motion explain? No motion (the HUD, a still
            // character), and the best moving candidate that is another motion (more than 2 pixels away: a moving object in
            // front).
            const FfxFloat32x2 fMvA = px[i].fMvA;
            FfxFloat32   fScoreB = FG_NONE;
            FfxFloat32x2 fMvB    = FfxFloat32x2(0.0f, 0.0f);
            FFX_UNROLL
            for (FfxInt32 k = 0; k < FG_CANDIDATES; k++)
            {
                if (px[i].fScore[k] < fScoreB && length((fCand[k] - fMvA) * fSize) > 2.0f)
                {
                    fScoreB = px[i].fScore[k];
                    fMvB    = fCand[k];
                }
            }
            const FfxFloat32x2 fUvP = px[i].fUv + fMvA * FgTP();
            const FfxFloat32x2 fUvC = px[i].fUv - fMvA * FgTC();

            FfxFloat32 fOwnP = 3.0f;
            FfxFloat32 fOwnC = 3.0f;
            if (length(fMvA * fSize) > 1.0f)
            {
                fOwnP = FgDiff(px[i].fPrevA, SampleCurrentBackbuffer(fUvP));
                fOwnC = FgDiff(px[i].fCurrA, SamplePreviousBackbuffer(fUvC));
            }
            if (fScoreB < FG_NONE)
            {
                fOwnP = ffxMin(fOwnP, FgDiff(px[i].fPrevA, SampleCurrentBackbuffer(fUvP - 2.0f * fMvB)));
                fOwnC = ffxMin(fOwnC, FgDiff(px[i].fCurrA, SamplePreviousBackbuffer(fUvC + 2.0f * fMvB)));
            }
            // a sample off the screen is not usable
            if (!IsUvInside(fUvP)) fOwnP = 0.0f;
            if (!IsUvInside(fUvC)) fOwnC = 0.0f;

            const FfxFloat32 fLo   = ffxMin(fOwnP, fOwnC);
            const FfxFloat32 fHi   = ffxMax(fOwnP, fOwnC);
            const FfxFloat32 fSure = ffxSaturate((FG_OWN - fLo) / (0.5f * FG_OWN)) * ffxSaturate((fHi - fLo - 0.5f * FG_OWN) / FG_OWN);
            const FfxFloat32x3 fSide = (fOwnP < fOwnC) ? px[i].fCurrA : px[i].fPrevA;

#if defined(FG_EDGE_FALLBACK_CURRENT)
            // tests: where neither sample is explained, the current frame's own pixel instead of the best candidate's blend
            fColor = ffxLerp(fColor, ffxLerp(px[i].fCurr0, fSide, fSure), fEdge);
#else
            fColor = ffxLerp(fColor, fSide, fEdge * fSure);
#endif
            // red: the previous frame's sample kept, blue: the current one's, magenta: neither explained (blended)
            const FfxFloat32x3 fSideDebug = (fOwnP < fOwnC) ? FfxFloat32x3(0.1f, 0.3f, 1.0f) : FfxFloat32x3(1.0f, 0.2f, 0.1f);
            fDebug = ffxLerp(fDebug, ffxLerp(FfxFloat32x3(1.0f, 0.0f, 1.0f), fSideDebug, fSure), fEdge);
        }

        // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW: the decisions in colour over the picture's luma
        if ((GetDispatchFlags() & FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW) != 0)
        {
#if defined(FG_DEBUG_VECTORS)
            // tests: r, g the chosen half vector in pixels (0.5 + v / 128), b the decision ((code + 0.5) / 32: 0 no motion, 1 + k
            // the moving candidate k, 13 still by the history; + 16 when the edge handling decided)
            const FfxFloat32x2 fMvPx = bStill ? FfxFloat32x2(0.0f, 0.0f) : px[i].fMvA * fSize;
            const FfxInt32     iCode = (bForced ? 13 : (bStill ? 0 : 1 + px[i].iA)) + ((fEdge > 0.0f) ? 16 : 0);
            fColor = FfxFloat32x3(ffxSaturate(0.5f + fMvPx / 128.0f), (FfxFloat32(iCode) + 0.5f) / 32.0f);
#else
            fColor = fDebug * (0.35f + 0.65f * dot(fColor, FfxFloat32x3(0.299f, 0.587f, 0.114f)));
#endif
        }

        // alpha: 1 where the edge handling decided the pixel, for fg_fi_cleanup_pass to give those the median of their neighbours (m73f).
        // Off since m73g (FG_EDGE_CLEANUP turns it on for tests): the specks it was for were not edge pixels (FgUnlike), and it
        // changed little else; the pass then copies the picture as it is.
#if defined(FG_EDGE_CLEANUP)
        StoreFrameinterpolationOutput(iPos, FfxFloat32x4(fColor, fEdge > 0.0f ? 1.0f : 0.0f));
#else
        StoreFrameinterpolationOutput(iPos, FfxFloat32x4(fColor, 0.0f));
#endif
    }
}
#else
void computeInterpolatedColor(FfxUInt32x2 iPxPos, out FfxFloat32x3 fInterpolatedColor, inout FfxFloat32 fInPaintingWeight)
{
    const FfxFloat32x2 fUvInInterpolationRect = (FfxFloat32x2(iPxPos - InterpolationRectBase()) + 0.5f) / InterpolationRectSize();
    const FfxFloat32x2 fUvInScreenSpace       = (FfxFloat32x2(iPxPos) + 0.5f) / DisplaySize();
    const FfxFloat32x2 fLrUvInInterpolationRect = fUvInInterpolationRect * (FfxFloat32x2(RenderSize()) / GetMaxRenderSize());

    const FfxFloat32x2 fUvLetterBoxScale = FfxFloat32x2(InterpolationRectSize()) / DisplaySize();

    // game MV are top left aligned, the function scales them to render res UV
    VectorFieldEntry gameMv;
    LoadInpaintedGameFieldMv(fUvInInterpolationRect, gameMv);

    // OF is done on the back buffers which already have black bars
    VectorFieldEntry ofMv;
    SampleOpticalFlowMotionVectorField(fUvInScreenSpace, ofMv);

    // Binarize disucclusion factor
    FfxFloat32x2 fDisocclusionFactor = FfxFloat32x2(FFX_EQUAL(ffxSaturate(SampleDisocclusionMask(fLrUvInInterpolationRect).xy), FfxFloat32x2(1.0, 1.0)));

    InterpolationSourceColor fPrevColorGame = SampleTextureBilinear(false, fUvInScreenSpace, +gameMv.fMotionVector * fUvLetterBoxScale, DisplaySize()); // Get in previous frame buffer, the color of interpolated pixel
    InterpolationSourceColor fCurrColorGame = SampleTextureBilinear(true, fUvInScreenSpace, -gameMv.fMotionVector * fUvLetterBoxScale, DisplaySize()); // Get color in current framebuffer, of color of interpolated pixel

    InterpolationSourceColor fPrevColorOF = SampleTextureBilinear(false, fUvInScreenSpace, +ofMv.fMotionVector * fUvLetterBoxScale, DisplaySize());
    InterpolationSourceColor fCurrColorOF = SampleTextureBilinear(true, fUvInScreenSpace, -ofMv.fMotionVector * fUvLetterBoxScale, DisplaySize());

    FfxFloat32 fDisoccludedFactor = 0.0f;

    // Disocclusion logic
    {
        fDisocclusionFactor.x *= FfxFloat32(!gameMv.bPosOutside); // fDisocclusionFactor.x of 1 means the pos of interpolated pixel is within bounds of previous frame.
        fDisocclusionFactor.y *= FfxFloat32(!gameMv.bNegOutside); // fDisocclusionFactor.y of 1 means the pos of interpolated pixel is within bounds of current frame

        // Inpaint in bi-directional disocclusion areas
        updateInPaintingWeight(fInPaintingWeight, FfxFloat32(length(fDisocclusionFactor) <= FFX_FRAMEINTERPOLATION_EPSILON));

        FfxFloat32 t = 0.5f;
        t += 0.5f * (1 - (fDisocclusionFactor.x));
        t -= 0.5f * (1 - (fDisocclusionFactor.y));
        // Say if fDisocclusionFactor.x is 1 and fDisocclusionFactor.y = 0, then t will be 0. fInterpolatedColor will be entirely from fPrevColorGame 
        fInterpolatedColor = ffxLerp(fPrevColorGame.fRaw, fCurrColorGame.fRaw, ffxSaturate(t));
        fDisoccludedFactor = ffxSaturate(1 - ffxMin(fDisocclusionFactor.x, fDisocclusionFactor.y));

        if (fPrevColorGame.fBilinearWeightSum == 0.0f)
        {
            fInterpolatedColor = fCurrColorGame.fRaw;
        }
        else if (fCurrColorGame.fBilinearWeightSum == 0.0f)
        {
            fInterpolatedColor = fPrevColorGame.fRaw;
        }
        if (fPrevColorGame.fBilinearWeightSum == 0 && fCurrColorGame.fBilinearWeightSum == 0)
        {
            fInPaintingWeight = 1.0f;
        }
    }

    {

        FfxFloat32 ofT = 0.5f;

        if (fPrevColorOF.fBilinearWeightSum > 0 && fCurrColorOF.fBilinearWeightSum > 0)
        {
            ofT = 0.5f;
        }
        else if (fPrevColorOF.fBilinearWeightSum > 0)
        {
            ofT = 0;
        } else {
            ofT = 1;
        }

        const FfxFloat32x3 ofColor = ffxLerp(fPrevColorOF.fRaw, fCurrColorOF.fRaw, ofT);

        FfxFloat32 fOF_Sim = NormalizedDot3(fPrevColorOF.fRaw, fCurrColorOF.fRaw);
        FfxFloat32 fGame_Sim = NormalizedDot3(fPrevColorGame.fRaw, fCurrColorGame.fRaw);

        fGame_Sim = ffxLerp(ffxMax(FFX_FRAMEINTERPOLATION_EPSILON, fGame_Sim), 1.0f, ffxSaturate(fDisoccludedFactor));
        FfxFloat32 fGameMvBias = ffxPow(ffxSaturate(fGame_Sim / ffxMax(FFX_FRAMEINTERPOLATION_EPSILON, fOF_Sim)), 1.0f);

        const FfxFloat32 fFrameIndexFactor = FfxFloat32(FrameIndexSinceLastReset() < 10);
        fGameMvBias = ffxLerp(fGameMvBias, 1.0f, fFrameIndexFactor);

        fInterpolatedColor = ffxLerp(ofColor, fInterpolatedColor, ffxSaturate(fGameMvBias));
    }
}

#endif // FFX_FG_OF_ONLY

#if !defined(FFX_FG_OF_ONLY)
void computeFrameinterpolation(FfxInt32x2 iPxPos)
{
    FfxFloat32x3 fColor            = FfxFloat32x3(0, 0, 0);
    FfxFloat32   fInPaintingWeight = 0.0f;

    if (IsInRect(iPxPos, InterpolationRectBase(), InterpolationRectSize()) == false || FrameIndexSinceLastReset() == 0)
    {
        // if we just reset or we are out of the interpolation rect, copy the current back buffer and don't interpolate
        fColor = LoadCurrentBackbuffer(iPxPos);
    }
    else
    {
        computeInterpolatedColor(iPxPos, fColor, fInPaintingWeight);
    }

    StoreFrameinterpolationOutput(FfxInt32x2(iPxPos), FfxFloat32x4(fColor, fInPaintingWeight));
}
#endif // !FFX_FG_OF_ONLY

#endif  // FFX_FRAMEINTERPOLATION_H
