#pragma once

#include <algorithm>

// The ONE CPU implementation of the wet-edges transfer, mirrored line for
// line by publish.frag. Both CPU consumers — StrokeBuilder's mask
// publication and ColorStrokeBuffer's colour publication — call this, so
// the CPU cannot fork against itself (the GrainField discipline).
//
// The transfer operates on the ACCUMULATED stroke coverage at the
// publication boundary, once per stroke — never per stamp, which would
// ring every dab and read as corduroy on a low-spacing stroke.
//
//   cRel = alpha / ceiling            coverage relative to the stroke's
//                                     opacity ceiling (the mask stores
//                                     alpha already scaled by opacity;
//                                     the colour buffer stores it
//                                     normalised, ceiling 1)
//   out  = alpha * (1 - amount * cRel^2)
//
// Properties, all load-bearing:
//   * out <= alpha everywhere, so the flow opacity ceiling can never be
//     exceeded — wet edges only ever removes paint.
//   * The factor falls with coverage, so the fully-covered interior
//     lightens to (1 - amount) while the falloff band keeps most of its
//     value: the profile peaks at cRel = 1/sqrt(3*amount) when that is
//     inside [0,1] — the darkened rim. A hard tip has almost no falloff
//     band, so it shows only the interior lightening (as in Photoshop).
//   * amount 0 is the identity; every call site guards it and takes the
//     pre-6a code path untouched, keeping wet-off rendering bit-identical.
inline qreal wetEdgesAlpha(qreal alpha, qreal ceiling, qreal amount)
{
    const qreal cRel = std::clamp(
        ceiling > 0.0001 ? alpha / ceiling : 0.0, 0.0, 1.0);
    return alpha * (1.0 - amount * cRel * cRel);
}
