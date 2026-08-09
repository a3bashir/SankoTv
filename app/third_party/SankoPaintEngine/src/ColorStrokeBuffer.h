#pragma once

#include "Brush.h"
#include "StrokeBuilder.h"

#include <QImage>

// Production CPU reference for Variant C's RGBA16 premultiplied stroke buffer.
class ColorStrokeBuffer
{
public:
    // subBrushSlot: 0 for a single-brush stroke, 1 for the secondary half
    // of a dual render — wet edges is suppressed for dual sub-strokes (the
    // dual publication shader is untouched in 6a, and the CPU must not
    // apply what the GPU will not).
    static QImage composite(const QImage &preStrokeLayer, const Brush &brush,
                            const QVector<StrokeStamp> &resolvedStamps,
                            quint32 subBrushSlot = 0);
};
