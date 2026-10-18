#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QSvgRenderer>

// SVG icons from the resource tree, rendered at a logical size and, when a
// state needs it, tinted: the design draws one colour per icon (the lock,
// eye and speaker in #a4a7ae; Frames and Snap in #cccccd2, Seconds in white
// because it is the checked one), and a checked / unchecked or on / off
// state is the same shape in another colour. Tinting keeps one file per
// shape instead of one per colour.
//
// A missing or unreadable file gives a null QIcon here, which is what the
// gate checks for: QIcon(":/missing.svg") itself is NOT null - it is a
// lazily failing icon that paints nothing.
namespace SankoIcons {

inline QPixmap renderSvg(const QString &resourcePath, const QSize &logicalSize,
                         qreal devicePixelRatio = 1.0,
                         const QColor &tint = QColor())
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid() || logicalSize.isEmpty())
        return QPixmap();
    QPixmap pm(logicalSize * devicePixelRatio);
    pm.setDevicePixelRatio(devicePixelRatio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Keep the file's own aspect: fit it inside the requested box, centred.
    const QSizeF natural = renderer.defaultSize();
    QRectF target(QPointF(0, 0), QSizeF(logicalSize));
    if (!natural.isEmpty()) {
        QSizeF fitted = natural;
        fitted.scale(QSizeF(logicalSize), Qt::KeepAspectRatio);
        target = QRectF(QPointF((logicalSize.width() - fitted.width()) / 2.0,
                                (logicalSize.height() - fitted.height()) / 2.0),
                        fitted);
    }
    renderer.render(&p, target);
    if (tint.isValid()) {
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(QRectF(QPointF(0, 0), QSizeF(logicalSize)), tint);
    }
    p.end();
    return pm;
}

// An icon with the 1x and 2x renderings a QIcon needs on mixed screens.
inline QIcon icon(const QString &resourcePath, const QSize &logicalSize,
                  const QColor &tint = QColor())
{
    QIcon ic;
    const QPixmap one = renderSvg(resourcePath, logicalSize, 1.0, tint);
    if (one.isNull())
        return ic; // null: the gate's "every icon loads" check reads this
    ic.addPixmap(one);
    ic.addPixmap(renderSvg(resourcePath, logicalSize, 2.0, tint));
    return ic;
}

} // namespace SankoIcons
