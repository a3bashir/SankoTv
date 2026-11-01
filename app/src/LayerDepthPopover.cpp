#include "LayerDepthPopover.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWidget>

#include <cmath>

namespace layerdepth {

namespace {
// The named stops (Figma 2B-2): their %, their label, and whether the label
// sits above the track (alternating, so neighbours never collide).
// The named stops' % (Figma 2B-2; the slider paints their labels).
constexpr int kStops[] = {0, 10, 50, 100, 150};

// "half", "one and a half times", "twice", else "N %".
QString howFar(int percent)
{
    if (percent == 50)
        return QStringLiteral("half");
    if (percent == 150)
        return QStringLiteral("one and a half times");
    if (percent == 200)
        return QStringLiteral("twice");
    return QStringLiteral("%1 %").arg(percent);
}

int percentOf(double parallax)
{
    return int(std::lround(parallax * 100.0));
}
} // namespace

QString percentText(double parallax)
{
    return QStringLiteral("%1 %").arg(percentOf(parallax));
}

QString badgeTooltip(double parallax)
{
    const int p = percentOf(parallax);
    const QString head = QStringLiteral("Depth %1 ").arg(percentText(parallax)) + QChar(0x00B7)
        + QLatin1Char(' ');
    if (p == 0)
        return head + QStringLiteral("fixed to the camera (the sky)");
    if (p < 100)
        return head + QStringLiteral("moves %1 as far as the drawing when the camera pans")
                          .arg(howFar(p));
    if (p == 100)
        return head + QStringLiteral("on the picture plane");
    return head + QStringLiteral("moves %1 as far (near)").arg(howFar(p));
}

QString readout(double parallax)
{
    const int p = percentOf(parallax);
    if (p == 0)
        return QStringLiteral("Fixed to the camera: it does not move or grow when the camera "
                              "does - a sky, a far horizon.");
    if (p == 100)
        return QStringLiteral("On the picture plane: moves with the drawing, exactly as in "
                              "Phase 1. No badge.");
    const QString far = howFar(p);
    if (p < 100)
        return QStringLiteral("Moves %1 as far as the drawing when the camera pans, and grows "
                              "less on a push-in: it reads as far away.").arg(far);
    return QStringLiteral("Moves %1 as far as the drawing, and grows more on a push-in: it "
                          "reads as close to the camera.").arg(far);
}

QString memberTooltip(const QString &folderName)
{
    return QStringLiteral("Set by folder ") + QChar(0x2018) + folderName + QChar(0x2019);
}

QString paperTooltip()
{
    return QStringLiteral("The paper stays on the picture plane");
}

int snapPercent(int percent)
{
    for (int s : kStops)
        if (qAbs(percent - s) <= 3)
            return s;
    return qBound(0, percent, 200);
}

} // namespace layerdepth

