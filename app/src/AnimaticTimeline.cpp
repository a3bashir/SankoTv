#include "AnimaticTimeline.h"

#include "AnimaticPage.h"
#include "StoryboardModel.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolTip>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr int kFps = 24;
constexpr double kMsPerFrame = 1000.0 / kFps;

constexpr int kLabelCol = 90; // left track-name column width

// Track geometry (top to bottom), all within the canvas.
constexpr int kRulerY = 0,   kRulerH = 24;
constexpr int kSceneY = 24,  kSceneH = 24;
constexpr int kPanelY = 48,  kPanelH = 80;
constexpr int kAudioY = 128, kAudioH = 36;
constexpr int kCamY   = 164, kCamH   = 24;
constexpr int kMarkY  = 188, kMarkH  = 24;
constexpr int kCanvasH = 212; // sum of the tracks above

constexpr int kHandleW = 6;   // trim handle width
constexpr int kGrab = 5;      // px grab tolerance
constexpr int kMinDur = 1, kMaxDur = 30;

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
        setFixedHeight(kCanvasH);
        setMouseTracking(true);
        setAttribute(Qt::WA_StyledBackground, true);
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
    root->addWidget(m_canvas);

    m_hScroll = new QScrollBar(Qt::Horizontal);
    m_hScroll->setFixedHeight(12);
    m_hScroll->setStyleSheet(QStringLiteral(
        "QScrollBar:horizontal { background: #0d0d0d; height: 12px; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #2f2f2f; border-radius: 4px; min-width: 24px; }"
        "QScrollBar::handle:horizontal:hover { background: #3d3d3d; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; }"));
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
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_scrollX = v;
        m_canvas->update();
    });
}

void AnimaticTimeline::setHost(AnimaticPage *host)
{
    m_host = host;
}

