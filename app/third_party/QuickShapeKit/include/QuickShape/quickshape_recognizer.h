#pragma once

#include "quickshape_types.h"

namespace quickshape {

class QuickShapeRecognizer final
{
public:
    QuickShapeRecognizer() = delete;

    // Points must be in one stable coordinate space, preferably document space.
    // The returned rough and target vectors contain the same number of samples.
    static QuickShapeResult recognize(const QVector<QPointF> &points);
};

} // namespace quickshape