// THE SLIDER (Figma 2B-2): a 260 px track from x 20 for 0 - 200 %, the stop
// ticks and their labels, the knob. Its own widget so the pointer and the
// test hooks take one path.
class LayerDepthSlider : public QWidget
{
public:
    explicit LayerDepthSlider(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(300, 70);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void(int)> onMoved;    // snapped %
    std::function<void(int)> onReleased; // snapped %

    int value() const { return m_value; }
    void setValue(int percent)
    {
        m_value = qBound(0, percent, 200);
        update();
    }
    QString activeStop() const
    {
        for (const auto &s : kStopsView())
            if (s.first == m_value)
                return s.second;
        return QString();
    }
    // The pointer's path, for the test hooks too.
    void moveTo(int percent)
    {
        const int v = layerdepth::snapPercent(percent);
        if (v != m_value) {
            m_value = v;
            update();
            if (onMoved)
                onMoved(v);
        }
    }
    void release()
    {
        if (onReleased)
            onReleased(m_value);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x33, 0x33, 0x33));
        p.drawRoundedRect(QRectF(kLeft, kTrackY, kWidth, 4), 2, 2);
        p.setBrush(QColor(0x7c, 0x6e, 0xf6));
        p.drawRoundedRect(QRectF(kLeft, kTrackY, xOf(m_value) - kLeft, 4), 2, 2);
        QFont f(QStringLiteral("Inter"));
        f.setPixelSize(9);
        p.setFont(f);
        const QFontMetricsF fm(f);
        for (const auto &s : kStopsView()) {
            const double x = xOf(s.first);
            p.setPen(Qt::NoPen);
            p.setBrush(s.first == 100 ? QColor(0xb3, 0xb3, 0xb3) : QColor(0x5a, 0x5a, 0x5a));
            p.drawRect(QRectF(std::round(x), kTrackY - 5, 1, 14));
            const double w = fm.horizontalAdvance(s.second);
            const bool above = s.first == 10 || s.first == 100;
            p.setPen(s.first == m_value ? QColor(0xd9, 0xd4, 0xff) : QColor(0x8a, 0x8a, 0x8a));
            p.drawText(QRectF(x - w / 2.0, above ? 8 : 42, w + 1, 11),
                       Qt::AlignCenter, s.second);
        }
        QFont small(QStringLiteral("Inter"));
        small.setPixelSize(8);
        p.setFont(small);
        p.setPen(QColor(0x5a, 0x5a, 0x5a));
        p.drawText(QRectF(18, 58, 10, 10), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("0"));
        p.drawText(QRectF(250, 58, 32, 10), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("200 %"));
        p.setPen(QPen(QColor(0x7c, 0x6e, 0xf6), 1.0));
        p.setBrush(QColor(0xe5, 0xe5, 0xed));
        p.drawEllipse(QPointF(xOf(m_value), kTrackY + 2), 7, 7);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = true;
        moveTo(percentAt(e->position().x()));
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging)
            moveTo(percentAt(e->position().x()));
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || !m_dragging)
            return;
        m_dragging = false;
        release();
    }

private:
    static constexpr double kLeft = 20.0, kWidth = 260.0, kTrackY = 28.0;
    static double xOf(int percent) { return kLeft + kWidth * percent / 200.0; }
    static int percentAt(double x) { return int(std::lround((x - kLeft) / kWidth * 200.0)); }
    static QVector<QPair<int, QString>> kStopsView()
    {
        return {{0, QStringLiteral("Fixed")},
                {10, QStringLiteral("Sky")},
                {50, QStringLiteral("Far")},
                {100, QStringLiteral("Picture plane")},
                {150, QStringLiteral("Near")}};
    }

    int m_value = 100;
    bool m_dragging = false;
};

