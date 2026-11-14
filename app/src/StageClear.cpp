#include "StageClear.h"

#include "FloatingToolWindow.h"

#include <QApplication>
#include <QEvent>
#include <QInputEvent>
#include <QVariantAnimation>
#include <QWidget>

StageClear::StageClear(QWidget *canvas, QObject *parent)
    : QObject(parent), m_canvas(canvas)
{
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, &StageClear::onIdle);
    // One filter for every bar: a bar's children get the presses, and the
    // bars are top-level windows, so only the application sees them all.
    qApp->installEventFilter(this);
}

StageClear::~StageClear()
{
    if (qApp)
        qApp->removeEventFilter(this);
    restore(false);
}

void StageClear::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    const bool was = stageActive();
    m_enabled = on;
    if (!on) {
        m_idle.stop();
        restore(false);
    } else if (stageActive()) {
        clear();
    }
    if (was != stageActive())
        emit stageActiveChanged(stageActive());
}

void StageClear::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    const bool was = stageActive();
    m_playing = playing;
    if (was != stageActive())
        emit stageActiveChanged(stageActive()); // the layout first (the compact bar, the strip)
    if (stageActive())
        clear(); // Play: the stage clears at once (the fade)
    else {
        m_idle.stop();
        restore(false); // Pause / Stop: everything back, no fade
    }
}

void StageClear::setPreviewShown(bool shown)
{
    if (m_preview == shown)
        return;
    const bool was = stageActive();
    m_preview = shown;
    if (was != stageActive())
        emit stageActiveChanged(stageActive());
    if (stageActive())
        clear();
    else {
        m_idle.stop();
        restore(false);
    }
}

void StageClear::pointerActivity(bool onBar)
{
    m_pointerOnBar = onBar;
    if (!stageActive())
        return;
    if (m_cleared)
        restore(true);
    armIdle();
}

void StageClear::armIdle()
{
    if (stageActive())
        m_idle.start();
}

void StageClear::onIdle()
{
    if (!stageActive() || m_cleared)
        return;
    if (m_pointerOnBar) { // the bar under the pointer never fades: wait
        armIdle();
        return;
    }
    clear();
}

QVector<QWidget *> StageClear::fadedBars() const
{
    QVector<QWidget *> list;
    for (const QPointer<FloatingToolWindow> &w : m_faded)
        if (w)
            list.append(w);
    return list;
}

void StageClear::clear()
{
    if (m_cleared)
        return;
    m_cleared = true;
    m_faded.clear();
    // Every bar over the canvas that is effectively visible now; a modal
    // surface (the Brush Settings studio) is not a bar.
    for (FloatingToolWindow *w : FloatingToolWindow::windowsOver(m_canvas)) {
        if (!w->isVisible() || w->isModalSurface())
            continue;
        m_faded.append(w);
        fadeTo(w, 0.0, true);
    }
    if (onChromeShown)
        onChromeShown(false);
    emit clearedChanged(true);
}

void StageClear::restore(bool animate)
{
    if (!m_cleared)
        return;
    m_cleared = false;
    for (const QPointer<FloatingToolWindow> &w : m_faded)
        if (w)
            fadeTo(w, 1.0, animate);
    m_faded.clear();
    if (onChromeShown)
        onChromeShown(true);
    emit clearedChanged(false);
}

void StageClear::fadeTo(FloatingToolWindow *bar, qreal opacity, bool animate)
{
    if (QVariantAnimation *running = m_fades.take(bar))
        running->stop(); // DeleteWhenStopped
    const int ms = m_fadeMs >= 0 ? m_fadeMs : (opacity < 0.5 ? kFadeOutMs : kFadeInMs);
    if (!animate || ms == 0) {
        bar->setWindowOpacity(opacity);
        return;
    }
    auto *fade = new QVariantAnimation(this);
    fade->setStartValue(bar->windowOpacity());
    fade->setEndValue(opacity);
    fade->setDuration(ms);
    QPointer<FloatingToolWindow> target(bar);
    connect(fade, &QVariantAnimation::valueChanged, this, [target](const QVariant &v) {
        if (target)
            target->setWindowOpacity(v.toReal());
    });
    m_fades.insert(bar, fade);
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

FloatingToolWindow *StageClear::barOf(QObject *object) const
{
    QWidget *w = qobject_cast<QWidget *>(object);
    if (!w || !m_canvas)
        return nullptr;
    auto *bar = qobject_cast<FloatingToolWindow *>(w->window());
    if (!bar || bar->anchorWidget() != m_canvas || bar->isModalSurface())
        return nullptr;
    return bar;
}

bool StageClear::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_cleared && !stageActive())
        return false;
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::HoverMove:
    case QEvent::TabletMove:
    case QEvent::Enter:
    case QEvent::HoverEnter:
        // The gate: the machine's own pointer is not the test's. Its moves come from the window
        // system (spontaneous); when a child does not take one, Qt passes a COPY up to the
        // parent with the same timestamp but not marked so (the Brush button's move reached the
        // bar that way, (ps8)'s first run); and the Enter and Hover events Qt derives are not
        // marked either. The checks send moves of their own, timestamp 0.
        if (m_realPointerIgnoredForTest) {
            const auto *input = static_cast<const QInputEvent *>(event);
            if (event->spontaneous()) {
                m_realPointerStamp = input->timestamp();
                break;
            }
            if ((m_realPointerStamp != 0 && input->timestamp() == m_realPointerStamp)
                || event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter
                || event->type() == QEvent::HoverMove)
                break;
        }
        if (barOf(watched))
            pointerActivity(true);
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        if (m_realPointerIgnoredForTest && event->spontaneous())
            break;
        // The bar itself is left (a child handing over to a sibling is
        // not): the pointer is off the bars, the idle count starts.
        if (FloatingToolWindow *bar = barOf(watched))
            if (bar == watched)
                pointerActivity(false);
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
        // THE USER'S RULE: a faded bar's control is never pressed; the
        // press wakes the stage and ends here.
        if (m_cleared && barOf(watched)) {
            pointerActivity(true);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}
