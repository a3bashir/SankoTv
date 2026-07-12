#include "ColorPanel.h"

#include <QConicalGradient>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QStackedLayout>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>

// All picker parts live here (no Q_OBJECT: they report through std::function
// callbacks, keeping the header minimal and the parts easy to extend).
namespace ColorPanelParts {

namespace {
constexpr int kPickerSize = 228;             // both pickers are this wide
const QColor kSurface(0x21, 0x21, 0x21);     // Sanko panel surface
const QColor kInset(0x16, 0x16, 0x16);       // recessed track/segment bg
const QColor kLine(0x2a, 0x2a, 0x2a);        // hairlines
const QColor kText(0xcc, 0xcc, 0xcc);
const QColor kTextDim(0x80, 0x80, 0x80);
const QColor kAccent(0x7c, 0x6e, 0xf6);      // Sanko accent

QColor hsv(qreal h, qreal s, qreal v)
{
    return QColor::fromHsvF(qBound(0.0, h, 1.0) >= 1.0 ? 0.0 : qBound(0.0, h, 1.0),
                            qBound(0.0, s, 1.0), qBound(0.0, v, 1.0));
}

// Round indicator ring used by every picker: white ring + dark halo, filled
// with the color it currently points at (reads on any background).
void drawIndicator(QPainter &p, const QPointF &pos, qreal r, const QColor &fill)
{
    p.setPen(QPen(QColor(0, 0, 0, 140), 3.5));
    p.setBrush(fill);
    p.drawEllipse(pos, r, r);
    p.setPen(QPen(Qt::white, 1.8));
    p.drawEllipse(pos, r, r);
}
} // namespace

// --- Wheel / Classic segmented switch ----------------------------------------
class SegmentedTabs : public QWidget
{
public:
    std::function<void(int)> onSelect;

    explicit SegmentedTabs(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
    }
    void setCurrent(int index)
    {
        m_current = index;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(kInset);
        p.drawRoundedRect(rect(), 6, 6);
        const qreal segW = width() / 2.0;
        p.setBrush(kAccent);
        p.drawRoundedRect(QRectF(m_current * segW + 2, 2, segW - 4, height() - 4), 5, 5);
        QFont f = font();
        f.setPixelSize(11);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(m_current == 0 ? Qt::white : kTextDim);
        p.drawText(QRectF(0, 0, segW, height()), Qt::AlignCenter, QStringLiteral("Wheel"));
        p.setPen(m_current == 1 ? Qt::white : kTextDim);
        p.drawText(QRectF(segW, 0, segW, height()), Qt::AlignCenter,
                   QStringLiteral("Classic"));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const int index = e->position().x() < width() / 2.0 ? 0 : 1;
        if (index != m_current) {
            m_current = index;
            update();
            if (onSelect)
                onSelect(index);
        }
    }

private:
    int m_current = 0;
};

// --- Wheel tab: hue ring + saturation/value disc ------------------------------
class WheelPicker : public QWidget
{
public:
    std::function<void(qreal, qreal, qreal)> onChange; // live h,s,v (0..1)
    std::function<void()> onCommit;                    // pointer released

    explicit WheelPicker(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(kPickerSize, kPickerSize);
        setCursor(Qt::CrossCursor);
    }
    void setHsv(qreal h, qreal s, qreal v)
    {
        m_h = h;
        m_s = s;
        m_v = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF c(width() / 2.0, height() / 2.0);

        // Hue ring: conical gradient (angle == hue) filling an annulus.
        QConicalGradient cone(c, 0);
        for (int i = 0; i <= 6; ++i)
            cone.setColorAt(i / 6.0, hsv(i / 6.0, 1.0, 1.0));
        QPainterPath ring;
        ring.addEllipse(c, outerR(), outerR());
        ring.addEllipse(c, innerR(), innerR());
        p.setPen(Qt::NoPen);
        p.setBrush(cone);
        p.drawPath(ring);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(kLine, 1));
        p.drawEllipse(c, outerR(), outerR());
        p.drawEllipse(c, innerR(), innerR());

