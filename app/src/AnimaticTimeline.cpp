#include "AnimaticTimeline.h"
#include "SankoIcons.h"
#include "SankoTheme.h"
#include "SankoTimelineTheme.h"

#include "AnimaticPage.h"
#include "StoryboardModel.h"
#include "camera/ShotCamera.h"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QIcon>
#include <QPixmap>
#include <QPolygonF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

// THE LABEL COLUMN, from the redesign (Figma 380:644): the painted column
// is 134 px wide (kLabelColW) with its separator line at x 134, and the
// time content begins at x 135 (kLabelCol), right after the separator (the
// user's design of 2026-10-23; Part A had 136 with the separator on the
// content's first pixel).
// Every mapping between content and screen reads kLabelCol.
constexpr int kLabelCol = 135;  // where content x 0 is on the canvas
constexpr int kLabelColW = 134; // the painted column's width (134 since the design of 2026-10-23)
// The track controls (lock, speaker, eye) in the column: 16 px icons at
// these x offsets, centred in their track rows. Painted in Part A; live in
// Part B.
constexpr int kControlX[3] = {8, 32, 56};
constexpr int kControlSize = 16;

// Track geometry (top to bottom), all within the canvas. The timeline sits
// under the drawing canvas now and its height is the user's to drag, so the
// SHOTS track is not a constant any more: the ruler, the scene track and the
// audio track keep their heights and the shots track takes whatever is left
// (panelTrackH), never less than kPanelMinH. The two reserved tracks that
// used to follow - Camera and Markers, 24 px each, both "coming soon" - are
// gone: 48 px of the workspace for two labels. THE CAMERA ROW came back as
// a real track in Phase 1C (28 px, between SHOTS and AUDIO, next to the
// clips it frames): ruler 24, SCENES 24, SHOTS flexible, CAMERA 28, AUDIO 36.
constexpr int kRulerY = 0,   kRulerH = 24;
constexpr int kSceneY = 24,  kSceneH = 24;
constexpr int kPanelY = 48;
constexpr int kPanelMinH = 48, kPanelDefaultH = 80;
constexpr int kCameraH = 28;
constexpr int kAudioH = 36;

// The CAMERA row's marks (Figma 1C-1 / 1C-2): a 10 px diamond whose hit
// area is 16 px wide (the row's full height); the segment's ease pill,
// 18 x 12, drawn only on a segment of 40 px or more; a drag starts after
// 4 px and snaps (with Snap on) to a cut or the playhead within 6 px.
constexpr qreal kKeyHalf = 5.0;
constexpr int kKeyHit = 8;
constexpr int kGlyphW = 18, kGlyphH = 12, kGlyphMinSegment = 40;
constexpr int kKeyDragThreshold = 4;
constexpr int kKeySnapPx = 6;

constexpr int kHandleW = 6;   // trim handle width
constexpr int kGrab = 5;      // px grab tolerance
constexpr int kMinDur = 1, kMaxDur = 30;
constexpr int kReorderThreshold = 8; // px before a clip press becomes a drag

// The clip, as the design lays it out on a 76 px body (every number is the
// design's; see HANDOFF "The timeline redesign, Part A"): the body is the
// clip's rect less a 2 px gap on the right, radius 5; the thumbnail sits
// at (6,6), body width - 12 wide, 42 tall, radius 3; the number at (10,9);
// the name at (8, bottom - 20) up to the pill; the pill 28 x 18 at
// (right - 33, bottom - 22).
constexpr int kClipGap = 2;
constexpr qreal kClipR = 5.0;
constexpr int kThumbInset = 6, kThumbH = 42, kThumbR = 3;
constexpr int kPillW = 28, kPillH = 18;
// What hides first as a clip narrows (the user's order, 2026-10-18): the
// name (below 86 px of body), then the thumbnail (below 48), then the pill
// (below 42); the number stays while the body is 20 px or more.
constexpr int kNameMinBody = 86, kThumbMinBody = 48, kPillMinBody = 42, kNumberMinBody = 20;
} // namespace

// =====================================================================
// TimelineCanvas — the painted surface. Forwards everything to its owner.
// =====================================================================
// THE ZOOM FIELD (Part D): a QLineEdit that is read-only and takes no
// focus at rest (so the timeline's keys stay the timeline's), and becomes a
// real editor on double-click; Escape is reported to the owner, everything
// else is the line edit's own.
class ZoomField : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onDoubleClick;
    std::function<void()> onEscape;

