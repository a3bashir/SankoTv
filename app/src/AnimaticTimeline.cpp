#include "AnimaticTimeline.h"
#include "SankoTheme.h"

#include "AnimaticPage.h"
#include "StoryboardModel.h"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <cmath>

namespace {

constexpr int kLabelCol = 90; // left track-name column width

// Track geometry (top to bottom), all within the canvas. The timeline sits
// under the drawing canvas now and its height is the user's to drag, so the
// SHOTS track is not a constant any more: the ruler, the scene track and the
// audio track keep their heights and the shots track takes whatever is left
// (panelTrackH), never less than kPanelMinH. The two reserved tracks that
// used to follow - Camera and Markers, 24 px each, both "coming soon" - are
// gone: 48 px of the workspace for two labels.
constexpr int kRulerY = 0,   kRulerH = 24;
constexpr int kSceneY = 24,  kSceneH = 24;
constexpr int kPanelY = 48;
constexpr int kPanelMinH = 48, kPanelDefaultH = 80;
constexpr int kAudioH = 36;

constexpr int kHandleW = 6;   // trim handle width
constexpr int kGrab = 5;      // px grab tolerance
constexpr int kMinDur = 1, kMaxDur = 30;
constexpr int kReorderThreshold = 8; // px before a clip press becomes a drag

constexpr int kToolbarH = 32;
} // namespace

// =====================================================================
// TimelineCanvas — the painted surface. Forwards everything to its owner.
// =====================================================================
class TimelineCanvas : public QWidget
{
public:
    explicit TimelineCanvas(AnimaticTimeline *owner)
        : QWidget(owner), m_owner(owner)
    {
        setMinimumHeight(kPanelY + kPanelMinH + kAudioH);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    QSize sizeHint() const override
    {
        return QSize(400, kPanelY + kPanelDefaultH + kAudioH);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        m_owner->renderCanvas(p);
    }
    void mousePressEvent(QMouseEvent *e) override { m_owner->canvasMousePress(e); }
    void mouseMoveEvent(QMouseEvent *e) override { m_owner->canvasMouseMove(e); }
    void mouseReleaseEvent(QMouseEvent *e) override { m_owner->canvasMouseRelease(e); }
    void leaveEvent(QEvent *) override { m_owner->canvasLeave(); }
    void resizeEvent(QResizeEvent *) override { m_owner->canvasResized(); }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        m_owner->canvasContextMenu(e);
    }

private:
    AnimaticTimeline *m_owner;
};

// =====================================================================
// AnimaticTimeline
// =====================================================================

AnimaticTimeline::AnimaticTimeline(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0d0d0d;"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- Zoom toolbar -----------------------------------------------------
    QWidget *toolbar = new QWidget;
    toolbar->setFixedHeight(kToolbarH);
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    toolbar->setStyleSheet(QStringLiteral(
        "background-color: #0d0d0d; border-bottom: 1px solid #1f1f1f;"));
    QHBoxLayout *tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(10, 0, 10, 0);
    tb->setSpacing(6);

    m_fitButton = new QPushButton(QStringLiteral("Fit"));
    m_zoomOutButton = new QPushButton(QStringLiteral("\xE2\x88\x92")); // minus
    m_zoomInButton = new QPushButton(QStringLiteral("+"));
    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setRange(10, 400);
    m_zoomSlider->setValue(100);
    m_zoomSlider->setFixedWidth(140);
    m_zoomSlider->setToolTip(QStringLiteral("Zoom"));

    m_framesButton = new QPushButton(QStringLiteral("Frames"));
    m_secondsButton = new QPushButton(QStringLiteral("Seconds"));
    m_snapButton = new QPushButton(QStringLiteral("Snap"));
    for (QPushButton *b : {m_fitButton, m_zoomOutButton, m_zoomInButton,
                           m_framesButton, m_secondsButton, m_snapButton})
        b->setCursor(Qt::PointingHandCursor);

    tb->addWidget(m_fitButton);
    tb->addWidget(m_zoomOutButton);
    tb->addWidget(m_zoomSlider);
    tb->addWidget(m_zoomInButton);
    tb->addStretch(1);
    tb->addWidget(m_framesButton);
    tb->addWidget(m_secondsButton);
    tb->addSpacing(8);
    tb->addWidget(m_snapButton);
    root->addWidget(toolbar);

    // --- Canvas + horizontal scrollbar -----------------------------------
    m_canvas = new TimelineCanvas(this);
    root->addWidget(m_canvas, 1); // the shots track takes the spare height

    m_hScroll = new QScrollBar(Qt::Horizontal);
    // Styled by the app-wide sheet (SankoScrollBarStyle.h) — this bar's
    // private copy of the style was the second of the two hand-styled
    // scrollbars and is retired with it. The track is transparent now
    // (was #0d0d0d) over the timeline's own dark fill, and the bar is the
    // shared 10 px instead of 12.
    root->addWidget(m_hScroll);

    styleToolbarButtons();

    connect(m_zoomSlider, &QSlider::valueChanged, this,
            [this](int v) { applyZoom(static_cast<float>(v)); });
    connect(m_zoomInButton, &QPushButton::clicked, this,
            [this] { m_zoomSlider->setValue(m_zoomSlider->value() + 25); });
    connect(m_zoomOutButton, &QPushButton::clicked, this,
            [this] { m_zoomSlider->setValue(m_zoomSlider->value() - 25); });
    connect(m_fitButton, &QPushButton::clicked, this, [this] { fitZoom(); });
    connect(m_framesButton, &QPushButton::clicked, this, [this] {
        m_framesMode = true;
        styleToolbarButtons();
        m_canvas->update();
    });
    connect(m_secondsButton, &QPushButton::clicked, this, [this] {
        m_framesMode = false;
        styleToolbarButtons();
        m_canvas->update();
    });
    connect(m_snapButton, &QPushButton::clicked, this, [this] {
        m_snap = !m_snap;
        styleToolbarButtons();
    });
    // valueChanged is WHERE the view is, whoever moved it; whether the user
    // moved it is decided from the bar's actions (onScrollbarAction).
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_scrollX = v;
        m_canvas->update();
    });
    connect(m_hScroll, &QScrollBar::actionTriggered, this,
            &AnimaticTimeline::onScrollbarAction);
    m_hScroll->installEventFilter(this); // to see the wheel arrive

    // The follow timer (followTick): 60 Hz, precise, only while playing.
    m_followTimer = new QTimer(this);
    m_followTimer->setInterval(16);
    m_followTimer->setTimerType(Qt::PreciseTimer);
    connect(m_followTimer, &QTimer::timeout, this, &AnimaticTimeline::followTick);
}

void AnimaticTimeline::setHost(AnimaticPage *host)
{
    m_host = host;
}

// --- Track geometry -------------------------------------------------------

int AnimaticTimeline::tracksH() const
{
    return qMax(kPanelY + kPanelMinH + kAudioH,
                m_canvas ? m_canvas->height() : 0);
}

int AnimaticTimeline::panelTrackH() const
{
    return tracksH() - kPanelY - kAudioH;
}

int AnimaticTimeline::audioTrackY() const
{
    return kPanelY + panelTrackH();
}