        // Saturation/value disc (x -> saturation, y -> value), regenerated
        // only when the hue changes.
        ensureDisc();
        QPainterPath clip;
        clip.addEllipse(c, discR(), discR());
        p.save();
        p.setClipPath(clip);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(QPointF(c.x() - discR(), c.y() - discR()), m_disc);
        p.restore();
        p.setPen(QPen(kLine, 1));
        p.drawEllipse(c, discR(), discR());

        // Indicators: hue dot on the ring, sat/val dot on the disc.
        const qreal a = m_h * 2.0 * M_PI;
        const qreal midR = (outerR() + innerR()) / 2.0;
        drawIndicator(p, QPointF(c.x() + midR * qCos(a), c.y() - midR * qSin(a)),
                      7.0, hsv(m_h, 1.0, 1.0));
        const QPointF sv(c.x() + (m_s * 2.0 - 1.0) * discR(),
                         c.y() + (1.0 - m_v * 2.0) * discR());
        drawIndicator(p, clampToDisc(sv), 6.0, hsv(m_h, m_s, m_v));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const QPointF d = e->position() - center();
        const qreal dist = std::hypot(d.x(), d.y());
        if (dist <= discR() + 4.0)
            m_drag = DragDisc;
        else if (dist >= innerR() - 6.0 && dist <= outerR() + 6.0)
            m_drag = DragRing;
        else
            m_drag = DragNone;
        applyDrag(e->position());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            applyDrag(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_drag != DragNone && onCommit)
            onCommit();
        m_drag = DragNone;
    }

private:
    enum Drag { DragNone, DragRing, DragDisc };

    QPointF center() const { return QPointF(width() / 2.0, height() / 2.0); }
    qreal outerR() const { return width() / 2.0 - 4.0; }
    qreal innerR() const { return outerR() - 24.0; }
    qreal discR() const { return innerR() - 10.0; }
    QPointF clampToDisc(const QPointF &pos) const
    {
        const QPointF d = pos - center();
        const qreal dist = std::hypot(d.x(), d.y());
        if (dist <= discR())
            return pos;
        return center() + d * (discR() / dist);
    }
    void ensureDisc()
    {
        const int side = int(discR() * 2.0);
        const int hueKey = int(m_h * 3600.0);
        if (!m_disc.isNull() && m_disc.width() == side && m_discHueKey == hueKey)
            return;
        m_discHueKey = hueKey;
        m_disc = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < side; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(m_disc.scanLine(y));
            const qreal v = 1.0 - qreal(y) / (side - 1);
            for (int x = 0; x < side; ++x) {
                const qreal s = qreal(x) / (side - 1);
                line[x] = hsv(m_h, s, v).rgba();
            }
        }
    }
    void applyDrag(const QPointF &pos)
    {
        if (m_drag == DragNone || !onChange)
            return;
        const QPointF d = pos - center();
        if (m_drag == DragRing) {
            qreal a = qAtan2(-d.y(), d.x()) / (2.0 * M_PI); // CCW, 0 at 3 o'clock
            if (a < 0)
                a += 1.0;
            m_h = a;
        } else {
            const QPointF cl = clampToDisc(pos) - center();
            m_s = qBound(0.0, (cl.x() / discR() + 1.0) / 2.0, 1.0);
            m_v = qBound(0.0, (1.0 - cl.y() / discR()) / 2.0, 1.0);
        }
        onChange(m_h, m_s, m_v);
    }

    qreal m_h = 0.0, m_s = 0.0, m_v = 0.0;
    QImage m_disc;
    int m_discHueKey = -1;
    Drag m_drag = DragNone;
};

// --- Classic tab: saturation/value square + hue strip --------------------------
class ClassicPicker : public QWidget
{
public:
    std::function<void(qreal, qreal, qreal)> onChange;
    std::function<void()> onCommit;