LayerDepthPopover::LayerDepthPopover(const QString &layerName, double parallax, QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("layerDepthPopover"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFixedSize(300, 200);
    setStyleSheet(QStringLiteral(
        "QFrame#layerDepthPopover { background: #1e1e1e; border: 1px solid #2a2a2a;"
        " border-radius: 8px; }"
        "QLabel { background: transparent; border: none; font-family: 'Inter'; }"));

    auto *title = new QLabel(QStringLiteral("Depth"), this);
    title->setStyleSheet(QStringLiteral("color: #e5e5ed; font-size: 12px; font-weight: 600;"));
    title->setGeometry(14, 10, 44, 18);
    auto *name = new QLabel(QChar(0x00B7) + QStringLiteral("  ") + layerName, this);
    name->setStyleSheet(QStringLiteral("color: #999999; font-size: 11px;"));
    name->setGeometry(58, 11, 158, 16);

    m_field = new QLineEdit(this);
    m_field->setObjectName(QStringLiteral("layerDepthField"));
    m_field->setGeometry(222, 9, 64, 24);
    m_field->setAlignment(Qt::AlignCenter);
    m_field->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #1c1c1c; border: 1px solid #333333; border-radius: 3px;"
        " color: #e5e5ed; font-family: 'Inter'; font-size: 11px; font-weight: 500;"
        // Selected text in the accent, never the system highlight (red on
        // the user's PC: the 2B hand test).
        " selection-background-color: #7c6ef6; selection-color: #ffffff; }"
        "QLineEdit:focus { border: 1px solid #7c6ef6; }"));
    connect(m_field, &QLineEdit::returnPressed, this, &LayerDepthPopover::applyField);

    m_slider = new LayerDepthSlider(this);
    m_slider->move(0, 50);
    m_slider->onMoved = [this](int v) {
        showValue(v);
        if (onLive)
            onLive(v / 100.0);
    };
    m_slider->onReleased = [this](int v) {
        if (onCommit)
            onCommit(v / 100.0);
    };

    auto *rule = new QFrame(this);
    rule->setGeometry(12, 128, 276, 1);
    rule->setStyleSheet(QStringLiteral("background: #2a2a2a; border: none;"));

    m_readout = new QLabel(this);
    m_readout->setObjectName(QStringLiteral("layerDepthReadout"));
    m_readout->setWordWrap(true);
    m_readout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_readout->setStyleSheet(QStringLiteral("color: #b3b3b3; font-size: 10px;"));
    m_readout->setGeometry(14, 136, 272, 36);

    auto *foot = new QLabel(QStringLiteral("One undo step when you let go."), this);
    foot->setStyleSheet(QStringLiteral("color: #6f6f80; font-size: 9px;"));
    foot->setGeometry(14, 176, 272, 14);

    const int percent = qBound(0, int(std::lround(parallax * 100.0)), 200);
    m_slider->setValue(percent);
    showValue(percent);
}

void LayerDepthPopover::popupBelow(const QPoint &anchor)
{
    QPoint at = anchor;
    if (QScreen *screen = QGuiApplication::screenAt(anchor)) {
        const QRect avail = screen->availableGeometry();
        at.setX(qBound(avail.left(), at.x(), avail.right() - width()));
        if (at.y() + height() > avail.bottom())
            at.setY(anchor.y() - height() - 40); // above the row instead
    }
    move(at);
    show();
    m_field->setFocus();
    m_field->selectAll();
}

void LayerDepthPopover::closeEvent(QCloseEvent *event)
{
    if (onClosed)
        onClosed();
    QFrame::closeEvent(event);
}

void LayerDepthPopover::showValue(int percent)
{
    m_field->setText(layerdepth::percentText(percent / 100.0));
    m_readout->setText(layerdepth::readout(percent / 100.0));
}

void LayerDepthPopover::applyField()
{
    QString t = m_field->text();
    t.remove(QLatin1Char('%'));
    bool ok = false;
    const int v = t.trimmed().toInt(&ok);
    if (!ok || v < 0 || v > 200) {
        showValue(m_slider->value()); // not a depth: the field shows the value again
        return;
    }
    m_slider->setValue(v); // typed values are exact, never snapped
    showValue(v);
    if (onLive)
        onLive(v / 100.0);
    if (onCommit)
        onCommit(v / 100.0);
}

int LayerDepthPopover::percentForTest() const
{
    return m_slider->value();
}

QString LayerDepthPopover::fieldTextForTest() const
{
    return m_field->text();
}

QString LayerDepthPopover::readoutForTest() const
{
    return m_readout->text();
}

QString LayerDepthPopover::activeStopForTest() const
{
    return m_slider->activeStop();
}

QColor LayerDepthPopover::fieldSelectionForTest() const
{
    m_field->ensurePolished();
    return m_field->palette().color(QPalette::Active, QPalette::Highlight);
}

void LayerDepthPopover::slideForTest(int percent, bool release)
{
    m_slider->moveTo(percent);
    if (release)
        m_slider->release();
}

void LayerDepthPopover::typeForTest(const QString &text)
{
    m_field->setText(text);
    applyField();
}
