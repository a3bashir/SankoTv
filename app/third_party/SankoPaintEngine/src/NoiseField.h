#pragma once

#include <QtGlobal>

#include <algorithm>

// The ONE CPU implementation of the stamp shader's noise term, mirrored
// line for line by stamp.frag and color_stamp.frag (the GrainField /
// WetEdges discipline). Both CPU consumers — StrokeBuilder::placeStamp
// (the coverage path) and ColorStrokeBuffer::composite (the colour path)
// — perturb tip coverage through these functions.
//
// THE MODEL. Photoshop's Noise roughens the tip's alpha FALLOFF: the soft
// edge of a dab breaks into a grainy, dispersed border while a hard tip
// is almost untouched (there is no falloff to perturb). Here that is a
// per-pixel symmetric perturbation of coverage, weighted by a tent that
// peaks at coverage 0.5 and vanishes at 0 and 1:
//
//   w  = 2 * min(c, 1 - c)          0 at empty and solid, 1 mid-falloff
//   c' = clamp(c + amount * n * w)  n in [-1, 1) from the indexed hash
//
// Properties, all load-bearing:
//   * c' == c at c = 0 and c = 1: nothing paints outside the tip's
//     support, the solid interior stays solid, and a hard tip (whose
//     falloff band is a pixel or two) shows almost nothing — the
//     hardness interaction the effect is defined by.
//   * c' <= 1 everywhere, so the flow opacity ceiling downstream can
//     never be exceeded: coverage still enters the same deposit rules.
//   * The transform is CONTINUOUS in c (|dc'/dc| <= 1 + 2*amount). The
//     two renderers agree on c only within quantisation (the CPU bakes
//     8-bit tip rasters; the GPU evaluates the falloff in float), so a
//     THRESHOLD model — dissolve-style c' = (n < c) — would amplify that
//     1/255 disagreement to full-scale single-pixel flips, exactly the
//     sampling divergence the custom-tip and grain fixes closed. The
//     tent keeps the divergence proportional instead.
//   * amount 0 is the identity; every call site guards it and takes the
//     pre-6e code path untouched, keeping noise-off rendering
//     bit-identical.
//
// DETERMINISM AND ANCHORING. The hash extends the indexed RNG pattern to
// the pixel: the per-stamp base seed is indexedHash(strokeSeed, stampIndex,
// property 105, subBrushSlot), resolved once in StrokeBuilder::resolveStamp
// and carried on the stamp (the GPU path never sees the stroke seed); the
// per-pixel key is the STAMP-LOCAL integer raster offset from the stamp's
// quantised raster centre — the same anchor Rolling grain uses, which is
// byte-for-byte the centre both GPU instance builders pass. Same seed,
// same stamp, same pixel always yields the same value, so undo/redo
// replay, QuickShape path replay, and preview regeneration all reproduce.
// The pattern is stamp-anchored: it moves with the dab.
//
// CPU/GPU HASH AGREEMENT. Everything is 32-bit unsigned arithmetic —
// wrapping multiply, xor, shift — which C++ quint32 and GLSL uint define
// identically; int -> uint of a negative offset is two's complement in
// both. The unit conversion keeps exactly 24 bits so float32 (the
// shader) and double (here) compute the identical value:
// (h >> 8) * 2^-24 is a 24-bit integer times a power of two — exact in
// both. The only cross-path difference left is coverage itself, which
// the tent bounds (above).
inline quint32 noiseHash(quint32 seed, qint32 ix, qint32 iy)
{
    quint32 v = seed ^ (quint32(ix) * 0x85ebca6bu)
        ^ (quint32(iy) * 0xc2b2ae35u);
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// Signed unit value in [-1, 1), exact in float32 and double alike.
inline qreal noiseSigned(quint32 seed, qint32 ix, qint32 iy)
{
    return (noiseHash(seed, ix, iy) >> 8) * (1.0 / 16777216.0) * 2.0 - 1.0;
}

// The coverage transform. (ix, iy) is the stamp-local integer pixel:
// floor(pixelCentre - rasterCentre + 0.25) per axis — the 0.25 bias puts
// the value 0.25 px from every integer boundary for BOTH even and odd
// rasters (the offset grid is integral or half-integral), so the GPU's
// interpolated offset can never floor to a different cell.
inline qreal noisyCoverage(qreal coverage, qreal amount, quint32 seed,
                           qint32 ix, qint32 iy)
{
    const qreal w = 2.0 * std::min(coverage, 1.0 - coverage);
    if (w <= 0.0)
        return coverage;
    return std::clamp(
        coverage + amount * noiseSigned(seed, ix, iy) * w, 0.0, 1.0);
}