    explicit ClassicPicker(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(kPickerSize, kPickerSize);
        setCursor(Qt::CrossCursor);
    }
    void setHsv(qreal h, qreal s, qreal v)
    {
        m_h = h;
        m_s = s;
        m_v = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // S/V square: white -> pure hue horizontally, then to black downward.
        QPainterPath sq;
        sq.addRoundedRect(squareRect(), 6, 6);
        QLinearGradient sat(squareRect().topLeft(), squareRect().topRight());
        sat.setColorAt(0.0, Qt::white);
        sat.setColorAt(1.0, hsv(m_h, 1.0, 1.0));
        p.fillPath(sq, sat);
        QLinearGradient val(squareRect().topLeft(), squareRect().bottomLeft());
        val.setColorAt(0.0, QColor(0, 0, 0, 0));
        val.setColorAt(1.0, Qt::black);
        p.fillPath(sq, val);
        p.setPen(QPen(kLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(sq);

        // Hue strip below, with its own handle.
        QPainterPath strip;
        strip.addRoundedRect(hueRect(), 6, 6);
        QLinearGradient hues(hueRect().topLeft(), hueRect().topRight());
        for (int i = 0; i <= 6; ++i)
            hues.setColorAt(i / 6.0, hsv(i / 6.0, 1.0, 1.0));
        p.fillPath(strip, hues);
        p.setPen(QPen(kLine, 1));
        p.drawPath(strip);

        // Indicators.
        drawIndicator(p,
                      QPointF(squareRect().left() + m_s * squareRect().width(),
                              squareRect().top() + (1.0 - m_v) * squareRect().height()),
                      6.0, hsv(m_h, m_s, m_v));
        drawIndicator(p,
                      QPointF(hueRect().left() + m_h * hueRect().width(),
                              hueRect().center().y()),
                      6.0, hsv(m_h, 1.0, 1.0));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (squareRect().adjusted(-4, -4, 4, 4).contains(e->position()))
            m_drag = DragSquare;
        else if (hueRect().adjusted(-4, -6, 4, 6).contains(e->position()))
            m_drag = DragHue;
        else
            m_drag = DragNone;
        applyDrag(e->position());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            applyDrag(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_drag != DragNone && onCommit)
            onCommit();
        m_drag = DragNone;
    }

private:
    enum Drag { DragNone, DragSquare, DragHue };

    QRectF squareRect() const { return QRectF(0, 0, width(), height() - 26); }
    QRectF hueRect() const { return QRectF(0, height() - 16, width(), 16); }
    void applyDrag(const QPointF &pos)
    {
        if (m_drag == DragNone || !onChange)
            return;
        if (m_drag == DragSquare) {
            m_s = qBound(0.0, (pos.x() - squareRect().left()) / squareRect().width(), 1.0);
            m_v = qBound(0.0, 1.0 - (pos.y() - squareRect().top()) / squareRect().height(),
                         1.0);
        } else {
            m_h = qBound(0.0, (pos.x() - hueRect().left()) / hueRect().width(), 1.0);
            if (m_h >= 1.0)
                m_h = 0.999;
        }
        onChange(m_h, m_s, m_v);
    }

    qreal m_h = 0.0, m_s = 0.0, m_v = 0.0;
    Drag m_drag = DragNone;
};

// --- H / S / B slider with a live gradient track --------------------------------
class ChannelSlider : public QWidget
{
public:
    enum Channel { Hue, Saturation, Brightness };
    std::function<void(qreal)> onChange; // this channel's new value, 0..1
    std::function<void()> onCommit;

    explicit ChannelSlider(Channel channel, QWidget *parent = nullptr)
        : QWidget(parent), m_channel(channel)
    {
        setFixedHeight(20);
        setCursor(Qt::PointingHandCursor);
    }
    void setHsv(qreal h, qreal s, qreal v)
    {
        m_h = h;
        m_s = s;
        m_v = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont f = font();
        f.setPixelSize(10);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(kTextDim);
        static const char *names[] = {"H", "S", "B"};
        p.drawText(QRectF(0, 0, 12, height()), Qt::AlignCenter,
                   QLatin1String(names[m_channel]));

        // Gradient track sweeping THIS channel with the others held live.
        const QRectF track = trackRect();
        QLinearGradient g(track.topLeft(), track.topRight());
        for (int i = 0; i <= 12; ++i) {
            const qreal t = i / 12.0;
            g.setColorAt(t, colorAt(t));
        }
        QPainterPath path;
        path.addRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
        p.fillPath(path, g);
        p.setPen(QPen(kLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        drawIndicator(p,
                      QPointF(track.left() + value() * track.width(), track.center().y()),
                      7.0, colorAt(value()));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        m_dragging = true;
        applyDrag(e->position());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging && (e->buttons() & Qt::LeftButton))
            applyDrag(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_dragging && onCommit)
            onCommit();
        m_dragging = false;
    }

private:
    QRectF trackRect() const { return QRectF(20, height() / 2.0 - 6, width() - 28, 12); }
    qreal value() const
    {
        switch (m_channel) {
        case Hue: return m_h;
        case Saturation: return m_s;
        default: return m_v;
        }
    }
    QColor colorAt(qreal t) const
    {
        switch (m_channel) {
        case Hue: return hsv(qMin(t, 0.999), m_s, m_v);
        case Saturation: return hsv(m_h, t, m_v);
        default: return hsv(m_h, m_s, t);
        }
    }
    void applyDrag(const QPointF &pos)
    {
        if (!onChange)
            return;
        const QRectF track = trackRect();
        qreal t = qBound(0.0, (pos.x() - track.left()) / track.width(), 1.0);
        if (m_channel == Hue && t >= 1.0)
            t = 0.999;
        onChange(t);
    }

    Channel m_channel;
    qreal m_h = 0.0, m_s = 0.0, m_v = 0.0;
    bool m_dragging = false;
};

// --- Current + previous swatch pair ---------------------------------------------
class SwatchPair : public QWidget
{
public:
    std::function<void()> onPreviousPicked;

    explicit SwatchPair(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(30);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Current | Previous"));
    }
    void setColors(const QColor &current, const QColor &previous)
    {
        m_current = current;
        m_previous = previous;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Two halves clipped inside one rounded frame (a subpath union would
        // XOR itself hollow under the default odd-even fill rule).
        QPainterPath frame;
        frame.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);
        p.save();
        p.setClipPath(frame);
        p.fillRect(QRectF(0, 0, width() / 2.0, height()), m_current);
        p.fillRect(QRectF(width() / 2.0, 0, width() / 2.0, height()), m_previous);
        p.restore();
        p.setPen(QPen(kLine, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(frame);
        p.drawLine(QPointF(width() / 2.0, 1), QPointF(width() / 2.0, height() - 1));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->position().x() > width() / 2.0 && onPreviousPicked)
            onPreviousPicked();
    }

private:
    QColor m_current{Qt::black};
    QColor m_previous{Qt::black};
};

// --- Swatch strip (history + palette share this) ---------------------------------
class SwatchStrip : public QWidget
{
public:
    std::function<void(const QColor &)> onPick;
    std::function<void(int)> onRemove; // right-click (palette); null = disabled

    explicit SwatchStrip(int rows, QWidget *parent = nullptr)
        : QWidget(parent), m_rows(rows)
    {
        setFixedHeight(rows * (kCell + kGap) - kGap);
        setCursor(Qt::PointingHandCursor);
    }
    void setSwatches(const QVector<QColor> &swatches)
    {
        m_swatches = swatches;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int perRow = columns();
        const int slotCount = perRow * m_rows; // "slots" is a Qt macro keyword
        for (int i = 0; i < slotCount; ++i) {
            const QRectF r = cellRect(i);
            if (i < m_swatches.size()) {
                QPainterPath path;
                path.addRoundedRect(r, 4, 4);
                p.fillPath(path, m_swatches.at(i));
                p.setPen(QPen(kLine, 1));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            } else { // empty slot: faint outline keeps the grid readable
                p.setPen(QPen(kInset, 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            }
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        const int index = indexAt(e->position());
        if (index < 0 || index >= m_swatches.size())
            return;
        if (e->button() == Qt::RightButton) {
            if (onRemove)
                onRemove(index);
        } else if (onPick) {
            onPick(m_swatches.at(index));
        }
    }

private:
    static constexpr int kCell = 20;
    static constexpr int kGap = 4;
    int columns() const { return qMax(1, (width() + kGap) / (kCell + kGap)); }
    QRectF cellRect(int index) const
    {
        const int perRow = columns();
        return QRectF((index % perRow) * (kCell + kGap),
                      (index / perRow) * (kCell + kGap), kCell, kCell);
    }
    int indexAt(const QPointF &pos) const
    {
        const int col = int(pos.x()) / (kCell + kGap);
        const int row = int(pos.y()) / (kCell + kGap);
        if (col >= columns() || row >= m_rows)
            return -1;
        return row * columns() + col;
    }

    int m_rows;
    QVector<QColor> m_swatches;
};

} // namespace ColorPanelParts

using namespace ColorPanelParts;

namespace {
// Tiny flat text button for the section header rows ("Clear", "+ Save").
QPushButton *miniButton(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; color: #808080;"
        " font-size: 10px; padding: 0 2px; }"
        "QPushButton:hover { color: #cccccc; }"));
    return b;
}

QLabel *sectionLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(QStringLiteral(
        "color: #808080; font-size: 9px; font-weight: 600; letter-spacing: 1px;"
        " background: transparent;"));
    return l;
}
} // namespace

ColorPanel::ColorPanel(QWidget *parent) : QWidget(parent)
{
    setFixedWidth(kPickerSize + 24);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #212121;"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    m_tabs = new SegmentedTabs;
    m_tabs->setObjectName(QStringLiteral("colorTabs"));
    root->addWidget(m_tabs);

    m_wheel = new WheelPicker;
    m_wheel->setObjectName(QStringLiteral("colorWheel"));
    m_classic = new ClassicPicker;
    m_classic->setObjectName(QStringLiteral("colorClassic"));
    auto *stackHost = new QWidget;
    m_stack = new QStackedLayout(stackHost);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_wheel);
    m_stack->addWidget(m_classic);
    root->addWidget(stackHost, 0, Qt::AlignHCenter);
    m_tabs->onSelect = [this](int index) { m_stack->setCurrentIndex(index); };

    m_hSlider = new ChannelSlider(ChannelSlider::Hue);
    m_sSlider = new ChannelSlider(ChannelSlider::Saturation);
    m_bSlider = new ChannelSlider(ChannelSlider::Brightness);
    root->addWidget(m_hSlider);
    root->addWidget(m_sSlider);
    root->addWidget(m_bSlider);

    m_swatches = new SwatchPair;
    root->addWidget(m_swatches);

    auto *historyHeader = new QHBoxLayout;
    historyHeader->setContentsMargins(0, 2, 0, 0);
    historyHeader->addWidget(sectionLabel(QStringLiteral("HISTORY")));
    historyHeader->addStretch(1);
    QPushButton *clearButton = miniButton(QStringLiteral("Clear"));
    historyHeader->addWidget(clearButton);
    root->addLayout(historyHeader);
    m_historyStrip = new SwatchStrip(1);
    root->addWidget(m_historyStrip);

    auto *paletteHeader = new QHBoxLayout;
    paletteHeader->setContentsMargins(0, 2, 0, 0);
    paletteHeader->addWidget(sectionLabel(QStringLiteral("PALETTE")));
    paletteHeader->addStretch(1);
    QPushButton *saveButton = miniButton(QStringLiteral("+ Save"));
    paletteHeader->addWidget(saveButton);
    root->addLayout(paletteHeader);
    m_paletteStrip = new SwatchStrip(2);
    m_paletteStrip->setToolTip(QStringLiteral("Click: select | Right-click: remove"));
    root->addWidget(m_paletteStrip);

    // --- wiring: every control feeds the single HSV state -------------------
    const auto liveChange = [this](qreal h, qreal s, qreal v) {
        applyHsv(h, s, v, true);
    };
    const auto commit = [this] { commitColor(); };
    m_wheel->onChange = liveChange;
    m_wheel->onCommit = commit;
    m_classic->onChange = liveChange;
    m_classic->onCommit = commit;
    m_hSlider->onChange = [this](qreal t) { applyHsv(t, m_s, m_v, true); };
    m_sSlider->onChange = [this](qreal t) { applyHsv(m_h, t, m_v, true); };
    m_bSlider->onChange = [this](qreal t) { applyHsv(m_h, m_s, t, true); };
    m_hSlider->onCommit = commit;
    m_sSlider->onCommit = commit;
    m_bSlider->onCommit = commit;

    m_swatches->onPreviousPicked = [this] {
        const QColor previous = m_previous;
        applyHsv(previous.hsvHueF() < 0 ? m_h : previous.hsvHueF(),
                 previous.hsvSaturationF(), previous.valueF(), true);
        commitColor();
    };
    const auto pick = [this](const QColor &c) {
        applyHsv(c.hsvHueF() < 0 ? 0.0 : c.hsvHueF(), c.hsvSaturationF(), c.valueF(),
                 true);
        commitColor();
    };
    m_historyStrip->onPick = pick;
    m_paletteStrip->onPick = pick;
    m_paletteStrip->onRemove = [this](int index) {
        if (index < 0 || index >= m_palette.size())
            return;
        m_palette.removeAt(index);
        m_paletteStrip->setSwatches(m_palette);
        savePalette();
    };
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_history.clear();
        m_historyStrip->setSwatches(m_history);
    });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        const QColor c = color();
        if (m_palette.contains(c))
            return;
        m_palette.prepend(c);
        while (m_palette.size() > 18)
            m_palette.removeLast();
        m_paletteStrip->setSwatches(m_palette);
        savePalette();
    });

    loadPalette();
    m_previous = Qt::black;
    syncControls();
}

