#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPoint>

class PixelCompositor
{
public:
    enum class Path { Auto, Scalar, Simd };

    static bool simdAvailable();
    static QString activePathName();
    static void compositeGrayscaleTile(QImage &destination, const QPoint &origin,
                                       const QImage &mask, const QColor &color,
                                       Path path = Path::Auto);
    // Erase composite: coverage removes alpha (see the .cpp). The colour
    // argument sourceOver takes is meaningless for erasing; strength is the
    // brush colour's alphaF, exactly the factor sourceOver would apply.
    static void eraseGrayscaleTile(QImage &destination, const QPoint &origin,
                                   const QImage &mask, qreal strength);
    static void compositeRgbaMaskTile(QImage &destination, const QPoint &origin,
                                      const QImage &mask, const QColor &color,
                                      Path path = Path::Auto);
    static QImage compositeRgbaMasks(const QImage &base,
                                     const QHash<QPoint, QImage> &masks,
                                     const QColor &color, Path path = Path::Auto);
    static QImage compositeRgbaMasksRegion(const QImage &baseRegion,
                                           const QPoint &regionCanvasOrigin,
                                           const QHash<QPoint, QImage> &masks,
                                           const QColor &color, Path path = Path::Auto);
    static QImage convertGrayscale16ToRgba(const QImage &source,
                                           bool mirrorVertically = false,
                                           Path path = Path::Auto);
};
