#include "AnimaticTimeline.h"
#include "SankoIcons.h"
#include "SankoKeyColors.h" // the keys' colours, Preferences > Keyframes (batch I-1b round 2)
#include "SankoOnionSkin.h" // the onion skin's colours on the drawings lane's blocks (batch I-2)
#include "SankoLightTable.h" // the Light Table's icon, the Onion Skin button's too (OS-1)
#include "SankoTip.h" // sankoHideTipSoon: the hovers' grace (batch C)
#include "SankoTheme.h"
#include "SankoTimelineTheme.h"
#include "SankoToggleControls.h"

#include "AnimaticPage.h"

#include <cmath>
#include "StoryboardModel.h"
#include "SceneColors.h" // the scenes' colours on the SCENES row and the clips (batch E)
#include "camera/ShotCamera.h"

#include <QContextMenuEvent>
#include <QDataStream>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QFrame>
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
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QSlider>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

// THE ONION SKIN'S BUTTON (OS-1, 2026-11-27): the user's Figma frame 749:892, "LAYER row button:
// onion (on)", drawn at four times the size - an 80 x 80 frame, its fill the accent at 28 %, a
// 4 px accent stroke inside, corners of 16; in it the Light Table's icon (lighttable.svg), 36 x 58
// at (22, 11). Here at its size, 20 x 20: the fill, a 1 px border inside, corners of 4, the icon
// 9 x 14.5 at (5.5, 2.75). Off it is the row's other buttons' (the + and Zoom to Frames):
// #1c1e25, the accent at 14 % over it under the pointer, no border. Painted here, so what is
// drawn is the frame and nothing else. THE ICON in the Eye's and the Speaker's grey and line
// (the user's word after hand test OS1; it was white): see SankoLightTable::bulbSvg.
class OnionSkinButton : public QToolButton
{
public:
    explicit OnionSkinButton(QWidget *parent) : QToolButton(parent)
    {
        setAttribute(Qt::WA_Hover, true); // drawn again as the pointer comes and goes
        m_icon.load(SankoLightTable::bulbSvg(SankoTimelineTheme::kTrackControl, SankoLightTable::kLayerRowLine));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box(0.0, 0.0, 20.0, 20.0);
        const QColor accent(0x7c, 0x6e, 0xf6);
        p.setPen(Qt::NoPen);
        if (isChecked()) {
            QColor fill = accent;
            fill.setAlphaF(0.28f);
            p.setBrush(fill);
            p.drawRoundedRect(box, 4.0, 4.0);
            p.setPen(QPen(accent, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 3.5, 3.5); // the stroke inside
        } else {
            p.setBrush(QColor(0x1c, 0x1e, 0x25));
            p.drawRoundedRect(box, 4.0, 4.0);
            if (underMouse()) {
                QColor hover = accent;
                hover.setAlphaF(0.14f); // kFlatHoverBg
                p.setBrush(hover);
                p.drawRoundedRect(box, 4.0, 4.0);
            }
        }
        m_icon.render(&p, QRectF(5.5, 2.75, 9.0, 14.5));
    }

private:
    QSvgRenderer m_icon;
};

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
// Batch E (Figma E-1): a move of 16 to 40 px gets the small mark; under 16 px no mark is
// drawn, and a tap on the line itself opens the five eases.
constexpr int kSmallMarkW = 12, kSmallMarkH = 10, kSmallMarkMinSegment = 16;
constexpr int kKeyDragThreshold = 4;
constexpr int kKeySnapPx = 6;

constexpr int kHandleW = 6;   // trim handle width
constexpr int kGrab = 5;      // px grab tolerance
// A panel lasts at least one frame and has no maximum (2026-11-06, batch 5;
// it was 1 to 30 whole seconds). A clip is drawn at least 4 px wide at any
// zoom, so a 1-frame clip stays visible and clickable (Figma DUR-1).
constexpr int kMinFrames = 1;
constexpr int kMinClipPx = 4;
// The amber chip's last size (GROW-1, Figma 552:696): a 14 px "!" marker, in the clip's
// corner while it fits, centred over the clip when not.
constexpr int kArtExitMarker = 14;
// How long a tooltip may take to appear (Qt rolls one in over 50 to 120 ms, or fades
// it in over 150): while it does, a drag's chip waits rather than ask for a new one.
constexpr int kChipRollMs = 200;
constexpr int kReorderThreshold = 8; // px before a clip press becomes a drag
// THE ZOOM'S LIMITS (batch C, the user's choice: 1600 %): 100 is 1 px a frame, so 1600 is
// 16 px a frame. One pair for every clamp - applyZoom, the field, Fit, Ctrl+wheel.
constexpr float kZoomMin = 10.0f, kZoomMax = 1600.0f;
// From 800 % (8 px a frame) a tick marks every frame on the ruler.
constexpr double kFrameTicksPpf = 8.0;

// The clip, as the design lays it out on a 76 px body (every number is the
// design's; see HANDOFF "The timeline redesign, Part A"): the body is the
// clip's whole frame span - less, at a scene's two ends, its share of the
// 3 px gap between scenes (SCENES + SHOTS, Figma 621:716; it was the rect
// less a 2 px gap on the right) - its scene's outer corners radius 5; the
// thumbnail sits at (6,6), body width - 12 wide, 42 tall, radius 3; the
// number at (10,9); the name at (8, bottom - 20) up to the pill; the pill
// 28 x 18 at (right - 33, bottom - 22).
constexpr qreal kClipR = 5.0;
constexpr int kThumbInset = 6, kThumbH = 42, kThumbR = 3;
constexpr int kPillW = 28, kPillH = 18;
// THE THUMBNAIL KEEPS ITS PICTURE'S SHAPE, AND STAYS IN ITS ROW (2026-11-26, the user's note 5
// after hand test I2-1; it was a strip kThumbH tall across the clip, cropped to fill, at any
// height of the track - cropped to a sliver on a wide clip, and over the CAMERA row once the
// track was shorter than it). The picture is fitted whole - never stretched, never cropped -
// at the clip's left, where the cut begins: over the strip that holds the name and the pill
// (kNameStripH; the design's 42 px on its 76 px body) while the body has room for a picture
// of kStoreyMinThumbH above it, else in the body's whole height less kShortInset above and
// below, the name beside it. Under kThumbMinH there is no picture.
constexpr int kNameStripH = 28, kStoreyMinThumbH = 24, kShortInset = 4, kThumbMinH = 10;
constexpr int kShortNameMinW = 30; // beside the picture, a name with less room than this is not drawn
// THE DRAWINGS LANE (2026-11-26, the user's note 7): the block under the pointer is white at
// this alpha over its own fill (8 %); Zoom to Frames makes the lane's shortest drawing
// kFramesZoomBlockPx wide.
constexpr int kDrawingHoverAlpha = 20;
constexpr int kFramesZoomBlockPx = 32;
// What hides first as a clip narrows (the user's order, 2026-10-18): the
// name (below 86 px of body), then the thumbnail (below 48), then the pill
// (below 42); the number stays while the body is 20 px or more.
constexpr int kNameMinBody = 86, kThumbMinBody = 48, kPillMinBody = 42, kNumberMinBody = 20;

// SCENES + SHOTS (2026-11-18, Figma 621:716): a clip's shape, its corners round (radius
// rad) only on the sides that are its scene's ends.
QPainterPath clipShape(const QRectF &r, qreal rad, bool roundL, bool roundR)
{
    const qreal rl = roundL ? rad : 0.0, rr = roundR ? rad : 0.0;
    QPainterPath path;
    path.moveTo(r.left() + rl, r.top());
    path.lineTo(r.right() - rr, r.top());
    if (rr > 0)
        path.arcTo(QRectF(r.right() - 2 * rr, r.top(), 2 * rr, 2 * rr), 90, -90);
    path.lineTo(r.right(), r.bottom() - rr);
    if (rr > 0)
        path.arcTo(QRectF(r.right() - 2 * rr, r.bottom() - 2 * rr, 2 * rr, 2 * rr), 0, -90);
    path.lineTo(r.left() + rl, r.bottom());
    if (rl > 0)
        path.arcTo(QRectF(r.left(), r.bottom() - 2 * rl, 2 * rl, 2 * rl), 270, -90);
    path.lineTo(r.left(), r.top() + rl);
    if (rl > 0)
        path.arcTo(QRectF(r.left(), r.top(), 2 * rl, 2 * rl), 180, -90);
    path.closeSubpath();
    return path;
}

// WHERE A MOVE'S EASE MARK SITS (2026-11-26, the user's note 6 after hand test I2-1: a mark sat
// on another layer's key, and a press on it took the key). Centred on the move, as ever, while
// nothing is in the way; else moved along the move by as little as puts it clear of every key
// drawn on the row (kMarkKeyGap px between them). The pill needs what it always needed of the
// move's ends - 11 px each side, the 40 px rule - and the small mark 2 (the 16 px rule); where
// the pill has no clear place the small mark is tried, and where that has none the move has no
// mark: the line itself opens the eases (the short-move rule of batch E). `keys`: each key
// drawn on the row but the move's own two - its centre x and its half width.
constexpr qreal kMarkKeyGap = 2.0;
struct EaseMarkPlace
{
    bool glyph = false, smallMark = false;
    qreal x = 0;
};
bool clearPlaceFor(qreal width, qreal endPad, qreal x0, qreal x1, const QVector<QPointF> &keys, qreal *centre)
{
    const qreal lo = x0 + endPad + width / 2.0, hi = x1 - endPad - width / 2.0;
    if (lo > hi + 1e-6)
        return false;
    const qreal mid = (x0 + x1) / 2.0;
    const auto blocked = [&](qreal c) {
        for (const QPointF &key : keys)
            if (qAbs(c - key.x()) < key.y() + kMarkKeyGap + width / 2.0 - 1e-6)
                return true;
        return false;
    };
    if (!blocked(mid)) {
        *centre = mid;
        return true;
    }
    // Beside a key in the way, on either side of it: the nearest such place to the middle.
    bool found = false;
    qreal best = 0;
    for (const QPointF &key : keys) {
        const qreal reach = key.y() + kMarkKeyGap + width / 2.0;
        for (const qreal c : {key.x() - reach, key.x() + reach}) {
            if (c < lo - 1e-6 || c > hi + 1e-6 || blocked(c))
                continue;
            if (!found || qAbs(c - mid) < qAbs(best - mid)) {
                best = c;
                found = true;
            }
        }
    }
    if (found)
        *centre = best;
    return found;
}
EaseMarkPlace placeEaseMark(qreal x0, qreal x1, const QVector<QPointF> &keys)
{
    EaseMarkPlace place;
    place.x = (x0 + x1) / 2.0;
    if (clearPlaceFor(kGlyphW, (kGlyphMinSegment - kGlyphW) / 2.0, x0, x1, keys, &place.x))
        place.glyph = true;
    else if (clearPlaceFor(kSmallMarkW, (kSmallMarkMinSegment - kSmallMarkW) / 2.0, x0, x1, keys, &place.x))
        place.smallMark = true;
    else
        place.x = (x0 + x1) / 2.0;
    return place;
}
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
    void mouseDoubleClickEvent(QMouseEvent *e) override { m_owner->canvasMouseDoubleClick(e); }
    void leaveEvent(QEvent *) override { m_owner->canvasLeave(); }
    void resizeEvent(QResizeEvent *) override { m_owner->canvasResized(); }
    void contextMenuEvent(QContextMenuEvent *e) override
    {
        m_owner->canvasContextMenu(e);
    }
    void wheelEvent(QWheelEvent *e) override { m_owner->canvasWheel(e); }
    // Qt's tooltip, due 0.35 s after the pointer stops (batch C): the camera key's, the
    // amber chip's or the scene's under it. Nothing there: on to the parents, as a
    // widget without a tooltip does.
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(e);
            const QString tip = m_owner->hoverTipAt(help->pos());
            if (tip.isEmpty()) {
                e->ignore();
                return false;
            }
            QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(e);
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
    m_zoomField->setToolTip(QStringLiteral("Zoom (double-click to type a percentage, 10 to 1600)"));
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

    // - and +: a step along the sqrt(2) ladder (batch C; 25 points until then, which at
    // 1600 % would take sixty presses), clamped by applyZoom.
    connect(m_zoomInButton, &QPushButton::clicked, this, [this] { stepZoom(1); });
    connect(m_zoomOutButton, &QPushButton::clicked, this, [this] { stepZoom(-1); });
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

    // What a drag's chip was asked while its tooltip was still appearing: given to
    // it once it is up (followChip, hideChip).
    m_chipFlush = new QTimer(this);
    m_chipFlush->setInterval(30);
    connect(m_chipFlush, &QTimer::timeout, this, [this] {
        const bool up = QToolTip::isVisible();
        if (!up && m_chipRoll.isValid() && m_chipRoll.elapsed() < kChipRollMs)
            return; // still appearing
        m_chipFlush->stop();
        if (!m_chipWaitingText.isEmpty()) {
            const QString text = m_chipWaitingText;
            m_chipWaitingText.clear();
            QToolTip::showText(m_chipWaitingPos, text, m_canvas);
        }
    });
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

// The ease glyph's curve (Figma 1C-2): w(u) drawn in the glyph's 18 x 12 units - Linear a
// diagonal, Ease In flat then rising, Ease Out rising then flat, Ease In/Out both, Hold a
// step - fitted to `box` (the pill's own 18 x 12, the small mark, a button's).
QPainterPath easeCurvePath(const QRectF &box, shotcam::Ease ease)
{
    const auto pt = [&](qreal x, qreal y) {
        return QPointF(box.left() + x * box.width() / kGlyphW, box.top() + y * box.height() / kGlyphH);
    };
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
    return path;
}

// The ease glyph: the curve in an 18 x 12 pill whose top-left is `r` - or, `compact`, in a
// 12 x 10 one (batch E: a move of 16 to 40 px). `hover`: the pill's edge the clips' hover
// edge, the selected border at 50 % (batch E: the mark is a button).
void paintEaseGlyph(QPainter &p, const QRectF &r, shotcam::Ease ease, bool hover = false, bool compact = false)
{
    using namespace SankoTimelineTheme;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor edge = kHairline;
    if (hover) {
        edge = kClipSelectedBorder;
        edge.setAlphaF(0.5);
    }
    p.setPen(QPen(edge, 1.0));
    p.setBrush(kPillBg);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kCameraGlyph, compact ? 1.0 : 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(easeCurvePath(compact ? r.adjusted(1, 1, -1, -1) : r, ease));
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

void AnimaticTimeline::paintEaseCurve(QPainter &p, const QRectF &box, int ease, const QColor &color, qreal width)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(easeCurvePath(box, shotcam::Ease(ease)));
    p.restore();
}

void AnimaticTimeline::buildTrackControls()
{
    if (!m_canvas)
        return;
    using namespace SankoTimelineTheme;
    // The button is SankoToggleControls' - the Layers panel's lock and eye
    // sit in the same one (one lock and one eye across SankoTV, 2026-10-30).
    auto make = [&](const QString &file, const QString &name, const QColor &on) {
        return SankoToggleControls::makeToggle(m_canvas, name, trackIcon(file, on));
    };
    const TimelineTrack tracks[4] = {TimelineTrack::Scenes, TimelineTrack::Shots,
                                     TimelineTrack::Audio, TimelineTrack::Camera};
    const char *suffix[4] = {"Scenes", "Shots", "Audio", "Camera"};
    for (int i = 0; i < 4; ++i) {
        const TimelineTrack t = tracks[i];
        // Locked reads as locked: the checked state is the CLOSED padlock in
        // the accent, the open one in the grey for unlocked.
        m_lockButtons[i] = SankoToggleControls::makeToggle(
            m_canvas, QStringLiteral("trackLock") + QLatin1String(suffix[i]),
            SankoToggleControls::lockIcon());
        connect(m_lockButtons[i], &QToolButton::clicked, this, [this, t](bool on) {
            if (m_host)
                m_host->setTrackLocked(t, on); // comes back through setTracks
        });
        m_eyeButtons[i] = SankoToggleControls::makeToggle(
            m_canvas, QStringLiteral("trackEye") + QLatin1String(suffix[i]),
            SankoToggleControls::eyeIcon());
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
    // NEW DRAWING (batch I-2, Figma I-2 section 2): the + at the LAYER row's left - beside the
    // drawings lane when the selected layer has one, else in the row. A click asks; the
    // workspace makes the drawing (its undo step, its refusals). Placed and shown by
    // layoutNewDrawingButton.
    m_newDrawingButton = new QToolButton(m_canvas);
    m_newDrawingButton->setObjectName(QStringLiteral("layerNewDrawing"));
    m_newDrawingButton->setText(QStringLiteral("+"));
    m_newDrawingButton->setAutoRaise(true);
    m_newDrawingButton->setCursor(Qt::PointingHandCursor);
    m_newDrawingButton->setFocusPolicy(Qt::NoFocus);
    m_newDrawingButton->setFixedSize(20, 20);
    m_newDrawingButton->setToolTip(QStringLiteral("New Frame (N)")); // "New Drawing (N)" until note 8
    // (Its font in POINTS: a tool button with a pixel-sized font logs "QFont::setPointSize: Point
    // size <= 0" at each hover - the census of (pz5), which caught this one's first sheet.)
    m_newDrawingButton->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; background: #1c1e25; color: #e5e5ed; border-radius: 4px;"
        " font-size: 10pt; padding: 0px 0px 2px 0px; }"
        "QToolButton:hover { background: %1; }")
                                          .arg(kFlatHoverBg));
    connect(m_newDrawingButton, &QToolButton::clicked, this, [this] { emit newDrawingRequested(); });
    m_newDrawingButton->hide();
    // ZOOM TO FRAMES (the user's note 7 after hand test I2-1, 2026-11-26): a small button
    // beside the lane's +. Its picture is drawn here - a magnifier, two strokes - so no icon
    // file (and no licence) comes with it.
    m_zoomFramesButton = new QToolButton(m_canvas);
    m_zoomFramesButton->setObjectName(QStringLiteral("layerZoomToFrames"));
    m_zoomFramesButton->setAutoRaise(true);
    m_zoomFramesButton->setCursor(Qt::PointingHandCursor);
    m_zoomFramesButton->setFocusPolicy(Qt::NoFocus);
    m_zoomFramesButton->setFixedSize(20, 20);
    // Its name only (the user's word after hand test LR, 2026-11-28: "Shorter tooltips, with no
    // other description"; it was "Zoom to Frames: each frame at least 32 px wide, the playhead in
    // the middle. A double-click on the lane does the same.").
    m_zoomFramesButton->setToolTip(QStringLiteral("Zoom to Frames"));
    {
        const qreal dpr = devicePixelRatioF();
        QPixmap glyph(QSize(16, 16) * dpr);
        glyph.setDevicePixelRatio(dpr);
        glyph.fill(Qt::transparent);
        QPainter g(&glyph);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setPen(QPen(QColor(0xe5, 0xe5, 0xed), 1.4, Qt::SolidLine, Qt::RoundCap));
        g.drawEllipse(QPointF(7.0, 7.0), 4.2, 4.2);
        g.drawLine(QPointF(10.2, 10.2), QPointF(13.6, 13.6));
        g.end();
        m_zoomFramesButton->setIcon(QIcon(glyph));
        m_zoomFramesButton->setIconSize(QSize(16, 16));
    }
    m_zoomFramesButton->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; background: #1c1e25; border-radius: 4px; padding: 0px; }"
        "QToolButton:hover { background: %1; }")
                                          .arg(kFlatHoverBg));
    connect(m_zoomFramesButton, &QToolButton::clicked, this, [this] { zoomToFrames(); });
    m_zoomFramesButton->hide();
    // THE ONION SKIN'S BUTTON (OS-1; see OnionSkinButton above): beside Zoom to Frames, there
    // while the lane is. Its tip is its name and its key, nothing else (the user's word after
    // hand test LR, 2026-11-28; until then it also said what it shows, its right-click, and that
    // the panels are the Light Table's - Figma 745:757 section 1).
    m_onionButton = new OnionSkinButton(m_canvas);
    m_onionButton->setObjectName(QStringLiteral("layerOnionSkin"));
    m_onionButton->setCheckable(true);
    m_onionButton->setCursor(Qt::PointingHandCursor);
    m_onionButton->setFocusPolicy(Qt::NoFocus);
    m_onionButton->setFixedSize(20, 20);
    m_onionButton->setToolTip(QStringLiteral("Onion Skin (O)"));
    m_onionButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_onionButton, &QToolButton::clicked, this, [this](bool on) { emit onionSkinButtonClicked(on); });
    connect(m_onionButton, &QWidget::customContextMenuRequested, this, [this] { emit onionSkinPageRequested(); });
    m_onionButton->hide();

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
    layoutNewDrawingButton();
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
    if (!m_tracks.cameraVisible) {
        m_camHoverScene = m_camHoverKey = -1;
        m_easeHoverScene = m_easeHoverKey = -1;
    }
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
        || shotcam::keyFrame(*scene, scene->camera.keys.at(m_camSelKey), m_fps,
                             qMax(0, sceneStartFrame(m_camSelScene))) < 0.0)
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