QColor ColorPanel::color() const
{
    return hsv(m_h, m_s, m_v);
}

void ColorPanel::setColor(const QColor &c)
{
    const qreal h = c.hsvHueF();
    applyHsv(h < 0 ? m_h : h, c.hsvSaturationF(), c.valueF(), false);
}

void ColorPanel::applyHsv(qreal h, qreal s, qreal v, bool fromUser)
{
    m_h = qBound(0.0, h, 0.999);
    m_s = qBound(0.0, s, 1.0);
    m_v = qBound(0.0, v, 1.0);
    syncControls();
    if (fromUser)
        emit colorChanged(color());
}

// A color was CHOSEN (release / swatch click): it becomes the reference for
// Previous and joins the history.
void ColorPanel::commitColor()
{
    const QColor c = color();
    if (m_history.isEmpty() || m_history.first() != c) {
        if (!m_history.isEmpty())
            m_previous = m_history.first();
        m_history.prepend(c);
        while (m_history.size() > 9)
            m_history.removeLast();
        m_historyStrip->setSwatches(m_history);
    }
    syncControls();
}

void ColorPanel::syncControls()
{
    m_wheel->setHsv(m_h, m_s, m_v);
    m_classic->setHsv(m_h, m_s, m_v);
    m_hSlider->setHsv(m_h, m_s, m_v);
    m_sSlider->setHsv(m_h, m_s, m_v);
    m_bSlider->setHsv(m_h, m_s, m_v);
    m_swatches->setColors(color(), m_previous);
}

void ColorPanel::loadPalette()
{
    const QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QStringList names =
        settings.value(QStringLiteral("colorPanel/palette")).toStringList();
    m_palette.clear();
    for (const QString &name : names) {
        const QColor c(name);
        if (c.isValid())
            m_palette.append(c);
    }
    m_paletteStrip->setSwatches(m_palette);
}

void ColorPanel::savePalette()
{
    QStringList names;
    names.reserve(m_palette.size());
    for (const QColor &c : std::as_const(m_palette))
        names.append(c.name(QColor::HexArgb));
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    settings.setValue(QStringLiteral("colorPanel/palette"), names);
}