QRect AnimaticTimeline::clipRect(int flatIndex) const
{
    if (flatIndex < 0 || flatIndex >= m_blocks.size())
        return QRect();
    const Block &b = m_blocks.at(flatIndex);
    const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
    const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
    return QRect(x0, kPanelY + 2, x1 - x0, panelTrackH() - 4);
}

QWidget *AnimaticTimeline::surfaceForTest() const
{
    return m_canvas;
}

void AnimaticTimeline::styleToolbarButtons()
{
    const QString plain = SankoTheme::themed("QPushButton { background: #1a1a1a; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 3px 9px; font-size: 11px; }"
        "QPushButton:hover { border-color: %ACCENT%; color: %ACCENT%; }");
    const QString active = SankoTheme::themed("QPushButton { background: %ACCENT%; color: #0a0a0a; border: 1px solid %ACCENT%;"
        " border-radius: 4px; padding: 3px 9px; font-size: 11px; font-weight: 600; }");

    m_fitButton->setStyleSheet(plain);
    m_zoomInButton->setStyleSheet(plain);
    m_zoomOutButton->setStyleSheet(plain);
    m_framesButton->setStyleSheet(m_framesMode ? active : plain);
    m_secondsButton->setStyleSheet(m_framesMode ? plain : active);
    m_snapButton->setStyleSheet(m_snap ? active : plain);
}

// --- Public slots ---------------------------------------------------------

void AnimaticTimeline::setScenes(const QVector<Scene *> &scenes)
{
    m_scenes = scenes;
    int count = 0;
    for (Scene *s : m_scenes)
        count += s->panels.size();
    if (m_current >= count)
        m_current = count - 1;
    if (m_selected >= count)
        m_selected = count - 1;
    // A reorder drag armed against the old block list means nothing now.
    if (m_drag == Drag::Move) {
        m_drag = Drag::None;
        m_moveActive = false;
        m_moveFrom = -1;
        m_moveGap = -1;
    }
    rebuildBlocks();
    updateScrollRange();
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setCurrentPanel(int flatIndex)
{
    if (m_current == flatIndex)
        return;
    // Playback stepping to the next panel is not a jump: the view is left
    // to following (or to the user, if they hold it). Anything else - a
    // selection, a scrub, Home / End, the loop restarting, the playhead
    // going home at the end - is a jump, which ends a hold and moves the
    // view only if the playhead would be off-screen.
    const bool stepping = m_playing && flatIndex == m_current + 1;
    m_current = flatIndex;
    m_liveElapsedMs = -1; // a new panel: the host's own value until the next tick
    if (!stepping && m_drag != Drag::Playhead) {
        m_viewHeld = false;
        jumpIfOffScreen();
    }
    if (m_canvas)
        m_canvas->update();
}

// STATE IN, NOTHING OUT. The workspace owns which panel is selected and
// pushes it here; this slot repaints and emits nothing, which is what keeps
// "select in the strip -> highlight in the timeline" from bouncing back as
// "select in the timeline -> select in the strip". The only signals this
// class emits come from the user's own mouse (see the canvas handlers).
void AnimaticTimeline::setSelectedPanel(int flatIndex)
{
    if (m_selected == flatIndex)
        return;
    m_selected = flatIndex;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::refreshThumbnails()
{
    if (m_canvas)
        m_canvas->update(); // clip thumbs re-read the panels' shared mip
}

void AnimaticTimeline::setPlaying(bool playing)
{
    m_playing = playing;
    m_following = false; // engages again when following next scrolls
    if (playing) {
        // Play: a hold ends, the playhead is brought into view if it is
        // not, and the follow timer runs. Stop, Pause, the end of the film
        // and a scrub all arrive here as false: the timer stops and the
        // view stays where it is.
        m_viewHeld = false;
        jumpIfOffScreen();
        if (m_followTimer)
            m_followTimer->start();
    } else {
        if (m_followTimer)
            m_followTimer->stop();
        m_liveElapsedMs = -1;
    }
    if (m_canvas)
        m_canvas->update();
}

// --- Following ------------------------------------------------------------

void AnimaticTimeline::followTick()
{
    if (!m_host)
        return;
    // ONE reading for this tick: the scroll below and the paint after it
    // both use it, so the playhead sits exactly on the follow point.
    m_liveElapsedMs = m_host->liveElapsedMsInCurrentPanel();
    if (m_follow && !m_viewHeld && m_drag != Drag::Playhead) {
        const int phx = qRound(playheadContentX());
        const int fp = followPoint() - kLabelCol;
        if (phx - m_scrollX > fp) {
            setScrollX(phx - fp); // held at the follow point (stops at the end)
            m_following = true;   // from here, geometry changes keep it there
        }
    }
    if (m_canvas)
        m_canvas->update();
}

int AnimaticTimeline::followPoint() const
{
    return kLabelCol + visibleWidth() * 2 / 3;
}

// A scroll made by following, not by the user: the scrollbar's own signal
// must not read it as the user's.
void AnimaticTimeline::setScrollX(int scrollX)
{
    if (!m_hScroll)
        return;
    const int clamped = qBound(0, scrollX, m_hScroll->maximum());
    if (clamped == m_scrollX)
        return;
    m_hScroll->setValue(clamped); // valueChanged sets m_scrollX; no action
}

// The scrollbar reports an action. Which of them are the user's is the
// whole question (see the header): steps, pages and to-min / to-max come
// only from the bar's own buttons, track and keys; a SliderMove is a drag
// when the handle is pressed, the wheel when a wheel event is being
// delivered, and otherwise a programmatic setSliderPosition - not the
// user's. The position the action chose is sliderPosition(), which is
// ahead of value() at this moment.
void AnimaticTimeline::onScrollbarAction(int action)
{
    bool user = false;
    switch (action) {
    case QAbstractSlider::SliderSingleStepAdd:
    case QAbstractSlider::SliderSingleStepSub:
    case QAbstractSlider::SliderPageStepAdd:
    case QAbstractSlider::SliderPageStepSub:
    case QAbstractSlider::SliderToMinimum:
    case QAbstractSlider::SliderToMaximum:
        user = true;
        break;
    case QAbstractSlider::SliderMove:
        user = (m_hScroll && m_hScroll->isSliderDown()) || m_wheelOnScrollbar;
        break;
    default:
        break;
    }
    if (user && m_hScroll)
        onUserScroll(m_hScroll->sliderPosition());
}

bool AnimaticTimeline::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_hScroll && event->type() == QEvent::Wheel) {
        // The bar handles the wheel after this returns, reporting a
        // SliderMove; the flag is what tells that move from a programmatic
        // one. Cleared once the event has been dealt with, whether or not
        // the bar moved (at the end of its range it reports nothing).
        m_wheelOnScrollbar = true;
        QTimer::singleShot(0, this, [this] { m_wheelOnScrollbar = false; });
    }
    return QWidget::eventFilter(watched, event);
}

// The user's scroll wins. With the playhead now RIGHT of the follow point
// (in view or off the right edge) the view is theirs until the next Play,
// scrub, seek or loop restart. Left of it, following simply has nothing to
// do until the playhead arrives there, and then carries on as usual.
void AnimaticTimeline::onUserScroll(int scrollX)
{
    const int phx = qRound(playheadContentX());
    m_viewHeld = phx - scrollX > followPoint() - kLabelCol;
    m_following = false; // the user placed the view; following re-engages at the point
}