void AnimaticTimeline::styleToolbarButtons()
{
    const QString plain = QStringLiteral(
        "QPushButton { background: #1a1a1a; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 3px 9px; font-size: 11px; }"
        "QPushButton:hover { border-color: #f5a623; color: #f5a623; }");
    const QString active = QStringLiteral(
        "QPushButton { background: #f5a623; color: #0a0a0a; border: 1px solid #f5a623;"
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
    rebuildBlocks();
    updateScrollRange();
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setCurrentPanel(int flatIndex)
{
    if (m_current == flatIndex)
        return;
    m_current = flatIndex;
    if (m_canvas)
        m_canvas->update();
}

void AnimaticTimeline::setPlaying(bool playing)
{
    m_playing = playing;
    if (m_canvas)
        m_canvas->update();
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
            b.frames = dur * kFps;
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
    return qMax(f, kFps); // never zero
}

int AnimaticTimeline::totalSeconds() const
{
    int s = 0;
    for (const Block &b : m_blocks)
        s += b.duration;
    return s;
}

double AnimaticTimeline::pxPerFrame() const { return m_zoom / 100.0; }
double AnimaticTimeline::pxPerSecond() const { return pxPerFrame() * kFps; }
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
    const int elapsedMs = m_host ? m_host->elapsedMsInCurrentPanel() : 0;
    int elapsedFrames = static_cast<int>(elapsedMs / kMsPerFrame);
    elapsedFrames = qBound(0, elapsedFrames, b.frames);
    return b.startFrame + elapsedFrames;
}

void AnimaticTimeline::updateScrollRange()
{
    if (!m_hScroll)
        return;
    const int maxScroll = qMax(0, static_cast<int>(std::ceil(contentWidthPx())) - visibleWidth());
    m_hScroll->setRange(0, maxScroll);
    m_hScroll->setPageStep(qMax(1, visibleWidth()));
    m_hScroll->setSingleStep(qMax(1, static_cast<int>(pxPerSecond())));
    if (m_scrollX > maxScroll) {
        m_scrollX = maxScroll;
        m_hScroll->setValue(maxScroll);
    }
    m_hScroll->setVisible(maxScroll > 0);
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
        m_hScroll->setValue(0);
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
    const int totalSec = frame / kFps;
    const int ff = frame % kFps;
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
    p.fillRect(QRect(0, kPanelY, W, kPanelH), QColor("#141414"));
    p.fillRect(QRect(0, kAudioY, W, kAudioH),
               QColor(m_audioLoaded ? "#0d1a26" : "#111111"));
    p.fillRect(QRect(0, kCamY, W, kCamH), QColor("#0d0d0d"));
    p.fillRect(QRect(0, kMarkY, W, kMarkH), QColor("#0d0d0d"));

    // Clip all time-based content to the area right of the label column.
    p.save();
    p.setClipRect(QRect(kLabelCol, 0, W - kLabelCol, kCanvasH));

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
                const int x = contentXToScreen((s2 * kFps / 2) * ppf);
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
                    ? QString::number(s * kFps)
                    : timecode(s * kFps);
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
            QRect r(x0, kPanelY + 2, x1 - x0, kPanelH - 4);
            const QRectF rf(r);

            const bool current = (b.flatIndex == m_current);
            const bool hovered = (b.flatIndex == m_hoverIndex);

            // Amber bloom around the selected clip: explicit filled rounded rects
            // stepping outward, opacity rising toward the clip body, drawn under
            // the clip fill so a visible halo punches through the dark background.
            if (current) {
                struct Glow { qreal off; int alpha; };
                const Glow glows[3] = {{6.0, 40}, {4.0, 60}, {2.0, 90}};
                for (const Glow &gl : glows) {
                    QColor c("f5a623");
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

            // Border (selected: 2px amber; else 1px purple, amber-50% on hover).
            p.setBrush(Qt::NoBrush);
            if (current) {
                QColor amber("f5a623");
                amber.setAlpha(255);
                p.setPen(QPen(amber, 2.5));
                p.drawRoundedRect(rf.adjusted(1, 1, -1, -1), kClipR, kClipR);
            } else {
                QColor bord("#3d3894");
                if (hovered) { bord = QColor("#f5a623"); bord.setAlphaF(0.50); }
                p.setPen(QPen(bord, 1));
                p.drawRoundedRect(rf.adjusted(0.5, 0.5, -0.5, -0.5), kClipR, kClipR);
            }

            // Thumbnail in the left portion of the clip.
            int thumbW = 0;
            const bool showThumb = (r.width() >= 50);
            if (showThumb) {
                thumbW = qMin(static_cast<int>(r.width() * 0.35), 60);
                const int thumbH = r.height() - 8;
                const QRect thumbRect(r.left() + 4, r.top() + 4, thumbW, thumbH);

                QPixmap pix;
                if (b.sceneIndex < m_scenes.size()) {
                    Scene *sc = m_scenes.at(b.sceneIndex);
                    if (b.panelIndex < sc->panels.size())
                        pix = sc->panels.at(b.panelIndex)->pixmap;
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
                p.setPen(QColor("#f5a623"));
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
                           QColor("#f5a623"));
                p.fillRect(QRect(r.right() - kHandleW + 1, r.top(), kHandleW, r.height()),
                           QColor("#f5a623"));
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
        p.fillRect(QRect(x0, kPanelY, x1 - x0, kPanelH), green);
        p.setPen(QPen(QColor(0x4d, 0xff, 0x91), 2));
        p.drawLine(x0 + 1, kPanelY, x0 + 1, kPanelY + kPanelH);
        p.drawLine(x1 - 1, kPanelY, x1 - 1, kPanelY + kPanelH);
    }

    // ----- Audio track -----
    {
        if (m_audioLoaded && m_audioDurationMs > 0) {
            const double audioFrames = (m_audioDurationMs / 1000.0) * kFps;
            const double audioWidthPx = audioFrames * ppf;
            const int x0 = contentXToScreen(0);
            const int x1 = contentXToScreen(audioWidthPx);
            const double midY = kAudioY + kAudioH / 2.0;
            const double amp = kAudioH * 0.40;
            QColor blue(0x4d, 0x9f, 0xff);
            blue.setAlphaF(0.60);
            p.setPen(QPen(blue, 1.0));
            QPainterPath wave;
            bool started = false;
            for (int x = qMax(x0, kLabelCol); x <= qMin(x1, contentRight); x += 2) {
                const double phase = (x - x0) * 0.10;
                const double env = 0.55 + 0.45 * std::sin((x - x0) * 0.012);
                const double y = midY + std::sin(phase) * amp * env;
                if (!started) { wave.moveTo(x, y); started = true; }
                else wave.lineTo(x, y);
            }
            if (started)
                p.drawPath(wave);
        }
    }

    p.restore(); // end content clip

    // ----- Playhead (full height, over content area) -----
    {
        const int px = contentXToScreen(playheadContentX());
        if (px >= kLabelCol && px <= W) {
            p.setPen(QPen(QColor("#f5a623"), 1.5));
            p.drawLine(px, 0, px, kCanvasH);
            // Triangle handle in the ruler.
            p.setRenderHint(QPainter::Antialiasing, true);
            QPolygon tri;
            tri << QPoint(px - 5, 0) << QPoint(px + 5, 0) << QPoint(px, 8);
            p.setBrush(QColor("#f5a623"));
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
    }

    // ----- Left label column (painted on top, fixed) -----
    {
        p.fillRect(QRect(0, 0, kLabelCol, kCanvasH), QColor("#0d0d0d"));
        p.setPen(QColor("#2a2a2a"));
        p.drawLine(kLabelCol - 1, 0, kLabelCol - 1, kCanvasH);

        QFont f = font();
        f.setPointSize(7);
        p.setFont(f);
        p.setPen(QColor("#666666"));
        struct TL { const char *name; int y, h; };
        const TL labels[] = {
            {"TIMECODE", kRulerY, kRulerH}, {"SCENES", kSceneY, kSceneH},
            {"SHOTS", kPanelY, kPanelH},    {"AUDIO", kAudioY, kAudioH},
            {"CAMERA", kCamY, kCamH},       {"MARKERS", kMarkY, kMarkH},
        };
        for (const TL &t : labels)
            p.drawText(QRect(0, t.y, kLabelCol - 8, t.h),
                       Qt::AlignVCenter | Qt::AlignRight,
                       QString::fromLatin1(t.name));

        // Audio filename (just right of the label column).
        if (m_audioLoaded && m_host) {
            const QString path = m_host->audioPath();
            if (!path.isEmpty()) {
                QString name = path.section('/', -1).section('\\', -1);
                if (name.size() > 20) name = name.left(19) + QChar(0x2026);
                p.setPen(QColor("#88a6c4"));
                p.drawText(QRect(kLabelCol + 4, kAudioY, 160, kAudioH),
                           Qt::AlignVCenter | Qt::AlignLeft, name);
            }
        }
    }

    // ----- Reserved-track "coming soon" labels -----
    {
        QFont f = font();
        f.setPointSize(7);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(QColor("#3a3a3a"));
        p.drawText(QRect(kLabelCol, kCamY, W - kLabelCol, kCamH),
                   Qt::AlignCenter, QStringLiteral("Camera moves — coming soon"));
        p.drawText(QRect(kLabelCol, kMarkY, W - kLabelCol, kMarkH),
                   Qt::AlignCenter, QStringLiteral("Markers — coming soon"));
    }

    // Empty-audio hint.
    if (!m_audioLoaded) {
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(QColor("#555555"));
        p.drawText(QRect(kLabelCol, kAudioY, W - kLabelCol, kAudioH),
                   Qt::AlignCenter,
                   QStringLiteral("Drop audio file here or use Import Audio"));
    }
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
        return;
    }

    // Trim handles on the selected clip (panel track only).
    if (m_current >= 0 && m_current < m_blocks.size()
        && pos.y() >= kPanelY && pos.y() < kPanelY + kPanelH) {
        const Block &b = m_blocks.at(m_current);
        const int x0 = contentXToScreen(b.startFrame * pxPerFrame());
        const int x1 = contentXToScreen((b.startFrame + b.frames) * pxPerFrame());
        if (qAbs(pos.x() - x1) <= kHandleW) {
            m_drag = Drag::ResizeRight;
            m_resizeIndex = m_current;
            m_dragDuration = b.duration;
            m_canvas->setCursor(Qt::SizeHorCursor);
            QToolTip::showText(e->globalPosition().toPoint(),
                               QStringLiteral("%1.0s").arg(m_dragDuration), m_canvas);
            return;
        }
        if (qAbs(pos.x() - x0) <= kHandleW) {
            m_drag = Drag::ResizeLeft; // visual only for now
            m_resizeIndex = m_current;
            m_dragLeftFrames = 0;
            m_canvas->setCursor(Qt::SizeHorCursor);
            return;
        }
    }

    // Otherwise: click a clip to select + seek.
    if (pos.y() >= kPanelY && pos.y() < kPanelY + kPanelH) {
        const int frame = frameAtScreenX(pos.x());
        const int idx = blockAtFrame(frame);
        if (idx >= 0) {
            setCurrentPanel(idx);
            emit panelSeekRequested(idx);
        }
    }
}

void AnimaticTimeline::canvasMouseMove(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();

    if (m_drag == Drag::Playhead) {
        m_dragPlayheadFrame = frameAtScreenX(pos.x());
        QToolTip::showText(e->globalPosition().toPoint(),
                           timecode(m_dragPlayheadFrame), m_canvas);
        m_canvas->update();
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
    if (pos.x() >= kLabelCol && pos.y() >= kPanelY && pos.y() < kPanelY + kPanelH) {
        const int frame = frameAtScreenX(pos.x());
        hover = blockAtFrame(frame);
        cursor = (hover >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor;
        // Resize-handle cursor on the selected clip edges.
        if (m_current >= 0 && m_current < m_blocks.size()) {
            const Block &b = m_blocks.at(m_current);
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
        if (idx >= 0) {
            setCurrentPanel(idx);
            emit panelSeekRequested(idx);
        }
        m_canvas->update();
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
