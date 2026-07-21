#pragma once

#include <QColor>
#include <QImage>

// Phase 1 brush data model.
//
// Plain-data and serializable-friendly: every field is a POD, QColor, or
// QImage, so later phases (pressure dynamics, GPU params, dual brush) can add
// members without changing how this struct is constructed or copied. The
// engine (BrushEngine) owns behaviour; this owns only state.
struct Brush
{
    // Optional custom grayscale tip mask. When null, BrushEngine synthesises a
    // procedural round tip from size + hardness. When set, its ALPHA (or, for
    // an opaque grayscale image, its luminance) is used as the stamp coverage.
    QImage shape;

    double size = 24.0;     // tip diameter, canvas px
    double spacing = 0.10;  // stamp interval as a FRACTION of size (0 < s)
    double opacity = 1.0;   // whole-stroke opacity, 0..1
    double hardness = 0.80; // edge falloff: 1 = crisp, 0 = soft gaussian-like
    QColor color = QColor(0, 0, 0);
};
