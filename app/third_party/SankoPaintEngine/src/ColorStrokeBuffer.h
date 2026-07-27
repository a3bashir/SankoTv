#pragma once

#include "Brush.h"
#include "StrokeBuilder.h"

#include <QImage>

// Production CPU reference for Variant C's RGBA16 premultiplied stroke buffer.
class ColorStrokeBuffer
{
public:
    static QImage composite(const QImage &preStrokeLayer, const Brush &brush,
                            const QVector<StrokeStamp> &resolvedStamps);
};
