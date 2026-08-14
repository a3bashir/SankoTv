#pragma once

#include "Brush.h"

#include <algorithm>

// The ONE CPU implementation of the stamp shaders' texture blend term,
// mirrored line for line by stamp.frag and color_stamp.frag (the
// GrainField / WetEdges / NoiseField discipline — it cannot be literally
// shared because one side is GLSL). Both CPU consumers —
// StrokeBuilder::placeStamp and ColorStrokeBuffer::composite — combine tip
// coverage with the grain texture through this function whenever the
// brush's TextureBlendMode is anything other than Multiply.
//
// MULTIPLY NEVER ROUTES THROUGH HERE ON THE RENDER PATHS. Every call site
// keeps its pre-existing expression — GrainField::modulation's
// c * (1 - d + d*t) — verbatim behind a guard, so the 62 built-ins stay
// bit-identical BY CONSTRUCTION rather than by re-derivation. The Multiply
// case below exists so the function is total (and testable), and is the
// same algebra.
//
// TERMS. c = tip coverage AFTER noise (6e's order: tip -> noise -> texture
// blend — this stage sits exactly where grain multiplication always sat);
// t = the contrast-shaped texture sample GrainField already computes
// (clamp((raw - .5) * grainContrast + .5, 0, 1)) — contrast keeps its
// meaning across every mode because shaping happens BEFORE the blend;
// d = the stamp's RESOLVED effectiveGrainDepth — Phase 1's single dynamic
// resolver ran in StrokeBuilder::resolveStamp; there is no second
// resolution site here.
//
// Every mode is the IDENTITY at d = 0 (Photoshop's depth-0 = no texture),
// and every mode clamps to [0, 1], so coverage still enters the unchanged
// deposit rules and the flow opacity ceiling cannot be exceeded.
//
// CONFIDENCE, per mode — read this before trusting a formula:
//   EXACT        Multiply: it is the engine's original expression.
//   PS-MATCHED   Height, LinearHeight: Krita's *_PHOTOSHOP composite ops
//                (KisMaskingBrushCompositeOp.h), the soft-texturing
//                variants — someone else's explicit reverse-engineering of
//                Photoshop, not an Adobe spec. If a Photoshop user ever
//                reports a mismatch, start here.
//   CONSTRUCTED  Subtract, Darken, Overlay, ColorBurn, LinearBurn,
//                HardMix: the standard blend formula f(c, t) applied under
//                mix(c, f(c, t), d) — mechanically defined, identity at
//                d = 0, but NOT verified against Photoshop.
inline qreal textureBlendCoverage(::Brush::TextureBlendMode mode,
                                  qreal c, qreal t, qreal d)
{
    using M = ::Brush::TextureBlendMode;
    qreal f = c;
    switch (mode) {
    case M::Multiply: // EXACT — c*(1-d+d*t) rewritten as the mix below
        f = c * t;
        break;
    case M::Subtract: // CONSTRUCTED
        f = std::max(0.0, c - (1.0 - t));
        break;
    case M::Darken: // CONSTRUCTED
        f = std::min(c, t);
        break;
    case M::Overlay: // CONSTRUCTED
        f = c <= 0.5 ? 2.0 * c * t
                     : 1.0 - 2.0 * (1.0 - c) * (1.0 - t);
        break;
    case M::ColorBurn: // CONSTRUCTED
        f = t > 0.0 ? 1.0 - std::min(1.0, (1.0 - c) / t) : 0.0;
        break;
    case M::LinearBurn: // CONSTRUCTED
        f = std::max(0.0, c + t - 1.0);
        break;
    case M::HardMix: // CONSTRUCTED
        f = c + t >= 1.0 ? 1.0 : 0.0;
        break;
    case M::Height: // PS-MATCHED (Krita HEIGHT_PHOTOSHOP, soft)
        return std::clamp(c * (1.0 + 9.0 * d) - d * t, 0.0, 1.0);
    case M::LinearHeight: { // PS-MATCHED (Krita LINEAR_HEIGHT_PHOTOSHOP, soft)
        const qreal m = c * (1.0 + 9.0 * d);
        return std::clamp(std::max(m * (1.0 - d * t), m - d * t), 0.0, 1.0);
    }
    }
    return std::clamp(c * (1.0 - d) + f * d, 0.0, 1.0);
}