bool AnimaticTimeline::playheadInView(int *screenX) const
{
    const int sx = contentXToScreen(playheadContentX());
    if (screenX)
        *screenX = sx;
    return sx >= kLabelCol && sx <= kLabelCol + visibleWidth();
}

// The jump rule: only if the playhead would be off-screen, and then a third
// of the way across, so two-thirds of the view shows what is coming. At the
// film's start that is simply the start.
void AnimaticTimeline::jumpIfOffScreen()
{
    if (!m_follow || !m_hScroll || m_hScroll->maximum() <= 0 || playheadInView())
        return;
    m_following = false; // a jump: following engages again at the point
    setScrollX(qRound(playheadContentX()) - visibleWidth() / 3);
}

int AnimaticTimeline::playheadScreenXForTest() const
{
    return contentXToScreen(playheadContentX());
}

int AnimaticTimeline::followPointForTest() const
{
    return followPoint();
}

bool AnimaticTimeline::followTimerActiveForTest() const
{
    return m_followTimer && m_followTimer->isActive();
}

QVariantMap AnimaticTimeline::devrecState() const
{
    return {{QStringLiteral("timeline.zoom"), double(m_zoom)},
            {QStringLiteral("timeline.scrollX"), m_scrollX},
            {QStringLiteral("timeline.scrollMax"), m_hScroll ? m_hScroll->maximum() : 0},
            {QStringLiteral("timeline.visibleWidth"), visibleWidth()},
            {QStringLiteral("timeline.playheadFrame"), playheadFrame()},
            {QStringLiteral("timeline.playheadX"), contentXToScreen(playheadContentX())},
            {QStringLiteral("timeline.followPoint"), followPoint()},
            {QStringLiteral("timeline.playing"), m_playing},
            {QStringLiteral("timeline.follow"), m_follow},
            {QStringLiteral("timeline.viewHeld"), m_viewHeld},
            {QStringLiteral("timeline.followTimer"),
             m_followTimer && m_followTimer->isActive()}};
}

void AnimaticTimeline::userScrollForTest(int scrollX)
{
    if (!m_hScroll)
        return;
    // A drag, as the bar reports one: handle pressed, position moved,
    // handle released. (Section (au) drives the real events as well.)
    m_hScroll->setSliderDown(true);
    m_hScroll->setSliderPosition(scrollX);
    m_hScroll->setSliderDown(false);
}