protected:
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (isReadOnly()) {
            if (onDoubleClick)
                onDoubleClick();
            return;
        }
        QLineEdit::mouseDoubleClickEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape && !isReadOnly()) {
            if (onEscape)
                onEscape();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

class TimelineCanvas : public QWidget
{
public:
    explicit TimelineCanvas(AnimaticTimeline *owner)
        : QWidget(owner), m_owner(owner)
    {
        setMinimumHeight(kPanelY + kPanelMinH + kCameraH + kAudioH);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    QSize sizeHint() const override
    {
        return QSize(400, kPanelY + kPanelDefaultH + kCameraH + kAudioH);
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
    void wheelEvent(QWheelEvent *e) override { m_owner->canvasWheel(e); }

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
    setStyleSheet(QStringLiteral("background-color: %1;")
                      .arg(SankoTimelineTheme::kPageBg.name()));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- The zoom and view controls (shown in the header row) ----------------
    // The redesign merged the zoom row into the header. The widgets are
    // still the timeline's (the handlers below are unchanged); the page
    // places the two containers. Sizes are the design's (2026-10-23): Fit
    // 33 x 28, 16 px, then - 27 x 28, 3 px, the zoom FIELD 46 x 28, 3 px,
    // + 27 x 28; Frames / Seconds / Snap 32 x 28 with 24 px icons. The
    // slider is gone (Part D), and with it the header's give-way stage.
    m_zoomControls = new QWidget;
    m_zoomControls->setObjectName(QStringLiteral("timelineZoomControls"));
    // Transparent by name: these containers live in the page's header, whose
    // page background would otherwise show through them as dark boxes now
    // that the controls are flat (Part D).
    m_zoomControls->setStyleSheet(QStringLiteral("QWidget#timelineZoomControls { background: transparent; }"));
    QHBoxLayout *zc = new QHBoxLayout(m_zoomControls);
    zc->setContentsMargins(0, 0, 0, 0);
    zc->setSpacing(16);

    m_fitButton = new QPushButton(QStringLiteral("Fit"));
    m_fitButton->setObjectName(QStringLiteral("timelineFit"));
    m_fitButton->setFixedSize(33, 28);
    zc->addWidget(m_fitButton, 0, Qt::AlignVCenter);

    QWidget *zoomGroup = new QWidget;
    zoomGroup->setObjectName(QStringLiteral("timelineZoomGroup"));
    zoomGroup->setStyleSheet(QStringLiteral("QWidget#timelineZoomGroup { background: transparent; }"));
    QHBoxLayout *zg = new QHBoxLayout(zoomGroup);
    zg->setContentsMargins(0, 0, 0, 0);
    zg->setSpacing(3);
    m_zoomOutButton = new QPushButton(QStringLiteral("\xE2\x88\x92")); // minus
    m_zoomOutButton->setObjectName(QStringLiteral("timelineZoomOut"));
    m_zoomOutButton->setFixedSize(27, 28);
    auto *field = new ZoomField;
    m_zoomField = field;
    m_zoomField->setObjectName(QStringLiteral("timelineZoomField"));
    m_zoomField->setFixedSize(46, 28);
    m_zoomField->setAlignment(Qt::AlignCenter);
    m_zoomField->setReadOnly(true);
    m_zoomField->setFocusPolicy(Qt::NoFocus);
    m_zoomField->setContextMenuPolicy(Qt::NoContextMenu);
    m_zoomField->setToolTip(QStringLiteral("Zoom (double-click to type a percentage, 10 to 400)"));
    m_zoomField->setStyleSheet(SankoTimelineTheme::kZoomFieldSheet);
    field->onDoubleClick = [this] { beginZoomEdit(); };
    field->onEscape = [this] { endZoomEdit(false); };
    connect(m_zoomField, &QLineEdit::editingFinished, this, [this] {
        if (m_zoomEditing)
            endZoomEdit(true); // Enter, or the focus went elsewhere
    });
    m_zoomInButton = new QPushButton(QStringLiteral("+"));
    m_zoomInButton->setObjectName(QStringLiteral("timelineZoomIn"));
    m_zoomInButton->setFixedSize(27, 28);
    zg->addWidget(m_zoomOutButton);
    zg->addWidget(m_zoomField);
    zg->addWidget(m_zoomInButton);
    zc->addWidget(zoomGroup, 0, Qt::AlignVCenter);
    updateZoomField();

    m_viewControls = new QWidget;
    m_viewControls->setObjectName(QStringLiteral("timelineViewControls"));
    m_viewControls->setStyleSheet(QStringLiteral("QWidget#timelineViewControls { background: transparent; }"));
    QHBoxLayout *vc = new QHBoxLayout(m_viewControls);
    vc->setContentsMargins(0, 0, 0, 0);
    vc->setSpacing(14);
    m_framesButton = new QPushButton;
    m_framesButton->setObjectName(QStringLiteral("timelineFrames"));
    m_framesButton->setToolTip(QStringLiteral("Ruler in frames"));
    m_secondsButton = new QPushButton;
    m_secondsButton->setObjectName(QStringLiteral("timelineSeconds"));
    m_secondsButton->setToolTip(QStringLiteral("Ruler in timecode"));
    m_snapButton = new QPushButton;
    m_snapButton->setObjectName(QStringLiteral("timelineSnap"));
    m_snapButton->setToolTip(QStringLiteral("Snap"));
    for (QPushButton *b : {m_framesButton, m_secondsButton, m_snapButton}) {
        b->setFixedSize(32, 28);
        b->setIconSize(QSize(24, 24));
        vc->addWidget(b);
    }
    for (QPushButton *b : {m_fitButton, m_zoomOutButton, m_zoomInButton,
                           m_framesButton, m_secondsButton, m_snapButton})
        b->setCursor(Qt::PointingHandCursor);

    // --- Canvas + horizontal scrollbar -----------------------------------
    m_canvas = new TimelineCanvas(this);
    root->addWidget(m_canvas, 1); // the shots track takes the spare height
    buildTrackControls();

    m_hScroll = new QScrollBar(Qt::Horizontal);
    // Styled by the app-wide sheet (SankoScrollBarStyle.h) — this bar's
    // private copy of the style was the second of the two hand-styled
    // scrollbars and is retired with it. The track is transparent now
    // (was #0d0d0d) over the timeline's own dark fill, and the bar is the
    // shared 10 px instead of 12.
    root->addWidget(m_hScroll);

    styleToolbarButtons();

    // - and +: the same 25-point steps the slider had, clamped by applyZoom.
    connect(m_zoomInButton, &QPushButton::clicked, this,
            [this] { applyZoom(m_zoom + 25.0f); });
    connect(m_zoomOutButton, &QPushButton::clicked, this,
            [this] { applyZoom(m_zoom - 25.0f); });
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

// --- The track controls (Part B) ------------------------------------------

namespace {

// One icon with the two states the button shows: unchecked in the column's
// grey; checked in `on` - the accent for a LOCK (the lock is "on"), the
// grey at 35 % for the eye and the speaker (hidden and muted read as
// "off"; the user's rule, 2026-10-18, until eye-off / speaker-off icons
// are drawn). The tint composes SourceIn, so a translucent `on` dims the
// whole glyph.
QIcon trackIcon(const QString &file, const QColor &on)
{
    using namespace SankoTimelineTheme;
    QIcon icon;
    for (qreal scale : {1.0, 2.0}) {
        icon.addPixmap(SankoIcons::renderSvg(file, QSize(kControlSize, kControlSize),
                                             scale, kTrackControl),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(SankoIcons::renderSvg(file, QSize(kControlSize, kControlSize),
                                             scale, on),
                       QIcon::Normal, QIcon::On);
    }
    return icon;
}

QColor dimmed(const QColor &c)
{
    QColor d = c;
    d.setAlphaF(0.35);
    return d;
}

const char *trackName(TimelineTrack t)
{
    switch (t) {
    case TimelineTrack::Scenes: return "SCENES";
    case TimelineTrack::Shots: return "SHOTS";
    case TimelineTrack::Audio: return "AUDIO";
    case TimelineTrack::Camera: return "CAMERA";
    }
    return "";
}

// THE ANIMATE BUTTON (Figma 1C-3, the user's change of 2026-10-28): a small
// record dot, never a diamond, so it cannot be mistaken for a key - a grey
// ring when off, a filled accent dot when on.
QIcon recordDotIcon()
{
    using namespace SankoTimelineTheme;
    QIcon icon;
    for (qreal scale : {1.0, 2.0}) {
        for (bool on : {false, true}) {
            QPixmap pm(QSize(kControlSize, kControlSize) * scale);
            pm.setDevicePixelRatio(scale);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            const QPointF c(kControlSize / 2.0, kControlSize / 2.0);
            if (on) {
                p.setPen(Qt::NoPen);
                p.setBrush(kCameraAccent);
                p.drawEllipse(c, 4.0, 4.0);
            } else {
                p.setPen(QPen(kTrackControl, 1.3));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, 3.5, 3.5);
            }
            p.end();
            icon.addPixmap(pm, QIcon::Normal, on ? QIcon::On : QIcon::Off);
        }
    }
    return icon;
}

// The ease glyph (Figma 1C-2): the curve w(u) in an 18 x 12 pill - Linear
// a diagonal, Ease In flat then rising, Ease Out rising then flat, Ease
// In/Out both, Hold a step. Drawn in a box whose top-left is `r`.
void paintEaseGlyph(QPainter &p, const QRectF &r, shotcam::Ease ease)
{
    using namespace SankoTimelineTheme;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(kHairline, 1.0));
    p.setBrush(kPillBg);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    const auto pt = [&](qreal x, qreal y) { return QPointF(r.left() + x, r.top() + y); };
    QPainterPath path;
    path.moveTo(pt(3, 9));
    switch (ease) {
    case shotcam::Ease::Linear:
        path.lineTo(pt(15, 3));
        break;
    case shotcam::Ease::EaseIn:
        path.cubicTo(pt(9, 9), pt(12, 7), pt(15, 3));
        break;
    case shotcam::Ease::EaseOut:
        path.cubicTo(pt(6, 5), pt(9, 3), pt(15, 3));
        break;
    case shotcam::Ease::EaseInOut:
        path.cubicTo(pt(9, 9), pt(9, 3), pt(15, 3));
        break;
    case shotcam::Ease::Hold:
        path = QPainterPath();
        path.moveTo(pt(3, 8));
        path.lineTo(pt(13, 8));
        path.lineTo(pt(13, 3));
        path.lineTo(pt(15, 3));
        break;
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kCameraGlyph, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.restore();
}

} // namespace

QIcon AnimaticTimeline::easeGlyphIcon(int ease)
{
    QIcon icon;
    for (qreal scale : {1.0, 2.0}) {
        QPixmap pm(QSize(kGlyphW, kGlyphH) * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paintEaseGlyph(p, QRectF(0, 0, kGlyphW, kGlyphH), shotcam::Ease(ease));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

void AnimaticTimeline::buildTrackControls()
{
    if (!m_canvas)
        return;
    using namespace SankoTimelineTheme;
    auto make = [&](const QString &file, const QString &name, const QColor &on) {
        auto *b = new QToolButton(m_canvas);
        b->setObjectName(name);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIcon(trackIcon(file, on));
        b->setIconSize(QSize(kControlSize, kControlSize));
        b->setFixedSize(kControlSize + 4, kControlSize + 4);
        b->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; padding: 0; }"
            "QToolButton:hover { background: #1e1e26; border-radius: 4px; }"));
        return b;
    };
    const TimelineTrack tracks[4] = {TimelineTrack::Scenes, TimelineTrack::Shots,
                                     TimelineTrack::Audio, TimelineTrack::Camera};
    const char *suffix[4] = {"Scenes", "Shots", "Audio", "Camera"};
    for (int i = 0; i < 4; ++i) {
        const TimelineTrack t = tracks[i];
        m_lockButtons[i] = make(QStringLiteral(":/icons/timeline/track_lock.svg"),
                                QStringLiteral("trackLock") + QLatin1String(suffix[i]),
                                kTrackControlActive);
        connect(m_lockButtons[i], &QToolButton::clicked, this, [this, t](bool on) {
            if (m_host)
                m_host->setTrackLocked(t, on); // comes back through setTracks
        });
        m_eyeButtons[i] = make(QStringLiteral(":/icons/timeline/track_eye.svg"),
                               QStringLiteral("trackEye") + QLatin1String(suffix[i]),
                               dimmed(kTrackControl));
        connect(m_eyeButtons[i], &QToolButton::clicked, this, [this, t](bool on) {
            if (m_host)
                m_host->setTrackVisible(t, !on); // checked = hidden
        });
    }
    // The speaker (Part D): OPENS the volume popup; its checked look is the
    // muted glyph, shown while the volume is 0 % (proposal frame 423:492).
    m_muteButton = make(QStringLiteral(":/icons/timeline/track_speaker.svg"),
                        QStringLiteral("trackMuteAudio"), dimmed(kTrackControl));
    {
        QIcon speaker;
        for (qreal scale : {1.0, 2.0}) {
            speaker.addPixmap(SankoIcons::renderSvg(QStringLiteral(":/icons/timeline/track_speaker.svg"),
                                                    QSize(kControlSize, kControlSize), scale, kTrackControl),
                              QIcon::Normal, QIcon::Off);
            speaker.addPixmap(SankoIcons::renderSvg(QStringLiteral(":/icons/timeline/speaker_muted.svg"),
                                                    QSize(kControlSize, kControlSize), scale, kTrackControl),
                              QIcon::Normal, QIcon::On);
        }
        m_muteButton->setIcon(speaker);
    }
    connect(m_muteButton, &QToolButton::clicked, this, [this] {
        toggleVolumePopup();
        styleTrackControls(); // a checkable button toggles itself on click: put the muted state back
    });
    // The CAMERA row's Animate (Phase 1C): the canvas Camera bar's switch,
    // the same state, owned by the workspace - a click asks, and the answer
    // comes back through setCameraAnimate. Its hover is the accent at 14 %
    // (the flat-controls rule, Figma 1C-3).
    m_animateButton = new QToolButton(m_canvas);
    m_animateButton->setObjectName(QStringLiteral("trackAnimateCamera"));
    m_animateButton->setCheckable(true);
    m_animateButton->setAutoRaise(true);
    m_animateButton->setCursor(Qt::PointingHandCursor);
    m_animateButton->setFocusPolicy(Qt::NoFocus);
    m_animateButton->setIcon(recordDotIcon());
    m_animateButton->setIconSize(QSize(kControlSize, kControlSize));
    m_animateButton->setFixedSize(kControlSize + 4, kControlSize + 4);
    m_animateButton->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; background: transparent; padding: 0; }"
        "QToolButton:hover { background: %1; border-radius: 4px; }")
                                       .arg(kFlatHoverBg));
    connect(m_animateButton, &QToolButton::clicked, this, [this](bool on) {
        m_animateButton->setChecked(m_cameraAnimate); // until the answer
        emit cameraAnimateToggled(on);
    });

    // The volume popup: a vertical slider, a child of the canvas, hidden
    // until the speaker opens it; placed by layoutVolumePopup (x 34 of the
    // column, rising 128 px from the speaker, or less on a short section).
    m_volumePopup = new QSlider(Qt::Vertical, m_canvas);
    m_volumePopup->setObjectName(QStringLiteral("trackVolumePopup"));
    m_volumePopup->setRange(0, 100);
    m_volumePopup->setValue(m_tracks.audioVolume);
    m_volumePopup->setFixedWidth(10);
    m_volumePopup->setStyleSheet(kVolumeSliderSheet);
    m_volumePopup->setFocusPolicy(Qt::StrongFocus);
    m_volumePopup->setToolTip(QStringLiteral("Audio volume"));
    m_volumePopup->hide();
    m_volumePopup->installEventFilter(this); // Escape and a lost focus close it
    connect(m_volumePopup, &QSlider::valueChanged, this, [this](int v) {
        if (!m_syncingVolume && m_host)
            m_host->setAudioVolume(v); // applied to the player at once; comes back through setTracks
    });
    styleTrackControls();
    layoutTrackControls();
}

void AnimaticTimeline::layoutTrackControls()
{
    if (!m_canvas || !m_lockButtons[0])
        return;
    const int rowY[4] = {kSceneY, kPanelY, audioTrackY(), cameraTrackY()};
    const int rowH[4] = {kSceneH, panelTrackH(), kAudioH, kCameraH};
    for (int i = 0; i < 4; ++i) {
        // The 16 px icon centred in the row at the design's x; the button
        // is 2 px larger on every side.
        const int y = rowY[i] + (rowH[i] - kControlSize) / 2 - 2;
        m_lockButtons[i]->move(kControlX[0] - 2, y);
        m_eyeButtons[i]->move(kControlX[2] - 2, y);
        if (i == 2)
            m_muteButton->move(kControlX[1] - 2, y);
        if (i == 3 && m_animateButton)
            m_animateButton->move(kControlX[1] - 2, y);
    }
    layoutVolumePopup();
}

// The popup rises from the speaker's icon top: x 34 of the column (the
// user's drawing), 10 wide, 128 tall - or as tall as the column allows,
// ending 10 px below its top, so it never leaves the column.
void AnimaticTimeline::layoutVolumePopup()
{
    if (!m_volumePopup)
        return;
    const int iconTop = audioTrackY() + (kAudioH - kControlSize) / 2;
    const int h = qMax(24, qMin(128, iconTop - 10));
    m_volumePopup->setGeometry(34, iconTop - h, 10, h);
}

void AnimaticTimeline::toggleVolumePopup()
{
    if (!m_volumePopup)
        return;
    if (m_volumePopup->isVisible()) {
        m_volumePopup->hide();
        return;
    }
    layoutVolumePopup();
    {
        m_syncingVolume = true;
        m_volumePopup->setValue(m_tracks.audioVolume);
        m_syncingVolume = false;
    }
    m_volumePopup->show();
    m_volumePopup->raise();
    m_volumePopup->setFocus(Qt::MouseFocusReason);
}

void AnimaticTimeline::setVolumeTooltip(const QString &tip)
{
    if (m_volumePopup)
        m_volumePopup->setToolTip(tip);
}

void AnimaticTimeline::styleTrackControls()
{
    if (!m_lockButtons[0])
        return;
    const TimelineTrack tracks[4] = {TimelineTrack::Scenes, TimelineTrack::Shots,
                                     TimelineTrack::Audio, TimelineTrack::Camera};
    for (int i = 0; i < 4; ++i) {
        const TimelineTrack t = tracks[i];
        const QString name = QLatin1String(trackName(t));
        const bool locked = m_tracks.locked(t);
        const bool visible = m_tracks.visible(t);
        m_lockButtons[i]->setChecked(locked);
        m_lockButtons[i]->setToolTip(
            locked ? QStringLiteral("Unlock the %1 track").arg(name)
                   : t == TimelineTrack::Scenes
                ? QStringLiteral("Lock the SCENES track: the Script Editor cannot "
                                 "replace the scenes while it is locked")
                : t == TimelineTrack::Shots
                    ? QStringLiteral("Lock the SHOTS track: no panel can be added, "
                                     "duplicated, deleted, moved or retimed; drawing "
                                     "stays allowed")
                : t == TimelineTrack::Camera
                    ? QStringLiteral("Lock the CAMERA track: no camera key can be "
                                     "added, moved, changed or deleted, here or with "
                                     "the Camera tool")
                    : QStringLiteral("Lock the AUDIO track: the file cannot be "
                                     "imported, removed, located or copied"));
        m_eyeButtons[i]->setChecked(!visible);
        // The CAMERA eye is not the timeline's only: hidden, the camera is
        // off in the preview, the clips and every export too.
        m_eyeButtons[i]->setToolTip(
            t == TimelineTrack::Camera
                ? (visible ? QStringLiteral("Hide the CAMERA track: the preview, the "
                                            "SHOTS clips and every export show the "
                                            "whole panel")
                           : QStringLiteral("Show the CAMERA track: the camera moves "
                                            "again in the preview, the clips and the "
                                            "exports"))
            : visible ? QStringLiteral("Hide the %1 track (the timeline only: playback "
                                       "and export are not affected)")
                            .arg(name)
                      : QStringLiteral("Show the %1 track").arg(name));
    }
    if (m_animateButton) {
        m_animateButton->setChecked(m_cameraAnimate);
        m_animateButton->setToolTip(
            m_cameraAnimate
                ? QStringLiteral("Animate is on: camera changes on the canvas add or "
                                 "change a key at the playhead. Same switch as the "
                                 "Camera bar.")
                : QStringLiteral("Animate: camera changes on the canvas add keys at "
                                 "the playhead. Same switch as the Camera bar."));
    }
    m_muteButton->setChecked(m_tracks.audioMuted());
    m_muteButton->setToolTip(
        m_tracks.audioMuted()
            ? QStringLiteral("Audio volume 0 % (muted: silent in playback, left out "
                             "of the MP4). Click to open the volume.")
            : QStringLiteral("Audio volume %1 %. Click to open the volume.")
                  .arg(m_tracks.audioVolume));
}

void AnimaticTimeline::setTracks(const TimelineTracks &tracks)
{
    m_tracks = tracks;
    styleTrackControls();
    if (m_volumePopup && m_volumePopup->value() != m_tracks.audioVolume) {
        m_syncingVolume = true;
        m_volumePopup->setValue(m_tracks.audioVolume);
        m_syncingVolume = false;
    }
    // A hidden SHOTS track ends any drag or hover on it.
    if (!m_tracks.shotsVisible) {
        if (m_drag == Drag::Move || m_drag == Drag::ResizeRight
            || m_drag == Drag::ResizeLeft)
            m_drag = Drag::None;
        m_moveActive = false;
        m_hoverIndex = -1;
    }
    // ...and a hidden or locked CAMERA track any key drag.
    if ((!m_tracks.cameraVisible || m_tracks.cameraLocked) && m_drag == Drag::CameraKey) {
        m_drag = Drag::None;
        m_camDragActive = false;
        m_camDragScene = m_camDragKey = -1;
    }
    if (!m_tracks.cameraVisible)
        m_camHoverScene = m_camHoverKey = -1;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setCameraAnimate(bool on)
{
    m_cameraAnimate = on;
    styleTrackControls();
}

bool AnimaticTimeline::selectedCameraKey(int *sceneIndex, int *keyIndex) const
{
    if (m_camSelScene < 0 || m_camSelScene >= m_scenes.size())
        return false;
    const Scene *scene = m_scenes.at(m_camSelScene);
    if (m_camSelKey < 0 || m_camSelKey >= scene->camera.keys.size()
        || shotcam::keyFrame(*scene, scene->camera.keys.at(m_camSelKey), m_fps) < 0.0)
        return false; // gone (deleted, or its panel is)
    if (sceneIndex)
        *sceneIndex = m_camSelScene;
    if (keyIndex)
        *keyIndex = m_camSelKey;
    return true;
}

void AnimaticTimeline::setSelectedCameraKey(int sceneIndex, int keyIndex)
{
    m_camSelScene = sceneIndex;
    m_camSelKey = keyIndex;
    if (m_canvas)
        m_canvas->update();
}

QToolButton *AnimaticTimeline::trackControlForTest(const QString &objectName) const
{
    return m_canvas ? m_canvas->findChild<QToolButton *>(objectName) : nullptr;
}

// A press the lock refuses: say why at the pointer (the same tooltip path
// the trim drag uses) and remember it for the gate.
void AnimaticTimeline::refuse(const QString &why, const QPoint &globalPos)
{
    m_lastRefusal = why;
    QToolTip::showText(globalPos, why, m_canvas);
}

// --- Track geometry -------------------------------------------------------

int AnimaticTimeline::tracksH() const
{
    return qMax(kPanelY + kPanelMinH + kCameraH + kAudioH,
                m_canvas ? m_canvas->height() : 0);
}

int AnimaticTimeline::panelTrackH() const
{
    return tracksH() - kPanelY - kCameraH - kAudioH;
}

int AnimaticTimeline::cameraTrackY() const
{
    return kPanelY + panelTrackH();
}

int AnimaticTimeline::audioTrackY() const
{
    return cameraTrackY() + kCameraH;
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

// The look of the six controls. Frames / Seconds / Snap are icon-only now:
// checked = the accent fill with a white icon, unchecked = no fill and the
// icon in the design's grey. The state itself is what it always was (the
// sheet swap on m_framesMode and m_snap); only the sheets changed.
void AnimaticTimeline::styleToolbarButtons()
{
    using namespace SankoTimelineTheme;
    m_fitButton->setStyleSheet(kZoomButtonSheet);
    m_zoomInButton->setStyleSheet(kZoomButtonSheet);
    m_zoomOutButton->setStyleSheet(kZoomButtonSheet);
    const struct { QPushButton *button; const char *file; bool checked; } views[] = {
        {m_framesButton, "view_frames", m_framesMode},
        {m_secondsButton, "view_seconds", !m_framesMode},
        {m_snapButton, "view_snap", m_snap},
    };
    for (const auto &v : views) {
        v.button->setStyleSheet(v.checked ? kViewButtonCheckedSheet : kViewButtonSheet);
        v.button->setIcon(SankoIcons::icon(
            QStringLiteral(":/icons/timeline/%1.svg").arg(QLatin1String(v.file)),
            QSize(24, 24), v.checked ? kViewIconChecked : kViewIcon));
    }
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
    // A reorder drag armed against the old block list means nothing now;
    // nor does a camera key drag.
    if (m_drag == Drag::Move) {
        m_drag = Drag::None;
        m_moveActive = false;
        m_moveFrom = -1;
        m_moveGap = -1;
    }
    if (m_drag == Drag::CameraKey) {
        m_drag = Drag::None;
        m_camDragActive = false;
        m_camDragScene = m_camDragKey = -1;
    }
    m_camHoverScene = m_camHoverKey = -1;
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
    // A selected camera key stays selected only while its clip is the
    // selected one (a key on a cut belongs to both clips): picking another
    // panel, anywhere, ends it.
    int si = -1, ki = -1;
    if (selectedCameraKey(&si, &ki)) {
        const Scene *scene = m_scenes.at(si);
        const int start = sceneStartFrame(si);
        const double f = start + shotcam::keyFrame(*scene, scene->camera.keys.at(ki), m_fps);
        const bool onIt = flatIndex >= 0 && flatIndex < m_blocks.size()
            && f >= m_blocks.at(flatIndex).startFrame - 0.5
            && f <= m_blocks.at(flatIndex).startFrame + m_blocks.at(flatIndex).frames + 0.5;
        if (!onIt)
            m_camSelScene = m_camSelKey = -1;
    }
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
    if (m_volumePopup && watched == m_volumePopup) {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            m_volumePopup->hide();
            return true;
        }
        if (event->type() == QEvent::FocusOut)
            m_volumePopup->hide(); // a click elsewhere, or the keyboard moved on
    }
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
             m_followTimer && m_followTimer->isActive()},
            // The tracks' lock / visibility / mute (Part B).
            {QStringLiteral("timeline.scenesLocked"), m_tracks.scenesLocked},
            {QStringLiteral("timeline.shotsLocked"), m_tracks.shotsLocked},
            {QStringLiteral("timeline.audioLocked"), m_tracks.audioLocked},
            {QStringLiteral("timeline.scenesVisible"), m_tracks.scenesVisible},
            {QStringLiteral("timeline.shotsVisible"), m_tracks.shotsVisible},
            {QStringLiteral("timeline.audioVisible"), m_tracks.audioVisible},
            {QStringLiteral("timeline.audioVolume"), m_tracks.audioVolume},
            // The CAMERA row (Phase 1C): its lock and eye, the Animate it
            // shows, the selected key ("scene:key", empty for none) and
            // whether a key is being dragged.
            {QStringLiteral("timeline.cameraLocked"), m_tracks.cameraLocked},
            {QStringLiteral("timeline.cameraVisible"), m_tracks.cameraVisible},
            {QStringLiteral("timeline.cameraAnimate"), m_cameraAnimate},
            {QStringLiteral("timeline.cameraKey"),
             selectedCameraKey(nullptr, nullptr)
                 ? QStringLiteral("%1:%2").arg(m_camSelScene).arg(m_camSelKey)
                 : QString()},
            {QStringLiteral("timeline.cameraDrag"), m_drag == Drag::CameraKey && m_camDragActive}};
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

// The frame inside the scrubbed panel (Phase 1A): sent on the press, on
// every move that changes it, and on the release.
void AnimaticTimeline::emitScrubFrame()
{
    if (m_dragPlayheadFrame < 0)
        return;
    const int idx = blockAtFrame(m_dragPlayheadFrame);
    if (idx < 0 || idx >= m_blocks.size())
        return;
    const Block &b = m_blocks.at(idx);
    const int frameIn = qBound(0, m_dragPlayheadFrame - b.startFrame, b.frames - 1);
    if (b.startFrame + frameIn == m_scrubFrameSent)
        return;
    m_scrubFrameSent = b.startFrame + frameIn;
    emit playheadScrubbedTo(idx, frameIn);
}

shotcam::CameraState AnimaticTimeline::clipCameraForTest(int flatIndex) const
{
    if (flatIndex < 0 || flatIndex >= m_blocks.size())
        return shotcam::CameraState();
    const Block &b = m_blocks.at(flatIndex);
    if (b.sceneIndex >= m_scenes.size())
        return shotcam::CameraState();
    const Scene *scene = m_scenes.at(b.sceneIndex);
    if (!m_tracks.cameraVisible && b.panelIndex < scene->panels.size())
        return shotcam::neutral(scene->panels.at(b.panelIndex)->canvasSize()); // the eye is off
    return shotcam::evaluateAtPanel(*scene, b.panelIndex, 0, m_fps);
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
    updateZoomField(); // whoever changed it: Fit, - / +, Ctrl+wheel, the field
    emit zoomChanged(m_zoom);
}

// --- The zoom field (Part D) ---------------------------------------------------

void AnimaticTimeline::updateZoomField()
{
    if (!m_zoomField || m_zoomEditing)
        return;
    m_zoomField->setText(QStringLiteral("%1%").arg(qRound(m_zoom)));
}

QString AnimaticTimeline::zoomFieldTextForTest() const
{
    return m_zoomField ? m_zoomField->text() : QString();
}

void AnimaticTimeline::beginZoomEdit()
{
    if (!m_zoomField || m_zoomEditing)
        return;
    m_zoomEditing = true;
    m_zoomTextBeforeEdit = m_zoomField->text();
    m_zoomField->setReadOnly(false);
    m_zoomField->setFocusPolicy(Qt::StrongFocus);
    m_zoomField->setStyleSheet(SankoTimelineTheme::kZoomFieldEditSheet);
    m_zoomField->setFocus(Qt::MouseFocusReason);
    m_zoomField->selectAll();
}

// Enter and a click elsewhere apply (editingFinished); Escape cancels. The
// text may be "150" or "150%" with spaces; out of range clamps to 10 or
// 400; anything else restores the value the field showed before.
void AnimaticTimeline::endZoomEdit(bool apply)
{
    if (!m_zoomField || !m_zoomEditing)
        return;
    m_zoomEditing = false; // before clearFocus: its editingFinished is ignored
    if (apply) {
        QString t = m_zoomField->text().trimmed();
        if (t.endsWith(QLatin1Char('%')))
            t.chop(1);
        t = t.trimmed();
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (ok)
            applyZoom(static_cast<float>(qBound(10.0, v, 400.0)));
    }
    m_zoomField->setReadOnly(true);
    m_zoomField->setFocusPolicy(Qt::NoFocus);
    m_zoomField->clearFocus();
    m_zoomField->setStyleSheet(SankoTimelineTheme::kZoomFieldSheet);
    m_zoomField->deselect();
    updateZoomField(); // the real zoom, whether applied, clamped or restored
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

    // Backgrounds per track (full width first), the redesign's: the ruler
    // and the empty audio row in kRulerBg, the scene track in kHeaderBg, the
    // shots track in the page colour. A row with a file named keeps the
    // blue it had (the design shows no track).
    using namespace SankoTimelineTheme;
    p.fillRect(QRect(0, kRulerY, W, kRulerH), kRulerBg);
    p.fillRect(QRect(0, kSceneY, W, kSceneH), kHeaderBg);
    p.fillRect(QRect(0, kPanelY, W, panelTrackH()), kPageBg);
    p.fillRect(QRect(0, cameraTrackY(), W, kCameraH), kCameraRowBg);
    p.fillRect(QRect(0, cameraTrackY(), W, 1), kHairline);
    p.fillRect(QRect(0, audioTrackY(), W, kAudioH),
               m_audioLoaded ? QColor("#0d1a26") : kRulerBg);

    // Clip all time-based content to the area right of the label column.
    p.save();
    p.setClipRect(QRect(kLabelCol, 0, W - kLabelCol, tracksH()));

    const double ppf = pxPerFrame();
    const double pps = pxPerSecond();

    // ----- Timecode ruler (the redesign) -----
    // Ticks hang from the ruler's bottom edge in one colour at three
    // strengths: 7 px at 65 % on a labelled second, 5 px at 30 % on the
    // other seconds, 3 px at 18 % on the half seconds (only when a second is
    // 30 px or wider). Labels every rulerLabelStep() seconds, Courier New
    // 11 px, 4 px right of the tick, in a 15 px line 2 px down. Below 8 px
    // per second no second ticks are drawn (they would be a comb).
    {
        QFont f(QStringLiteral("Courier New"));
        f.setPixelSize(11);
        p.setFont(f);
        const int step = rulerLabelStep();
        const int tSecs = totalSeconds();
        const bool secondTicks = pps >= 8.0;
        const bool halfTicks = pps >= 30.0;
        const int bottom = kRulerY + kRulerH;
        QColor major = kHairline, minor = kHairline, half = kHairline;
        major.setAlphaF(0.65);
        minor.setAlphaF(0.30);
        half.setAlphaF(0.18);
        if (halfTicks) {
            for (int s2 = 1; s2 <= tSecs * 2; s2 += 2) {
                const int x = contentXToScreen((s2 * m_fps / 2.0) * ppf);
                if (x < kLabelCol - 2 || x > contentRight) continue;
                p.fillRect(QRect(x, bottom - 3, 1, 3), half);
            }
        }
        for (int s = 0; s <= tSecs; ++s) {
            const int x = contentXToScreen(s * pps);
            if (x < kLabelCol - 120 || x > contentRight + 10) continue;
            const bool labelled = (s % step == 0);
            if (secondTicks)
                p.fillRect(QRect(x, bottom - (labelled ? 7 : 5), 1, labelled ? 7 : 5),
                           labelled ? major : minor);
            if (labelled) {
                p.setPen(kRulerLabel);
                const QString lbl = m_framesMode
                    ? QString::number(s * m_fps)
                    : timecode(s * m_fps);
                p.drawText(QRect(x + 4, kRulerY + 2, 110, 15),
                           Qt::AlignVCenter | Qt::AlignLeft, lbl);
            }
        }
    }

    // ----- Scene track -----
    // A HIDDEN track (Part B) paints its background and nothing on it.
    if (m_tracks.scenesVisible) {
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
            // One block colour in the design (it shows one scene); the
            // ordinal is kept for when a second colour is given.
            Q_UNUSED(sceneOrdinal);
            p.fillRect(r, kSceneBlock);
            p.setPen(kSceneBaseline);
            p.drawLine(r.left(), kSceneY + kSceneH - 1, r.right(), kSceneY + kSceneH - 1);
            // Scene separator.
            p.setPen(kSceneSeparator);
            p.drawLine(x0, kSceneY, x0, kSceneY + kSceneH);
            p.setFont(f);
            p.setPen(kSceneName);
            p.drawText(r, Qt::AlignCenter, name);
            i = j;
            ++sceneOrdinal;
        }
    }

    // ----- Panel / shot clips (the redesign) -----
    // Flat bodies with a 2 px gap between neighbours, the thumbnail across
    // the top with the number over it, the name bottom-left and the
    // duration pill bottom-right; the selected clip is a lighter body with
    // a 2 px border, nothing else (the design's glow and trim handles are
    // hidden layers, not built; the 6 px grab zones at the edges stay, with
    // the size cursor as their only cue). Layout: clipLayout().
    // A hidden SHOTS track paints no clip (and, below, no loop overlay and
    // no drop indicator; a press on it finds nothing - canvasMousePress).
    if (m_tracks.shotsVisible) {
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont nameFont = font();
        nameFont.setPixelSize(11);
        QFont numberFont = nameFont;
        numberFont.setBold(true);
        QFont mono(QStringLiteral("Consolas"));
        mono.setPixelSize(10);

        for (const Block &b : m_blocks) {
            const int x0 = contentXToScreen(b.startFrame * ppf);
            const int x1 = contentXToScreen((b.startFrame + b.frames) * ppf);
            if (x1 < kLabelCol || x0 > contentRight) continue;
            const QRect r(x0, kPanelY + 2, x1 - x0, panelTrackH() - 4);
            const ClipLayout L = clipLayout(r);
            const QRectF bf(L.body);

            // "current" is the SELECTED panel - the one on the drawing
            // canvas - not the one under the playhead: playback moves the
            // playhead through the clips and leaves the selection alone.
            const bool current = (b.flatIndex == m_selected);
            const bool hovered = (b.flatIndex == m_hoverIndex);

            p.setPen(Qt::NoPen);
            p.setBrush(current ? kClipSelectedBg : kClipBg);
            p.drawRoundedRect(bf, kClipR, kClipR);
            p.setBrush(Qt::NoBrush);
            if (current) {
                p.setPen(QPen(kClipSelectedBorder, 2));
                p.drawRoundedRect(bf.adjusted(1, 1, -1, -1), kClipR, kClipR);
            } else {
                QColor bord = kClipBorder;
                if (hovered) { bord = kClipSelectedBorder; bord.setAlphaF(0.50); }
                p.setPen(QPen(bord, 1));
                p.drawRoundedRect(bf.adjusted(0.5, 0.5, -0.5, -0.5), kClipR, kClipR);
            }

            if (L.thumbShown) {
                QPixmap pix;
                // THE SHOT CAMERA (Phase 1A): the clip shows the cut as it
                // begins - the camera at the panel's first frame. A scene
                // without a camera move draws exactly as before.
                bool shot = false;
                shotcam::CameraState shotCamera;
                QSize canvas;
                if (b.sceneIndex < m_scenes.size()) {
                    Scene *sc = m_scenes.at(b.sceneIndex);
                    if (b.panelIndex < sc->panels.size()) {
                        // The cached 512px mip, not the full flatten: this
                        // runs per visible clip on EVERY repaint (each scrub
                        // mouse-move and every playback tick); the full
                        // composite cost 16 ms/clip at 4K, measured.
                        pix = sc->panels.at(b.panelIndex)->flattenedThumb();
                        // The CAMERA eye off (Phase 1C): the whole panel.
                        if (!sc->camera.isEmpty() && m_tracks.cameraVisible) {
                            canvas = sc->panels.at(b.panelIndex)->canvasSize();
                            shotCamera = shotcam::evaluateAtPanel(*sc, b.panelIndex, 0,
                                                                  m_fps);
                            shot = !shotcam::isNeutral(shotCamera, canvas);
                        }
                    }
                }
                p.save();
                QPainterPath clip;
                clip.addRoundedRect(QRectF(L.thumb), kThumbR, kThumbR);
                p.setClipPath(clip, Qt::IntersectClip);
                if (!pix.isNull() && shot) {
                    // The shot, cropped to fill like the plain thumbnail.
                    p.fillRect(L.thumb, Qt::black);
                    shotcam::drawShot(p, pix, canvas, shotCamera, QRectF(L.thumb), true);
                } else if (!pix.isNull()) {
                    // Crop to fill, centred (one setting; "fit with bars" is
                    // the other, not built - the user's choice 2026-10-18).
                    const QPixmap scaled = pix.scaled(
                        L.thumb.size(), Qt::KeepAspectRatioByExpanding,
                        Qt::SmoothTransformation);
                    const int sx = (scaled.width() - L.thumb.width()) / 2;
                    const int sy = (scaled.height() - L.thumb.height()) / 2;
                    p.drawPixmap(L.thumb, scaled,
                                 QRect(sx, sy, L.thumb.width(), L.thumb.height()));
                } else {
                    p.fillRect(L.thumb, kThumbPlaceholder);
                }
                p.restore();
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kHairline, 1));
                p.drawRoundedRect(QRectF(L.thumb).adjusted(0.5, 0.5, -0.5, -0.5),
                                  kThumbR, kThumbR);
            }

            if (L.body.width() >= kNumberMinBody) {
                p.setFont(numberFont);
                p.setPen(kClipNumber);
                p.drawText(L.number, Qt::AlignTop | Qt::AlignLeft,
                           QString::number(b.flatIndex + 1));
            }
            if (L.nameShown) {
                p.setFont(nameFont);
                p.setPen(kClipName);
                const QFontMetrics fm(nameFont);
                p.drawText(L.name, Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(b.sceneName, Qt::ElideRight, L.name.width()));
            }
            if (L.pillShown) {
                p.setPen(QPen(kHairline, 1));
                p.setBrush(kPillBg);
                p.drawRoundedRect(QRectF(L.pill).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                p.setBrush(Qt::NoBrush);
                p.setFont(mono);
                p.setPen(kPillText);
                p.drawText(L.pill, Qt::AlignCenter, QStringLiteral("%1s").arg(b.duration));
            }
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ----- Loop region overlay (on the panel track) -----
    if (m_tracks.shotsVisible && m_loopStart >= 0 && m_loopEnd >= 0
        && m_loopStart < m_blocks.size()
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
    if (m_tracks.shotsVisible && m_drag == Drag::Move && m_moveActive
        && m_moveGap >= 0 && !m_blocks.isEmpty()) {
        const int frame = m_moveGap < m_blocks.size()
            ? m_blocks.at(m_moveGap).startFrame
            : m_blocks.last().startFrame + m_blocks.last().frames;
        const int x = contentXToScreen(frame * ppf);
        p.setPen(QPen(kClipSelectedBorder, 3)); // the design's selection colour
        p.drawLine(x, kPanelY + 1, x, kPanelY + panelTrackH() - 1);
    }

    // ----- The CAMERA row (Phase 1C) -----
    paintCameraRow(p);

    // ----- Audio track -----
    // A PLAIN BAR, as long as the audio, with the file's name on it. This
    // used to draw a sine wave under an envelope - a picture of a waveform
    // that had nothing to do with the audio's own, on the track that claims
    // to show the audio. A bar says only what is known: there is a track,
    // this is its file, it runs this long. (A real waveform is a separate,
    // later piece of work.) A hidden AUDIO track paints no bar and no hint.
    if (m_tracks.audioVisible) {
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
            p.setPen(QPen(kPlayhead, 1.5));
            p.drawLine(px, 0, px, tracksH());
            // Triangle handle in the ruler.
            p.setRenderHint(QPainter::Antialiasing, true);
            QPolygon tri;
            tri << QPoint(px - 5, 0) << QPoint(px + 5, 0) << QPoint(px, 8);
            p.setBrush(kPlayhead);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    // ----- Left label column (painted on top, fixed) -----
    // 136 px, its separator on the content's first pixel; the labels end
    // 15 px before the column's edge; the track controls (lock, speaker on
    // AUDIO, eye) are 16 px icons centred in their rows - real buttons
    // since Part B (buildTrackControls), children of the canvas.
    {
        p.fillRect(QRect(0, 0, kLabelColW, tracksH()), kLabelColumnBg);
        p.fillRect(QRect(kLabelColW, 0, 1, tracksH()), kLabelColumnSeparator);

        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);
        p.setPen(kTrackLabel);
        struct TL { const char *name; int y, h; };
        const TL labels[] = {
            {"TIMECODE", kRulerY, kRulerH}, {"SCENES", kSceneY, kSceneH},
            {"SHOTS", kPanelY, panelTrackH()},    {"CAMERA", cameraTrackY(), kCameraH},
            {"AUDIO", audioTrackY(), kAudioH},
        };
        for (const TL &t : labels)
            p.drawText(QRect(0, t.y, kLabelColW - 15, t.h),
                       Qt::AlignVCenter | Qt::AlignRight,
                       QString::fromLatin1(t.name));

        // The lock / speaker / eye controls are real buttons over this
        // column (buildTrackControls), not painted here any more.
    }

    // Empty-audio hint. It names the thing that works: this used to say
    // "Drop audio file here", and nothing here accepts a drop.
    if (m_tracks.audioVisible && audioBar().state == AudioBar::None) {
        QFont f = font();
        f.setPixelSize(10);
        p.setFont(f);
        p.setPen(kAudioHint);
        p.drawText(QRect(kLabelCol, audioTrackY(), W - kLabelCol, kAudioH),
                   Qt::AlignCenter, audioBar().hint);
    }
}

// The ruler's label step (see rulerLabelStep in the header).
int AnimaticTimeline::rulerLabelStep() const
{
    const double pps = pxPerSecond();
    for (int step : {1, 2, 3, 5, 10, 15, 30, 60})
        if (step * pps >= 120.0)
            return step;
    return 60;
}

// The design's clip layout for a clip rect r (see the constants at the top).
AnimaticTimeline::ClipLayout AnimaticTimeline::clipLayout(const QRect &r) const
{
    ClipLayout L;
    L.body = QRect(r.left(), r.top(), qMax(1, r.width() - kClipGap), r.height());
    const int bw = L.body.width();
    L.thumbShown = bw >= kThumbMinBody;
    L.thumb = QRect(L.body.left() + kThumbInset, L.body.top() + kThumbInset,
                    bw - 2 * kThumbInset, kThumbH);
    L.number = QRect(L.body.left() + 10, L.body.top() + 9, qMax(1, bw - 14), 12);
    L.pillShown = bw >= kPillMinBody;
    L.pill = QRect(L.body.right() - kPillW - 5, L.body.bottom() - kPillH - 4, kPillW, kPillH);
    const int nameRight = L.pillShown ? L.pill.left() - 4 : L.body.right() - 8;
    L.name = QRect(L.body.left() + 8, L.body.bottom() - 20,
                   qMax(0, nameRight - (L.body.left() + 8)), 15);
    L.nameShown = bw >= kNameMinBody && L.name.width() > 0;
    return L;
}

QVariantMap AnimaticTimeline::clipPartsForTest(int flatIndex) const
{
    const QRect r = clipRect(flatIndex);
    if (r.isEmpty())
        return {};
    const ClipLayout L = clipLayout(r);
    const auto rs = [](const QRect &q) {
        return QStringLiteral("%1,%2 %3x%4").arg(q.x()).arg(q.y()).arg(q.width()).arg(q.height());
    };
    return {{QStringLiteral("clip"), rs(r)},       {QStringLiteral("body"), rs(L.body)},
            {QStringLiteral("thumb"), rs(L.thumb)}, {QStringLiteral("number"), rs(L.number)},
            {QStringLiteral("name"), rs(L.name)},   {QStringLiteral("pill"), rs(L.pill)},
            {QStringLiteral("thumbShown"), L.thumbShown},
            {QStringLiteral("nameShown"), L.nameShown},
            {QStringLiteral("pillShown"), L.pillShown}};
}

int AnimaticTimeline::labelColumnWidthForTest()
{
    return kLabelColW;
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

// --- The CAMERA row (Phase 1C) ---------------------------------------------

int AnimaticTimeline::sceneStartFrame(int sceneIndex) const
{
    for (const Block &b : m_blocks)
        if (b.sceneIndex == sceneIndex)
            return b.startFrame;
    return -1;
}

int AnimaticTimeline::sceneEndFrame(int sceneIndex) const
{
    int end = -1;
    for (const Block &b : m_blocks)
        if (b.sceneIndex == sceneIndex)
            end = b.startFrame + b.frames;
    return end;
}

QVector<AnimaticTimeline::CameraMark> AnimaticTimeline::cameraMarks() const
{
    QVector<CameraMark> marks;
    const double ppf = pxPerFrame();
    for (int si = 0; si < m_scenes.size(); ++si) {
        const Scene *scene = m_scenes.at(si);
        const int start = sceneStartFrame(si);
        if (scene->camera.isEmpty() || start < 0)
            continue;
        QVector<CameraMark> sceneMarks;
        for (int k = 0; k < scene->camera.keys.size(); ++k) {
            const shotcam::CameraKey &key = scene->camera.keys.at(k);
            double f = shotcam::keyFrame(*scene, key, m_fps);
            if (f < 0.0)
                continue; // inert: its panel is gone (its delete still undoable)
            if (m_drag == Drag::CameraKey && m_camDragActive && si == m_camDragScene
                && k == m_camDragKey)
                f = m_camDragFrame - start; // where the drag has it
            CameraMark m;
            m.sceneIndex = si;
            m.keyIndex = k;
            m.frame = start + f;
            m.x = contentXToScreen(m.frame * ppf);
            m.ease = int(key.ease);
            sceneMarks.append(m);
        }
        std::stable_sort(sceneMarks.begin(), sceneMarks.end(),
                         [](const CameraMark &a, const CameraMark &b) { return a.frame < b.frame; });
        marks += sceneMarks;
    }
    return marks;
}

QVector<AnimaticTimeline::CameraSegment>
AnimaticTimeline::cameraSegments(const QVector<CameraMark> &marks) const
{
    QVector<CameraSegment> segments;
    int selScene = -1, selKey = -1;
    const bool hasSel = selectedCameraKey(&selScene, &selKey);
    for (int i = 0; i + 1 < marks.size(); ++i) {
        const CameraMark &a = marks.at(i);
        const CameraMark &b = marks.at(i + 1);
        if (a.sceneIndex != b.sceneIndex)
            continue; // no move crosses a cut between scenes
        CameraSegment s;
        s.sceneIndex = a.sceneIndex;
        s.fromKey = a.keyIndex;
        s.toKey = b.keyIndex;
        s.ease = a.ease;
        s.x0 = a.x + kKeyHalf + 1.0;
        s.x1 = b.x - kKeyHalf - 1.0;
        s.dashed = shotcam::Ease(a.ease) == shotcam::Ease::Hold;
        s.glyph = s.x1 - s.x0 >= kGlyphMinSegment;
        s.accent = hasSel && a.sceneIndex == selScene && a.keyIndex == selKey;
        if (s.x1 > s.x0)
            segments.append(s);
    }
    return segments;
}

bool AnimaticTimeline::inCameraRow(const QPoint &pos) const
{
    return pos.x() >= kLabelCol && pos.y() >= cameraTrackY()
        && pos.y() < cameraTrackY() + kCameraH;
}

bool AnimaticTimeline::cameraKeyAt(const QPoint &pos, CameraMark *mark) const
{
    if (!inCameraRow(pos))
        return false;
    int best = kKeyHit + 1;
    bool found = false;
    for (const CameraMark &m : cameraMarks()) {
        const int d = qAbs(m.x - pos.x());
        if (d < best) { // the nearer of two close keys wins
            best = d;
            *mark = m;
            found = true;
        }
    }
    return found;
}

// "Key 2 · 00:00:06:00 · panel 3 · Ease In/Out" (Figma 1C-1's tooltip): the
// key's place among its scene's keys, its film timecode, the clip it sits
// on (the number the clip shows), and the ease of the move it starts - or
// "last key", which starts none.
QString AnimaticTimeline::cameraKeyTip(const CameraMark &mark) const
{
    const QVector<CameraMark> marks = cameraMarks();
    int number = 0, inScene = 0;
    for (const CameraMark &m : marks) {
        if (m.sceneIndex != mark.sceneIndex)
            continue;
        ++inScene;
        if (m.keyIndex == mark.keyIndex)
            number = inScene;
    }
    const int frame = int(std::lround(mark.frame));
    const int clip = blockAtFrame(frame);
    return QString::fromUtf8("Key %1 \xC2\xB7 %2 \xC2\xB7 panel %3 \xC2\xB7 %4")
        .arg(number)
        .arg(timecode(frame))
        .arg(clip + 1)
        .arg(number == inScene ? QStringLiteral("last key")
                               : shotcam::easeLabel(shotcam::Ease(mark.ease)));
}

double AnimaticTimeline::cameraRowOpacityForTest() const
{
    return (!m_tracks.cameraVisible || m_tracks.cameraLocked) ? 0.4 : 1.0;
}

void AnimaticTimeline::paintCameraRow(QPainter &p)
{
    using namespace SankoTimelineTheme;
    const QVector<CameraMark> marks = cameraMarks();
    if ((marks.isEmpty() && !m_camPreview) || !m_canvas)
        return;
    const int y0 = cameraTrackY();
    const qreal cy = y0 + kCameraH / 2.0;
    p.save();
    p.setClipRect(QRect(kLabelCol, y0, m_canvas->width() - kLabelCol, kCameraH),
                  Qt::IntersectClip);
    // Locked or hidden: the whole row at 40 % (Figma 1C-1, 1C-3).
    p.setOpacity(cameraRowOpacityForTest());
    p.setRenderHint(QPainter::Antialiasing, true);

    for (const CameraSegment &s : cameraSegments(marks)) {
        const QColor c = s.accent ? kCameraAccent : kCameraSegment;
        if (s.dashed) {
            QPen pen(c, 2.0, Qt::CustomDashLine, Qt::FlatCap);
            pen.setDashPattern({2.5, 2.0}); // 5 px on, 4 off (units of the width)
            p.setPen(pen);
            p.drawLine(QPointF(s.x0, cy), QPointF(s.x1, cy));
        } else {
            p.fillRect(QRectF(s.x0, cy - 1.0, s.x1 - s.x0, 2.0), c);
        }
        if (s.glyph)
            paintEaseGlyph(p, QRectF((s.x0 + s.x1) / 2.0 - kGlyphW / 2.0, cy - kGlyphH / 2.0,
                                     kGlyphW, kGlyphH),
                           shotcam::Ease(s.ease));
    }

    int selScene = -1, selKey = -1;
    const bool hasSel = selectedCameraKey(&selScene, &selKey);
    const int phFrame = playheadFrame();
    for (const CameraMark &m : marks) {
        const bool selected = hasSel && m.sceneIndex == selScene && m.keyIndex == selKey;
        const bool hover = m.sceneIndex == m_camHoverScene && m.keyIndex == m_camHoverKey;
        const bool atPlayhead = qAbs(m.frame - phFrame) < 0.5;
        const QPointF c(m.x + 0.5, cy);
        if (hover) { // the accent at 14 %, the flat controls' hover
            QColor halo = kCameraAccent;
            halo.setAlphaF(0.14);
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(c, 8.0, 8.0);
        }
        if (selected && atPlayhead) { // the playhead's ring around the accent
            p.setPen(QPen(kPlayhead, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 7.5, 7.5);
        }
        QColor fill = kCameraKeyFill, edge = kCameraKeyEdge;
        if (selected) {
            fill = kCameraAccent;
            edge = atPlayhead ? QColor(Qt::white) : kClipSelectedBorder;
        } else if (atPlayhead) {
            fill = kPlayhead;
            edge = Qt::white;
        } else if (hover) {
            fill = kCameraAccent;
            fill.setAlphaF(0.35);
            edge = kCameraAccent;
        }
        QPolygonF diamond;
        diamond << QPointF(c.x(), c.y() - kKeyHalf) << QPointF(c.x() + kKeyHalf, c.y())
                << QPointF(c.x(), c.y() + kKeyHalf) << QPointF(c.x() - kKeyHalf, c.y());
        p.setPen(QPen(edge, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(fill);
        p.drawPolygon(diamond);
    }

    // The Quick Move under the pointer (Figma 1D-2): the keys it would
    // replace crossed out, the two it would add hollow and dashed, joined by
    // a dashed accent bar.
    if (m_camPreview) {
        for (const CameraMark &m : marks) {
            if (m.sceneIndex != m_camPreviewScene || !m_camPreviewReplaced.contains(m.keyIndex))
                continue;
            const QPointF c(m.x + 0.5, cy);
            p.setPen(QPen(QColor(0xe0, 0x56, 0x5b), 1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
            p.drawLine(c + QPointF(4, -4), c + QPointF(-4, 4));
        }
        const qreal xa = contentXToScreen(m_camPreviewStart * pxPerFrame()) + 0.5;
        const qreal xb = contentXToScreen(m_camPreviewEnd * pxPerFrame()) + 0.5;
        QPen dash(kCameraAccent, 1.5, Qt::CustomDashLine, Qt::FlatCap, Qt::RoundJoin);
        dash.setDashPattern({1.7, 1.2});
        if (xb - xa > 2 * kKeyHalf + 2) {
            QPen bar(kCameraAccent, 2.0, Qt::CustomDashLine, Qt::FlatCap);
            bar.setDashPattern({2.5, 2.0});
            p.setPen(bar);
            p.drawLine(QPointF(xa + kKeyHalf + 1, cy), QPointF(xb - kKeyHalf - 1, cy));
        }
        p.setPen(dash);
        p.setBrush(Qt::NoBrush);
        for (qreal x : {xa, xb}) {
            QPolygonF diamond;
            diamond << QPointF(x, cy - kKeyHalf) << QPointF(x + kKeyHalf, cy)
                    << QPointF(x, cy + kKeyHalf) << QPointF(x - kKeyHalf, cy);
            p.drawPolygon(diamond);
        }
    }
    p.restore();
}

void AnimaticTimeline::setCameraPreview(int sceneIndex, double startFrame, double endFrame,
                                        const QVector<int> &replacedKeys)
{
    m_camPreview = true;
    m_camPreviewScene = sceneIndex;
    m_camPreviewStart = startFrame;
    m_camPreviewEnd = endFrame;
    m_camPreviewReplaced = replacedKeys;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::clearCameraPreview()
{
    if (!m_camPreview)
        return;
    m_camPreview = false;
    m_camPreviewReplaced.clear();
    if (m_canvas)
        m_canvas->update();
}

QVariantMap AnimaticTimeline::cameraPreviewForTest() const
{
    QStringList replaced;
    for (int k : m_camPreviewReplaced)
        replaced << QStringLiteral("%1:%2").arg(m_camPreviewScene).arg(k);
    return {{QStringLiteral("shown"), m_camPreview},
            {QStringLiteral("scene"), m_camPreviewScene},
            {QStringLiteral("x0"), contentXToScreen(m_camPreviewStart * pxPerFrame())},
            {QStringLiteral("x1"), contentXToScreen(m_camPreviewEnd * pxPerFrame())},
            {QStringLiteral("replaced"), replaced}};
}

QRect AnimaticTimeline::cameraRowForTest() const
{
    return QRect(0, cameraTrackY(), m_canvas ? m_canvas->width() : 0, kCameraH);
}

QVariantList AnimaticTimeline::cameraKeysForTest() const
{
    QVariantList list;
    int selScene = -1, selKey = -1;
    const bool hasSel = selectedCameraKey(&selScene, &selKey);
    const int phFrame = playheadFrame();
    for (const CameraMark &m : cameraMarks())
        list.append(QVariantMap{
            {QStringLiteral("scene"), m.sceneIndex},
            {QStringLiteral("key"), m.keyIndex},
            {QStringLiteral("x"), m.x},
            {QStringLiteral("frame"), m.frame},
            {QStringLiteral("selected"), hasSel && m.sceneIndex == selScene && m.keyIndex == selKey},
            {QStringLiteral("atPlayhead"), qAbs(m.frame - phFrame) < 0.5},
            {QStringLiteral("hover"), m.sceneIndex == m_camHoverScene && m.keyIndex == m_camHoverKey},
            {QStringLiteral("tip"), cameraKeyTip(m)}});
    return list;
}

QVariantList AnimaticTimeline::cameraSegmentsForTest() const
{
    QVariantList list;
    for (const CameraSegment &s : cameraSegments(cameraMarks()))
        list.append(QVariantMap{
            {QStringLiteral("scene"), s.sceneIndex},
            {QStringLiteral("from"), s.fromKey},
            {QStringLiteral("to"), s.toKey},
            {QStringLiteral("ease"), shotcam::easeLabel(shotcam::Ease(s.ease))},
            {QStringLiteral("dashed"), s.dashed},
            {QStringLiteral("glyph"), s.glyph},
            {QStringLiteral("accent"), s.accent},
            {QStringLiteral("x0"), s.x0},
            {QStringLiteral("x1"), s.x1}});
    return list;
}

// --- Interaction ----------------------------------------------------------

// ONE named step per notch. Smaller than a third of the view (about 610 px
// here): the user's wheel arrives in fast runs of notches, 35 ms apart,
// and a run of ten should not fly past a page. To be tuned by them.
constexpr int kWheelNotchPx = 120;
// Ctrl+wheel: one notch multiplies the zoom by this.
constexpr double kWheelZoomStep = 1.15;

void AnimaticTimeline::canvasWheel(QWheelEvent *e)
{
    if (!m_canvasWheel) {
        e->ignore();
        return;
    }
    // Notches, with a high-resolution wheel's part-notches carried over.
    // The Y axis: a mouse wheel has no other.
    m_wheelRemainder += e->angleDelta().y() / 120.0;
    const int notches = int(m_wheelRemainder);
    m_wheelRemainder -= notches;
    e->accept();
    if (notches == 0)
        return;

    if (e->modifiers() & Qt::ControlModifier) {
        // ZOOM AROUND THE POINTER: the content under the pointer before is
        // the content under it after. Through applyZoom (the limits, the
        // range) and setScrollX (following's own path, never the user's),
        // so a zoom can never hold the view. While the film plays and
        // following is engaged, applyZoom re-anchors the playhead and the
        // next tick keeps it there; this pointer anchor is for the rest.
        const int anchorX = e->position().toPoint().x();
        const double contentBefore = screenXToContent(anchorX);
        const double ppfBefore = pxPerFrame();
        double z = m_zoom;
        for (int i = 0; i < qAbs(notches); ++i)
            z = notches > 0 ? z * kWheelZoomStep : z / kWheelZoomStep;
        z = qBound(10.0, z, 400.0);
        applyZoom(float(z));
        const double contentAfter = contentBefore * (pxPerFrame() / ppfBefore);
        if (m_hScroll && m_hScroll->maximum() > 0)
            setScrollX(qRound(contentAfter - (anchorX - kLabelCol)));
        return;
    }

    // SCROLL SIDEWAYS: up (a positive notch) toward the start, down toward
    // the end. Through the scrollbar's own position, flagged as a wheel, so
    // onScrollbarAction reads it as the user's scroll - which it is - and
    // the hold rule applies exactly as for the wheel over the bar. When the
    // film fits, the bar's range is 0 and nothing moves.
    if (!m_hScroll || m_hScroll->maximum() <= 0)
        return;
    const int wanted = qBound(0, m_scrollX - notches * kWheelNotchPx, m_hScroll->maximum());
    if (wanted == m_scrollX)
        return;
    m_wheelOnScrollbar = true;
    m_hScroll->setSliderPosition(wanted); // actionTriggered(SliderMove) -> the user's
    m_wheelOnScrollbar = false;
}

void AnimaticTimeline::canvasResized()
{
    updateScrollRange();
    layoutTrackControls(); // the rows moved with the height
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::canvasMousePress(QMouseEvent *e)
{
    if (m_volumePopup && m_volumePopup->isVisible())
        m_volumePopup->hide(); // a press on the canvas is a press outside the popup
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint pos = e->position().toPoint();
    if (pos.x() < kLabelCol)
        return;

    // THE CAMERA ROW (Phase 1C), BEFORE the playhead: a key often sits under
    // the playhead (a click on a key puts it there), and the key must win.
    // A press on a key selects it and asks for the playhead to go to it;
    // unlocked, it may become a drag. A press on the row off any key clears
    // the selection - unless it is on the playhead's line, which it grabs.
    // A hidden row takes no press of its own.
    if (inCameraRow(pos) && m_tracks.cameraVisible) {
        CameraMark mark;
        if (cameraKeyAt(pos, &mark)) {
            m_camSelScene = mark.sceneIndex;
            m_camSelKey = mark.keyIndex;
            m_canvas->update();
            emit cameraKeyActivated(mark.sceneIndex, mark.keyIndex);
            m_drag = Drag::CameraKey; // locked: a drag is refused when it starts
            m_camDragScene = mark.sceneIndex;
            m_camDragKey = mark.keyIndex;
            m_camDragFromFrame = int(std::lround(mark.frame));
            m_camDragFrame = m_camDragFromFrame;
            m_camDragActive = false;
            m_camPress = pos;
            return;
        }
        if (qAbs(pos.x() - contentXToScreen(playheadContentX())) > kGrab) {
            m_camSelScene = m_camSelKey = -1;
            m_canvas->update();
            return;
        }
    }
    // Any other press on the timeline ends the key selection: Delete over
    // the timeline must never take a key the artist is no longer looking at
    // when they have just picked a clip.
    if (m_camSelKey >= 0) {
        m_camSelScene = m_camSelKey = -1;
        m_canvas->update();
    }

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
        if (m_scrubIndex >= 0) {
            emit playheadScrubbed(m_scrubIndex);
            emitScrubFrame();
        }
        return;
    }

    // A hidden SHOTS track has nothing to press (Part B).
    if (!m_tracks.shotsVisible && pos.y() >= kPanelY
        && pos.y() < kPanelY + panelTrackH())
        return;

    // Trim handles on the selected clip (panel track only). A LOCKED SHOTS
    // track refuses the trim with the message at the pointer; the press
    // then goes on to select, which a lock never blocks.
    if (m_selected >= 0 && m_selected < m_blocks.size()
        && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const Block &b = m_blocks.at(m_selected);
        const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
        const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
        const bool onHandle = qAbs(pos.x() - x1) <= kHandleW || qAbs(pos.x() - x0) <= kHandleW;
        if (onHandle && m_tracks.shotsLocked) {
            refuse(QStringLiteral("SHOTS track is locked"), e->globalPosition().toPoint());
        } else if (qAbs(pos.x() - x1) <= kHandleW) {
            m_drag = Drag::ResizeRight;
            m_resizeIndex = m_selected;
            m_dragDuration = b.duration;
            m_canvas->setCursor(Qt::SizeHorCursor);
            QToolTip::showText(e->globalPosition().toPoint(),
                               QStringLiteral("%1.0s").arg(m_dragDuration), m_canvas);
            return;
        } else if (qAbs(pos.x() - x0) <= kHandleW) {
            m_drag = Drag::ResizeLeft; // visual only for now
            m_resizeIndex = m_selected;
            m_dragLeftFrames = 0;
            m_canvas->setCursor(Qt::SizeHorCursor);
            return;
        }
    }

    // Otherwise: a press on a clip SELECTS that panel (the workspace does
    // the selecting and pushes the highlight back through setSelectedPanel),
    // and may turn into a reorder drag once it has moved far enough - not
    // on a locked SHOTS track, where the press selects and nothing more.
    if (pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int frame = frameAtScreenX(pos.x());
        const int idx = blockAtFrame(frame);
        if (idx >= 0) {
            emit panelSeekRequested(idx);
            // The request may have rebuilt the blocks (it never does today,
            // but nothing here should assume it): re-check before arming.
            if (idx < m_blocks.size() && !m_tracks.shotsLocked) {
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
    // THE CAMERA ROW (Phase 1C): on a key, select it (the playhead goes
    // there, as for a left press) and ask for the key's menu; off a key,
    // the row's menu. A hidden row takes no press, this one included.
    if (inCameraRow(pos)) {
        if (m_tracks.cameraVisible) {
            CameraMark mark;
            if (cameraKeyAt(pos, &mark)) {
                m_camSelScene = mark.sceneIndex;
                m_camSelKey = mark.keyIndex;
                m_canvas->update();
                emit cameraKeyActivated(mark.sceneIndex, mark.keyIndex);
                emit cameraKeyContextMenuRequested(mark.sceneIndex, mark.keyIndex,
                                                   e->globalPos());
            } else {
                emit cameraRowContextMenuRequested(blockAtFrame(frameAtScreenX(pos.x())),
                                                   e->globalPos());
            }
        }
        e->accept();
        return;
    }
    // The AUDIO row, its label included: the track's own menu. Whoever owns
    // the audio builds it; nothing is selected by asking. A hidden track
    // (Part B) takes no press, this one included.
    if (pos.y() >= audioTrackY() && pos.y() < audioTrackY() + kAudioH) {
        if (m_tracks.audioVisible)
            emit audioContextMenuRequested(e->globalPos());
        e->accept();
        return;
    }
    if (pos.x() < kLabelCol)
        return;
    // THE SCENES TRACK (Phase 1D): the scene under the pointer's own menu
    // (Use Last Framing / Continue Camera). A hidden track takes no press.
    if (pos.y() >= kSceneY && pos.y() < kSceneY + kSceneH) {
        if (m_tracks.scenesVisible) {
            const int idx = blockAtFrame(frameAtScreenX(pos.x()));
            if (idx >= 0 && idx < m_blocks.size())
                emit sceneContextMenuRequested(m_blocks.at(idx).sceneIndex, e->globalPos());
        }
        e->accept();
        return;
    }
    if (m_tracks.shotsVisible && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
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
        emitScrubFrame(); // ...and the frame inside it (Phase 1A)
        return;
    }

    // A camera key dragged in time (Phase 1C): within its own scene, from
    // its first frame to its end; with Snap on, onto a cut or the playhead
    // when within kKeySnapPx. A LOCKED row refuses the drag as it starts.
    if (m_drag == Drag::CameraKey) {
        if (!(e->buttons() & Qt::LeftButton)) {
            m_drag = Drag::None; // the release went elsewhere
            m_camDragActive = false;
            m_canvas->update();
            return;
        }
        if (!m_camDragActive) {
            if ((pos - m_camPress).manhattanLength() < kKeyDragThreshold)
                return;
            if (m_tracks.cameraLocked) {
                refuse(QStringLiteral("CAMERA track is locked"),
                       e->globalPosition().toPoint());
                m_drag = Drag::None;
                return;
            }
            m_camDragActive = true;
            m_canvas->setCursor(Qt::SizeHorCursor);
        }
        const int lo = sceneStartFrame(m_camDragScene);
        const int hi = sceneEndFrame(m_camDragScene);
        int frame = qBound(lo, frameAtScreenX(pos.x()), hi);
        if (m_snap) {
            QVector<int> targets;
            for (const Block &b : m_blocks)
                if (b.sceneIndex == m_camDragScene)
                    targets << b.startFrame;
            targets << hi;
            const int ph = playheadFrame();
            if (ph >= lo && ph <= hi)
                targets << ph;
            int best = kKeySnapPx + 1;
            for (int t : targets) {
                const int d = qAbs(contentXToScreen(t * pxPerFrame()) - pos.x());
                if (d < best) {
                    best = d;
                    frame = t;
                }
            }
        }
        if (frame != m_camDragFrame) {
            m_camDragFrame = frame;
            m_canvas->update();
        }
        QToolTip::showText(e->globalPosition().toPoint(), timecode(m_camDragFrame), m_canvas);
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

    // Idle hover: cursor + highlight. A hidden SHOTS track has nothing to
    // hover; a locked one shows no resize cursor (there is no trim to cue).
    Qt::CursorShape cursor = Qt::ArrowCursor;
    int hover = -1;
    if (m_tracks.shotsVisible && pos.x() >= kLabelCol && pos.y() >= kPanelY
        && pos.y() < kPanelY + panelTrackH()) {
        const int frame = frameAtScreenX(pos.x());
        hover = blockAtFrame(frame);
        cursor = (hover >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor;
        // Resize-handle cursor on the selected clip edges.
        if (!m_tracks.shotsLocked && m_selected >= 0 && m_selected < m_blocks.size()) {
            const Block &b = m_blocks.at(m_selected);
            const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
            const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
            if (qAbs(pos.x() - x0) <= kHandleW || qAbs(pos.x() - x1) <= kHandleW)
                cursor = Qt::SizeHorCursor;
        }
    } else if (pos.y() < kRulerY + kRulerH) {
        cursor = Qt::PointingHandCursor;
    }
    // The CAMERA row: the key under the pointer is lit (the accent halo)
    // and named in a tooltip (Figma 1C-1).
    int camScene = -1, camKey = -1;
    CameraMark mark;
    if (m_tracks.cameraVisible && cameraKeyAt(pos, &mark)) {
        camScene = mark.sceneIndex;
        camKey = mark.keyIndex;
        cursor = Qt::PointingHandCursor;
    }
    m_canvas->setCursor(cursor);
    if (hover != m_hoverIndex) {
        m_hoverIndex = hover;
        m_canvas->update();
    }
    if (camScene != m_camHoverScene || camKey != m_camHoverKey) {
        m_camHoverScene = camScene;
        m_camHoverKey = camKey;
        if (camKey >= 0)
            QToolTip::showText(e->globalPosition().toPoint(), cameraKeyTip(mark), m_canvas);
        else
            QToolTip::hideText();
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
        // The panel it was dropped in is PREVIEWED, not selected (this used
        // to emit panelSeekRequested), and since Phase 1A the playhead
        // comes to rest on the FRAME it was dropped on, not the panel's
        // start (emitScrubFrame, before the drag state is cleared).
        if (idx >= 0 && idx != m_scrubIndex)
            emit playheadScrubbed(idx);
        emitScrubFrame();
        m_drag = Drag::None;
        m_dragPlayheadFrame = -1;
        m_scrubIndex = -1;
        m_scrubFrameSent = -1;
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
    if (m_drag == Drag::CameraKey) {
        const bool moved = m_camDragActive && m_camDragFrame != m_camDragFromFrame;
        const int scene = m_camDragScene, key = m_camDragKey;
        const int sceneFrame = m_camDragFrame - sceneStartFrame(m_camDragScene);
        m_drag = Drag::None;
        m_camDragActive = false;
        m_camDragScene = m_camDragKey = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        m_canvas->update();
        QToolTip::hideText();
        // ONE request on release: the workspace re-anchors the key to the
        // panel it landed in, as one undo step.
        if (moved)
            emit cameraKeyMoveRequested(scene, key, sceneFrame);
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
    if (m_hoverIndex != -1 || m_camHoverKey != -1) {
        m_hoverIndex = -1;
        m_camHoverScene = m_camHoverKey = -1;
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