// A press the lock refuses: say why - in the workspace's note (batch I-1b; the status bar, in
// amber, since round 1), else at the pointer (the same tooltip path the trim drag uses) - and
// remember it for the gate.
void AnimaticTimeline::refuse(const QString &why, const QPoint &globalPos)
{
    m_lastRefusal = why;
    if (m_noteSink)
        m_noteSink(why, true);
    else
        QToolTip::showText(globalPos, why, m_canvas);
}

// --- Track geometry -------------------------------------------------------

// The least the rows need, top to bottom. (While the LAYER row holds a drawings lane - batch
// I-2 - the SHOTS track gives it its height, down to kPanelRunMinH, so the section keeps the
// height the artist gave it.)
int AnimaticTimeline::rowsMinH() const
{
    return kPanelY + (m_layerRowRun ? kPanelRunMinH : kPanelMinH) + kCameraH + layerRowH() + kAudioH;
}

// THE CANVAS IS NEVER SHORTER THAN ITS ROWS (2026-11-26, the user's note 5: "every row stays
// inside its own bounds, whatever the timeline's height"). Its minimum was the four rows of
// before the LAYER row, so with that row shown - 28 px, 71 with a drawings lane - a timeline
// dragged to its smallest had its AUDIO row past its bottom edge. The minimum follows the rows
// now, and the splitter above gives the timeline the room when a row appears.
void AnimaticTimeline::syncMinimumHeight()
{
    if (!m_canvas || m_canvas->minimumHeight() == rowsMinH())
        return;
    m_canvas->setMinimumHeight(rowsMinH());
    m_canvas->updateGeometry();
    updateGeometry();
    emit rowsMinimumChanged();
}

int AnimaticTimeline::tracksH() const
{
    return qMax(rowsMinH(), m_canvas ? m_canvas->height() : 0);
}

int AnimaticTimeline::panelTrackH() const
{
    return tracksH() - kPanelY - kCameraH - layerRowH() - kAudioH;
}

int AnimaticTimeline::cameraTrackY() const
{
    return kPanelY + panelTrackH();
}

// THE LAYER ROW (batch I-1): under CAMERA; 0 px when no layer has motion.
int AnimaticTimeline::layerTrackY() const
{
    return cameraTrackY() + kCameraH;
}

int AnimaticTimeline::audioTrackY() const
{
    return layerTrackY() + layerRowH();
}

QRect AnimaticTimeline::clipRect(int flatIndex) const
{
    if (flatIndex < 0 || flatIndex >= m_blocks.size())
        return QRect();
    const Block &b = m_blocks.at(flatIndex);
    const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
    const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
    // At least kMinClipPx wide (a 1-frame clip at a low zoom), over the
    // start of the clip after it; any clip of 4 px or more is exactly the
    // rect it always was.
    return QRect(x0, kPanelY + 2, qMax(kMinClipPx, x1 - x0), panelTrackH() - 4);
}

int AnimaticTimeline::blockAtScreenX(int screenX) const
{
    // A clip narrower than it is drawn (see clipRect) is found first: it is
    // painted over its neighbour, so a click on what is seen reaches it.
    for (int i = 0; i < m_blocks.size(); ++i) {
        const Block &b = m_blocks.at(i);
        if (b.frames * pxPerFrame() >= kMinClipPx)
            continue;
        const QRect r = clipRect(i);
        if (screenX >= r.left() && screenX < r.left() + r.width())
            return i;
    }
    return blockAtFrame(frameAtScreenX(screenX));
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
    // The typing field is for a panel by its place: a new list closes it.
    if (m_durFlat >= 0)
        closeDurationField(false);
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
    if (m_drag == Drag::LayerKey) { // ...nor a layer key's (batch I-1)
        m_drag = Drag::None;
        m_layerDragActive = false;
        m_layerDragFlat = m_layerDragLayer = m_layerDragKey = -1;
    }
    m_camHoverScene = m_camHoverKey = -1;
    m_easeHoverScene = m_easeHoverKey = -1; // the scenes changed: the keys may have
    rebuildBlocks();
    updateScrollRange();
    refreshLayerRow();
    if (m_canvas)
        m_canvas->update();
}

// --- THE LAYER ROW (batch I-1, Figma I-1 693:755 option A) ------------------------------

bool AnimaticTimeline::computeLayerRowShown() const
{
    for (const Scene *scene : m_scenes)
        for (const Panel *panel : scene->panels)
            if (panel)
                for (const Layer &layer : panel->layers)
                    // ...or a run of drawings (batch I-2): the row is where its blocks are.
                    if ((layer.hasMotionData() || layer.hasRun()) && !isGroupLayer(layer)
                        && layer.type != QLatin1String("background"))
                        return true;
    return false;
}

void AnimaticTimeline::refreshLayerRow()
{
    const bool shown = computeLayerRowShown();
    // The drawings lane (batch I-2): the row is taller (kLayerRowRunH) while the selected layer has a run.
    const bool run = shown && runLayer() != nullptr;
    if (shown != m_layerRowShown || run != m_layerRowRun) {
        m_layerRowShown = shown;
        m_layerRowRun = run;
        syncMinimumHeight();   // the canvas is never shorter than its rows
        layoutTrackControls(); // the AUDIO row's controls move with it
    }
    if (!run) {
        m_selectedDrawings.clear();
        m_hoverDrawing = -1;
        if (m_drag == Drag::DrawingCut)
            m_drag = Drag::None;
    }
    layoutNewDrawingButton();
    if (m_canvas)
        m_canvas->update();
}

QRect AnimaticTimeline::layerRowForTest() const
{
    return m_layerRowShown ? QRect(0, layerTrackY(), m_canvas ? m_canvas->width() : 0, layerRowH()) : QRect();
}

// --- THE DRAWINGS LANE (batch I-2, 2026-11-26; Figma I-2 727:757) -------------------------

const Layer *AnimaticTimeline::runLayer(int *layerIndex, int *flat) const
{
    if (m_selected < 0 || m_selected >= m_blocks.size())
        return nullptr;
    const Block &b = m_blocks.at(m_selected);
    const Panel *panel = b.sceneIndex < m_scenes.size() ? m_scenes.at(b.sceneIndex)->panels.value(b.panelIndex)
                                                         : nullptr;
    const Layer *layer = panel ? panel->activeLayer() : nullptr;
    if (!layer || !panel->layerHasRun(*layer) || layer->type == QLatin1String("background"))
        return nullptr;
    if (layerIndex)
        *layerIndex = panel->activeLayerIndex;
    if (flat)
        *flat = m_selected;
    return layer;
}

QVector<int> AnimaticTimeline::runStarts(const Layer &layer) const
{
    // In the project's frames, as the workspace's edits count them (Layer::countRunAt).
    Layer counted = layer;
    counted.countRunAt(m_fps);
    QVector<int> starts;
    for (const LayerDrawing &drawing : counted.drawings)
        starts.append(drawing.start);
    if (m_drag == Drag::DrawingCut && m_cutDragActive && m_cutDragDrawing >= 1 && m_cutDragDrawing < starts.size()) {
        const int delta = m_cutDragFrame - starts.at(m_cutDragDrawing);
        if (m_cutDragAlone)
            starts[m_cutDragDrawing] = m_cutDragFrame;
        else
            for (int i = m_cutDragDrawing; i < starts.size(); ++i)
                starts[i] += delta;
    }
    return starts;
}

QRect AnimaticTimeline::drawingsLaneRect() const
{
    if (!m_layerRowShown || !m_layerRowRun || !m_canvas)
        return QRect();
    return QRect(kLabelCol, layerTrackY() + kDrawingsLaneY, m_canvas->width() - kLabelCol, kDrawingsLaneH);
}

QVector<AnimaticTimeline::DrawingBlock> AnimaticTimeline::drawingBlocks() const
{
    QVector<DrawingBlock> blocks;
    const Layer *layer = runLayer();
    if (!layer || !m_layerRowRun || m_selected < 0 || m_selected >= m_blocks.size())
        return blocks;
    const Block &b = m_blocks.at(m_selected);
    const QVector<int> starts = runStarts(*layer);
    const double ppf = pxPerFrame();
    const int inPanel = playheadFrame() - b.startFrame;
    using SankoKeyColors::Row;
    const QColor own = SankoKeyColors::key(Row::Layer);
    // The drawing under the playhead - or, off this panel, the one the layer shows.
    int current = qBound(0, layer->shown, int(starts.size()) - 1);
    if (inPanel >= 0 && inPanel < b.frames) {
        current = 0;
        for (int i = 1; i < starts.size() && starts.at(i) <= inPanel; ++i)
            current = i;
    }
    for (int i = 0; i < starts.size(); ++i) {
        if (starts.at(i) >= b.frames)
            break; // past the panel's end: kept, unseen
        DrawingBlock block;
        block.index = i;
        block.start = starts.at(i);
        block.last = i + 1 >= starts.size() || starts.at(i + 1) >= b.frames;
        block.end = block.last ? b.frames : starts.at(i + 1);
        block.x0 = contentXToScreen((b.startFrame + block.start) * ppf);
        block.x1 = contentXToScreen((b.startFrame + block.end) * ppf);
        block.shown = i == current;
        block.selected = m_selectedDrawings.contains(i);
        block.hovered = i == m_hoverDrawing;
        block.tint = own;
        // The blocks the onion skin ghosts, in its colours (Preferences > Onion Skin).
        if (m_onionShown && i != current) {
            using SankoOnionSkin::Side;
            const Side side = i < current ? Side::Before : Side::After;
            if (qAbs(i - current) <= SankoOnionSkin::choice(side).count)
                block.tint = SankoOnionSkin::choice(side).color;
        }
        const bool plainTint = block.tint == own;
        // A NEW, EMPTY FRAME IS GREY (the user's note 7, 2026-11-26) until something is drawn
        // on it; then it takes the colour of the others. (An empty drawing has no ghost either.)
        block.blank = drawingBlank(layer->pixelsOf(i));
        if (block.blank)
            block.tint = QColor(0x8a, 0x8c, 0x99);
        block.strength = block.shown ? 0.46 : (block.last && i + 1 >= starts.size() && plainTint ? 0.10 : 0.22);
        blocks.append(block);
    }
    return blocks;
}

bool AnimaticTimeline::inDrawingsLane(const QPoint &pos) const
{
    // The pointer's lane: below the keys' lane, to the row's bottom.
    return m_layerRowShown && m_layerRowRun && pos.x() >= kLabelCol && pos.y() >= layerTrackY() + kKeysLaneH
        && pos.y() < layerTrackY() + layerRowH();
}

int AnimaticTimeline::drawingBlockAt(const QPoint &pos) const
{
    if (!inDrawingsLane(pos))
        return -1;
    for (const DrawingBlock &block : drawingBlocks())
        if (pos.x() >= block.x0 && pos.x() < block.x1)
            return block.index;
    return -1;
}

int AnimaticTimeline::drawingCutAt(const QPoint &pos) const
{
    if (!inDrawingsLane(pos))
        return -1;
    constexpr int kCutGrab = 4; // px either side, as a clip's edge takes
    int best = -1, bestD = kCutGrab + 1;
    for (const DrawingBlock &block : drawingBlocks()) {
        if (block.index < 1)
            continue; // the first drawing starts the panel
        const int d = qAbs(int(std::lround(block.x0)) - pos.x());
        if (d < bestD) {
            bestD = d;
            best = block.index;
        }
    }
    return best;
}

bool AnimaticTimeline::drawingBlank(const LayerPixels &px) const
{
    const qint64 key = px.contentKey();
    const auto known = m_blankByKey.constFind(key);
    if (known != m_blankByKey.constEnd())
        return known.value();
    if (m_blankByKey.size() > 512) // a session's worth of drawings looked at: start again
        m_blankByKey.clear();
    const bool blank = px.blank();
    m_blankByKey.insert(key, blank);
    return blank;
}

QString AnimaticTimeline::drawingBlockLabel(const DrawingBlock &block) const
{
    QFont f = font();
    f.setPixelSize(9);
    if (block.shown)
        f.setWeight(QFont::DemiBold); // as it is drawn (paintDrawingsLane)
    const QFontMetrics fm(f);
    const qreal room = block.x1 - block.x0 - 8.0;
    const QString number = QString::number(block.index + 1);
    if (block.last) {
        const QString held = number + QStringLiteral("  ") + QChar(0x00B7) + QStringLiteral("  holds to the panel's end");
        if (fm.horizontalAdvance(held) <= room)
            return held;
    }
    return fm.horizontalAdvance(number) <= room ? number : QString();
}