void AnimaticTimeline::setLoopRegion(int startIndex, int endIndex)
{
    m_loopStart = startIndex;
    m_loopEnd = endIndex;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setAudioLoaded(bool loaded, qint64 audioDurationMs)
{
    m_audioLoaded = loaded;
    m_audioDurationMs = audioDurationMs;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::updatePlayhead()
{
    if (m_canvas)
        m_canvas->update();
}

// --- Geometry / model -----------------------------------------------------

void AnimaticTimeline::rebuildBlocks()
{
    m_blocks.clear();
    int flat = 0;
    int frameCursor = 0;
    for (int si = 0; si < m_scenes.size(); ++si) {
        Scene *scene = m_scenes.at(si);
        for (int pi = 0; pi < scene->panels.size(); ++pi) {
            Panel *panel = scene->panels.at(pi);
            int dur = qBound(kMinDur, panel->duration, kMaxDur);
            if (m_drag == Drag::ResizeRight && flat == m_resizeIndex)
                dur = qBound(kMinDur, m_dragDuration, kMaxDur);

            Block b;
            b.flatIndex = flat;
            b.sceneIndex = si;
            b.panelIndex = pi;
            b.sceneNumber = scene->number;
            b.sceneName = scene->location.isEmpty()
                ? QStringLiteral("Scene %1").arg(scene->number)
                : scene->location;
            b.duration = qBound(kMinDur, panel->duration, kMaxDur);
            b.frames = dur * m_fps;
            b.startFrame = frameCursor;
            b.sceneStart = (pi == 0);
            m_blocks.append(b);

            frameCursor += b.frames;
            ++flat;
        }
    }
}

int AnimaticTimeline::totalFrames() const
{
    int f = 0;
    for (const Block &b : m_blocks)
        f += b.frames;
    return qMax(f, m_fps); // never zero
}

int AnimaticTimeline::totalSeconds() const
{
    int s = 0;
    for (const Block &b : m_blocks)
        s += b.duration;
    return s;
}

double AnimaticTimeline::pxPerFrame() const { return m_zoom / 100.0; }
double AnimaticTimeline::pxPerSecond() const { return pxPerFrame() * m_fps; }
double AnimaticTimeline::contentWidthPx() const { return totalFrames() * pxPerFrame(); }

int AnimaticTimeline::visibleWidth() const
{
    return qMax(0, (m_canvas ? m_canvas->width() : width()) - kLabelCol);
}

int AnimaticTimeline::contentXToScreen(double contentX) const
{
    return kLabelCol + qRound(contentX - m_scrollX);
}

double AnimaticTimeline::screenXToContent(int screenX) const
{
    return (screenX - kLabelCol) + m_scrollX;
}

int AnimaticTimeline::frameAtScreenX(int screenX) const
{
    const double cx = screenXToContent(screenX);
    const double ppf = qMax(0.0001, pxPerFrame());
    return qBound(0, static_cast<int>(std::lround(cx / ppf)), totalFrames());
}

int AnimaticTimeline::blockAtFrame(int frame) const
{
    for (const Block &b : m_blocks) {
        if (frame >= b.startFrame && frame < b.startFrame + b.frames)
            return b.flatIndex;
    }
    if (!m_blocks.isEmpty() && frame >= m_blocks.last().startFrame)
        return m_blocks.last().flatIndex;
    return -1;
}

double AnimaticTimeline::playheadContentX() const
{
    if (m_drag == Drag::Playhead && m_dragPlayheadFrame >= 0)
        return m_dragPlayheadFrame * pxPerFrame();
    return playheadFrame() * pxPerFrame();
}

int AnimaticTimeline::playheadFrame() const
{
    if (m_drag == Drag::Playhead && m_dragPlayheadFrame >= 0)
        return m_dragPlayheadFrame;
    if (m_current < 0 || m_current >= m_blocks.size())
        return 0;
    const Block &b = m_blocks.at(m_current);
    // While following, the value read at this tick (see followTick); else
    // the host's own.
    const int elapsedMs = m_liveElapsedMs >= 0 ? m_liveElapsedMs
        : m_host ? m_host->elapsedMsInCurrentPanel() : 0;
    int elapsedFrames = static_cast<int>(elapsedMs / (1000.0 / m_fps));
    elapsedFrames = qBound(0, elapsedFrames, b.frames);
    return b.startFrame + elapsedFrames;
}

void AnimaticTimeline::updateScrollRange()
{
    if (!m_hScroll)
        return;
    // ROUNDED, not ceil'd: Fit chooses a zoom whose content width is the
    // view's width in floating point, and ceil turned a width of
    // 1047.00006 into a scroll range of 1 px - which following then used
    // (measured in the gate's "whole film fits" check, 2026-10-16). A film
    // that fits has no range.
    const int maxScroll = qMax(0, qRound(contentWidthPx()) - visibleWidth());
    m_hScroll->setRange(0, maxScroll);
    m_hScroll->setPageStep(qMax(1, visibleWidth()));
    m_hScroll->setSingleStep(qMax(1, static_cast<int>(pxPerSecond())));
    if (m_scrollX > maxScroll) {
        m_scrollX = maxScroll;
        m_hScroll->setValue(maxScroll); // a clamp: valueChanged, no action
    }
    m_hScroll->setVisible(maxScroll > 0);
    // A GEOMETRY CHANGE WHILE THE STRIP IS SCROLLING UNDER THE PLAYHEAD -
    // a zoom, the timeline's width, a panel lengthened - moves the
    // playhead within the view (a zoom-out halves its content x and can
    // put it off the left edge). The user's rule: the playhead stays in
    // view, held at the follow point while there is room. So the view is
    // re-anchored to it here, through following's own path - never the
    // user's. Before following has engaged nothing is moved; while the
    // user holds the view (m_viewHeld) nothing is moved and the hold
    // stands - only Play, a scrub, a seek or a loop restart end a hold.
    // PAUSED OR STOPPED, A CHANGE OF SIZE MOVES NOTHING (the user's rule,
    // 2026-10-17): a timing edit, a duration drag, zoom, resize, the
    // splitter, collapse / expand. If the playhead ends up off-screen,
    // the next Play brings it back by the jump rule.
    if (m_following && m_playing && m_follow && !m_viewHeld && m_host
        && m_drag != Drag::Playhead)
        setScrollX(qRound(playheadContentX()) - (followPoint() - kLabelCol));
}

void AnimaticTimeline::applyZoom(float z)
{
    m_zoom = qBound(10.0f, z, 400.0f);
    updateScrollRange();
    if (m_canvas)
        m_canvas->update();
    emit zoomChanged(m_zoom);
}

void AnimaticTimeline::fitZoom()
{
    const int vw = visibleWidth();
    if (vw <= 0)
        return;
    // contentWidth == vw  =>  totalFrames * (zoom/100) == vw
    float z = static_cast<float>(vw) * 100.0f / static_cast<float>(totalFrames());
    z = qBound(10.0f, z, 400.0f);
    m_scrollX = 0;
    if (m_hScroll)
        m_hScroll->setValue(0); // Fit: valueChanged, no action
    if (m_zoomSlider) {
        QSignalBlocker blk(m_zoomSlider);
        m_zoomSlider->setValue(qRound(z));
    }
    applyZoom(z);
}

int AnimaticTimeline::snapFrame(int frame) const
{
    return frame; // frames are already the finest grid; kept for clarity
}

QString AnimaticTimeline::timecode(int frame) const
{
    const int totalSec = frame / m_fps;
    const int ff = frame % m_fps;
    const int ss = totalSec % 60;
    const int mm = (totalSec / 60) % 60;
    const int hh = totalSec / 3600;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

// --- Painting -------------------------------------------------------------

void AnimaticTimeline::renderCanvas(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    const int W = m_canvas->width();
    const int contentRight = W;

    // Backgrounds per track (full width first).
    p.fillRect(QRect(0, kRulerY, W, kRulerH), QColor("#0d0d0d"));
    p.fillRect(QRect(0, kSceneY, W, kSceneH), QColor("#181818"));
    p.fillRect(QRect(0, kPanelY, W, panelTrackH()), QColor("#141414"));
    p.fillRect(QRect(0, audioTrackY(), W, kAudioH),
               QColor(m_audioLoaded ? "#0d1a26" : "#111111"));

    // Clip all time-based content to the area right of the label column.
    p.save();
    p.setClipRect(QRect(kLabelCol, 0, W - kLabelCol, tracksH()));

    const double ppf = pxPerFrame();
    const double pps = pxPerSecond();

    // ----- Timecode ruler -----
    {
        QFont f(QStringLiteral("Courier New"));
        f.setPointSizeF(6.5);
        p.setFont(f);

        // Choose the major-label interval in seconds so labels stay readable.
        int labelSecs = 1;
        if (pps < 16) labelSecs = 10;
        else if (pps < 32) labelSecs = 5;
        else labelSecs = 1;

        const int tSecs = totalSeconds();
        const bool frameTicks = ppf >= 8.0;     // per-frame ticks when zoomed in
        const bool halfTicks = pps >= 30.0;     // 0.5s minor ticks

        if (frameTicks) {
            for (int fr = 0; fr <= totalFrames(); ++fr) {
                const int x = contentXToScreen(fr * ppf);
                if (x < kLabelCol - 2 || x > contentRight) continue;
                p.setPen(QColor("#333333"));
                p.drawLine(x, kRulerY + kRulerH - 4, x, kRulerY + kRulerH);
            }
        }
        if (halfTicks) {
            for (int s2 = 0; s2 <= tSecs * 2; ++s2) {
                if (s2 % 2 == 0) continue; // skip whole seconds (drawn below)
                const int x = contentXToScreen((s2 * m_fps / 2) * ppf);
                if (x < kLabelCol - 2 || x > contentRight) continue;
                p.setPen(QColor("#444444"));
                p.drawLine(x, kRulerY + kRulerH - 7, x, kRulerY + kRulerH);
            }
        }
        for (int s = 0; s <= tSecs; ++s) {
            const int x = contentXToScreen(s * pps);
            if (x < kLabelCol - 40 || x > contentRight + 10) continue;
            p.setPen(QColor("#444444"));
            p.drawLine(x, kRulerY, x, kRulerY + kRulerH);
            if (s % labelSecs == 0) {
                p.setPen(QColor("#666666"));
                const QString lbl = m_framesMode
                    ? QString::number(s * m_fps)
                    : timecode(s * m_fps);
                p.drawText(QRect(x + 2, kRulerY, 80, kRulerH),
                           Qt::AlignVCenter | Qt::AlignLeft, lbl);
            }
        }
    }

    // ----- Scene track -----
    {
        QFont f = font();
        f.setPointSize(7);
        f.setBold(true);
        // Group consecutive blocks by sceneIndex.
        int i = 0;
        int sceneOrdinal = 0;
        while (i < m_blocks.size()) {
            const int si = m_blocks.at(i).sceneIndex;
            const int startFrame = m_blocks.at(i).startFrame;
            int endFrame = m_blocks.at(i).startFrame + m_blocks.at(i).frames;
            const QString name = m_blocks.at(i).sceneName;
            int j = i + 1;
            while (j < m_blocks.size() && m_blocks.at(j).sceneIndex == si) {
                endFrame = m_blocks.at(j).startFrame + m_blocks.at(j).frames;
                ++j;
            }
            const int x0 = contentXToScreen(startFrame * ppf);
            const int x1 = contentXToScreen(endFrame * ppf);
            QRect r(x0, kSceneY, x1 - x0, kSceneH);
            p.fillRect(r, QColor((sceneOrdinal % 2 == 0) ? "#13112e" : "#0f0d24"));
            p.setPen(QColor("#2a2766"));
            p.drawLine(r.left(), kSceneY + kSceneH - 1, r.right(), kSceneY + kSceneH - 1);
            // Scene separator.
            p.setPen(QColor("#444444"));
            p.drawLine(x0, kSceneY, x0, kSceneY + kSceneH);
            p.setFont(f);
            p.setPen(QColor("#ffffff"));
            p.drawText(r, Qt::AlignCenter, name);
            i = j;
            ++sceneOrdinal;
        }
    }

    // ----- Panel / shot clips -----
    {
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont small = font();
        small.setPointSize(7);
        QFont mono(QStringLiteral("Consolas"));
        mono.setPointSize(7);

        constexpr qreal kClipR = 4.0; // corner radius

        for (const Block &b : m_blocks) {
            const int x0 = contentXToScreen(b.startFrame * ppf);
            const int x1 = contentXToScreen((b.startFrame + b.frames) * ppf);
            if (x1 < kLabelCol || x0 > contentRight) continue;
            QRect r(x0, kPanelY + 2, x1 - x0, panelTrackH() - 4);
            const QRectF rf(r);

            // "current" is the SELECTED panel - the one on the drawing
            // canvas - not the one under the playhead: playback moves the
            // playhead through the clips and leaves the selection alone.
            const bool current = (b.flatIndex == m_selected);
            const bool hovered = (b.flatIndex == m_hoverIndex);

            // Accent bloom around the selected clip: explicit filled rounded rects
            // stepping outward, opacity rising toward the clip body, drawn under
            // the clip fill so a visible halo punches through the dark background.
            if (current) {
                struct Glow { qreal off; int alpha; };
                const Glow glows[3] = {{6.0, 40}, {4.0, 60}, {2.0, 90}};
                for (const Glow &gl : glows) {
                    QColor c = SankoTheme::kAccent;
                    c.setAlpha(gl.alpha);
                    p.setPen(Qt::NoPen);
                    p.setBrush(c);
                    p.drawRoundedRect(rf.adjusted(-gl.off, -gl.off, gl.off, gl.off),
                                      kClipR, kClipR);
                }
            }

            // Vertical gradient fill (SankoTV purple).
            QLinearGradient grad(r.left(), r.top(), r.left(), r.bottom());
            if (current) {
                grad.setColorAt(0.0, QColor("#3d3894"));
                grad.setColorAt(1.0, QColor("#2a2570"));
            } else {
                grad.setColorAt(0.0, QColor("#2a2766"));
                grad.setColorAt(1.0, QColor("#1e1a4d"));
            }
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawRoundedRect(rf, kClipR, kClipR);

            // Border (selected: 2.5px accent; else 1px #3d3894, accent-50% on hover).
            p.setBrush(Qt::NoBrush);
            if (current) {
                QColor sel = SankoTheme::kAccentLight;
                sel.setAlpha(255);
                p.setPen(QPen(sel, 2.5));
                p.drawRoundedRect(rf.adjusted(1, 1, -1, -1), kClipR, kClipR);
            } else {
                QColor bord("#3d3894");
                if (hovered) { bord = SankoTheme::kAccentLight; bord.setAlphaF(0.50); }
                p.setPen(QPen(bord, 1));
                p.drawRoundedRect(rf.adjusted(0.5, 0.5, -0.5, -0.5), kClipR, kClipR);
            }

            // Thumbnail in the left portion of the clip.
            int thumbW = 0;
            const bool showThumb = (r.width() >= 50);
            if (showThumb) {
                // A taller shots track gets a wider thumbnail (16:9 of its
                // height) rather than a taller crop of a 60 px sliver.
                const int thumbH = r.height() - 8;
                thumbW = qMin(static_cast<int>(r.width() * 0.35),
                              qMax(60, thumbH * 16 / 9));
                const QRect thumbRect(r.left() + 4, r.top() + 4, thumbW, thumbH);

                QPixmap pix;
                if (b.sceneIndex < m_scenes.size()) {
                    Scene *sc = m_scenes.at(b.sceneIndex);
                    if (b.panelIndex < sc->panels.size())
                        // The cached 512px mip, not the full flatten: this
                        // runs per visible clip on EVERY repaint — each
                        // scrub mouse-move and every playback tick — and
                        // the full composite here cost 16 ms/clip at 4K
                        // (287 ms per repaint, measured). The mip
                        // self-validates against layer edits, and the
                        // clip thumb is at most ~60 px wide.
                        pix = sc->panels.at(b.panelIndex)->flattenedThumb();
                }

                p.save();
                QPainterPath clip;
                clip.addRoundedRect(QRectF(thumbRect), 2, 2);
                p.setClipPath(clip, Qt::IntersectClip);
                if (!pix.isNull()) {
                    const QPixmap scaled = pix.scaled(
                        thumbRect.size(), Qt::KeepAspectRatioByExpanding,
                        Qt::SmoothTransformation);
                    const int sx = (scaled.width() - thumbRect.width()) / 2;
                    const int sy = (scaled.height() - thumbRect.height()) / 2;
                    p.drawPixmap(thumbRect, scaled,
                                 QRect(sx, sy, thumbRect.width(), thumbRect.height()));
                } else {
                    p.fillRect(thumbRect, QColor("#0d0d0d"));
                }
                // Dark gradient overlay (left transparent -> right #0a0a0a 60%).
                QLinearGradient ov(thumbRect.left(), 0, thumbRect.right(), 0);
                ov.setColorAt(0.0, QColor(0x0a, 0x0a, 0x0a, 0));
                ov.setColorAt(1.0, QColor(0x0a, 0x0a, 0x0a, 153));
                p.fillRect(thumbRect, ov);
                p.restore();

                // Thumbnail border.
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor("#2a2a2a"), 1));
                p.drawRoundedRect(QRectF(thumbRect).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
            }

            // Clip text, shifted right past the thumbnail.
            const int textX = showThumb ? (r.left() + thumbW + 8) : (r.left() + 6);
            const QRect textRect(textX, r.top() + 4, r.right() - textX - 4, r.height() - 8);
            if (textRect.width() > 8) {
                QFont bold = small; bold.setBold(true);
                p.setFont(bold);
                p.setPen(SankoTheme::kAccentLight);
                p.drawText(textRect, Qt::AlignTop | Qt::AlignLeft,
                           QString::number(b.flatIndex + 1));
                p.setFont(small);
                p.setPen(QColor("#888888"));
                QFontMetrics fm(small);
                p.drawText(textRect, Qt::AlignBottom | Qt::AlignLeft,
                           fm.elidedText(b.sceneName, Qt::ElideRight, textRect.width() - 24));
            }

            // Duration badge: rounded pill, bottom-right (only on wider clips).
            if (r.width() > 70) {
                const QString dtext = QStringLiteral("%1s").arg(b.duration);
                QFontMetrics fmm(mono);
                const int bw = fmm.horizontalAdvance(dtext) + 8;
                const QRectF pill(r.right() - bw - 4, r.bottom() - 17, bw, 14);
                if (pill.left() > textX) {
                    p.setPen(QPen(QColor("#333333"), 1));
                    p.setBrush(QColor("#111111"));
                    p.drawRoundedRect(pill, 4, 4);
                    p.setBrush(Qt::NoBrush);
                    p.setFont(mono);
                    p.setPen(QColor("#ffffff"));
                    p.drawText(pill, Qt::AlignCenter, dtext);
                }
            }

            // Trim handles on the selected clip.
            if (current) {
                p.fillRect(QRect(r.left(), r.top(), kHandleW, r.height()),
                           SankoTheme::kAccentLight);
                p.fillRect(QRect(r.right() - kHandleW + 1, r.top(), kHandleW, r.height()),
                           SankoTheme::kAccentLight);
            }
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ----- Loop region overlay (on the panel track) -----
    if (m_loopStart >= 0 && m_loopEnd >= 0 && m_loopStart < m_blocks.size()
        && m_loopEnd < m_blocks.size() && m_loopStart <= m_loopEnd) {
        const Block &a = m_blocks.at(m_loopStart);
        const Block &z = m_blocks.at(m_loopEnd);
        const int x0 = contentXToScreen(a.startFrame * ppf);
        const int x1 = contentXToScreen((z.startFrame + z.frames) * ppf);
        QColor green(0x4d, 0xff, 0x91);
        green.setAlphaF(0.12);
        p.fillRect(QRect(x0, kPanelY, x1 - x0, panelTrackH()), green);
        p.setPen(QPen(QColor(0x4d, 0xff, 0x91), 2));
        p.drawLine(x0 + 1, kPanelY, x0 + 1, kPanelY + panelTrackH());
        p.drawLine(x1 - 1, kPanelY, x1 - 1, kPanelY + panelTrackH());
    }

    // ----- Reorder drop indicator (a clip is being dragged) -----
    if (m_drag == Drag::Move && m_moveActive && m_moveGap >= 0
        && !m_blocks.isEmpty()) {
        const int frame = m_moveGap < m_blocks.size()
            ? m_blocks.at(m_moveGap).startFrame
            : m_blocks.last().startFrame + m_blocks.last().frames;
        const int x = contentXToScreen(frame * ppf);
        p.setPen(QPen(SankoTheme::kAccentLight, 3));
        p.drawLine(x, kPanelY + 1, x, kPanelY + panelTrackH() - 1);
    }

    // ----- Audio track -----
    // A PLAIN BAR, as long as the audio, with the file's name on it. This
    // used to draw a sine wave under an envelope - a picture of a waveform
    // that had nothing to do with the audio's own, on the track that claims
    // to show the audio. A bar says only what is known: there is a track,
    // this is its file, it runs this long. (A real waveform is a separate,
    // later piece of work.)
    {
        const AudioBar bar = audioBar();
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        const QFontMetrics fm(f);
        if (bar.state == AudioBar::Present) {
            QColor blue(0x4d, 0x9f, 0xff);
            QColor fill = blue;
            fill.setAlphaF(0.22);
            blue.setAlphaF(0.60);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(blue, 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(bar.rect).adjusted(0.5, 0.5, -0.5, -0.5),
                              3, 3);
            p.setRenderHint(QPainter::Antialiasing, false);
            // The texts stay inside the VISIBLE part of the bar, so the
            // name is readable however far the timeline is scrolled.
            const int left = qMax(bar.rect.left(), kLabelCol) + 8;
            const int right = qMin(bar.rect.right(), contentRight) - 8;
            const int lengthW = fm.horizontalAdvance(bar.length);
            const bool showLength = right - left > lengthW + 60;
            p.setPen(QColor("#88a6c4"));
            if (showLength)
                p.drawText(QRect(right - lengthW, bar.rect.top(), lengthW,
                                 bar.rect.height()),
                           Qt::AlignVCenter | Qt::AlignRight, bar.length);
            const int nameW = right - left - (showLength ? lengthW + 12 : 0);
            if (nameW > 12)
                p.drawText(QRect(left, bar.rect.top(), nameW, bar.rect.height()),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(bar.name, Qt::ElideMiddle, nameW));
        } else if (bar.state == AudioBar::Missing
                   || bar.state == AudioBar::Unplayable) {
            // The project names a file that is not there - or one that is
            // there and cannot be played. Its length is unknown, so the bar
            // spans the film, HATCHED and outlined in the warning colour -
            // nothing about it can be mistaken for a track that will play.
            QColor line = SankoTheme::kWarning;
            QColor hatch = SankoTheme::kWarning;
            hatch.setAlphaF(0.35);
            p.setPen(QPen(line, 1.0, Qt::DashLine));
            p.setBrush(QBrush(hatch, Qt::BDiagPattern));
            p.drawRect(bar.rect.adjusted(0, 0, -1, -1));
            const int left = qMax(bar.rect.left(), kLabelCol) + 8;
            const int right = qMin(bar.rect.right(), contentRight) - 8;
            const int textW = qMin(fm.horizontalAdvance(bar.label) + 12,
                                   right - left);
            if (textW > 24) {
                // The words sit on an opaque chip so the hatch does not run
                // through them.
                const QRect chip(left, bar.rect.top() + 2, textW,
                                 bar.rect.height() - 4);
                p.fillRect(chip, QColor("#111111"));
                p.setPen(line);
                p.drawText(chip.adjusted(6, 0, -6, 0),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(bar.label, Qt::ElideMiddle,
                                         textW - 12));
            }
        } else if (bar.state == AudioBar::Loading) {
            // The track is set but its length has not been read yet: the
            // name, and no bar - never a bar of a guessed length.
            p.setPen(QColor("#88a6c4"));
            p.drawText(QRect(kLabelCol + 8, audioTrackY(), W - kLabelCol - 16,
                             kAudioH),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(bar.name, Qt::ElideMiddle,
                                     W - kLabelCol - 16));
        }
    }

    p.restore(); // end content clip

    // ----- Playhead (full height, over content area) -----
    {
        const int px = contentXToScreen(playheadContentX());
        if (px >= kLabelCol && px <= W) {
            p.setPen(QPen(SankoTheme::kAccent, 1.5));
            p.drawLine(px, 0, px, tracksH());
            // Triangle handle in the ruler.
            p.setRenderHint(QPainter::Antialiasing, true);
            QPolygon tri;
            tri << QPoint(px - 5, 0) << QPoint(px + 5, 0) << QPoint(px, 8);
            p.setBrush(SankoTheme::kAccent);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    // ----- Left label column (painted on top, fixed) -----
    {
        p.fillRect(QRect(0, 0, kLabelCol, tracksH()), QColor("#0d0d0d"));
        p.setPen(QColor("#2a2a2a"));
        p.drawLine(kLabelCol - 1, 0, kLabelCol - 1, tracksH());

        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);
        p.setPen(QColor("#666666"));
        struct TL { const char *name; int y, h; };
        const TL labels[] = {
            {"TIMECODE", kRulerY, kRulerH}, {"SCENES", kSceneY, kSceneH},
            {"SHOTS", kPanelY, panelTrackH()},    {"AUDIO", audioTrackY(), kAudioH},
        };
        for (const TL &t : labels)
            p.drawText(QRect(0, t.y, kLabelCol - 8, t.h),
                       Qt::AlignVCenter | Qt::AlignRight,
                       QString::fromLatin1(t.name));
    }

    // Empty-audio hint. It names the thing that works: this used to say
    // "Drop audio file here", and nothing here accepts a drop.
    if (audioBar().state == AudioBar::None) {
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(QColor("#555555"));
        p.drawText(QRect(kLabelCol, audioTrackY(), W - kLabelCol, kAudioH),
                   Qt::AlignCenter, audioBar().hint);
    }
}

// What the audio track shows, in one place: the painter draws exactly this
// and the gate reads exactly this, so a check on "what the bar says" cannot
// pass over a bar that paints something else.
AnimaticTimeline::AudioBar AnimaticTimeline::audioBar() const
{
    AudioBar bar;
    const QString path = (m_audioLoaded && m_host) ? m_host->audioPath()
                                                    : QString();
    if (path.isEmpty()) {
        bar.hint = QStringLiteral("Right-click to import audio");
        return bar;
    }
    bar.name = path.section('/', -1).section('\\', -1);
    if (m_audioMissing) {
        bar.state = AudioBar::Missing;
        bar.label = QString::fromUtf8("MISSING \xE2\x80\x94 %1 \xC2\xB7 "
                                      "right-click to locate").arg(bar.name);
        const int x0 = contentXToScreen(0);
        const int x1 = contentXToScreen(contentWidthPx());
        bar.rect = QRect(x0, audioTrackY() + 6, qMax(2, x1 - x0), kAudioH - 12);
        return bar;
    }
    if (m_audioUnplayable) {
        // The file is there and the player could not open it. This used to
        // fall through to Loading - the name and no bar, for ever.
        bar.state = AudioBar::Unplayable;
        bar.label = QString::fromUtf8("CANNOT BE PLAYED \xE2\x80\x94 %1 \xC2\xB7 "
                                      "right-click to import another").arg(bar.name);
        const int x0 = contentXToScreen(0);
        const int x1 = contentXToScreen(contentWidthPx());
        bar.rect = QRect(x0, audioTrackY() + 6, qMax(2, x1 - x0), kAudioH - 12);
        return bar;
    }
    if (m_audioDurationMs <= 0) {
        bar.state = AudioBar::Loading;
        return bar;
    }
    bar.state = AudioBar::Present;
    // A LINKED track (an old project's file outside the project) says so on
    // the bar itself, as the header label does: it is the one that loses
    // its sound when the project folder moves.
    if (m_host->audioLinked())
        bar.name += QString::fromUtf8(" \xE2\x80\x94 linked");
    const int seconds = int((m_audioDurationMs + 500) / 1000);
    bar.length = QStringLiteral("%1:%2").arg(seconds / 60)
                     .arg(seconds % 60, 2, 10, QChar('0'));
    const double audioFrames = (m_audioDurationMs / 1000.0) * m_fps;
    const int x0 = contentXToScreen(0);
    const int x1 = contentXToScreen(audioFrames * pxPerFrame());
    bar.rect = QRect(x0, audioTrackY() + 6, qMax(2, x1 - x0), kAudioH - 12);
    return bar;
}

QString AnimaticTimeline::audioBarStateForTest() const
{
    switch (audioBar().state) {
    case AudioBar::None: return QStringLiteral("none");
    case AudioBar::Loading: return QStringLiteral("loading");
    case AudioBar::Present: return QStringLiteral("present");
    case AudioBar::Missing: return QStringLiteral("missing");
    case AudioBar::Unplayable: return QStringLiteral("unplayable");
    }
    return QString();
}

QString AnimaticTimeline::audioBarTextForTest() const
{
    const AudioBar bar = audioBar();
    if (bar.state == AudioBar::None)
        return bar.hint;
    if (bar.state == AudioBar::Missing || bar.state == AudioBar::Unplayable)
        return bar.label;
    return bar.length.isEmpty() ? bar.name
                                : bar.name + QStringLiteral(" | ") + bar.length;
}

void AnimaticTimeline::setAudioUnplayable(bool unplayable)
{
    if (m_audioUnplayable == unplayable)
        return;
    m_audioUnplayable = unplayable;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setAudioMissing(bool missing)
{
    if (m_audioMissing == missing)
        return;
    m_audioMissing = missing;
    if (m_canvas)
        m_canvas->update();
}

QRect AnimaticTimeline::audioRowForTest() const
{
    return QRect(0, audioTrackY(), m_canvas ? m_canvas->width() : 0, kAudioH);
}

// --- Interaction ----------------------------------------------------------

void AnimaticTimeline::canvasResized()
{
    updateScrollRange();
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::canvasMousePress(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint pos = e->position().toPoint();
    if (pos.x() < kLabelCol)
        return;

    // Playhead: triangle in the ruler, or near the line anywhere.
    const int phx = contentXToScreen(playheadContentX());
    if (pos.y() < kRulerY + kRulerH || qAbs(pos.x() - phx) <= kGrab) {
        m_drag = Drag::Playhead;
        m_dragPlayheadFrame = frameAtScreenX(pos.x());
        QToolTip::showText(e->globalPosition().toPoint(),
                           timecode(m_dragPlayheadFrame), m_canvas);
        m_canvas->update();
        // Scrubbing LOOKS without selecting: the host previews the panel
        // under the playhead and the drawing canvas stays where it is.
        m_scrubIndex = blockAtFrame(m_dragPlayheadFrame);
        if (m_scrubIndex >= 0)
            emit playheadScrubbed(m_scrubIndex);
        return;
    }

    // Trim handles on the selected clip (panel track only).
    if (m_selected >= 0 && m_selected < m_blocks.size()
        && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const Block &b = m_blocks.at(m_selected);
        const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
        const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
        if (qAbs(pos.x() - x1) <= kHandleW) {
            m_drag = Drag::ResizeRight;
            m_resizeIndex = m_selected;
            m_dragDuration = b.duration;
            m_canvas->setCursor(Qt::SizeHorCursor);
            QToolTip::showText(e->globalPosition().toPoint(),
                               QStringLiteral("%1.0s").arg(m_dragDuration), m_canvas);
            return;
        }
        if (qAbs(pos.x() - x0) <= kHandleW) {
            m_drag = Drag::ResizeLeft; // visual only for now
            m_resizeIndex = m_selected;
            m_dragLeftFrames = 0;
            m_canvas->setCursor(Qt::SizeHorCursor);
            return;
        }
    }

    // Otherwise: a press on a clip SELECTS that panel (the workspace does
    // the selecting and pushes the highlight back through setSelectedPanel),
    // and may turn into a reorder drag once it has moved far enough.
    if (pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int frame = frameAtScreenX(pos.x());
        const int idx = blockAtFrame(frame);
        if (idx >= 0) {
            emit panelSeekRequested(idx);
            // The request may have rebuilt the blocks (it never does today,
            // but nothing here should assume it): re-check before arming.
            if (idx < m_blocks.size()) {
                m_drag = Drag::Move;
                m_moveFrom = idx;
                m_moveGap = -1;
                m_moveActive = false;
                m_movePress = pos;
            }
        }
    }
}

// Right-click: on a clip, select it first (the menu acts on the selection,
// so a menu opened over an unselected clip would otherwise act on another
// one) and hand the menu to the workspace, which owns the panel operations.
void AnimaticTimeline::canvasContextMenu(QContextMenuEvent *e)
{
    const QPoint pos = e->pos();
    if (m_drag != Drag::None)
        return;
    // The AUDIO row, its label included: the track's own menu. Whoever owns
    // the audio builds it; nothing is selected by asking.
    if (pos.y() >= audioTrackY() && pos.y() < audioTrackY() + kAudioH) {
        emit audioContextMenuRequested(e->globalPos());
        e->accept();
        return;
    }
    if (pos.x() < kLabelCol)
        return;
    if (pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int idx = blockAtFrame(frameAtScreenX(pos.x()));
        if (idx >= 0) {
            emit panelSeekRequested(idx);
            emit clipContextMenuRequested(idx, e->globalPos());
            e->accept();
        }
    }
}

// The gap a dragged clip would drop into, as a FLAT index: the block it
// would land in front of, or one past the scene's last block. Confined to
// the dragged clip's own scene - panels do not move between scenes.
int AnimaticTimeline::moveGapAt(int screenX) const
{
    if (m_moveFrom < 0 || m_moveFrom >= m_blocks.size())
        return -1;
    const int scene = m_blocks.at(m_moveFrom).sceneIndex;
    int last = -1;
    for (const Block &b : m_blocks) {
        if (b.sceneIndex != scene)
            continue;
        last = b.flatIndex;
        const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
        const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
        if (screenX < (x0 + x1) / 2)
            return b.flatIndex;
    }
    return last + 1;
}

void AnimaticTimeline::canvasMouseMove(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();

    if (m_drag == Drag::Playhead) {
        m_dragPlayheadFrame = frameAtScreenX(pos.x());
        QToolTip::showText(e->globalPosition().toPoint(),
                           timecode(m_dragPlayheadFrame), m_canvas);
        m_canvas->update();
        const int idx = blockAtFrame(m_dragPlayheadFrame);
        if (idx >= 0 && idx != m_scrubIndex) {
            m_scrubIndex = idx; // one preview per panel crossed, not per pixel
            emit playheadScrubbed(idx);
        }
        return;
    }

    if (m_drag == Drag::Move) {
        if (!(e->buttons() & Qt::LeftButton)) {
            m_drag = Drag::None; // the release went elsewhere
            m_moveActive = false;
            m_canvas->update();
            return;
        }
        if (!m_moveActive
            && (pos - m_movePress).manhattanLength() >= kReorderThreshold) {
            m_moveActive = true;
            m_canvas->setCursor(Qt::ClosedHandCursor);
        }
        if (m_moveActive) {
            const int gap = moveGapAt(pos.x());
            if (gap != m_moveGap) {
                m_moveGap = gap;
                m_canvas->update();
            }
        }
        return;
    }

    if (m_drag == Drag::ResizeRight && m_resizeIndex >= 0) {
        const Block &b = m_blocks.at(m_resizeIndex);
        const double startX = b.startFrame * pxPerFrame();
        const double widthPx = screenXToContent(pos.x()) - startX;
        int newDur = static_cast<int>(std::lround(widthPx / qMax(1.0, pxPerSecond())));
        newDur = qBound(kMinDur, newDur, kMaxDur);
        if (newDur != m_dragDuration) {
            m_dragDuration = newDur;
            rebuildBlocks();
            updateScrollRange();
        }
        QToolTip::showText(e->globalPosition().toPoint(),
                           QStringLiteral("%1.0s").arg(m_dragDuration), m_canvas);
        m_canvas->update();
        return;
    }

    if (m_drag == Drag::ResizeLeft) {
        QToolTip::showText(e->globalPosition().toPoint(),
                           QStringLiteral("in/out — coming soon"), m_canvas);
        return;
    }

    // Idle hover: cursor + highlight.
    Qt::CursorShape cursor = Qt::ArrowCursor;
    int hover = -1;
    if (pos.x() >= kLabelCol && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int frame = frameAtScreenX(pos.x());
        hover = blockAtFrame(frame);
        cursor = (hover >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor;
        // Resize-handle cursor on the selected clip edges.
        if (m_selected >= 0 && m_selected < m_blocks.size()) {
            const Block &b = m_blocks.at(m_selected);
            const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
            const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
            if (qAbs(pos.x() - x0) <= kHandleW || qAbs(pos.x() - x1) <= kHandleW)
                cursor = Qt::SizeHorCursor;
        }
    } else if (pos.y() < kRulerY + kRulerH) {
        cursor = Qt::PointingHandCursor;
    }
    m_canvas->setCursor(cursor);
    if (hover != m_hoverIndex) {
        m_hoverIndex = hover;
        m_canvas->update();
    }
}

void AnimaticTimeline::canvasMouseRelease(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;

    if (m_drag == Drag::ResizeRight && m_resizeIndex >= 0) {
        const Block &b = m_blocks.at(m_resizeIndex);
        emit durationChanged(b.sceneIndex, b.panelIndex, m_dragDuration);
        m_drag = Drag::None;
        m_resizeIndex = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        QToolTip::hideText();
        return;
    }
    if (m_drag == Drag::ResizeLeft) {
        m_drag = Drag::None;
        m_resizeIndex = -1;
        m_dragLeftFrames = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        return;
    }
    if (m_drag == Drag::Playhead) {
        const int idx = blockAtFrame(m_dragPlayheadFrame);
        m_drag = Drag::None;
        m_dragPlayheadFrame = -1;
        // The playhead comes to rest at the start of the panel it was
        // dropped in, as it always did - but that panel is PREVIEWED, not
        // selected (this used to emit panelSeekRequested).
        if (idx >= 0 && idx != m_scrubIndex)
            emit playheadScrubbed(idx);
        m_scrubIndex = -1;
        // The playhead came to rest at a panel's start, which may be off
        // the left edge of the view: the jump rule, once, now that the
        // drag is over (jumps are suppressed while it lasts - the strip
        // must not move under a dragging hand).
        m_viewHeld = false;
        jumpIfOffScreen();
        m_canvas->update();
        QToolTip::hideText();
        return;
    }
    if (m_drag == Drag::Move) {
        const bool moved = m_moveActive && m_moveGap >= 0;
        const int from = m_moveFrom;
        const int gap = m_moveGap;
        m_drag = Drag::None;
        m_moveActive = false;
        m_moveFrom = -1;
        m_moveGap = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        m_canvas->update();
        // Dropping a clip right before or right after itself moves nothing.
        if (moved && gap != from && gap != from + 1)
            emit panelMoveRequested(from, gap);
        return;
    }
    m_drag = Drag::None;
}

void AnimaticTimeline::canvasLeave()
{
    if (m_hoverIndex != -1) {
        m_hoverIndex = -1;
        if (m_canvas)
            m_canvas->update();
    }
    if (m_canvas)
        m_canvas->setCursor(Qt::ArrowCursor);
}

void AnimaticTimeline::setFps(int fps)
{
    fps = qBound(1, fps, 120);
    if (fps == m_fps)
        return;
    m_fps = fps;
    rebuildBlocks(); // per-block frame counts derive from the rate
    update();
}
