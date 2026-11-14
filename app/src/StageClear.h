#pragma once

// CLEAR THE STAGE WHILE THE FILM PLAYS (2026-11-02; the user's approval of
// Figma PLAYBACK-2, Option A, Documents\SankoTV-Design\proposals-2026-11-02.md).
//
// The preview is a widget over the drawing canvas, but every bar that floats
// over the canvas is a top-level tool window (FloatingToolWindow), so the
// preview cannot cover it: in the 2E hand test five of them sat on the
// picture while it played. On Play every canvas-anchored bar and the
// preview's own caption chip FADE OUT together (150 ms) and stay out while
// the film runs. A pointer move over the stage - the preview or a bar -
// brings them back (120 ms); 1.5 s after the last move they fade again,
// unless the pointer rests on a bar. Pause, Stop, Esc and leaving the
// preview bring everything back at once, no fade: the artist is going to
// draw. Nothing moves or resizes: the bars keep their places and the
// preview its size; the fade is window opacity, so a bar that is coming
// back is usable as soon as it can be seen.
//
// THE USER'S RULE (batch 1): while a bar is faded, a click on it never
// presses a control - the first click, like the first move, only brings
// the bars back. The application event filter below consumes a press on a
// faded bar and restores the stage instead.
//
// The setting "Clear the stage while the film plays" (View menu and
// Preferences > Playback; storyboard/clearStageOnPlay, default on); off is
// exactly the behaviour before this.
//
// Nothing here depends on the wall clock in the gate: the fades take a
// duration the checks set to 0 and the idle timer is fired through a seam.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <functional>

class QVariantAnimation;
class QWidget;
class FloatingToolWindow;

class StageClear : public QObject
{
    Q_OBJECT

public:
    static constexpr int kFadeOutMs = 150;
    static constexpr int kFadeInMs = 120;
    static constexpr int kIdleMs = 1500;

    explicit StageClear(QWidget *canvas, QObject *parent = nullptr);
    ~StageClear() override;

    void setEnabled(bool on); // the setting
    bool isEnabled() const { return m_enabled; }

    // The film's state, from the animatic page.
    void setPlaying(bool playing);
    void setPreviewShown(bool shown);
    // A pointer move over the stage: the preview (onBar false) or a bar.
    void pointerActivity(bool onBar);

    bool cleared() const { return m_cleared; }
    QVector<QWidget *> fadedBars() const;

    // The preview's own chrome (its caption chip) follows the bars.
    std::function<void(bool shown)> onChromeShown;

    // The gate's seams: no fade (opacity set at once) and the idle timer
    // fired by hand.
    void setFadeMsForTest(int ms) { m_fadeMs = ms; }
    void fireIdleForTest() { onIdle(); }
    bool idleArmedForTest() const { return m_idle.isActive(); }
    // The gate's pointer (StoryboardPage::setRealPointerIgnoredForTest): a REAL pointer
    // move over a bar - the machine's own pointer resting where a bar shows - wakes
    // nothing; the moves a check sends itself still do. Presses are not touched.
    void setRealPointerIgnoredForTest(bool ignored) { m_realPointerIgnoredForTest = ignored; }

signals:
    void clearedChanged(bool cleared);
    // The stage is PLAYING (enabled, the film running, the preview shown)
    // or not: what the compact bar and the strip follow (StoryboardPage),
    // independent of the bars' wake-and-fade cycle.
    void stageActiveChanged(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool stageActive() const { return m_enabled && m_playing && m_preview; }
    void clear();
    void restore(bool animate);
    void onIdle();
    void armIdle();
    FloatingToolWindow *barOf(QObject *object) const;
    void fadeTo(FloatingToolWindow *bar, qreal opacity, bool animate);

    QPointer<QWidget> m_canvas;
    bool m_enabled = true;
    bool m_playing = false;
    bool m_preview = false;
    bool m_cleared = false;
    bool m_pointerOnBar = false;
    int m_fadeMs = -1; // -1: the constants above; >= 0: the gate's duration
    bool m_realPointerIgnoredForTest = false; // off outside the tests
    quint64 m_realPointerStamp = 0;           // the gate: the last real pointer event's time
    QTimer m_idle;
    QVector<QPointer<FloatingToolWindow>> m_faded;
    QHash<FloatingToolWindow *, QPointer<QVariantAnimation>> m_fades;
};