void AnimaticTimeline::paintDrawingsLane(QPainter &p)
{
    const QRect lane = drawingsLaneRect();
    if (lane.isEmpty())
        return;
    const QVector<DrawingBlock> blocks = drawingBlocks();
    if (blocks.isEmpty())
        return;
    p.save();
    p.setClipRect(lane.adjusted(0, -1, 0, 1), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, false);
    using SankoKeyColors::Row;
    const QColor own = SankoKeyColors::key(Row::Layer);
    QFont f = font();
    f.setPixelSize(9);
    QFont shownFont = f; // the drawing under the playhead: its number heavier (Figma 737:872)
    shownFont.setWeight(QFont::DemiBold);
    for (const DrawingBlock &block : blocks) {
        const QRectF r(block.x0, lane.top(), block.x1 - block.x0, lane.height());
        QColor fill = block.tint;
        fill.setAlphaF(float(block.strength));
        p.fillRect(r, fill);
        // THE POINTER OVER IT (the user's note 7, 2026-11-26): a little lighter, no more.
        if (block.hovered)
            p.fillRect(r, QColor(255, 255, 255, kDrawingHoverAlpha));
        // The cut where it starts, a little stronger than the tint: 1 px, the lane's height.
        QColor cut = own;
        cut.setAlphaF(0.70f);
        p.fillRect(QRectF(block.x0, lane.top(), 1.0, lane.height()), cut);
        const QString label = drawingBlockLabel(block);
        if (!label.isEmpty()) {
            p.setFont(block.shown ? shownFont : f);
            p.setPen(block.shown ? QColor(Qt::white) : QColor(0xcf, 0xcb, 0xea));
            p.drawText(r.adjusted(5, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }
    // THE SELECTION (round FC-a, Figma FC-1 section 1): each run of selected frames outlined as one,
    // in the selected frame's outline - one frame alone as before; frames Ctrl+clicked apart each
    // their own.
    p.setPen(QPen(SankoKeyColors::keyEdge(Row::Layer), 1.0));
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < blocks.size(); ++i) {
        if (!blocks.at(i).selected)
            continue;
        int j = i;
        while (j + 1 < blocks.size() && blocks.at(j + 1).selected && blocks.at(j + 1).index == blocks.at(j).index + 1)
            ++j;
        const QRectF r(blocks.at(i).x0, lane.top(), blocks.at(j).x1 - blocks.at(i).x0, lane.height());
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
        i = j;
    }
    // The run as one band: a line along its top and its bottom (kDrawingsBandH: 3 px since
    // the user's proposal; 1 px before).
    QColor band = own;
    band.setAlphaF(0.50f);
    const qreal x0 = blocks.first().x0, x1 = blocks.last().x1;
    p.fillRect(QRectF(x0, lane.top(), x1 - x0, kDrawingsBandH), band);
    p.fillRect(QRectF(x0, lane.bottom() + 1 - kDrawingsBandH, x1 - x0, kDrawingsBandH), band);
    p.restore();
}

void AnimaticTimeline::layoutNewDrawingButton()
{
    if (!m_newDrawingButton)
        return;
    // At the row's left: beside the drawings lane when there is one, else in the row.
    const bool can = m_layerRowShown && m_selected >= 0 && layerRowLabel() != QLatin1String("LAYER");
    m_newDrawingButton->setVisible(can);
    // Zoom to Frames beside it, while there is a lane to zoom to; the Onion Skin's switch next
    // (OS-1), while there are frames for it.
    if (m_zoomFramesButton)
        m_zoomFramesButton->setVisible(can && m_layerRowRun);
    if (m_onionButton)
        m_onionButton->setVisible(can && m_layerRowRun);
    if (!can)
        return;
    // (Centred on the lane: 41 px since 2026-11-26.)
    const int y = m_layerRowRun ? layerTrackY() + kDrawingsLaneY + (kDrawingsLaneH - 20) / 2
                                : layerTrackY() + (kLayerRowH - 20) / 2;
    m_newDrawingButton->move(4, y);
    if (m_zoomFramesButton)
        m_zoomFramesButton->move(4 + 20 + 3, y);
    if (m_onionButton)
        m_onionButton->move(4 + 2 * (20 + 3), y); // x 50
}

// ZOOM TO FRAMES (the user's note 7, 2026-11-26): the timeline zoomed so each drawing of the lane
// is at least 32 px wide - on 2s that is 16 px a frame, the zoom's own top, which nothing passes
// - with the playhead in the middle of the view. Already that close or closer, the zoom stays
// and only the view moves. Fit goes back, as ever.
void AnimaticTimeline::zoomToFrames()
{
    const QVector<DrawingBlock> blocks = drawingBlocks();
    if (blocks.isEmpty())
        return;
    int shortest = blocks.first().end - blocks.first().start;
    for (const DrawingBlock &block : blocks)
        shortest = qMin(shortest, block.end - block.start);
    // 100 % is 1 px a frame.
    const float wanted = qBound(kZoomMin, 100.0f * kFramesZoomBlockPx / float(qMax(1, shortest)), kZoomMax);
    if (wanted > m_zoom)
        applyZoom(wanted);
    setScrollX(qRound(playheadContentX()) - visibleWidth() / 2);
    if (m_canvas)
        m_canvas->update();
}

QAbstractButton *AnimaticTimeline::zoomToFramesButtonForTest() const
{
    return m_zoomFramesButton;
}

QVariantMap AnimaticTimeline::rowsForTest() const
{
    const auto row = [](int y, int h) { return QStringLiteral("%1,%2").arg(y).arg(h); };
    return {{QStringLiteral("timecode"), row(kRulerY, kRulerH)},
            {QStringLiteral("scenes"), row(kSceneY, kSceneH)},
            {QStringLiteral("shots"), row(kPanelY, panelTrackH())},
            {QStringLiteral("camera"), row(cameraTrackY(), kCameraH)},
            {QStringLiteral("layer"), row(layerTrackY(), layerRowH())},
            {QStringLiteral("audio"), row(audioTrackY(), kAudioH)},
            {QStringLiteral("canvas"), m_canvas ? m_canvas->height() : 0}};
}

void AnimaticTimeline::setSelectedDrawings(const QVector<int> &drawingIndices)
{
    QVector<int> sorted = drawingIndices;
    std::sort(sorted.begin(), sorted.end());
    if (m_selectedDrawings == sorted)
        return;
    m_selectedDrawings = sorted;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setOnionSkinShown(bool on)
{
    if (m_onionButton && m_onionButton->isChecked() != on) { // the switch's state, whoever turned it (OS-1)
        const QSignalBlocker quiet(m_onionButton);
        m_onionButton->setChecked(on);
    }
    if (m_onionShown == on)
        return;
    m_onionShown = on;
    if (m_canvas)
        m_canvas->update();
}

QAbstractButton *AnimaticTimeline::onionSkinButtonForTest() const
{
    return m_onionButton;
}

QRect AnimaticTimeline::drawingsLaneForTest() const
{
    return drawingsLaneRect();
}

QVariantList AnimaticTimeline::drawingBlocksForTest() const
{
    QVariantList list;
    for (const DrawingBlock &block : drawingBlocks())
        list.append(QVariantMap{{QStringLiteral("index"), block.index},
                                {QStringLiteral("start"), block.start},
                                {QStringLiteral("end"), block.end},
                                {QStringLiteral("x0"), block.x0},
                                {QStringLiteral("x1"), block.x1},
                                {QStringLiteral("shown"), block.shown},
                                {QStringLiteral("selected"), block.selected},
                                {QStringLiteral("blank"), block.blank},     // nothing drawn on it: grey
                                {QStringLiteral("hovered"), block.hovered}, // the pointer is over it
                                {QStringLiteral("tint"), block.tint.name()},
                                {QStringLiteral("strength"), block.strength},
                                {QStringLiteral("label"), drawingBlockLabel(block)}});
    return list;
}

QAbstractButton *AnimaticTimeline::newDrawingButtonForTest() const
{
    return m_newDrawingButton;
}

QString AnimaticTimeline::layerRowLabel() const
{
    const QString dot = QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ");
    if (m_selected < 0 || m_selected >= m_blocks.size())
        return QStringLiteral("LAYER");
    const Block &b = m_blocks.at(m_selected);
    const Panel *panel = b.sceneIndex < m_scenes.size() ? m_scenes.at(b.sceneIndex)->panels.value(b.panelIndex)
                                                         : nullptr;
    const Layer *layer = panel ? panel->activeLayer() : nullptr;
    if (!layer || isGroupLayer(*layer) || layer->type == QLatin1String("background"))
        return QStringLiteral("LAYER");
    return QStringLiteral("LAYER") + dot + layer->name;
}

QVector<AnimaticTimeline::LayerMark> AnimaticTimeline::layerMarks() const
{
    QVector<LayerMark> marks;
    if (!m_layerRowShown)
        return marks;
    const double ppf = pxPerFrame();
    for (int flat = 0; flat < m_blocks.size(); ++flat) {
        const Block &b = m_blocks.at(flat);
        if (b.sceneIndex >= m_scenes.size())
            continue;
        const Panel *panel = m_scenes.at(b.sceneIndex)->panels.value(b.panelIndex);
        if (!panel)
            continue;
        for (int li = 0; li < panel->layers.size(); ++li) {
            const Layer &layer = panel->layers.at(li);
            if (layer.keys.isEmpty() || isGroupLayer(layer) || layer.type == QLatin1String("background"))
                continue;
            const bool own = flat == m_selected && li == panel->activeLayerIndex;
            QVector<LayerMark> mine;
            for (int k = 0; k < layer.keys.size(); ++k) {
                LayerMark m;
                m.flat = flat;
                m.layer = li;
                m.key = k;
                double f = layer.keys.at(k).at * b.frames;
                if (m_drag == Drag::LayerKey && m_layerDragActive && flat == m_layerDragFlat
                    && li == m_layerDragLayer && k == m_layerDragKey)
                    f = m_layerDragFrame - b.startFrame; // where the drag has it
                m.frame = b.startFrame + f;
                m.x = contentXToScreen(m.frame * ppf);
                m.ease = int(layer.keys.at(k).ease);
                m.own = own;
                m.greyed = !layer.animate;
                mine.append(m);
            }
            std::stable_sort(mine.begin(), mine.end(),
                             [](const LayerMark &a, const LayerMark &c) { return a.frame < c.frame; });
            marks += mine;
        }
    }
    return marks;
}

QVector<AnimaticTimeline::LayerSegment> AnimaticTimeline::layerSegments(const QVector<LayerMark> &marks) const
{
    QVector<LayerSegment> segments;
    for (int i = 0; i + 1 < marks.size(); ++i) {
        const LayerMark &a = marks.at(i);
        const LayerMark &b = marks.at(i + 1);
        if (!a.own || a.flat != b.flat || a.layer != b.layer)
            continue; // the selected layer's moves only, within its panel
        LayerSegment s;
        s.flat = a.flat;
        s.layer = a.layer;
        s.fromKey = a.key;
        s.toKey = b.key;
        s.ease = a.ease;
        s.x0 = a.x + kKeyHalf + 1.0;
        s.x1 = b.x - kKeyHalf - 1.0;
        s.dashed = shotcam::Ease(a.ease) == shotcam::Ease::Hold;
        // The mark clear of the other layers' keys on the row (placeEaseMark): their small
        // hollow diamonds, kKeyHalf - 2 a side and their line.
        QVector<QPointF> others;
        for (const LayerMark &m : marks)
            if (!m.own && m.x > s.x0 - kGlyphW && m.x < s.x1 + kGlyphW)
                others.append(QPointF(m.x + 0.5, kKeyHalf - 1.0));
        const EaseMarkPlace place = placeEaseMark(s.x0, s.x1, others);
        s.glyph = place.glyph;
        s.smallMark = place.smallMark;
        s.markX = place.x;
        s.lineOnly = !s.glyph && !s.smallMark;
        s.greyed = a.greyed;
        if (s.x1 > s.x0)
            segments.append(s);
    }
    return segments;
}

QRectF AnimaticTimeline::layerEaseMarkRect(const LayerSegment &s) const
{
    const qreal cx = s.markX, cy = layerTrackY() + kLayerRowH / 2.0;
    if (s.glyph)
        return QRectF(cx - kGlyphW / 2.0, cy - kGlyphH / 2.0, kGlyphW, kGlyphH);
    if (s.smallMark)
        return QRectF(cx - kSmallMarkW / 2.0, cy - kSmallMarkH / 2.0, kSmallMarkW, kSmallMarkH);
    return QRectF(s.x0, cy - 5.0, s.x1 - s.x0, 10.0);
}

bool AnimaticTimeline::inLayerRow(const QPoint &pos) const
{
    return m_layerRowShown && pos.x() >= kLabelCol && pos.y() >= layerTrackY()
        && pos.y() < layerTrackY() + layerRowH(); // with its drawings lane (batch I-2)
}

bool AnimaticTimeline::pressOnLayerRow(const QObject *receiver, const QPoint &globalPos) const
{
    return m_canvas && receiver == m_canvas && m_canvas->isVisible()
        && inLayerRow(m_canvas->mapFromGlobal(globalPos));
}

// THE TIMECODE ROW (the user's answer after hand test I1b-3, 2026-11-26: twice a key was copied,
// the playhead moved by a tap on the ruler, and Ctrl+V did nothing - seven presses). A press
// there moves the playhead, or with Ctrl pans the view; it is not "a press somewhere else" for
// the LAYER row's hold on the edit keys, so a copied key stays ready for Ctrl+V.
bool AnimaticTimeline::pressOnTimecodeRow(const QObject *receiver, const QPoint &globalPos) const
{
    if (!m_canvas || receiver != m_canvas || !m_canvas->isVisible())
        return false;
    const QPoint pos = m_canvas->mapFromGlobal(globalPos);
    return m_canvas->rect().contains(pos) && pos.y() >= kRulerY && pos.y() < kRulerY + kRulerH;
}

// A pen's press that the rows do not take is handed on by Qt to the rows' parents, one after
// the other, before the mouse press Qt makes of it arrives at the rows. Those copies are the
// same press, already judged when the rows had it.
bool AnimaticTimeline::pressPassedOnFromRows(const QObject *receiver, const QPoint &globalPos) const
{
    if (!m_canvas || !m_canvas->isVisible() || !receiver || !receiver->isWidgetType() || receiver == m_canvas)
        return false;
    return static_cast<const QWidget *>(receiver)->isAncestorOf(m_canvas)
        && m_canvas->rect().contains(m_canvas->mapFromGlobal(globalPos));
}

bool AnimaticTimeline::layerKeyAt(const QPoint &pos, LayerMark *mark) const
{
    if (!inLayerRow(pos))
        return false;
    int best = kKeyHit + 1;
    bool found = false;
    for (const LayerMark &m : layerMarks()) {
        const int d = qAbs(m.x - pos.x()) - (m.own ? 1 : 0); // the selected layer's wins a tie
        if (d < best) {
            best = d;
            *mark = m;
            found = true;
        }
    }
    return found;
}

bool AnimaticTimeline::layerEaseMarkAt(const QPoint &pos, LayerSegment *segment, QRectF *markRect) const
{
    if (!inLayerRow(pos))
        return false;
    for (const LayerSegment &s : layerSegments(layerMarks())) {
        const QRectF r = layerEaseMarkRect(s);
        if (r.adjusted(-1, -2, 1, 2).contains(QPointF(pos))) {
            *segment = s;
            *markRect = r;
            return true;
        }
    }
    return false;
}

// A DRAWN ease mark under the point - the pill or the small mark, not a short move's line, and
// not a greyed move's (Animate off: no mark is drawn). A press there is the mark's whatever
// key reaches under it (the user's note 6, 2026-11-26).
bool AnimaticTimeline::onLayerEaseMark(const QPoint &pos) const
{
    LayerSegment segment;
    QRectF markRect;
    return layerEaseMarkAt(pos, &segment, &markRect) && !segment.lineOnly && !segment.greyed;
}

bool AnimaticTimeline::onCameraEaseMark(const QPoint &pos) const
{
    CameraSegment segment;
    QRectF markRect;
    return m_tracks.cameraVisible && easeMarkAt(pos, &segment, &markRect) && !segment.lineOnly;
}

void AnimaticTimeline::paintLayerRow(QPainter &p)
{
    using namespace SankoTimelineTheme;
    if (!m_layerRowShown || !m_canvas)
        return;
    paintDrawingsLane(p); // under the keys' lane, the selected layer's drawings (batch I-2)
    const int y0 = layerTrackY();
    const qreal cy = y0 + kLayerRowH / 2.0;
    p.save();
    p.setClipRect(QRect(kLabelCol, y0, m_canvas->width() - kLabelCol, kLayerRowH), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QVector<LayerMark> marks = layerMarks();
    const QColor grey(0x6f, 0x6f, 0x80);
    // The layer keys' colour in full (Preferences > Keyframes; SankoKeyColors): the line and a
    // resting key's fill the colour, its outline the colour toward white, a dim mark the colour at
    // 60 %. Greyed (Animate off) stays grey; the key at the playhead takes the playhead's colours.
    using SankoKeyColors::Row;
    const QColor segment = SankoKeyColors::key(Row::Layer);
    const QColor keyFill = SankoKeyColors::key(Row::Layer);
    const QColor keyEdge = SankoKeyColors::keyEdge(Row::Layer);
    const QColor dimInk = SankoKeyColors::dim(Row::Layer);
    for (const LayerSegment &s : layerSegments(marks)) {
        const QColor c = s.greyed ? grey : segment;
        if (s.dashed) {
            QPen pen(c, 2.0, Qt::CustomDashLine, Qt::FlatCap);
            pen.setDashPattern({2.5, 2.0});
            p.setPen(pen);
            p.drawLine(QPointF(s.x0, cy), QPointF(s.x1, cy));
        } else {
            p.fillRect(QRectF(s.x0, cy - 1.0, s.x1 - s.x0, 2.0), c);
        }
        if ((s.glyph || s.smallMark) && !s.greyed)
            paintEaseGlyph(p, layerEaseMarkRect(s), shotcam::Ease(s.ease), false, s.smallMark);
    }
    const int phFrame = playheadFrame();
    for (const LayerMark &m : marks) {
        const QPointF c(m.x + 0.5, cy);
        const bool atPlayhead = qAbs(m.frame - phFrame) < 0.5;
        if (!m.own) {
            // ANOTHER LAYER'S KEY, dim: a small hollow diamond - where the film has motion.
            const qreal h = kKeyHalf - 2.0;
            QPolygonF d;
            d << QPointF(c.x(), c.y() - h) << QPointF(c.x() + h, c.y()) << QPointF(c.x(), c.y() + h)
              << QPointF(c.x() - h, c.y());
            QColor ink = dimInk;
            if (m.greyed) {
                ink = grey;
                ink.setAlphaF(0.55);
            }
            p.setPen(QPen(ink, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(d);
            continue;
        }
        QColor fill = m.greyed ? QColor(0x2a, 0x2a, 0x30) : keyFill;
        QColor edge = m.greyed ? grey : keyEdge;
        if (atPlayhead && !m.greyed) {
            fill = kPlayhead;
            edge = Qt::white;
        }
        QPolygonF diamond;
        diamond << QPointF(c.x(), c.y() - kKeyHalf) << QPointF(c.x() + kKeyHalf, c.y())
                << QPointF(c.x(), c.y() + kKeyHalf) << QPointF(c.x() - kKeyHalf, c.y());
        p.setPen(QPen(edge, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(fill);
        p.drawPolygon(diamond);
    }
    p.restore();
}

QVariantList AnimaticTimeline::layerKeysForTest() const
{
    QVariantList list;
    const int phFrame = playheadFrame();
    for (const LayerMark &m : layerMarks())
        list.append(QVariantMap{{QStringLiteral("flat"), m.flat},
                                {QStringLiteral("layer"), m.layer},
                                {QStringLiteral("key"), m.key},
                                {QStringLiteral("x"), m.x},
                                {QStringLiteral("frame"), m.frame},
                                {QStringLiteral("own"), m.own},
                                {QStringLiteral("greyed"), m.greyed},
                                {QStringLiteral("atPlayhead"), qAbs(m.frame - phFrame) < 0.5}});
    return list;
}

QVariantList AnimaticTimeline::layerSegmentsForTest() const
{
    QVariantList list;
    for (const LayerSegment &s : layerSegments(layerMarks()))
        list.append(QVariantMap{{QStringLiteral("flat"), s.flat},
                                {QStringLiteral("layer"), s.layer},
                                {QStringLiteral("from"), s.fromKey},
                                {QStringLiteral("to"), s.toKey},
                                {QStringLiteral("ease"), shotcam::easeLabel(shotcam::Ease(s.ease))},
                                {QStringLiteral("dashed"), s.dashed},
                                {QStringLiteral("glyph"), s.glyph},
                                {QStringLiteral("small"), s.smallMark},
                                {QStringLiteral("lineOnly"), s.lineOnly},
                                {QStringLiteral("greyed"), s.greyed},
                                {QStringLiteral("markRect"), layerEaseMarkRect(s)},
                                {QStringLiteral("x0"), s.x0},
                                {QStringLiteral("x1"), s.x1}});
    return list;
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
        const double f = start + shotcam::keyFrame(*scene, scene->camera.keys.at(ki), m_fps,
                                                   qMax(0, start));
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
        user = (m_hScroll && m_hScroll->isSliderDown()) || m_wheelOnScrollbar || m_panScrolling;
        break;
    default:
        break;
    }
    if (user && m_hScroll)
        onUserScroll(m_hScroll->sliderPosition());
}

bool AnimaticTimeline::eventFilter(QObject *watched, QEvent *event)
{
    // The typing field: Enter sets, Esc cancels (Figma DUR-1).
    if (m_durEdit && watched == m_durEdit && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            closeDurationField(true);
            return true;
        }
        if (key == Qt::Key_Escape) {
            closeDurationField(false);
            return true;
        }
    }
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
            // The waveform (Phase 2D): none / reading / ready / plain.
            {QStringLiteral("timeline.waveform"), audioWaveformStateForTest()},
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
            // Its length at the timeline's rate - or, while a length drag
            // holds it, the length the drag has reached.
            int frames = qMax(kMinFrames, shotcam::panelFrames(*panel, m_fps));
            if (m_drag != Drag::None && m_dragFramesFor.contains(flat))
                frames = qMax(kMinFrames, m_dragFramesFor.value(flat));

            Block b;
            b.flatIndex = flat;
            b.sceneIndex = si;
            b.panelIndex = pi;
            b.sceneNumber = scene->number;
            b.sceneName = scene->location.isEmpty()
                ? QStringLiteral("Scene %1").arg(scene->number)
                : scene->location;
            b.frames = frames;
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
    // The whole seconds the clips fill (the ruler ticks up to here).
    int f = 0;
    for (const Block &b : m_blocks)
        f += b.frames;
    return f / qMax(1, m_fps);
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
        return shotcam::neutral(shotcam::panelStage(*scene->panels.at(b.panelIndex), m_stageMargin)); // the eye is off
    return shotcam::evaluateAtPanel(*scene, b.panelIndex, 0, m_fps,
                                    qMax(0, sceneStartFrame(b.sceneIndex)));
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

// The next rung of the ladder above (direction > 0) or below the zoom: 100 * 2^(k/2), so
// every second rung is 100 times a power of two (200, 400, 800, 1600; 50, 25, 12.5);
// a zoom on a rung (within 0.5 %) moves a whole rung. Clamped to 10..1600: below 12.5 the
// next rung down is the minimum, 10.
float AnimaticTimeline::zoomStepFrom(float zoom, int direction)
{
    const double k = 2.0 * std::log2(double(zoom) / 100.0); // the zoom's place on the ladder
    const double nearest = std::round(k);
    const bool onRung = std::abs(k - nearest) < 2.0 * std::log2(1.005);
    double next;
    if (direction > 0)
        next = onRung ? nearest + 1.0 : std::ceil(k);
    else
        next = onRung ? nearest - 1.0 : std::floor(k);
    const double z = 100.0 * std::pow(2.0, next / 2.0);
    return float(qBound(double(kZoomMin), z, double(kZoomMax)));
}

void AnimaticTimeline::stepZoom(int direction)
{
    applyZoom(zoomStepFrom(m_zoom, direction));
}

void AnimaticTimeline::applyZoom(float z)
{
    m_zoom = qBound(kZoomMin, z, kZoomMax);
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

int AnimaticTimeline::rulerBottomForTest() const
{
    return kRulerY + kRulerH;
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
// 1600; anything else restores the value the field showed before.
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
            applyZoom(static_cast<float>(qBound(double(kZoomMin), v, double(kZoomMax))));
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
    z = qBound(kZoomMin, z, kZoomMax);
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
    // The clips' art exits (batch 6d) checked once a paint, scene by scene; the
    // clips, the chips and the CAMERA row then read what this found.
    m_artExitsTrusted = false;
    for (int s = 0; s < m_scenes.size(); ++s)
        artExitsOf(s);
    m_artExitsTrusted = true;
    const auto untrustArtExits = qScopeGuard([this] { m_artExitsTrusted = false; });
    p.fillRect(QRect(0, kRulerY, W, kRulerH), kRulerBg);
    p.fillRect(QRect(0, kSceneY, W, kSceneH), kHeaderBg);
    p.fillRect(QRect(0, kPanelY, W, panelTrackH()), kPageBg);
    p.fillRect(QRect(0, cameraTrackY(), W, kCameraH), kCameraRowBg);
    p.fillRect(QRect(0, cameraTrackY(), W, 1), kHairline);
    if (m_layerRowShown) { // the LAYER row (batch I-1), as the CAMERA row is
        p.fillRect(QRect(0, layerTrackY(), W, layerRowH()), kCameraRowBg); // taller with a drawings lane
        p.fillRect(QRect(0, layerTrackY(), W, 1), kHairline);
    }
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
        // EVERY FRAME from 800 % (batch C, Figma C-2): 2 px at 14 %, under the half
        // seconds' and the seconds', where none of those stands; only the frames in view.
        if (ppf >= kFrameTicksPpf) {
            QColor frameTick = kHairline;
            frameTick.setAlphaF(0.14);
            const int first = qMax(1, int(std::floor(screenXToContent(kLabelCol) / ppf)));
            const int last = qMin(totalFrames(), int(std::ceil(screenXToContent(contentRight) / ppf)));
            for (int f = first; f <= last; ++f) {
                if (f % m_fps == 0 || (halfTicks && (2 * f) % m_fps == 0))
                    continue; // a second's tick, or a half second's
                const int x = contentXToScreen(f * ppf);
                if (x < kLabelCol || x > contentRight) continue;
                p.fillRect(QRect(x, bottom - 2, 1, 2), frameTick);
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
            Q_UNUSED(startFrame);
            Q_UNUSED(endFrame);
            // SCENES + SHOTS (Figma 621:716): the block over its group of clips - its span less
            // its share of the 3 px gaps (sceneBlockRect) - and no separator line: the gap,
            // the row's own ground, is what parts two scenes.
            const QRect r = sceneBlockRect(si);
            Q_UNUSED(sceneOrdinal);
            p.fillRect(r, kSceneBlock);
            // THE SCENE'S COLOUR (batch E, Figma E-1): behind its name at 40 %, and as its
            // baseline, 2 px - Auto the muted set's at its place (SceneColors.h).
            const QColor sceneColor = si < m_scenes.size() ? scenecolor::colorOf(*m_scenes.at(si), si)
                                                           : QColor(kSceneBaseline);
            QColor tint = sceneColor;
            tint.setAlphaF(0.4);
            p.fillRect(r, tint);
            // A LOCKED SCENE (batch F1, Figma F-1): its baseline in the accent, 2 px.
            const bool locked = sceneLocked(si);
            p.fillRect(QRect(r.left(), kSceneY + kSceneH - 2, r.width(), 2),
                       locked ? kTrackControlActive : sceneColor);
            p.setFont(f);
            p.setPen(kSceneName);
            // Its lock before the name: always on a locked scene, faint on hover on
            // another (a click locks it); the two centred together. Otherwise as before.
            if (locked || si == m_sceneHover) {
                const QRect g = sceneLockGlyphRect(si);
                const QColor faint = m_sceneLockHover ? kTrackControl : QColor(0x6f, 0x6f, 0x80);
                paintSceneLock(p, g, locked ? kTrackControlActive : faint);
                p.setPen(kSceneName);
                p.drawText(QRect(g.right() + 5, kSceneY, qMax(0, r.right() - g.right() - 4), kSceneH),
                           Qt::AlignVCenter | Qt::AlignLeft, name);
            } else {
                p.drawText(r, Qt::AlignCenter, name);
            }
            i = j;
            ++sceneOrdinal;
        }
    }

    // ----- Panel / shot clips (the redesign) -----
    // Flat bodies, touching within a scene (SCENES + SHOTS, 2026-11-18; they
    // had a 2 px gap between neighbours), the thumbnail across
    // the top with the number over it, the name bottom-left and the
    // duration pill bottom-right; the selected clip is a lighter body with
    // a 2 px border, nothing else (the design's glow and trim handles are
    // hidden layers, not built; the 6 px grab zones at the edges stay, with
    // the size cursor as their only cue). Layout: clipLayout().
    // A hidden SHOTS track paints no clip (and, below, no loop overlay and
    // no drop indicator; a press on it finds nothing - canvasMousePress).
    if (m_tracks.shotsVisible) {
        // EVERY ROW INSIDE ITS OWN BOUNDS (the user's note 5, 2026-11-26): whatever the track's
        // height, nothing of a clip is drawn outside the SHOTS track.
        p.save();
        p.setClipRect(QRect(kLabelCol, kPanelY, qMax(0, contentRight - kLabelCol + 1), panelTrackH()),
                      Qt::IntersectClip);
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont nameFont = font();
        nameFont.setPixelSize(11);
        QFont numberFont = nameFont;
        numberFont.setBold(true);
        QFont mono(QStringLiteral("Consolas"));
        mono.setPixelSize(10);

        // A clip narrower than kMinClipPx at this zoom (a 1-frame clip) is
        // drawn that wide, and LAST, over the start of the clip after it,
        // so it stays visible (2026-11-06). At any zoom where every clip is
        // wider, the order and every rect are what they always were.
        QVector<int> order;
        order.reserve(m_blocks.size());
        for (int i = 0; i < m_blocks.size(); ++i)
            if (m_blocks.at(i).frames * ppf >= kMinClipPx)
                order.append(i);
        for (int i = 0; i < m_blocks.size(); ++i)
            if (m_blocks.at(i).frames * ppf < kMinClipPx)
                order.append(i);
        for (int oi : std::as_const(order)) {
            const Block &b = m_blocks.at(oi);
            const int x0 = contentXToScreen(b.startFrame * ppf);
            const int x1 = qMax(x0 + kMinClipPx, contentXToScreen((b.startFrame + b.frames) * ppf));
            if (x1 < kLabelCol || x0 > contentRight) continue;
            const QRect r(x0, kPanelY + 2, x1 - x0, panelTrackH() - 4);
            const ClipLayout L = clipLayout(oi, r);
            const QRectF bf(L.body);

            // "current" is the SELECTED panel - the one on the drawing
            // canvas - not the one under the playhead: playback moves the
            // playhead through the clips and leaves the selection alone.
            const bool current = (b.flatIndex == m_selected);
            const bool hovered = (b.flatIndex == m_hoverIndex);

            // SCENES + SHOTS (2026-11-18, Figma 621:716): a scene's clips touch. Each clip
            // owns the line at its own cut (its left edge); only its scene's last clip - or
            // one drawn wider than its frames, over its neighbour - closes its right side, so
            // two neighbours share one line. The corners are round only at the scene's ends.
            // The states are drawn inside the clip's own span, over the strip.
            const bool roundL = L.edges.first, roundR = L.edges.last;
            const bool closed = roundR || b.frames * ppf < kMinClipPx;
            const qreal rad = L.edges.radius;
            // A plain clip's 1 px line is the ring its body leaves: the shape in the line's
            // colour, the body over it 1 px in (not on the open right side). Filled, not
            // stroked - a stroke's flat end left the line's last column part-covered.
            const bool plainLine = !current && !hovered;
            p.setPen(Qt::NoPen);
            p.setBrush(plainLine ? kClipBorder : (current ? kClipSelectedBg : kClipBg));
            p.drawPath(clipShape(bf, rad, roundL, roundR));
            if (plainLine) {
                p.setBrush(kClipBg);
                p.drawPath(clipShape(bf.adjusted(1, 1, closed ? -1 : 0, -1), qMax(0.0, rad - 1), roundL, roundR));
            }
            p.setBrush(Qt::NoBrush);
            // THE SCENE'S COLOUR (batch E, Figma E-1; placed by 621:716): a strip 1 px under the
            // clip's top, 3 px in from each side, 3 px high - under a selected or amber clip's
            // 2 px edge, which stays whole, and over the thumbnail (6 px down).
            if (b.sceneIndex < m_scenes.size()) {
                p.setBrush(scenecolor::colorOf(*m_scenes.at(b.sceneIndex), b.sceneIndex));
                p.drawRoundedRect(QRectF(bf.left() + 3, bf.top() + 1, qMax(0.0, bf.width() - 6), 3), 1, 1);
                p.setBrush(Qt::NoBrush);
            }
            if (current) {
                p.setPen(QPen(kClipSelectedBorder, 2));
                p.drawPath(clipShape(bf.adjusted(1, 1, -1, -1), rad, roundL, roundR));
            } else if (hovered) {
                QColor bord = kClipSelectedBorder;
                bord.setAlphaF(0.50);
                p.setPen(QPen(bord, 1));
                p.drawPath(clipShape(bf.adjusted(0.5, 0.5, -0.5, -0.5), rad, roundL, roundR));
            }

            if (L.thumbShown) {
                QPixmap pix;
                // THE SHOT CAMERA (Phase 1A): the clip shows the cut as it
                // begins - the camera at the panel's first frame. A scene
                // without a camera move draws exactly as before.
                bool shot = false;
                shotcam::CameraState shotCamera;
                shotcam::Stage stage;
                const Panel *shotPanel = nullptr; // depth's layered clip (Phase 2B)
                if (b.sceneIndex < m_scenes.size()) {
                    Scene *sc = m_scenes.at(b.sceneIndex);
                    if (b.panelIndex < sc->panels.size()) {
                        const Panel *panel = sc->panels.at(b.panelIndex);
                        shotPanel = panel;
                        stage = shotcam::panelStage(*panel, m_stageMargin);
                        // The CAMERA eye off (Phase 1C): the neutral camera.
                        if (!sc->camera.isEmpty() && m_tracks.cameraVisible) {
                            shotCamera = shotcam::evaluateAtPanel(
                                *sc, b.panelIndex, 0, m_fps, qMax(0, sceneStartFrame(b.sceneIndex)));
                            shot = !shotcam::isNeutral(shotCamera, stage);
                        }
                        // The cached 512px mip, not the full flatten: this
                        // runs per visible clip on EVERY repaint (each scrub
                        // mouse-move and every playback tick); the full
                        // composite cost 16 ms/clip at 4K, measured. With a
                        // margin (Phase 2A) the base framing is the FRAME's
                        // own mip, cropped to fill as the whole panel was.
                        pix = (stage.hasMargin() && !shot)
                            ? panel->flattenedFrameThumb(stage.frameRect())
                            : panel->flattenedThumb();
                    }
                }
                p.save();
                QPainterPath clip;
                clip.addRoundedRect(QRectF(L.thumb), kThumbR, kThumbR);
                p.setClipPath(clip, Qt::IntersectClip);
                if (!pix.isNull() && shot && shotPanel
                    && shotcam::layeredShot(*shotPanel, shotCamera, stage)) {
                    // Depth: every layer placed for its depth, drawn once
                    // into a clip-sized picture and cached by what shapes it.
                    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
                    QImage clipImage = shotcam::shotThumb(
                        *shotPanel, shotCamera, stage,
                        QSize(qRound(L.thumb.width() * dpr), qRound(L.thumb.height() * dpr)),
                        true);
                    clipImage.setDevicePixelRatio(dpr);
                    p.drawImage(L.thumb.topLeft(), clipImage);
                } else if (!pix.isNull() && shot) {
                    // The shot, cropped to fill like the plain thumbnail.
                    p.fillRect(L.thumb, Qt::black);
                    shotcam::drawShot(p, pix, stage, shotCamera, QRectF(L.thumb), true);
                } else if (!pix.isNull() && L.thumb.width() > pix.width()) {
                    // LARGER THAN ITS PICTURE (a tall SHOTS track): drawn straight from the
                    // 512 px picture, whole - the thumbnail's rect has the picture's shape
                    // (clipLayout, 2026-11-26), so nothing is cropped and nothing stretched.
                    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                    p.drawPixmap(QRectF(L.thumb), pix, QRectF(pix.rect()));
                } else if (!pix.isNull()) {
                    // The whole picture, reduced to the thumbnail's rect - which has its
                    // shape. (It was cropped to fill a strip across the clip: the user's
                    // choice of 2026-10-18, changed by their note 5 of 2026-11-26.)
                    p.drawPixmap(L.thumb.topLeft(),
                                 pix.scaled(L.thumb.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
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
                p.drawText(L.pill, Qt::AlignCenter, pillText(b));
            }
            // A MOVE THAT LEAVES THIS PANEL'S ART (batch 6d, Figma MARGIN-2): the clip
            // outlined amber - a picture, not a button (GROW-1); its chip, below.
            if (artExitOf(b.flatIndex, nullptr, nullptr, nullptr, nullptr)) {
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(QPen(QColor(0xf5, 0xa6, 0x23), 2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(clipShape(bf.adjusted(1, 1, -1, -1), rad, roundL, roundR));
            }
        }
        // THE AMBER CHIPS (GROW-1), after every clip: a marker centred over a clip too
        // narrow for it reaches over its neighbours, and is drawn above them, as it is
        // hit before them (artExitChipAt). Pressed: Grow to fit.
        for (const Block &b : std::as_const(m_blocks)) {
            QString label;
            bool over = false;
            const QRect chip = artExitChipRect(b.flatIndex, &label, &over);
            if (chip.isEmpty() || chip.right() < kLabelCol || chip.left() > contentRight)
                continue;
            const QColor amber(0xf5, 0xa6, 0x23);
            p.setRenderHint(QPainter::Antialiasing, true);
            if (over) { // a ring of the shots track's ground parts it from the clips under it
                p.setPen(Qt::NoPen);
                p.setBrush(kPageBg);
                p.drawRoundedRect(QRectF(chip).adjusted(-1.5, -1.5, 1.5, 1.5), 4, 4);
            }
            QFont f = font();
            f.setPixelSize(10);
            f.setBold(label == QStringLiteral("!"));
            p.setFont(f);
            p.setPen(Qt::NoPen);
            p.setBrush(amber);
            p.drawRoundedRect(QRectF(chip), 3, 3);
            p.setPen(QColor(0x1c, 0x1e, 0x25));
            p.drawText(chip, Qt::AlignCenter, label);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        // A LENGTH DRAG (2026-11-06, Figma DUR-1): a dashed line where the
        // moving edge started.
        if (m_dragOriginFrame >= 0
            && (m_drag == Drag::ResizeRight || m_drag == Drag::ResizeLeft || m_drag == Drag::Roll)) {
            const int x = contentXToScreen(m_dragOriginFrame * ppf);
            p.setPen(QPen(kClipSelectedBorder, 1, Qt::DashLine));
            p.drawLine(x, kPanelY + 2, x, kPanelY + panelTrackH() - 3);
        }
        p.restore(); // the SHOTS track's clip
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
    paintLayerRow(p); // batch I-1

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
            // THE WAVEFORM (Phase 2D, Figma 2D-1 / 2D-2): once the track's
            // peaks are in, each screen column of the visible bar is a line
            // from its lowest peak to its highest, in the blue at 85 %,
            // about a zero line - the columns are the ruler's own mapping
            // (waveformColumns), so the sound zooms and scrolls with the
            // clips and the CAMERA row. Before they are in, the plain bar.
            const AudioPeaks::State wave = audioWaveformState();
            const QVector<WaveColumn> columns = waveformColumns(bar);
            if (!columns.isEmpty()) {
                QColor ink(0x4d, 0x9f, 0xff);
                ink.setAlphaF(0.5);
                p.setPen(QPen(ink, 1.0));
                const int cy = bar.rect.top() + bar.rect.height() / 2;
                p.drawLine(qMax(bar.rect.left(), kLabelCol), cy,
                           qMin(bar.rect.right(), contentRight), cy);
                ink.setAlphaF(0.85);
                p.setPen(QPen(ink, 1.0));
                for (const WaveColumn &c : columns)
                    p.drawLine(c.x, qRound(c.top), c.x, qRound(c.bottom));
            }
            paintPinLines(p); // the pinned keys' lines (Phase 2E), under the chips
            // The texts stay inside the VISIBLE part of the bar, so the
            // name is readable however far the timeline is scrolled. Over a
            // waveform they sit on OPAQUE chips (rule 9) - hidden while the
            // pointer is over the row, so the sound under them can be seen
            // (the user's addition, 2026-11-01). While the track is being
            // read the name says so.
            const int left = qMax(bar.rect.left(), kLabelCol) + 8;
            const int right = qMin(bar.rect.right(), contentRight) - 8;
            const bool chips = !columns.isEmpty();
            if (!(chips && m_audioHover)) {
                const QColor chipFill(0x0d, 0x1a, 0x26); // the row's own fill
                const QString nameText = wave == AudioPeaks::Reading
                    ? bar.name + QStringLiteral("  ") + QChar(0x00B7)
                        + QStringLiteral("  reading the sound") + QChar(0x2026)
                    : bar.name;
                const int lengthW = fm.horizontalAdvance(bar.length);
                const bool showLength = right - left > lengthW + 60;
                if (showLength) {
                    const QRect lengthR(right - lengthW, bar.rect.top(), lengthW,
                                        bar.rect.height());
                    if (chips)
                        p.fillRect(lengthR.adjusted(-6, 3, 6, -3), chipFill);
                    p.setPen(QColor("#88a6c4"));
                    p.drawText(lengthR, Qt::AlignVCenter | Qt::AlignRight, bar.length);
                }
                const int nameW = right - left - (showLength ? lengthW + 12 : 0);
                if (nameW > 12) {
                    const QString shown = fm.elidedText(nameText, Qt::ElideMiddle, nameW);
                    if (chips)
                        p.fillRect(QRect(left - 6, bar.rect.top() + 3,
                                         fm.horizontalAdvance(shown) + 12,
                                         bar.rect.height() - 6),
                                   chipFill);
                    p.setPen(QColor("#88a6c4"));
                    p.drawText(QRect(left, bar.rect.top(), nameW, bar.rect.height()),
                               Qt::AlignVCenter | Qt::AlignLeft, shown);
                }
            }
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
        if (bar.state != AudioBar::Present) // no bar to draw them under: now
            paintPinLines(p);
    }

    // THE LOCKED SCENES' ENDS (batch F1, Figma F-1; Q-F2): a 2 px accent line across
    // SCENES and SHOTS with a flag at its top pointing into the scene, and 1 px at 45 %
    // through CAMERA and AUDIO - the end against the sound.
    if (m_tracks.scenesVisible)
        for (int si = 0; si < m_scenes.size(); ++si) {
            if (!sceneLocked(si))
                continue;
            const int end = sceneEndFrame(si);
            if (end < 0)
                continue;
            const int x = contentXToScreen(end * ppf);
            const int shotsBottom = kPanelY + panelTrackH();
            p.fillRect(QRect(x - 1, kSceneY, 2, shotsBottom - kSceneY), kTrackControlActive);
            QColor faint = kTrackControlActive;
            faint.setAlphaF(0.45);
            p.fillRect(QRect(x - 1, shotsBottom, 1, qMax(0, tracksH() - shotsBottom)), faint);
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(kTrackControlActive);
            p.drawPolygon(QPolygonF({QPointF(x - 1, kSceneY + 2), QPointF(x - 9, kSceneY + 5.5),
                                     QPointF(x - 1, kSceneY + 9)}));
            p.restore();
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
        // THE LAYER ROW (batch I-1): "LAYER · Boat", the selected layer's name, elided to
        // the column.
        if (m_layerRowShown) {
            // (Without a drawings lane the + - New Drawing, batch I-2 - sits at the row's
            // left, and the name gives it its 20 px; with one, the + is beside the lane.)
            const bool plusInRow = m_newDrawingButton && m_newDrawingButton->isVisible() && !m_layerRowRun;
            const int left = plusInRow ? 28 : 8;
            const QRect r(left, layerTrackY(), kLabelColW - 15 - left, kLayerRowH);
            p.drawText(r, Qt::AlignVCenter | Qt::AlignRight,
                       QFontMetrics(f).elidedText(layerRowLabel(), Qt::ElideRight, r.width()));
            // The lane's own label: FRAMES, a track's label like SCENES, SHOTS, CAMERA and AUDIO -
            // their font, their colour, their right edge (the user's word, 2026-11-27; until
            // then a small dimmer "drawings").
            if (m_layerRowRun) {
                // (After the + and Zoom to Frames: 4 + 20 + 3 + 20, and 3 px.)
                p.drawText(QRect(50, layerTrackY() + kDrawingsLaneY, kLabelColW - 15 - 50, kDrawingsLaneH),
                           Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("FRAMES"));
            }
        }

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

// SCENES + SHOTS (2026-11-18, Figma 621:716): where a clip sits in its scene. The gap
// between two scenes is the cut's own column and one each side of it (the left scene
// gives up 1 px, the right 2) - whole px, so every line stays one crisp pixel - and
// shrinks with the room: none from a scene under 6 px, 1 px from one under 12; never at
// the film's two ends. The outer corners' radius is the design's 5, at most half the
// scene's drawn width.
AnimaticTimeline::ClipEdges AnimaticTimeline::clipEdges(int flatIndex) const
{
    ClipEdges e;
    if (flatIndex < 0 || flatIndex >= m_blocks.size())
        return e;
    const int si = m_blocks.at(flatIndex).sceneIndex;
    int a = flatIndex, z = flatIndex;
    while (a > 0 && m_blocks.at(a - 1).sceneIndex == si)
        --a;
    while (z + 1 < m_blocks.size() && m_blocks.at(z + 1).sceneIndex == si)
        ++z;
    e.first = a == flatIndex;
    e.last = z == flatIndex;
    const double ppf = pxPerFrame();
    const int w = contentXToScreen((m_blocks.at(z).startFrame + m_blocks.at(z).frames) * ppf)
        - contentXToScreen(m_blocks.at(a).startFrame * ppf);
    const int startIn = a > 0 ? (w >= 12 ? 2 : w >= 6 ? 1 : 0) : 0;
    const int endIn = z + 1 < m_blocks.size() ? (w >= 6 ? 1 : 0) : 0;
    e.startInset = e.first ? startIn : 0;
    e.endInset = e.last ? endIn : 0;
    e.radius = qMax(0.0, qMin(kClipR, (w - startIn - endIn) / 2.0));
    return e;
}

QRect AnimaticTimeline::sceneBlockRect(int sceneIndex) const
{
    const QRect span = sceneRectOnScreen(sceneIndex);
    if (span.isEmpty())
        return span;
    int first = -1, last = -1;
    for (int i = 0; i < m_blocks.size(); ++i)
        if (m_blocks.at(i).sceneIndex == sceneIndex) {
            if (first < 0)
                first = i;
            last = i;
        }
    if (first < 0)
        return span;
    const int startIn = clipEdges(first).startInset, endIn = clipEdges(last).endInset;
    return QRect(span.left() + startIn, span.top(), qMax(1, span.width() - startIn - endIn), span.height());
}

// The shape of what a clip's thumbnail shows: the panel's FRAME - what the camera sees at its
// base framing, and what every picture the clip draws is cut to (the frame's own mip, the shot
// through the camera). 16:9 for a clip with no panel behind it.
QSizeF AnimaticTimeline::clipPictureSize(int flatIndex) const
{
    if (flatIndex >= 0 && flatIndex < m_blocks.size()) {
        const Block &b = m_blocks.at(flatIndex);
        const Panel *panel = b.sceneIndex < m_scenes.size() ? m_scenes.at(b.sceneIndex)->panels.value(b.panelIndex)
                                                             : nullptr;
        if (panel) {
            const QSize frame = shotcam::panelStage(*panel, m_stageMargin).frameRect().size();
            if (frame.width() > 0 && frame.height() > 0)
                return QSizeF(frame);
        }
    }
    return QSizeF(16.0, 9.0);
}

// The design's clip layout for clip flatIndex drawn in rect r (see the constants at the
// top): the body is r less its scene's gaps (SCENES + SHOTS: a clip inside its scene
// keeps its whole span).
AnimaticTimeline::ClipLayout AnimaticTimeline::clipLayout(int flatIndex, const QRect &r) const
{
    ClipLayout L;
    const QString pillText = flatIndex >= 0 && flatIndex < m_blocks.size() ? this->pillText(m_blocks.at(flatIndex))
                                                                            : QString();
    L.edges = clipEdges(flatIndex);
    L.body = QRect(r.left() + L.edges.startInset, r.top(),
                   qMax(1, r.width() - L.edges.startInset - L.edges.endInset), r.height());
    const int bw = L.body.width();
    // A clip's own corners: round only at its scene's ends, no wider than it is.
    const bool both = L.edges.first && L.edges.last;
    L.edges.radius = (L.edges.first || L.edges.last) ? qMin(L.edges.radius, both ? bw / 2.0 : double(bw)) : 0.0;
    // THE THUMBNAIL (see kNameStripH): its picture's shape, fitted whole into the room it has -
    // the body less the insets across, and down either the storey over the name's strip or,
    // on a body too short for two storeys, the body's height less a small inset.
    const int bh = L.body.height();
    const QSizeF picture = clipPictureSize(flatIndex);
    const int roomW = bw - 2 * kThumbInset;
    const bool storeys = bh >= kThumbInset + kStoreyMinThumbH + kNameStripH;
    const int roomH = storeys ? bh - kThumbInset - kNameStripH : bh - 2 * kShortInset;
    const double fit = roomW > 0 && roomH > 0
        ? qMin(roomW / picture.width(), roomH / picture.height()) : 0.0;
    const int tw = qMin(roomW, qRound(picture.width() * fit)), th = qMin(roomH, qRound(picture.height() * fit));
    L.thumbShown = bw >= kThumbMinBody && th >= kThumbMinH && tw > 0;
    L.thumb = QRect(L.body.left() + kThumbInset, L.body.top() + (storeys ? kThumbInset : (bh - th) / 2),
                    qMax(0, tw), qMax(0, th));
    L.number = QRect(L.body.left() + 10, L.body.top() + qMin(9, qMax(1, bh - 13)), qMax(1, bw - 14), 12);
    // The pill fits its text (2026-11-06): 28 px for "3s", wider for
    // "1s 12f"; shown while the body has room for it and 14 px more.
    int pillW = kPillW;
    if (!pillText.isEmpty()) {
        QFont mono(QStringLiteral("Consolas"));
        mono.setPixelSize(10);
        pillW = qMax(kPillW, QFontMetrics(mono).horizontalAdvance(pillText) + 10);
    }
    // (On a body shorter than the pill and its 4 px, the pill keeps to the body: centred in it.)
    L.pillShown = bw >= qMax(kPillMinBody, pillW + 14) && bh >= kPillH;
    L.pill = QRect(L.body.right() - pillW - 5,
                   bh >= kPillH + 8 ? L.body.bottom() - kPillH - 4 : L.body.top() + (bh - kPillH) / 2, pillW, kPillH);
    const int nameRight = L.pillShown ? L.pill.left() - 4 : L.body.right() - 8;
    // The name: under the picture on a body with two storeys; beside it on a shorter one.
    const int nameLeft = storeys || !L.thumbShown ? L.body.left() + 8 : L.thumb.right() + 7;
    L.name = QRect(nameLeft, bh >= 23 ? L.body.bottom() - 20 : L.body.top() + qMax(0, (bh - 15) / 2),
                   qMax(0, nameRight - nameLeft), qMin(15, bh));
    L.nameShown = bw >= kNameMinBody && L.name.width() > 0
        && (storeys || !L.thumbShown || L.name.width() >= kShortNameMinW);
    return L;
}

QVariantMap AnimaticTimeline::clipPartsForTest(int flatIndex) const
{
    const QRect r = clipRect(flatIndex);
    if (r.isEmpty())
        return {};
    const ClipLayout L = clipLayout(flatIndex, r);
    const auto rs = [](const QRect &q) {
        return QStringLiteral("%1,%2 %3x%4").arg(q.x()).arg(q.y()).arg(q.width()).arg(q.height());
    };
    return {{QStringLiteral("clip"), rs(r)},       {QStringLiteral("body"), rs(L.body)},
            {QStringLiteral("thumb"), rs(L.thumb)}, {QStringLiteral("number"), rs(L.number)},
            {QStringLiteral("name"), rs(L.name)},   {QStringLiteral("pill"), rs(L.pill)},
            {QStringLiteral("thumbShown"), L.thumbShown},
            {QStringLiteral("nameShown"), L.nameShown},
            {QStringLiteral("pillShown"), L.pillShown},
            // SCENES + SHOTS: the body's place in its scene
            {QStringLiteral("first"), L.edges.first},
            {QStringLiteral("last"), L.edges.last},
            {QStringLiteral("radius"), L.edges.radius}};
}

int AnimaticTimeline::labelColumnWidthForTest()
{
    return kLabelColW;
}

QString AnimaticTimeline::pillText(const Block &b) const
{
    return m_framesMode ? shotcam::framesText(b.frames) : shotcam::lengthText(b.frames, m_fps);
}

QString AnimaticTimeline::pillTextForTest(int flatIndex) const
{
    return flatIndex >= 0 && flatIndex < m_blocks.size() ? pillText(m_blocks.at(flatIndex)) : QString();
}

QRect AnimaticTimeline::pillRectForTest(int flatIndex) const
{
    const QRect r = clipRect(flatIndex);
    if (r.isEmpty() || flatIndex >= m_blocks.size())
        return QRect();
    const ClipLayout L = clipLayout(flatIndex, r);
    return L.pillShown ? L.pill : QRect();
}

// --- THE TYPING FIELD (2026-11-06, Figma DUR-1) ----------------------------
// "Panel 2 · Duration", the number in frames, "= 1s 12f at 24 fps" beside
// it, Cancel and Set. A child of the window (so it is not cut off by the
// timeline's height), over the clip. Enter sets, Esc cancels, Set and
// Cancel do the same; the length goes out as the drag's does
// (durationChanged), so the SHOTS lock and the history treat it alike.
void AnimaticTimeline::openDurationField(int flatIndex)
{
    if (flatIndex < 0 || flatIndex >= m_blocks.size() || !m_canvas)
        return;
    if (m_tracks.shotsLocked) { // as the drag is refused (Part B)
        refuse(QStringLiteral("SHOTS track is locked"), m_canvas->mapToGlobal(clipRect(flatIndex).center()));
        return;
    }
    QWidget *host = window();
    if (!m_durField) {
        m_durField = new QFrame(host);
        m_durField->setObjectName(QStringLiteral("timelineDurationField"));
        m_durField->setAttribute(Qt::WA_StyledBackground, true);
        m_durField->setStyleSheet(SankoTheme::themed(
            "#timelineDurationField { background: #161616; border: 1px solid #2a2a2a; border-radius: 6px; }"
            "QLabel { color: #888888; font-size: 11px; background: transparent; border: none; }"
            "QLabel#durTitle { color: #dddddd; }"
            "QLineEdit { background: #0f0f0f; color: #ffffff; border: 1px solid #3a3a3a; border-radius: 4px;"
            "  padding: 2px 6px; font-size: 12px; }"
            "QLineEdit:focus { border-color: %ACCENT%; }"
            "QPushButton { background: #262626; color: #cccccc; border: none; border-radius: 4px;"
            "  padding: 3px 12px; font-size: 11px; }"
            "QPushButton:hover { color: #ffffff; }"
            "QPushButton#durSet { background: %ACCENT%; color: #ffffff; }"));
        auto *col = new QVBoxLayout(m_durField);
        col->setContentsMargins(12, 10, 12, 10);
        col->setSpacing(8);
        m_durTitle = new QLabel;
        m_durTitle->setObjectName(QStringLiteral("durTitle"));
        col->addWidget(m_durTitle);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        m_durEdit = new QLineEdit;
        m_durEdit->setObjectName(QStringLiteral("durFrames"));
        m_durEdit->setFixedWidth(64);
        m_durEdit->installEventFilter(this);
        row->addWidget(m_durEdit);
        row->addWidget(new QLabel(QStringLiteral("frames")));
        m_durEquals = new QLabel;
        m_durEquals->setObjectName(QStringLiteral("durEquals"));
        row->addWidget(m_durEquals);
        row->addStretch(1);
        col->addLayout(row);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        auto *cancel = new QPushButton(QStringLiteral("Cancel"));
        cancel->setObjectName(QStringLiteral("durCancel"));
        auto *set = new QPushButton(QStringLiteral("Set"));
        set->setObjectName(QStringLiteral("durSet"));
        for (QPushButton *b : {cancel, set}) {
            b->setCursor(Qt::PointingHandCursor);
            b->setFocusPolicy(Qt::NoFocus);
        }
        buttons->addWidget(cancel);
        buttons->addWidget(set);
        buttons->addStretch(1);
        col->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, this, [this] { closeDurationField(false); });
        connect(set, &QPushButton::clicked, this, [this] { closeDurationField(true); });
        connect(m_durEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            bool ok = false;
            const int f = text.trimmed().toInt(&ok);
            m_durEquals->setText(ok && f >= kMinFrames
                                     ? QStringLiteral("= %1 at %2 fps").arg(shotcam::lengthText(f, m_fps)).arg(m_fps)
                                     : QStringLiteral("at least 1 frame"));
        });
    }
    m_durFlat = flatIndex;
    const Block &b = m_blocks.at(flatIndex);
    m_durTitle->setText(QStringLiteral("Panel %1 \xC2\xB7 Duration").arg(b.panelIndex + 1));
    m_durEdit->setText(QString::number(b.frames));
    m_durEquals->setText(QStringLiteral("= %1 at %2 fps").arg(shotcam::lengthText(b.frames, m_fps)).arg(m_fps));
    m_durField->adjustSize();
    m_durField->setFixedWidth(qMax(m_durField->sizeHint().width(), 210));
    positionDurationField();
    m_durField->show();
    m_durField->raise();
    m_durEdit->setFocus(Qt::OtherFocusReason);
    m_durEdit->selectAll();
}

void AnimaticTimeline::positionDurationField()
{
    if (!m_durField || m_durFlat < 0 || !m_canvas)
        return;
    QWidget *host = m_durField->parentWidget();
    const QRect clip = clipRect(m_durFlat);
    // Above the clip's right end, inside the window.
    QPoint at = m_canvas->mapTo(host, QPoint(clip.right() - m_durField->width() + 8, clip.top() - m_durField->height() - 6));
    at.setX(qBound(4, at.x(), qMax(4, host->width() - m_durField->width() - 4)));
    at.setY(qBound(4, at.y(), qMax(4, host->height() - m_durField->height() - 4)));
    m_durField->move(at);
}

void AnimaticTimeline::closeDurationField(bool apply)
{
    if (!m_durField || m_durFlat < 0)
        return;
    const int flat = m_durFlat;
    m_durFlat = -1;
    m_durField->hide();
    if (m_canvas)
        m_canvas->setFocus(Qt::OtherFocusReason);
    if (!apply || flat >= m_blocks.size())
        return;
    bool ok = false;
    const int frames = m_durEdit->text().trimmed().toInt(&ok);
    if (!ok || frames < kMinFrames)
        return; // not a length: nothing changes (the field said so)
    const Block &b = m_blocks.at(flat);
    if (frames != b.frames)
        emit durationChanged(b.sceneIndex, b.panelIndex, frames);
}

int AnimaticTimeline::durationFieldPanelForTest() const
{
    return m_durField && m_durField->isVisible() ? m_durFlat : -1;
}

QWidget *AnimaticTimeline::durationFieldForTest() const
{
    return m_durField;
}

QString AnimaticTimeline::durationFieldEqualsForTest() const
{
    return m_durEquals ? m_durEquals->text() : QString();
}

void AnimaticTimeline::showDragChip(const QPoint &globalPos)
{
    followChip(globalPos, m_dragChip);
}

// A CHIP THAT FOLLOWS THE POINTER (the trial of 2026-11-09). QToolTip builds a NEW
// tooltip for every showText made while the last one is still appearing - until its
// roll or fade ends the tip "is not visible", so it cannot be reused - and a chip
// asked for on every pointer move was thrown away and begun again on every move:
// 36,318 tips for the playhead's timecode in one session, up to 190 a second, none of
// them seen while the pointer moved. So one tip is asked for, and while it appears the
// newest text and place wait; once it is up Qt reuses it for each move, and it follows.
void AnimaticTimeline::followChip(const QPoint &globalPos, const QString &text)
{
    if (!QToolTip::isVisible() && m_chipRoll.isValid() && m_chipRoll.elapsed() < kChipRollMs) {
        m_chipWaitingText = text;
        m_chipWaitingPos = globalPos;
        if (!m_chipFlush->isActive())
            m_chipFlush->start();
        return;
    }
    m_chipWaitingText.clear();
    m_chipFlush->stop();
    if (!QToolTip::isVisible())
        m_chipRoll.start();
    QToolTip::showText(globalPos, text, m_canvas);
}

// The drag ended: nothing is left waiting to be shown.
void AnimaticTimeline::hideChip()
{
    m_chipWaitingText.clear();
    m_chipFlush->stop();
    m_chipRoll.invalidate(); // the next drag's first chip is asked for at once
    QToolTip::hideText();
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

// --- The waveform (Phase 2D) ---------------------------------------------------

AudioPeaks::State AnimaticTimeline::audioWaveformState() const
{
    const AudioPeaks *peaks = (m_audioLoaded && m_host) ? m_host->audioPeaks() : nullptr;
    if (!peaks || m_audioMissing || m_audioUnplayable)
        return AudioPeaks::None;
    return peaks->state(m_host->audioPath());
}

QVector<AnimaticTimeline::WaveColumn> AnimaticTimeline::waveformColumns(const AudioBar &bar) const
{
    QVector<WaveColumn> out;
    if (bar.state != AudioBar::Present || !m_canvas || !m_host || !m_host->audioPeaks())
        return out;
    const audiopeaks::PeakData *data = m_host->audioPeaks()->peaks(m_host->audioPath());
    if (!data || data->count() == 0)
        return out;
    const double pxPerSec = pxPerFrame() * m_fps;
    if (pxPerSec <= 0.0)
        return out;
    // The bar's left is the track's start (contentXToScreen(0)), on or off
    // the screen; a column's time span is its distance from it, in the
    // ruler's px - the same mapping the clips and the CAMERA row use.
    const int x0 = bar.rect.left();
    const int left = qMax(bar.rect.left(), kLabelCol);
    const int right = qMin(bar.rect.right(), m_canvas->width() - 1); // the last pixel
    const double cy = bar.rect.top() + bar.rect.height() / 2.0;
    const double half = bar.rect.height() / 2.0 - 1.0;
    for (int x = left; x <= right; ++x) {
        const double t0 = (x - x0) / pxPerSec, t1 = (x + 1 - x0) / pxPerSec;
        int i0 = int(std::floor(t0 * audiopeaks::kPerSecond));
        int i1 = qMax(i0, int(std::ceil(t1 * audiopeaks::kPerSecond)) - 1);
        if (i0 >= data->count())
            break;
        i0 = qMax(0, i0);
        i1 = qMin(i1, data->count() - 1);
        qint16 mx = -32768, mn = 32767;
        for (int i = i0; i <= i1; ++i) {
            mx = qMax(mx, data->maxes.at(i));
            mn = qMin(mn, data->mins.at(i));
        }
        WaveColumn c;
        c.x = x;
        c.top = cy - mx / 32767.0 * half;
        c.bottom = cy - mn / 32767.0 * half;
        out.append(c);
    }
    return out;
}

QString AnimaticTimeline::audioWaveformStateForTest() const
{
    switch (audioWaveformState()) {
    case AudioPeaks::Reading: return QStringLiteral("reading");
    case AudioPeaks::Ready: return QStringLiteral("ready");
    case AudioPeaks::Plain: return QStringLiteral("plain");
    case AudioPeaks::None: break;
    }
    return QStringLiteral("none");
}

bool AnimaticTimeline::audioChipsHiddenForTest() const
{
    return m_audioHover && !waveformColumns(audioBar()).isEmpty();
}

QVariantList AnimaticTimeline::waveformColumnsForTest() const
{
    QVariantList out;
    for (const WaveColumn &c : waveformColumns(audioBar()))
        out << QVariantMap{{QStringLiteral("x"), c.x},
                           {QStringLiteral("top"), c.top},
                           {QStringLiteral("bottom"), c.bottom}};
    return out;
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

// --- THE SCENE LENGTH LOCK (batch F1, 2026-11-12; Figma F-1) -----------------------
bool AnimaticTimeline::sceneLocked(int sceneIndex) const
{
    return sceneIndex >= 0 && sceneIndex < m_scenes.size() && m_scenes.at(sceneIndex)
        && m_scenes.at(sceneIndex)->lengthLocked;
}

QRect AnimaticTimeline::sceneRectOnScreen(int sceneIndex) const
{
    const int s = sceneStartFrame(sceneIndex), e = sceneEndFrame(sceneIndex);
    if (s < 0 || e < 0)
        return QRect();
    const double ppf = pxPerFrame();
    const int x0 = contentXToScreen(s * ppf), x1 = contentXToScreen(e * ppf);
    return QRect(x0, kSceneY, x1 - x0, kSceneH);
}

int AnimaticTimeline::sceneAtScreenX(int x) const
{
    if (x < kLabelCol)
        return -1;
    const double ppf = pxPerFrame();
    for (const Block &b : m_blocks) { // one pass: the block under x is its scene's
        const int x0 = contentXToScreen(b.startFrame * ppf);
        const int x1 = contentXToScreen((b.startFrame + b.frames) * ppf);
        if (x >= x0 && x < x1)
            return b.sceneIndex;
    }
    return -1;
}

QFont AnimaticTimeline::sceneNameFont() const
{
    QFont f = font(); // as renderCanvas draws the names
    f.setPointSize(7);
    f.setBold(true);
    return f;
}

QString AnimaticTimeline::sceneNameOf(int sceneIndex) const
{
    for (const Block &b : m_blocks)
        if (b.sceneIndex == sceneIndex)
            return b.sceneName;
    return QString();
}

QRect AnimaticTimeline::sceneLockGlyphRect(int sceneIndex) const
{
    const QRect r = sceneRectOnScreen(sceneIndex);
    if (r.isEmpty())
        return QRect();
    constexpr int kLockW = 8, kLockH = 9, kLockGap = 4;
    const int nameW = QFontMetrics(sceneNameFont()).horizontalAdvance(sceneNameOf(sceneIndex));
    const int left = r.left() + (r.width() - (kLockW + kLockGap + nameW)) / 2;
    return QRect(qMax(r.left() + 3, left), kSceneY + (kSceneH - kLockH) / 2, kLockW, kLockH);
}

QString AnimaticTimeline::sceneEndTip(int sceneIndex) const
{
    const int s = sceneStartFrame(sceneIndex), e = sceneEndFrame(sceneIndex);
    if (s < 0 || e < 0 || sceneIndex >= m_scenes.size())
        return QString();
    const QChar dot(0x00B7);
    return QStringLiteral("Scene %1 ends %2 %3 %4 %3 %5 f%6")
        .arg(m_scenes.at(sceneIndex)->number)
        .arg(shotcam::timecodeText(e, m_fps))
        .arg(dot)
        .arg(shotcam::lengthText(e - s, m_fps))
        .arg(e - s)
        .arg(sceneLocked(sceneIndex) ? QStringLiteral(" %1 locked").arg(dot) : QString());
}

QString AnimaticTimeline::sceneLockTip(int sceneIndex) const
{
    const int s = sceneStartFrame(sceneIndex), e = sceneEndFrame(sceneIndex);
    if (s < 0 || e < 0 || sceneIndex >= m_scenes.size())
        return QString();
    const int number = m_scenes.at(sceneIndex)->number;
    if (sceneLocked(sceneIndex))
        return QStringLiteral("Unlock scene %1's length").arg(number);
    return QStringLiteral("Lock scene %1's length (%2 %3 %4 f)")
        .arg(number).arg(shotcam::lengthText(e - s, m_fps)).arg(QChar(0x00B7)).arg(e - s);
}

int AnimaticTimeline::sceneEndLineXForTest(int sceneIndex) const
{
    if (!sceneLocked(sceneIndex) || !m_tracks.scenesVisible)
        return -1;
    const int e = sceneEndFrame(sceneIndex);
    return e < 0 ? -1 : contentXToScreen(e * pxPerFrame());
}

// The lock: its shackle, 5 x 6 px with round ends, behind an 8 x 5.5 px body.
void AnimaticTimeline::paintSceneLock(QPainter &p, const QRect &glyph, const QColor &colour)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(colour, 1.2));
    p.drawRoundedRect(QRectF(glyph.left() + 1.5, glyph.top() + 0.6, 5.0, 6.0), 2.5, 2.5);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawRoundedRect(QRectF(glyph.left(), glyph.top() + 3.5, 8.0, 5.5), 1.0, 1.0);
    p.restore();
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
            double f = shotcam::keyFrame(*scene, key, m_fps, start);
            if (f < 0.0)
                continue; // inert: its panel is gone (its delete still undoable);
                          // or stranded (Phase 2E): strandedMarks draws it
            if (m_drag == Drag::CameraKey && m_camDragActive && si == m_camDragScene
                && k == m_camDragKey)
                f = m_camDragFrame - start; // where the drag has it
            CameraMark m;
            m.sceneIndex = si;
            m.keyIndex = k;
            m.frame = start + f;
            m.x = contentXToScreen(m.frame * ppf);
            m.ease = int(key.ease);
            m.pinned = key.pinned;
            sceneMarks.append(m);
        }
        std::stable_sort(sceneMarks.begin(), sceneMarks.end(),
                         [](const CameraMark &a, const CameraMark &b) { return a.frame < b.frame; });
        marks += sceneMarks;
    }
    return marks;
}

QVector<AnimaticTimeline::CameraMark> AnimaticTimeline::strandedMarks() const
{
    QVector<CameraMark> marks;
    const double ppf = pxPerFrame();
    for (int si = 0; si < m_scenes.size(); ++si) {
        const Scene *scene = m_scenes.at(si);
        const int start = sceneStartFrame(si);
        if (scene->camera.isEmpty() || start < 0)
            continue;
        for (int k = 0; k < scene->camera.keys.size(); ++k) {
            const shotcam::CameraKey &key = scene->camera.keys.at(k);
            if (!shotcam::isStranded(*scene, key, m_fps, start))
                continue;
            CameraMark m;
            m.sceneIndex = si;
            m.keyIndex = k;
            m.frame = shotcam::keyFilmFrame(key, m_fps); // its time, outside its scene
            m.x = contentXToScreen(m.frame * ppf);
            m.ease = int(key.ease);
            m.pinned = true;
            m.stranded = true;
            marks.append(m);
        }
    }
    return marks;
}

QVector<AnimaticTimeline::CameraSegment>
AnimaticTimeline::cameraSegments(const QVector<CameraMark> &marks) const
{
    QVector<CameraSegment> segments;
    int selScene = -1, selKey = -1;
    const bool hasSel = selectedCameraKey(&selScene, &selKey);
    const QVector<CameraMark> stranded = marks.size() > 1 ? strandedMarks() : QVector<CameraMark>();
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
        // The mark clear of every key on the row (placeEaseMark, 2026-11-26): between two live
        // keys only a stranded key (Phase 2E) can be in its way.
        QVector<QPointF> others;
        for (const CameraMark &m : stranded)
            if (m.x > s.x0 - kGlyphW && m.x < s.x1 + kGlyphW)
                others.append(QPointF(m.x + 0.5, kKeyHalf));
        const EaseMarkPlace place = placeEaseMark(s.x0, s.x1, others);
        s.glyph = place.glyph;
        s.smallMark = place.smallMark;
        s.markX = place.x;
        s.lineOnly = !s.glyph && !s.smallMark;
        s.accent = hasSel && a.sceneIndex == selScene && a.keyIndex == selKey;
        if (s.x1 > s.x0)
            segments.append(s);
    }
    return segments;
}

// The ease mark's place (batch E, Figma E-1): the pill centred on the move, the small mark
// on a move of 16 to 40 px, and under 16 px the line itself, a little taller than drawn.
QRectF AnimaticTimeline::easeMarkRect(const CameraSegment &s) const
{
    const qreal cx = s.markX, cy = cameraTrackY() + kCameraH / 2.0;
    if (s.glyph)
        return QRectF(cx - kGlyphW / 2.0, cy - kGlyphH / 2.0, kGlyphW, kGlyphH);
    if (s.smallMark)
        return QRectF(cx - kSmallMarkW / 2.0, cy - kSmallMarkH / 2.0, kSmallMarkW, kSmallMarkH);
    return QRectF(s.x0, cy - 5.0, s.x1 - s.x0, 10.0);
}

bool AnimaticTimeline::easeMarkAt(const QPoint &pos, CameraSegment *segment, QRectF *markRect) const
{
    if (!inCameraRow(pos))
        return false;
    for (const CameraSegment &s : cameraSegments(cameraMarks())) {
        const QRectF r = easeMarkRect(s);
        if (r.adjusted(-1, -2, 1, 2).contains(QPointF(pos))) {
            *segment = s;
            *markRect = r;
            return true;
        }
    }
    return false;
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
    // The live keys first, so one wins a tie with a stranded key (Phase 2E)
    // drawn at the same x.
    for (const CameraMark &m : cameraMarks() + strandedMarks()) {
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
    // Phase 2E (Figma 2E-1): a stranded key says where it belongs and what
    // brings it back; a pinned key names its time, not a clip.
    if (mark.stranded && mark.sceneIndex < m_scenes.size())
        return QString::fromUtf8("Pinned key of scene %1 \xC2\xB7 outside the scene \xC2\xB7 "
                                 "lengthen the scene or move the key")
            .arg(m_scenes.at(mark.sceneIndex)->number);
    if (mark.pinned)
        return QString::fromUtf8("Key %1 \xC2\xB7 pinned to %2 \xC2\xB7 %3")
            .arg(number)
            .arg(timecode(frame))
            .arg(number == inScene ? QStringLiteral("last key")
                                   : shotcam::easeLabel(shotcam::Ease(mark.ease)));
    const int clip = blockAtFrame(frame);
    return QString::fromUtf8("Key %1 \xC2\xB7 %2 \xC2\xB7 panel %3 \xC2\xB7 %4")
        .arg(number)
        .arg(timecode(frame))
        .arg(clip + 1)
        .arg(number == inScene ? QStringLiteral("last key")
                               : shotcam::easeLabel(shotcam::Ease(mark.ease)));
}

// --- The part of a move that leaves a clip's art (batch 6d, Q9, Figma MARGIN-2) ---
// Never clamped: the camera plays as keyed, and the CAMERA row says where its shot
// goes past a panel's stage, in amber, with the clip outlined and a chip naming it.

const AnimaticTimeline::ArtExits *AnimaticTimeline::artExitsOf(int sceneIndex) const
{
    const Scene *scene = m_scenes.value(sceneIndex);
    if (!scene || scene->camera.isEmpty())
        return nullptr;
    if (m_artExitsTrusted) { // inside a paint, already checked at its start
        const auto it = m_artExits.constFind(scene);
        if (it != m_artExits.constEnd() && it->frames.size() == scene->panels.size())
            return &it.value();
    }
    const int filmStart = qMax(0, sceneStartFrame(sceneIndex));
    // What shapes them: the camera (its file form), the rate, where the scene
    // starts, and each panel's length, stage and art.
    QByteArray shape;
    {
        QDataStream s(&shape, QIODevice::WriteOnly);
        s << m_fps << filmStart << m_stageMargin
          << QJsonDocument(shotcam::toJson(*scene)).toJson(QJsonDocument::Compact);
        for (const Panel *panel : scene->panels) {
            s << quint64(quintptr(panel)) << shotcam::panelFrames(*panel, m_fps) << panel->margin
              << panel->ownMargin << panel->flattenFingerprint();
            for (const Layer &layer : panel->layers)
                s << panel->effectiveParallax(layer);
        }
    }
    ArtExits &cached = m_artExits[scene];
    if (cached.shape == shape && cached.frames.size() == scene->panels.size())
        return &cached;
    cached = ArtExits();
    cached.shape = shape;
    for (int p = 0; p < scene->panels.size(); ++p) {
        const Panel *panel = scene->panels.at(p);
        const shotcam::ArtExit exit = panel
            ? shotcam::artExit(*scene, p, m_fps, filmStart, shotcam::panelStage(*panel, m_stageMargin))
            : shotcam::ArtExit();
        cached.frames.append(exit.frames);
        cached.first.append(exit.first);
        cached.last.append(exit.last);
        cached.grow.append(exit.grow);
    }
    return &cached;
}

bool AnimaticTimeline::artExitOf(int flatIndex, int *frames, int *first, int *last, QMargins *grow) const
{
    if (flatIndex < 0 || flatIndex >= m_blocks.size() || !m_tracks.cameraVisible)
        return false;
    const Block &b = m_blocks.at(flatIndex);
    const ArtExits *exits = artExitsOf(b.sceneIndex);
    if (!exits || b.panelIndex >= exits->frames.size() || exits->frames.at(b.panelIndex) <= 0)
        return false;
    if (frames)
        *frames = exits->frames.at(b.panelIndex);
    if (first)
        *first = exits->first.at(b.panelIndex);
    if (last)
        *last = exits->last.at(b.panelIndex);
    if (grow)
        *grow = exits->grow.at(b.panelIndex);
    return true;
}

QString AnimaticTimeline::artExitChipText(int flatIndex) const
{
    int frames = 0;
    if (!artExitOf(flatIndex, &frames, nullptr, nullptr, nullptr))
        return QString();
    return QStringLiteral("leaves Panel %1's art for %2 f").arg(m_blocks.at(flatIndex).panelIndex + 1).arg(frames);
}

// THE CHIP STEPS DOWN, IT NEVER GOES (GROW-1, the user's approval of Figma 552:696): at
// the clip's bottom right, the sentence while it fits, then the count ("72 f"), then a
// 14 px "!" marker; on a clip too narrow even for that, the marker centred over the clip
// and over its edges. So a leaving clip has its button at any zoom.
QRect AnimaticTimeline::artExitChipRect(int flatIndex, QString *label, bool *over) const
{
    const QString text = artExitChipText(flatIndex);
    if (over)
        *over = false;
    if (text.isEmpty() || !m_tracks.shotsVisible)
        return QRect();
    const Block &b = m_blocks.at(flatIndex);
    const double ppf = pxPerFrame();
    const int x0 = contentXToScreen(b.startFrame * ppf);
    const int x1 = qMax(x0 + kMinClipPx, contentXToScreen((b.startFrame + b.frames) * ppf));
    const QRect body = clipLayout(flatIndex, QRect(x0, kPanelY + 2, x1 - x0, panelTrackH() - 4)).body;
    QFont f = font();
    f.setPixelSize(10);
    const QFontMetrics fm(f);
    int frames = 0;
    artExitOf(flatIndex, &frames, nullptr, nullptr, nullptr);
    for (const QString &t : {text, QStringLiteral("%1 f").arg(frames)}) {
        const int w = fm.horizontalAdvance(t) + 12;
        if (w + 8 <= body.width()) {
            if (label)
                *label = t;
            return QRect(body.right() - 4 - w, body.bottom() - 4 - 16, w, 16);
        }
    }
    if (label)
        *label = QStringLiteral("!");
    const int top = body.bottom() - 4 - kArtExitMarker;
    if (kArtExitMarker + 8 <= body.width())
        return QRect(body.right() - 4 - kArtExitMarker, top, kArtExitMarker, kArtExitMarker);
    if (over)
        *over = true;
    return QRect(body.left() + (body.width() - kArtExitMarker) / 2, top, kArtExitMarker, kArtExitMarker);
}

int AnimaticTimeline::artExitChipAt(const QPoint &pos) const
{
    const bool wasTrusted = m_artExitsTrusted;
    if (!wasTrusted) { // checked once, scene by scene, not once a clip
        for (int s = 0; s < m_scenes.size(); ++s)
            artExitsOf(s);
        m_artExitsTrusted = true;
    }
    // From the last clip back: the chips are painted in clip order, so where two
    // markers over narrow clips overlap, the one on top is hit (GROW-1).
    int hit = -1;
    for (int i = m_blocks.size() - 1; i >= 0 && hit < 0; --i)
        if (artExitChipRect(i).contains(pos))
            hit = i;
    m_artExitsTrusted = wasTrusted;
    return hit;
}

QString AnimaticTimeline::artExitChipLabelForTest(int flatIndex) const
{
    QString label;
    bool over = false;
    if (artExitChipRect(flatIndex, &label, &over).isEmpty())
        return QString();
    return over ? QStringLiteral("over") : label;
}

int AnimaticTimeline::artExitFramesForTest(int flatIndex) const
{
    int frames = 0;
    return artExitOf(flatIndex, &frames, nullptr, nullptr, nullptr) ? frames : 0;
}

QMargins AnimaticTimeline::artExitGrowForTest(int flatIndex) const
{
    QMargins grow;
    return artExitOf(flatIndex, nullptr, nullptr, nullptr, &grow) ? grow : QMargins();
}

QRect AnimaticTimeline::artExitChipRectForTest(int flatIndex) const
{
    return artExitChipRect(flatIndex);
}

QString AnimaticTimeline::artExitChipTextForTest(int flatIndex) const
{
    return artExitChipText(flatIndex);
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

    // THE PART OFF A PANEL'S ART (batch 6d, Figma MARGIN-2): the frames of each clip
    // whose shot leaves its stage, amber under the move, the move dashed amber there.
    // The band first, under the move; the dashes after it, in its place (below).
    const QColor amber(0xf5, 0xa6, 0x23);
    QColor amberBand = amber;
    amberBand.setAlphaF(0.28);
    QVector<QPair<qreal, qreal>> offArt; // screen x from, to
    {
        const double ppf = pxPerFrame();
        for (int i = 0; i < m_blocks.size(); ++i) {
            int frames = 0, first = -1, last = -1;
            if (!artExitOf(i, &frames, &first, &last, nullptr))
                continue;
            const Block &b = m_blocks.at(i);
            const qreal xa = contentXToScreen((b.startFrame + first) * ppf);
            const qreal xb = qMax(xa + 1.0, qreal(contentXToScreen((b.startFrame + last + 1) * ppf)));
            offArt.append({xa, xb});
            p.fillRect(QRectF(xa, y0 + 3, xb - xa, kCameraH - 6), amberBand);
        }
    }

    // The camera keys' colour in full (Preferences > Keyframes; SankoKeyColors): the line and a
    // resting key's fill the colour, its outline the colour toward white; the selected key's move
    // and the selected key itself the lighter shade, the selected key ringed white; a hover rings it
    // white. The key at the playhead takes the playhead's colours; pinned keys keep the AUDIO row's
    // blue.
    using SankoKeyColors::Row;
    const QColor keyAccent = SankoKeyColors::key(Row::Camera);
    const QColor keySegment = SankoKeyColors::key(Row::Camera);
    const QColor keyFill = SankoKeyColors::key(Row::Camera);
    const QColor keyEdge = SankoKeyColors::keyEdge(Row::Camera);
    for (const CameraSegment &s : cameraSegments(marks)) {
        const QColor c = s.accent ? keyEdge : keySegment;
        if (s.dashed) {
            QPen pen(c, 2.0, Qt::CustomDashLine, Qt::FlatCap);
            pen.setDashPattern({2.5, 2.0}); // 5 px on, 4 off (units of the width)
            p.setPen(pen);
            p.drawLine(QPointF(s.x0, cy), QPointF(s.x1, cy));
        } else {
            p.fillRect(QRectF(s.x0, cy - 1.0, s.x1 - s.x0, 2.0), c);
        }
        // The mark: the pill, or on a move of 16 to 40 px the small one (batch E); the one
        // under the pointer with the clips' hover edge - it is a button.
        const bool markHover = s.sceneIndex == m_easeHoverScene && s.fromKey == m_easeHoverKey;
        if (s.glyph || s.smallMark)
            paintEaseGlyph(p, easeMarkRect(s), shotcam::Ease(s.ease), markHover, s.smallMark);
    }
    // Where the shot is off the art, the move dashed amber in place of its line;
    // the keys are painted after, over it.
    for (const QPair<qreal, qreal> &span : offArt) {
        const QRectF under(span.first, cy - 1.5, span.second - span.first, 3.0);
        p.fillRect(under, kCameraRowBg);
        p.fillRect(under, amberBand);
        QPen dash(amber, 2.0, Qt::CustomDashLine, Qt::FlatCap);
        dash.setDashPattern({2.5, 2.0});
        p.setPen(dash);
        p.drawLine(QPointF(span.first, cy), QPointF(span.second, cy));
    }

    int selScene = -1, selKey = -1;
    const bool hasSel = selectedCameraKey(&selScene, &selKey);
    const int phFrame = playheadFrame();
    // PINNED (Phase 2E, D16; Figma 2E-1): the diamond in the AUDIO row's
    // blue, a 3 px stem below it (the dotted line through the AUDIO row is
    // paintPinLines'); selected and at the playhead as any key, the stem
    // kept. STRANDED: hollow grey, with its stem, at its time.
    const QColor pinBlue(0x4d, 0x9f, 0xff);
    const QColor strandGrey(0x6f, 0x6f, 0x80);
    // The stem: 1 px wide, 3 px long, crisp in the key's own column - the
    // column the dotted line (paintPinLines) carries on down from it.
    auto stem = [&](int x, const QColor &ink) {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(ink, 1.0));
        p.drawLine(x, y0 + kCameraH / 2 + int(kKeyHalf) + 1, x, y0 + kCameraH / 2 + int(kKeyHalf) + 3);
        p.setRenderHint(QPainter::Antialiasing, true);
    };
    for (const CameraMark &m : marks) {
        const bool selected = hasSel && m.sceneIndex == selScene && m.keyIndex == selKey;
        const bool hover = m.sceneIndex == m_camHoverScene && m.keyIndex == m_camHoverKey;
        const bool atPlayhead = qAbs(m.frame - phFrame) < 0.5;
        const QPointF c(m.x + 0.5, cy);
        if (hover) { // the accent at 14 %, the flat controls' hover
            QColor halo = keyAccent;
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
        QColor fill = m.pinned ? pinBlue : keyFill;
        QColor edge = m.pinned ? pinBlue : keyEdge;
        if (selected) {
            fill = keyEdge;
            edge = Qt::white;
        } else if (atPlayhead) {
            fill = kPlayhead;
            edge = Qt::white;
        } else if (hover && !m.pinned) {
            edge = Qt::white;
        }
        QPolygonF diamond;
        diamond << QPointF(c.x(), c.y() - kKeyHalf) << QPointF(c.x() + kKeyHalf, c.y())
                << QPointF(c.x(), c.y() + kKeyHalf) << QPointF(c.x() - kKeyHalf, c.y());
        p.setPen(QPen(edge, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(fill);
        p.drawPolygon(diamond);
        if (m.pinned)
            stem(m.x, pinBlue);
    }
    for (const CameraMark &m : strandedMarks()) {
        const bool hover = m.sceneIndex == m_camHoverScene && m.keyIndex == m_camHoverKey;
        const QPointF c(m.x + 0.5, cy);
        if (hover) {
            QColor halo = keyAccent;
            halo.setAlphaF(0.14);
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(c, 8.0, 8.0);
        }
        QPolygonF diamond;
        diamond << QPointF(c.x(), c.y() - kKeyHalf) << QPointF(c.x() + kKeyHalf, c.y())
                << QPointF(c.x(), c.y() + kKeyHalf) << QPointF(c.x() - kKeyHalf, c.y());
        p.setPen(QPen(strandGrey, 1.5, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(diamond);
        stem(m.x, strandGrey);
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
        QPen dash(keyAccent, 1.5, Qt::CustomDashLine, Qt::FlatCap, Qt::RoundJoin);
        dash.setDashPattern({1.7, 1.2});
        if (xb - xa > 2 * kKeyHalf + 2) {
            QPen bar(keyAccent, 2.0, Qt::CustomDashLine, Qt::FlatCap);
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

// The 1 px dotted blue line from each pinned key down through the AUDIO row
// (Phase 2E, D16): it ties the key to the sound. Drawn from the audio
// paint, after the waveform and before the name and length chips (the
// user's Q7: under the chips), at the CAMERA row's own dimming.
void AnimaticTimeline::paintPinLines(QPainter &p)
{
    if (!m_canvas)
        return;
    const QVector<CameraMark> marks = cameraMarks();
    bool any = false;
    for (const CameraMark &m : marks)
        any = any || m.pinned;
    if (!any)
        return;
    p.save();
    p.setClipRect(QRect(kLabelCol, cameraTrackY(), m_canvas->width() - kLabelCol,
                        kCameraH + kAudioH),
                  Qt::IntersectClip);
    p.setOpacity(cameraRowOpacityForTest());
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(QColor(0x4d, 0x9f, 0xff), 1.0, Qt::DotLine));
    const int top = cameraTrackY() + kCameraH / 2 + int(kKeyHalf) + 4; // under the stem
    const int bottom = audioTrackY() + kAudioH - 1;
    for (const CameraMark &m : marks)
        if (m.pinned)
            p.drawLine(m.x, top, m.x, bottom);
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
    for (const CameraMark &m : cameraMarks() + strandedMarks())
        list.append(QVariantMap{
            {QStringLiteral("scene"), m.sceneIndex},
            {QStringLiteral("key"), m.keyIndex},
            {QStringLiteral("x"), m.x},
            {QStringLiteral("frame"), m.frame},
            {QStringLiteral("selected"), hasSel && m.sceneIndex == selScene && m.keyIndex == selKey},
            {QStringLiteral("atPlayhead"), qAbs(m.frame - phFrame) < 0.5},
            {QStringLiteral("hover"), m.sceneIndex == m_camHoverScene && m.keyIndex == m_camHoverKey},
            {QStringLiteral("pinned"), m.pinned},     // Phase 2E
            {QStringLiteral("stranded"), m.stranded}, // Phase 2E
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
            {QStringLiteral("small"), s.smallMark},
            {QStringLiteral("lineOnly"), s.lineOnly},
            {QStringLiteral("hover"), s.sceneIndex == m_easeHoverScene && s.fromKey == m_easeHoverKey},
            {QStringLiteral("markRect"), easeMarkRect(s)},
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
        z = qBound(double(kZoomMin), z, double(kZoomMax));
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
    if (m_durFlat >= 0)
        closeDurationField(false); // ...and outside the typing field: it cancels
    // PANNING (batch I-1b, note 8): before anything a press could start - a pan moves no
    // playhead, panel, key or trim. Ctrl was read nowhere on a press before it (no
    // multi-select on the timeline), and the middle button was not used at all.
    if (panPress(e) && m_drag == Drag::None) {
        m_drag = Drag::Pan;
        m_panStartGlobalX = e->globalPosition().x();
        m_panStartGlobalY = e->globalPosition().y();
        m_panStartScroll = m_scrollX;
        // CTRL+CLICK ON A FRAME (round FC-a, the user's answer 2: one frame more or one less): a
        // Ctrl press on a block of the FRAMES lane is a pan only once it moves; let go where it
        // was pressed, it is a Ctrl+click on that frame (canvasMouseRelease).
        m_panLaneBlock = m_panLaneLayer = -1;
        const QPoint at = e->position().toPoint();
        if (e->button() == Qt::LeftButton && inDrawingsLane(at)) {
            int layerIndex = -1;
            if (runLayer(&layerIndex)) {
                m_panLaneBlock = drawingBlockAt(at);
                m_panLaneLayer = layerIndex;
            }
        }
        m_canvas->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint pos = e->position().toPoint();
    if (pos.x() < kLabelCol)
        return;

    // A clip's amber chip (batch 6d): its Grow to fit, offered.
    if (const int chipClip = artExitChipAt(pos); chipClip >= 0) {
        emit artExitChipClicked(chipClip, e->globalPosition().toPoint());
        e->accept();
        return;
    }

    // THE CAMERA ROW (Phase 1C), BEFORE the playhead: a key often sits under
    // the playhead (a click on a key puts it there), and the key must win.
    // A press on a key selects it and asks for the playhead to go to it;
    // unlocked, it may become a drag. A press on the row off any key clears
    // the selection - unless it is on the playhead's line, which it grabs.
    // A hidden row takes no press of its own.
    if (inCameraRow(pos) && m_tracks.cameraVisible) {
        CameraMark mark;
        // (A drawn ease mark is asked first - onCameraEaseMark, 2026-11-26: a press on it never
        // takes a key whose 16 px reach it is in.)
        if (!onCameraEaseMark(pos) && cameraKeyAt(pos, &mark)) {
            if (mark.stranded) // Phase 2E: no selection, no drag; its menu is the right button's
                return;
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
        // A MOVE'S EASE MARK (batch E, Figma E-1): a tap asks for the five eases of that
        // move; under 16 px, where no mark is drawn, the line itself does. A key wins over
        // the line (above); a drawn mark wins over a key (the user's note 6, 2026-11-26).
        CameraSegment segment;
        QRectF markRect;
        if (easeMarkAt(pos, &segment, &markRect)) {
            const QRect onScreen(m_canvas->mapToGlobal(markRect.topLeft().toPoint()),
                                 markRect.size().toSize());
            emit cameraEaseMarkClicked(segment.sceneIndex, segment.fromKey, onScreen);
            e->accept();
            return;
        }
        if (qAbs(pos.x() - contentXToScreen(playheadContentX())) > kGrab) {
            m_camSelScene = m_camSelKey = -1;
            m_canvas->update();
            return;
        }
    }
    // THE LAYER ROW (batch I-1): a key - the selected layer's, or another's dim mark - takes
    // the playhead (and that panel and layer); it may become a drag in its panel. An ease
    // mark asks for the five eases. Before the playhead, as the CAMERA row's keys are.
    if (inLayerRow(pos)) {
        // THE ROW TAKES THE EDIT KEYS (batch I-1b round 2, note 5): a key pressed here is the
        // one Ctrl+C / X and Delete act on; a press on the empty track chooses the row with no
        // key, for Ctrl+V. One selection at a time: a camera key's ends here.
        if (m_camSelKey >= 0) {
            m_camSelScene = m_camSelKey = -1;
            m_canvas->update();
        }
        // THE DRAWINGS LANE (batch I-2): the pointer's lane decides what a press takes. Here a
        // cut between two blocks may become a drag (the press alone is a tap on the drawing
        // that starts there); else the block pressed takes the playhead to its first frame and
        // is the selected thing on the row. The row is the last thing pressed either way.
        if (inDrawingsLane(pos)) {
            int layerIndex = -1;
            if (runLayer(&layerIndex)) {
                // (Round FC-a: the press says itself what it chose - a block with Shift or Ctrl
                // keeps the selection it adds to - so the row's "no key" goes only with a block's
                // press or, -1, the lane off a block. A cut's press waits for its release.)
                const int cut = drawingCutAt(pos);
                if (cut >= 1) {
                    m_drag = Drag::DrawingCut;
                    m_cutDragDrawing = cut;
                    m_cutDragLayer = layerIndex;
                    m_cutDragFrom = 0;
                    for (const DrawingBlock &block : drawingBlocks())
                        if (block.index == cut)
                            m_cutDragFrom = block.start;
                    m_cutDragFrame = m_cutDragFrom;
                    m_cutDragAlone = e->modifiers().testFlag(Qt::AltModifier);
                    m_cutDragMods = e->modifiers();
                    m_cutDragActive = false;
                    m_layerPress = pos;
                    e->accept();
                    return;
                }
                emit drawingBlockPressed(layerIndex, drawingBlockAt(pos), e->modifiers());
            }
            e->accept();
            return;
        }
        LayerMark mark;
        // THE EASE MARK BEFORE THE KEY (the user's note 6 after hand test I2-1, 2026-11-26: a
        // press on a mark that sat on another layer's key took that key - and with it the
        // layer - four times before the eases opened). A press on a drawn mark always opens
        // the eases and never selects or moves a key; the mark is drawn clear of every key
        // (placeEaseMark), so each key keeps its own place to be pressed.
        const bool onKey = !onLayerEaseMark(pos) && layerKeyAt(pos, &mark);
        emit layerRowPressed(onKey ? mark.flat : -1, onKey ? mark.layer : -1, onKey ? mark.key : -1);
        if (onKey) {
            emit layerKeyActivated(mark.flat, mark.layer, mark.key);
            m_drag = Drag::LayerKey;
            m_layerDragFlat = mark.flat;
            m_layerDragLayer = mark.layer;
            m_layerDragKey = mark.key;
            m_layerDragFromFrame = int(std::lround(mark.frame));
            m_layerDragFrame = m_layerDragFromFrame;
            m_layerDragActive = false;
            m_layerPress = pos;
            e->accept();
            return;
        }
        LayerSegment segment;
        QRectF markRect;
        if (layerEaseMarkAt(pos, &segment, &markRect)) {
            const QRect onScreen(m_canvas->mapToGlobal(markRect.topLeft().toPoint()), markRect.size().toSize());
            emit layerEaseMarkClicked(segment.flat, segment.layer, segment.fromKey, onScreen);
            e->accept();
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

    // THE SCENE LENGTH LOCK (batch F1, Q-F3): a press on a scene's lock - drawn on a
    // locked scene, faint on hover on another - locks or unlocks its length. Before the
    // playhead, as a key is: the lock may sit under its line.
    if (m_tracks.scenesVisible && pos.y() >= kSceneY && pos.y() < kSceneY + kSceneH) {
        const int si = sceneAtScreenX(pos.x());
        if (si >= 0 && sceneLockGlyphRect(si).adjusted(-3, -3, 3, 3).contains(pos)) {
            emit sceneLengthLockToggleRequested(si);
            e->accept();
            return;
        }
    }

    // Playhead: triangle in the ruler, or near the line anywhere - except
    // on the SELECTED CLIP'S EDGES on the SHOTS track (2026-11-06, batch
    // 5): selecting a clip puts the playhead on its start, its left edge,
    // and the edge is what a press there is for (the left edge was a stub
    // before, so nothing used to compete for it).
    const int phx = contentXToScreen(playheadContentX());
    bool onSelectedEdge = false;
    if (m_tracks.shotsVisible && m_selected >= 0 && m_selected < m_blocks.size()
        && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const Block &sb = m_blocks.at(m_selected);
        const int ex0 = contentXToScreen(sb.startFrame * pxPerFrame());
        const int ex1 = contentXToScreen((sb.startFrame + sb.frames) * pxPerFrame());
        onSelectedEdge = qAbs(pos.x() - ex0) <= kHandleW || qAbs(pos.x() - ex1) <= kHandleW;
    }
    if (pos.y() < kRulerY + kRulerH || (!onSelectedEdge && qAbs(pos.x() - phx) <= kGrab)) {
        m_drag = Drag::Playhead;
        emit playheadDragStarted(); // batch I-1b: the Move tool's box waits for its end
        m_dragPlayheadFrame = frameAtScreenX(pos.x());
        followChip(e->globalPosition().toPoint(), timecode(m_dragPlayheadFrame));
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
    // A clip drawn wider than it is (a 1-frame clip at a low zoom) under the
    // pointer is selected, never the selected clip's edge beside it.
    const int underPress = (pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH())
        ? blockAtScreenX(pos.x()) : -1;
    const bool tinyOther = underPress >= 0 && underPress != m_selected
        && m_blocks.at(underPress).frames * pxPerFrame() < kMinClipPx;
    if (!tinyOther && m_selected >= 0 && m_selected < m_blocks.size()
        && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const Block &b = m_blocks.at(m_selected);
        const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
        const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
        const bool onHandle = qAbs(pos.x() - x1) <= kHandleW || qAbs(pos.x() - x0) <= kHandleW;
        // LENGTHS BY THE FRAME (2026-11-06, batch 5; Figma DUR-1). The
        // right edge trims the end, the left edge the start; both ripple
        // (the clip keeps its start and later clips move up, so the left
        // edge's drag shows its result: the clip shorter, the rest moved).
        // With Alt, the edge is the CUT between this clip and its
        // neighbour on that side, and the drag ROLLS it: both change,
        // nothing after them moves. No neighbour: an ordinary trim.
        const bool alt = e->modifiers().testFlag(Qt::AltModifier);
        const bool right = qAbs(pos.x() - x1) <= kHandleW;
        // THE SCENE LENGTH LOCK (batch F1, Figma F-1): inside a locked scene an edge rolls
        // its cut with the neighbour on that side, Alt or not; the scene's own ends hold;
        // and a roll across the cut to a locked scene is refused.
        const int first = right ? m_selected : m_selected - 1; // the roll's left clip
        const bool hasNext = first >= 0 && first + 1 < m_blocks.size();
        const bool acrossScenes = hasNext && m_blocks.at(first).sceneIndex != m_blocks.at(first + 1).sceneIndex;
        const bool lockedHere = sceneLocked(b.sceneIndex);
        QString lockRefusal;
        if (lockedHere && (!hasNext || acrossScenes) && !(alt && acrossScenes)) {
            lockRefusal = QStringLiteral("Scene %1's length is locked: its ends stay where they are.")
                              .arg(b.sceneNumber);
        } else if (alt && acrossScenes) {
            const Block &l = m_blocks.at(first), &n = m_blocks.at(first + 1);
            if (sceneLocked(l.sceneIndex) || sceneLocked(n.sceneIndex))
                lockRefusal = QStringLiteral("Scene %1's length is locked: the cut between scenes %2 and %3 stays.")
                                  .arg(sceneLocked(l.sceneIndex) ? l.sceneNumber : n.sceneNumber)
                                  .arg(l.sceneNumber).arg(n.sceneNumber);
        }
        if (onHandle && m_tracks.shotsLocked) {
            refuse(QStringLiteral("SHOTS track is locked"), e->globalPosition().toPoint());
        } else if (onHandle && !lockRefusal.isEmpty()) {
            // The press ends here: on a scene's very end the frame under it is the next
            // scene's, and selecting that clip would change what is on the canvas.
            refuse(lockRefusal, e->globalPosition().toPoint());
            e->accept();
            return;
        } else if (onHandle) {
            const bool roll = (alt || lockedHere) && hasNext;
            m_lockedRoll = lockedHere && roll;
            m_dragFramesFor.clear();
            m_dragPressX = screenXToContent(pos.x());
            m_dragChip.clear();
            if (roll) {
                m_drag = Drag::Roll;
                m_resizeIndex = first;
                m_rollNext = first + 1;
                m_dragFromFrames = m_blocks.at(first).frames;
                m_dragFromFramesNext = m_blocks.at(first + 1).frames;
                m_dragOriginFrame = m_blocks.at(first + 1).startFrame;
            } else {
                m_drag = right ? Drag::ResizeRight : Drag::ResizeLeft;
                m_resizeIndex = m_selected;
                m_rollNext = -1;
                m_dragFromFrames = b.frames;
                m_dragOriginFrame = right ? b.startFrame + b.frames : b.startFrame;
            }
            m_canvas->setCursor(Qt::SizeHorCursor);
            m_canvas->update();
            return;
        }
    }

    // Otherwise: a press on a clip SELECTS that panel (the workspace does
    // the selecting and pushes the highlight back through setSelectedPanel),
    // and may turn into a reorder drag once it has moved far enough - not
    // on a locked SHOTS track, where the press selects and nothing more.
    if (pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int idx = blockAtScreenX(pos.x()); // a 1-frame clip as drawn (2026-11-06)
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
            if (!onCameraEaseMark(pos) && cameraKeyAt(pos, &mark)) { // (a mark takes no key)
                if (!mark.stranded) { // a stranded key (Phase 2E) is not selected
                    m_camSelScene = mark.sceneIndex;
                    m_camSelKey = mark.keyIndex;
                    m_canvas->update();
                    emit cameraKeyActivated(mark.sceneIndex, mark.keyIndex);
                }
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
    // THE LAYER ROW (batch I-1): on a key, the playhead goes there and its menu opens (Copy, Cut,
    // Paste at the Playhead, Delete); off a key, the row's own menu - Paste Key at the Playhead
    // alone (batch I-1b round 2). Either way the row is the last thing pressed.
    if (inLayerRow(pos)) {
        if (m_camSelKey >= 0) {
            m_camSelScene = m_camSelKey = -1;
            m_canvas->update();
        }
        // THE DRAWINGS LANE (batch I-2): its own menu - New Drawing, New Drawing from This
        // One, Delete Drawing - the block under the pointer chosen first. The keys' lane keeps
        // the keys' menus, as built.
        if (inDrawingsLane(pos)) {
            int layerIndex = -1;
            if (runLayer(&layerIndex)) {
                // A right-click on a selected frame keeps the selection - its menu acts on all of
                // them (round FC-a); on another frame it selects that one, as a click does; off a
                // frame, none.
                const int block = drawingBlockAt(pos);
                if (block < 0 || !m_selectedDrawings.contains(block))
                    emit drawingBlockPressed(layerIndex, block, Qt::NoModifier);
                emit drawingsLaneContextMenuRequested(layerIndex, block, e->globalPos());
            }
            e->accept();
            return;
        }
        LayerMark mark;
        if (!onLayerEaseMark(pos) && layerKeyAt(pos, &mark)) { // (a mark takes no key)
            emit layerRowPressed(mark.flat, mark.layer, mark.key);
            emit layerKeyActivated(mark.flat, mark.layer, mark.key);
            emit layerKeyContextMenuRequested(mark.flat, mark.layer, mark.key, e->globalPos());
        } else {
            emit layerRowPressed(-1, -1, -1);
            emit layerRowContextMenuRequested(e->globalPos());
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
        const int idx = blockAtScreenX(pos.x());
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

bool AnimaticTimeline::panPress(const QMouseEvent *e)
{
    return e->button() == Qt::MiddleButton
        || (e->button() == Qt::LeftButton && e->modifiers().testFlag(Qt::ControlModifier));
}

void AnimaticTimeline::canvasMouseMove(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();

    // A pan (batch I-1b): the content follows the pointer, pixel for pixel, sideways only.
    // At full view the bar has no range and nothing moves.
    if (m_drag == Drag::Pan) {
        if (m_hScroll && m_hScroll->maximum() > 0) {
            const int wanted = qBound(0, m_panStartScroll - qRound(e->globalPosition().x() - m_panStartGlobalX),
                                      m_hScroll->maximum());
            if (wanted != m_scrollX) {
                m_panScrolling = true;
                m_hScroll->setSliderPosition(wanted); // actionTriggered(SliderMove) -> the user's
                m_panScrolling = false;
            }
        }
        return;
    }

    if (m_drag == Drag::Playhead) {
        m_dragPlayheadFrame = frameAtScreenX(pos.x());
        followChip(e->globalPosition().toPoint(), timecode(m_dragPlayheadFrame));
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
        followChip(e->globalPosition().toPoint(), timecode(m_camDragFrame));
        return;
    }

    // A CUT OF THE DRAWINGS LANE dragged (batch I-2; the user's answer 4): a frame at a time,
    // never onto the drawing before it; the later drawings move with it, and with Alt - held
    // or let go as the drag goes - the cut alone, never onto the drawing after it. With Snap
    // on it takes the playhead and this layer's keys, as a dragged key takes its targets.
    if (m_drag == Drag::DrawingCut) {
        if (!(e->buttons() & Qt::LeftButton)) {
            m_drag = Drag::None; // the release went elsewhere
            m_cutDragActive = false;
            m_canvas->update();
            return;
        }
        if (!m_cutDragActive) {
            if ((pos - m_layerPress).manhattanLength() < kKeyDragThreshold)
                return;
            m_cutDragActive = true;
            m_canvas->setCursor(Qt::SizeHorCursor);
        }
        const Layer *layer = runLayer();
        if (!layer || m_selected < 0 || m_selected >= m_blocks.size() || m_cutDragDrawing < 1
            || m_cutDragDrawing >= layer->drawings.size())
            return;
        const Block &b = m_blocks.at(m_selected);
        m_cutDragAlone = e->modifiers().testFlag(Qt::AltModifier);
        Layer counted = *layer;
        counted.countRunAt(m_fps);
        const int least = counted.drawingStart(m_cutDragDrawing - 1) + 1;
        const int most = qMax(least, m_cutDragAlone && m_cutDragDrawing + 1 < counted.drawings.size()
                                         ? counted.drawingStart(m_cutDragDrawing + 1) - 1
                                         : b.frames - 1);
        int frame = qBound(least, frameAtScreenX(pos.x()) - b.startFrame, most);
        if (m_snap) {
            QVector<int> targets;
            targets << playheadFrame() - b.startFrame;
            for (const shotcam::LayerKey &key : layer->keys)
                targets << layerKeyFrame(key, b.frames);
            int best = kKeySnapPx + 1;
            for (int t : targets) {
                if (t < least || t > most)
                    continue;
                const int d = qAbs(contentXToScreen((b.startFrame + t) * pxPerFrame()) - pos.x());
                if (d < best) {
                    best = d;
                    frame = t;
                }
            }
        }
        if (frame != m_cutDragFrame) {
            m_cutDragFrame = frame;
            m_canvas->update();
        }
        // The chip: the drawing the cut ends, and how long it is shown now.
        const int length = m_cutDragFrame - counted.drawingStart(m_cutDragDrawing - 1);
        followChip(e->globalPosition().toPoint(),
                   QStringLiteral("frame %1  ").arg(m_cutDragDrawing) + QChar(0x00B7)
                       + (length == 1 ? QStringLiteral("  held 1 frame") : QStringLiteral("  held %1 frames").arg(length)));
        return;
    }

    // A LAYER KEY dragged in time (batch I-1): within its own panel, from its first frame to
    // its end.
    if (m_drag == Drag::LayerKey) {
        if (!(e->buttons() & Qt::LeftButton)) {
            m_drag = Drag::None; // the release went elsewhere
            m_layerDragActive = false;
            m_canvas->update();
            return;
        }
        if (!m_layerDragActive) {
            if ((pos - m_layerPress).manhattanLength() < kKeyDragThreshold)
                return;
            m_layerDragActive = true;
            m_canvas->setCursor(Qt::SizeHorCursor);
        }
        if (m_layerDragFlat < 0 || m_layerDragFlat >= m_blocks.size())
            return;
        const Block &b = m_blocks.at(m_layerDragFlat);
        const int frame = qBound(b.startFrame, frameAtScreenX(pos.x()), b.startFrame + b.frames - 1);
        if (frame != m_layerDragFrame) {
            m_layerDragFrame = frame;
            m_canvas->update();
        }
        followChip(e->globalPosition().toPoint(), timecode(m_layerDragFrame));
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

    if ((m_drag == Drag::ResizeRight || m_drag == Drag::ResizeLeft || m_drag == Drag::Roll)
        && m_resizeIndex >= 0 && m_resizeIndex < m_blocks.size()) {
        // The frame under the pointer decides, at any zoom (a frame may be
        // less than a pixel: the nearest one). Back within a pixel of where
        // it was pressed, the drag is where it began: nothing changes.
        const double ppf = qMax(1e-6, pxPerFrame());
        const double cx = screenXToContent(pos.x());
        const bool home = qAbs(cx - m_dragPressX) <= 1.0;
        const int at = static_cast<int>(std::lround(cx / ppf)); // the frame the edge would be on
        const Block &b = m_blocks.at(m_resizeIndex);
        const auto signedFrames = [](int d) {
            return d > 0 ? QStringLiteral("+%1 f").arg(d) : QString::fromUtf8("\xE2\x88\x92%1 f").arg(-d);
        };
        if (m_drag == Drag::Roll && m_rollNext >= 0 && m_rollNext < m_blocks.size()) {
            const int total = m_dragFromFrames + m_dragFromFramesNext;
            const int left = home ? m_dragFromFrames : qBound(1, at - b.startFrame, total - 1);
            m_dragFramesFor.insert(m_resizeIndex, left);
            m_dragFramesFor.insert(m_rollNext, total - left);
            const Block &n = m_blocks.at(m_rollNext);
            const int d = left - m_dragFromFrames;
            // A locked scene's roll says so; one across a cut between scenes names them
            // (batch F1: "P5 +6 f, P1 -6 f" named no scene).
            const QString lead = m_lockedRoll ? QStringLiteral("locked") : QStringLiteral("Alt");
            const QString sceneOfB = b.sceneIndex != n.sceneIndex ? QStringLiteral("S%1 ").arg(b.sceneNumber) : QString();
            const QString sceneOfN = b.sceneIndex != n.sceneIndex ? QStringLiteral("S%1 ").arg(n.sceneNumber) : QString();
            m_dragChip = QStringLiteral("%1 \xC2\xB7 roll: %2P%3 %4, %5P%6 %7")
                             .arg(lead, sceneOfB).arg(b.panelIndex + 1)
                             .arg(d == 0 ? QStringLiteral("0 f") : signedFrames(d), sceneOfN).arg(n.panelIndex + 1)
                             .arg(d == 0 ? QStringLiteral("0 f") : signedFrames(-d));
        } else {
            int frames = m_dragFromFrames;
            if (!home) {
                frames = m_drag == Drag::ResizeRight
                    ? at - b.startFrame                               // the end follows the pointer
                    : m_dragFromFrames - (at - m_dragOriginFrame);    // the start: trimmed off the head
            }
            frames = qMax(kMinFrames, frames);
            m_dragFramesFor.insert(m_resizeIndex, frames);
            const int d = frames - m_dragFromFrames;
            const QString change = d == 0 ? QString() : QStringLiteral(" (%1)").arg(signedFrames(d));
            m_dragChip = m_drag == Drag::ResizeRight
                ? QStringLiteral("%1 \xC2\xB7 %2 f%3").arg(shotcam::lengthText(frames, m_fps)).arg(frames).arg(change)
                : QStringLiteral("start %1 \xC2\xB7 %2").arg(d == 0 ? QStringLiteral("0 f") : signedFrames(d))
                      .arg(shotcam::lengthText(frames, m_fps));
        }
        rebuildBlocks();
        updateScrollRange();
        showDragChip(e->globalPosition().toPoint());
        m_canvas->update();
        return;
    }

    // Idle hover: cursor + highlight. A hidden SHOTS track has nothing to
    // hover; a locked one shows no resize cursor (there is no trim to cue).
    Qt::CursorShape cursor = Qt::ArrowCursor;
    int hover = -1;
    if (m_tracks.shotsVisible && pos.x() >= kLabelCol && pos.y() >= kPanelY
        && pos.y() < kPanelY + panelTrackH()) {
        hover = blockAtScreenX(pos.x());
        cursor = (hover >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor;
        // Resize-handle cursor on the selected clip edges.
        if (!m_tracks.shotsLocked && m_selected >= 0 && m_selected < m_blocks.size()) {
            const Block &b = m_blocks.at(m_selected);
            const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
            const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
            if (qAbs(pos.x() - x0) <= kHandleW || qAbs(pos.x() - x1) <= kHandleW) {
                cursor = Qt::SizeHorCursor;
                // A locked scene's own ends hold (batch F1): "not allowed" there.
                if (sceneLocked(b.sceneIndex)) {
                    const int first = qAbs(pos.x() - x1) <= kHandleW ? m_selected : m_selected - 1;
                    const bool inner = first >= 0 && first + 1 < m_blocks.size()
                        && m_blocks.at(first).sceneIndex == m_blocks.at(first + 1).sceneIndex;
                    if (!inner)
                        cursor = Qt::ForbiddenCursor;
                }
            }
        }
    } else if (inDrawingsLane(pos)) {
        // The drawings lane (batch I-2): over a cut the clip edge's cursor, over a block the
        // hand a clip shows.
        cursor = drawingCutAt(pos) >= 1 ? Qt::SizeHorCursor
                                        : (drawingBlockAt(pos) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    } else if (pos.y() < kRulerY + kRulerH) {
        cursor = Qt::PointingHandCursor;
    }
    // The block of the drawings lane under the pointer: a little lighter (2026-11-26).
    {
        const int hoverDrawing = inDrawingsLane(pos) ? drawingBlockAt(pos) : -1;
        if (hoverDrawing != m_hoverDrawing) {
            m_hoverDrawing = hoverDrawing;
            m_canvas->update();
        }
    }
    // THE SCENES ROW (batch F1): the scene under the pointer - its lock shows, faint, on
    // an unlocked one - and whether the pointer is on that lock (the hand: a click
    // toggles it).
    int sceneHover = -1;
    bool lockHover = false;
    if (m_tracks.scenesVisible && pos.y() >= kSceneY && pos.y() < kSceneY + kSceneH) {
        sceneHover = sceneAtScreenX(pos.x());
        lockHover = sceneHover >= 0 && sceneLockGlyphRect(sceneHover).adjusted(-3, -3, 3, 3).contains(pos);
        if (lockHover)
            cursor = Qt::PointingHandCursor;
    }
    // The CAMERA row: the key under the pointer is lit (the accent halo)
    // and named in a tooltip (Figma 1C-1).
    int camScene = -1, camKey = -1;
    CameraMark mark;
    if (m_tracks.cameraVisible && !onCameraEaseMark(pos) && cameraKeyAt(pos, &mark)) {
        camScene = mark.sceneIndex;
        camKey = mark.keyIndex;
        cursor = Qt::PointingHandCursor;
    }
    // A move's ease mark (batch E): a button - the hand, and its edge lit. Not over a key.
    int easeScene = -1, easeKey = -1;
    {
        CameraSegment segment;
        QRectF markRect;
        if (m_tracks.cameraVisible && camKey < 0 && easeMarkAt(pos, &segment, &markRect)) {
            easeScene = segment.sceneIndex;
            easeKey = segment.fromKey;
            cursor = Qt::PointingHandCursor;
        }
    }
    if (easeScene != m_easeHoverScene || easeKey != m_easeHoverKey) {
        m_easeHoverScene = easeScene;
        m_easeHoverKey = easeKey;
        m_canvas->update();
    }
    // An amber chip at any of its sizes (batch 6d, GROW-1): the hand, and as its
    // tooltip the whole sentence and what a press does.
    const int chipHover = artExitChipAt(pos);
    if (chipHover >= 0)
        cursor = Qt::PointingHandCursor;
    // Ctrl held with the view zoomed in: the open hand - a drag pans (batch I-1b, note 8).
    if (e->modifiers().testFlag(Qt::ControlModifier) && m_hScroll && m_hScroll->maximum() > 0)
        cursor = Qt::OpenHandCursor;
    m_canvas->setCursor(cursor);
    if (hover != m_hoverIndex) {
        m_hoverIndex = hover;
        m_canvas->update();
    }
    // THE HOVERS' TOOLTIPS WAIT (batch C): each is given when Qt's tooltip is due, 0.35 s
    // after the pointer stops, as every tooltip in the app (TimelineCanvas::event,
    // hoverTipAt) - not at once from the move. Moving onto the next one while a tooltip
    // is up shows its own at once, as Qt's next tooltip comes at once; moving off hides it.
    const QPoint globalPos = e->globalPosition().toPoint();
    auto retarget = [&](const QString &tip) {
        if (QToolTip::isVisible())
            QToolTip::showText(globalPos, tip, m_canvas);
    };
    if (camScene != m_camHoverScene || camKey != m_camHoverKey) {
        m_camHoverScene = camScene;
        m_camHoverKey = camKey;
        if (camKey >= 0)
            retarget(cameraKeyTip(mark));
        else
            sankoHideTipSoon(); // the Brush bar's 0.12 s grace (Qt's hideText waits 0.3 s)
        m_canvas->update();
    }
    if (chipHover != m_chipHover) {
        m_chipHover = chipHover;
        if (chipHover >= 0)
            retarget(artExitChipHoverText(chipHover));
        else if (camKey < 0)
            sankoHideTipSoon();
    }
    // Its tooltip: over the lock, what a click does; over the block, where the scene
    // ends and how long it is (batch F1).
    if (sceneHover != m_sceneHover || lockHover != m_sceneLockHover) {
        m_sceneHover = sceneHover;
        m_sceneLockHover = lockHover;
        if (sceneHover >= 0)
            retarget(lockHover ? sceneLockTip(sceneHover) : sceneEndTip(sceneHover));
        else if (camKey < 0 && chipHover < 0)
            sankoHideTipSoon();
        m_canvas->update();
    }
    // The AUDIO row under the pointer (Phase 2D): its name and length chips
    // hide, so the waveform under them can be seen.
    const bool overAudio = m_tracks.audioVisible
        && QRect(kLabelCol, audioTrackY(), m_canvas->width() - kLabelCol, kAudioH).contains(pos);
    if (overAudio != m_audioHover) {
        m_audioHover = overAudio;
        m_canvas->update();
    }
}

// The tooltip of what is under a point of the canvas: a camera key's, an amber chip's, or
// the SCENES row's (the lock's or the block's) - what the hover lights up; empty
// elsewhere. Asked by Qt's tooltip event (TimelineCanvas::event).
QString AnimaticTimeline::hoverTipAt(const QPoint &pos) const
{
    CameraMark mark;
    if (m_tracks.cameraVisible && !onCameraEaseMark(pos) && cameraKeyAt(pos, &mark))
        return cameraKeyTip(mark);
    const int chip = artExitChipAt(pos);
    if (chip >= 0)
        return artExitChipHoverText(chip);
    if (m_tracks.scenesVisible && pos.y() >= kSceneY && pos.y() < kSceneY + kSceneH) {
        const int scene = sceneAtScreenX(pos.x());
        if (scene >= 0)
            return sceneLockGlyphRect(scene).adjusted(-3, -3, 3, 3).contains(pos) ? sceneLockTip(scene)
                                                                                 : sceneEndTip(scene);
    }
    return QString();
}

QString AnimaticTimeline::artExitChipHoverText(int flatIndex) const
{
    return artExitChipText(flatIndex) + QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" click for Grow to fit");
}

// A double-click on a clip's duration pill opens the typing field (Figma
// DUR-1). Anything else is what the second press always was: Qt sends it
// here in place of a press, so it goes on to the press handler.
void AnimaticTimeline::canvasMouseDoubleClick(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!panPress(e) && e->button() == Qt::LeftButton && m_tracks.shotsVisible && pos.x() >= kLabelCol
        && pos.y() >= kPanelY && pos.y() < kPanelY + panelTrackH()) {
        const int idx = blockAtScreenX(pos.x());
        if (idx >= 0 && idx < m_blocks.size()) {
            const ClipLayout L = clipLayout(idx, clipRect(idx));
            if (L.pillShown && L.pill.contains(pos)) {
                openDurationField(idx);
                return;
            }
        }
    }
    // A DOUBLE-CLICK ON THE DRAWINGS LANE - its blocks or its label - is Zoom to Frames (the
    // user's note 7, 2026-11-26). The first click has already put the playhead on the block
    // under it, so the view is centred there; the second starts no drag of a cut.
    if (!panPress(e) && e->button() == Qt::LeftButton && m_layerRowShown && m_layerRowRun
        && pos.y() >= layerTrackY() + kKeysLaneH && pos.y() < layerTrackY() + layerRowH()) {
        zoomToFrames();
        e->accept();
        return;
    }
    canvasMousePress(e);
}

void AnimaticTimeline::canvasMouseRelease(QMouseEvent *e)
{
    if (m_drag == Drag::Pan) { // the pan ends with its button's release (batch I-1b)
        if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
            m_drag = Drag::None;
            m_canvas->setCursor(Qt::ArrowCursor);
            // A Ctrl press on a frame let go where it was pressed: a Ctrl+click on it (round FC-a).
            constexpr double kClickSlop = 3.0;
            if (e->button() == Qt::LeftButton && m_panLaneBlock >= 0
                && qAbs(e->globalPosition().x() - m_panStartGlobalX) <= kClickSlop
                && qAbs(e->globalPosition().y() - m_panStartGlobalY) <= kClickSlop && m_scrollX == m_panStartScroll)
                emit drawingBlockPressed(m_panLaneLayer, m_panLaneBlock, e->modifiers() | Qt::ControlModifier);
            m_panLaneBlock = m_panLaneLayer = -1;
        }
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;

    if (m_drag == Drag::ResizeRight || m_drag == Drag::ResizeLeft || m_drag == Drag::Roll) {
        // The lengths the drag reached go out as ONE request (a roll's two
        // as one); a drag back where it began sends nothing. The blocks are
        // rebuilt from the panels once the workspace has answered.
        const Drag kind = m_drag;
        const int first = m_resizeIndex, next = m_rollNext;
        const int framesA = m_dragFramesFor.value(first, m_dragFromFrames);
        const int framesB = m_dragFramesFor.value(next, m_dragFromFramesNext);
        const int fromA = m_dragFromFrames;
        m_drag = Drag::None;
        m_resizeIndex = -1;
        m_rollNext = -1;
        m_lockedRoll = false;
        m_dragFramesFor.clear();
        m_dragOriginFrame = -1;
        m_dragChip.clear();
        m_canvas->setCursor(Qt::ArrowCursor);
        hideChip();
        if (first >= 0 && first < m_blocks.size()) {
            const Block a = m_blocks.at(first);
            if (kind == Drag::Roll && next >= 0 && next < m_blocks.size()) {
                const Block bb = m_blocks.at(next);
                if (framesA != fromA)
                    emit rollRequested(a.sceneIndex, a.panelIndex, framesA, bb.sceneIndex, bb.panelIndex,
                                       framesB);
            } else {
                // durationChanged also answers a drag that ends where it
                // began (the page redraws the clip from the panel).
                emit durationChanged(a.sceneIndex, a.panelIndex, framesA);
            }
        }
        rebuildBlocks();
        updateScrollRange();
        m_canvas->update();
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
        emit playheadDragEnded(); // batch I-1b: now the Move tool's box
        m_scrubFrameSent = -1;
        // The playhead came to rest at a panel's start, which may be off
        // the left edge of the view: the jump rule, once, now that the
        // drag is over (jumps are suppressed while it lasts - the strip
        // must not move under a dragging hand).
        m_viewHeld = false;
        jumpIfOffScreen();
        m_canvas->update();
        hideChip();
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
        hideChip();
        // ONE request on release: the workspace re-anchors the key to the
        // panel it landed in, as one undo step.
        if (moved)
            emit cameraKeyMoveRequested(scene, key, sceneFrame);
        return;
    }
    if (m_drag == Drag::DrawingCut) { // batch I-2: one request on release, the workspace's step
        const bool moved = m_cutDragActive && m_cutDragFrame != m_cutDragFrom;
        const bool tapped = !m_cutDragActive;
        const int layer = m_cutDragLayer, drawing = m_cutDragDrawing, frame = m_cutDragFrame;
        const bool alone = m_cutDragAlone;
        m_drag = Drag::None;
        m_cutDragActive = false;
        m_cutDragDrawing = m_cutDragLayer = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        m_canvas->update();
        hideChip();
        if (tapped) {
            // A tap on a cut: the drawing that starts there, with the keys held at the press.
            emit drawingBlockPressed(layer, drawing, m_cutDragMods & ~Qt::AltModifier);
        } else {
            if (moved)
                emit drawingCutMoveRequested(layer, drawing, frame, alone);
            emit drawingBlockPressed(layer, -1, Qt::NoModifier); // the row chosen, no frame (as before)
        }
        return;
    }
    if (m_drag == Drag::LayerKey) { // batch I-1: one request on release, the workspace's step
        const bool moved = m_layerDragActive && m_layerDragFrame != m_layerDragFromFrame;
        const int flat = m_layerDragFlat, layer = m_layerDragLayer, key = m_layerDragKey;
        const int inPanel = flat >= 0 && flat < m_blocks.size() ? m_layerDragFrame - m_blocks.at(flat).startFrame : 0;
        m_drag = Drag::None;
        m_layerDragActive = false;
        m_layerDragFlat = m_layerDragLayer = m_layerDragKey = -1;
        m_canvas->setCursor(Qt::ArrowCursor);
        m_canvas->update();
        hideChip();
        if (moved)
            emit layerKeyMoveRequested(flat, layer, key, inPanel);
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
    // A hovered key's, chip's or scene's tooltip goes after the Brush bar's 0.12 s grace, as
    // when the pointer moves off it on the canvas (batch C).
    if (m_chipHover != -1 || m_camHoverKey != -1 || m_sceneHover != -1)
        sankoHideTipSoon();
    m_chipHover = -1; // its tooltip goes with the pointer's next crossing, as any tooltip
    if (m_hoverIndex != -1 || m_camHoverKey != -1 || m_easeHoverKey != -1 || m_audioHover
        || m_sceneHover != -1 || m_hoverDrawing != -1) {
        m_hoverIndex = -1;
        m_hoverDrawing = -1; // the drawings lane's lit block goes out
        m_camHoverScene = m_camHoverKey = -1;
        m_easeHoverScene = m_easeHoverKey = -1; // a lit ease mark goes out (batch E)
        m_sceneHover = -1; // an unlocked scene's faint lock goes (batch F1)
        m_sceneLockHover = false;
        m_audioHover = false; // the chips come back
        if (m_canvas)
            m_canvas->update();
    }
    if (m_canvas)
        m_canvas->setCursor(Qt::ArrowCursor);
}

void AnimaticTimeline::setStageMargin(const QSize &margin)
{
    m_stageMargin = margin;
    if (m_canvas)
        m_canvas->update();
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
