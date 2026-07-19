#include "DockOverlay.h"

#include <QDockWidget>
#include <QPainter>
#include <QPropertyAnimation>

DockOverlay::DockOverlay(QWidget *host, const QColor &accent)
    : QWidget(host), m_accent(accent)
{
    // Purely visual: never intercepts the drag's mouse events, never draws a
    // system background (the glow floats over the live layout).
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);

    // Soft breathing glow while a preview is visible.
    m_animation = new QPropertyAnimation(this, "glowOpacity", this);
    m_animation->setDuration(650);
    m_animation->setStartValue(0.25);
    m_animation->setEndValue(0.95);
    m_animation->setLoopCount(-1);
    m_animation->setEasingCurve(QEasingCurve::InOutSine);

    hide();
}

void DockOverlay::showPreview(const QRect &previewRect, DockZone zone,
                              QDockWidget *target)
{
    m_previewRect = previewRect;
    m_zone = zone;
    m_targetDock = target;
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    if (m_animation->state() != QAbstractAnimation::Running)
        m_animation->start();
    show();
    raise();
    update();
}

void DockOverlay::hidePreview()
{
    m_animation->stop();
    m_previewRect = QRect();
    m_zone = DockZone::None;
    m_targetDock.clear();
    hide();
}

void DockOverlay::setGlowOpacity(qreal opacity)
{
    m_glowOpacity = opacity;
    update();
}

void DockOverlay::paintEvent(QPaintEvent *)
{
    if (m_zone == DockZone::None || !m_previewRect.isValid())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(0, 0, 0, 35)); // dim the layout underneath

    // Dashed outline around the target dock so the user sees WHICH panel the
    // drop relates to (host coordinates; the target may sit in a tab group).
    if (m_targetDock) {
        const QRect targetRect(
            m_targetDock->mapTo(parentWidget(), QPoint(0, 0)),
            m_targetDock->size());
        QColor outline = m_accent;
        outline.setAlpha(95);
        QPen dash(outline, 2, Qt::DashLine);
        p.setPen(dash);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(targetRect.adjusted(1, 1, -1, -1), 12, 12);
    }

    const bool splitIndicator = m_targetDock && m_zone != DockZone::Center;
    if (splitIndicator) {
        // Thin insertion line where the split would land, wrapped in a glow.
        QLine line;
        if (m_zone == DockZone::Left || m_zone == DockZone::Right) {
            const int x = m_previewRect.center().x();
            line = QLine(x, m_previewRect.top(), x, m_previewRect.bottom());
        } else {
            const int y = m_previewRect.center().y();
            line = QLine(m_previewRect.left(), y, m_previewRect.right(), y);
        }
        for (int width = 14; width >= 5; width -= 3) {
            QColor glow = m_accent;
            glow.setAlphaF(m_glowOpacity * (15 - width) / 38.0);
            QPen pen(glow, width, Qt::SolidLine, Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(line);
        }
        QPen crisp(m_accent, 3, Qt::SolidLine, Qt::RoundCap);
        p.setPen(crisp);
        p.drawLine(line);
        return;
    }

    // Area / tab preview: glow rings around a filled rounded rectangle.
    for (int width = 18; width >= 4; width -= 3) {
        QColor glow = m_accent;
        glow.setAlphaF(m_glowOpacity * (20 - width) / 115.0);
        QPen pen(glow, width);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(m_previewRect.adjusted(-width / 2, -width / 2,
                                                 width / 2, width / 2),
                          18, 18);
    }
    QColor fill = m_accent;
    fill.setAlpha(72);
    QColor border = m_accent;
    border.setAlphaF(qBound(0.0, 0.75 + m_glowOpacity * 0.25, 1.0));
    p.setPen(QPen(border, 3));
    p.setBrush(fill);
    p.drawRoundedRect(m_previewRect, 16, 16);
}
