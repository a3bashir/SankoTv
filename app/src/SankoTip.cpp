#include "SankoTip.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QToolTip>

// --- The look ------------------------------------------------------------------------
// The Brush bar's popup as it was built (2026-07): a translucent frameless tool window,
// its rounded rectangle painted antialiased, a label with its own small sheet inside.

SankoTipFrame::SankoTipFrame(QWidget *parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_label = new QLabel(this);
    m_label->setTextFormat(Qt::RichText);
    m_label->setStyleSheet(QStringLiteral("color: #cccccc; font-size: 11px; background: transparent;"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->addWidget(m_label);
    m_fade = new QPropertyAnimation(this, "windowOpacity", this);
    m_fade->setDuration(kFadeMs);
}

void SankoTipFrame::setTipText(const QString &text, Qt::TextFormat format)
{
    m_label->setTextFormat(format);
    m_label->setWordWrap(false);
    m_label->setMinimumWidth(0);
    m_label->setMaximumWidth(QWIDGETSIZE_MAX);
    m_label->setText(text);
    if (m_label->sizeHint().width() > kMaxWidth) {
        m_label->setWordWrap(true);
        m_label->setFixedWidth(kMaxWidth);
    }
    adjustSize();
}

QString SankoTipFrame::tipText() const
{
    return m_label->text();
}

void SankoTipFrame::showFaded()
{
    m_fade->stop();
    if (isVisible()) {
        setWindowOpacity(1.0); // steady while it moves or changes its text
        update();
        return;
    }
    setWindowOpacity(0.0);
    show();
    m_fade->setStartValue(0.0);
    m_fade->setEndValue(1.0);
    m_fade->start();
}

void SankoTipFrame::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1));
    p.setBrush(QColor(0x1a, 0x1a, 0x1a));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);
}

// --- The delay, and Qt's label made transparent --------------------------------------

namespace {
bool g_bypass = false;
}

int SankoTipStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                             QStyleHintReturn *returnData) const
{
    if (!g_bypass) {
        if (hint == SH_ToolTip_WakeUpDelay)
            return SankoTipFrame::kShowDelayMs;
        if (hint == SH_ToolTipLabel_Opacity)
            return 0; // the label is there - Qt places it, times it, hides it - but unseen
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

// --- The mirror ----------------------------------------------------------------------

namespace {

bool isQtTipLabel(QObject *o)
{
    return o->isWidgetType() && static_cast<QWidget *>(o)->windowType() == Qt::ToolTip
        && o->inherits("QTipLabel");
}

class SankoTipMirror : public QObject
{
public:
    SankoTipMirror() : QObject(qApp)
    {
        m_hideTimer.setSingleShot(true);
        m_hideTimer.setInterval(SankoTipFrame::kHideDelayMs);
        // The grace over: gone now. QToolTip::hideText() would only start Qt's own 0.3 s;
        // Qt's label is hidden here (its next tooltip makes a new one).
        connect(&m_hideTimer, &QTimer::timeout, this, [this] {
            if ((m_owner || m_hideAsked) && m_label && m_label->isVisible()) {
                QWidget *label = m_label;
                hideFrame();
                label->hide();
            }
            m_hideAsked = false;
        });
    }

    void hideSoon()
    {
        if (m_label && m_label->isVisible()) {
            m_hideAsked = true;
            m_hideTimer.start();
        }
    }

    SankoTipFrame *frame()
    {
        if (!m_frame) {
            m_frame = new SankoTipFrame; // parentless: no page's style sheet reaches it
            m_frame->setObjectName(QStringLiteral("sankoTip"));
        }
        return m_frame;
    }
    QWidget *label() const { return m_label; }

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void follow(QWidget *label);
    void sync(bool mayShow);
    void hideFrame()
    {
        m_hideTimer.stop();
        m_hideAsked = false;
        m_owner = nullptr;
        if (m_frame)
            m_frame->hide();
    }

    QPointer<SankoTipFrame> m_frame;
    QPointer<QWidget> m_label;     // Qt's label being mirrored
    QObject *m_labelId = nullptr;  // ...the same, for its destroyed() (the pointer is null by then)
    QPointer<QWidget> m_owner;     // the hovered widget the tip came from; none for a note
    QPointer<QWidget> m_asking;    // a widget whose ToolTip event is being answered
    QString m_textBefore;          // ...and what the tip said before it
    bool m_upBefore = false;
    bool m_hideAsked = false;      // sankoHideTipSoon(): the grace for any tip
    QTimer m_hideTimer;
};

SankoTipMirror *g_mirror = nullptr;

bool SankoTipMirror::eventFilter(QObject *o, QEvent *e)
{
    if (g_bypass)
        return false;
    switch (e->type()) {
    case QEvent::ToolTip:
        // A hover asking for its tooltip. Whether the tip that follows is this widget's is
        // known once Qt has answered (the next pass of the event loop): it is if the tip
        // came up, or changed, meanwhile.
        if (o->isWidgetType()) {
            m_asking = static_cast<QWidget *>(o);
            m_upBefore = QToolTip::isVisible();
            m_textBefore = QToolTip::text();
            QTimer::singleShot(0, this, [this, asking = QPointer<QWidget>(m_asking)] {
                if (!asking || asking != m_asking)
                    return;
                m_asking = nullptr;
                if (QToolTip::isVisible() && (!m_upBefore || QToolTip::text() != m_textBefore)) {
                    m_owner = asking;
                    m_hideTimer.stop();
                }
            });
        }
        return false;
    case QEvent::Leave:
        if (m_owner && o == m_owner.data())
            m_hideTimer.start(); // the Brush bar's grace; the next hover's tip cancels it
        return false;
    case QEvent::Polish:
        if (isQtTipLabel(o)) {
            // Made, not yet shown: QToolTip looks at the effects right after this. A roll
            // or a fade would draw the label's own pixels in a window of its own.
            QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
            QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
        }
        return false;
    case QEvent::Show:
        if (isQtTipLabel(o)) {
            auto *w = static_cast<QWidget *>(o);
            // The style made it transparent as it was made; this catches a page's style sheet
            // that would set a tooltip's opacity (the red run took both away to see a check
            // fail: either alone keeps the label unseen).
            if (w->windowOpacity() > 0.0)
                w->setWindowOpacity(0.0); // before it reaches the screen
            follow(w);
            sync(true);
        }
        return false;
    case QEvent::Move:
    case QEvent::Resize:
        if (isQtTipLabel(o) && o == m_label.data() && m_label->isVisible())
            sync(true);
        return false;
    case QEvent::Paint:
        // A new text in a label already up and of the same size: no Move, no Resize.
        if (isQtTipLabel(o) && o == m_label.data())
            sync(false);
        return false;
    case QEvent::Hide:
        if (isQtTipLabel(o) && o == m_label.data())
            hideFrame();
        return false;
    default:
        return false;
    }
}

void SankoTipMirror::follow(QWidget *label)
{
    if (label == m_labelId)
        return;
    m_label = label;
    m_labelId = label;
    connect(label, &QObject::destroyed, this, [this, label] {
        if (m_labelId == label) {
            m_labelId = nullptr;
            hideFrame();
        }
    });
}

void SankoTipMirror::sync(bool mayShow)
{
    auto *label = qobject_cast<QLabel *>(m_label.data());
    if (!label)
        return;
    SankoTipFrame *f = frame();
    const bool up = f->isVisible();
    if (!mayShow && !up)
        return;
    bool changed = false;
    if (!up || f->tipText() != label->text()) {
        // A new text that no hover asked for is a note: Qt's timing, not a hover's.
        if (!m_asking)
            m_owner = nullptr;
        m_hideTimer.stop();
        m_hideAsked = false;
        f->setTipText(label->text(), label->textFormat());
        changed = true;
    }
    // Where Qt put its label, kept on the screen at the frame's own size.
    QPoint at = label->pos();
    if (const QScreen *screen = label->screen()) {
        const QRect avail = screen->availableGeometry();
        at.setX(qBound(avail.left(), at.x(), qMax(avail.left(), avail.right() + 1 - f->width())));
        at.setY(qBound(avail.top(), at.y(), qMax(avail.top(), avail.bottom() + 1 - f->height())));
    }
    if (f->pos() != at) {
        f->move(at);
        changed = true;
    }
    // Shown fading in; once up, a new text or place is taken at once and opaque, as the
    // Brush bar's tip retargets - and an event that changes nothing leaves it alone.
    if (!up || changed)
        f->showFaded();
}

} // namespace

void installSankoTips()
{
    if (g_mirror || !qobject_cast<QApplication *>(QCoreApplication::instance()))
        return;
    QApplication::setStyle(new SankoTipStyle);
    g_mirror = new SankoTipMirror;
    qApp->installEventFilter(g_mirror);
}

void sankoHideTipSoon()
{
    if (g_mirror && !g_bypass)
        g_mirror->hideSoon();
    else
        QToolTip::hideText();
}

SankoTipFrame *sankoTipFrameForTest()
{
    return g_mirror ? g_mirror->frame() : nullptr;
}

QWidget *sankoQtTipLabelForTest()
{
    return g_mirror ? g_mirror->label() : nullptr;
}

void setSankoTipsBypassedForTest(bool bypass)
{
    g_bypass = bypass;
    if (bypass && g_mirror && sankoTipFrameForTest())
        sankoTipFrameForTest()->hide();
}
