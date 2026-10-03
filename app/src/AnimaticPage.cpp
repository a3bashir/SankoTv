#include "AnimaticPage.h"
#include "SankoTheme.h"

#include "AnimaticTimeline.h"
#include "StoryboardModel.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QTabletEvent>
#include <QWheelEvent>
#include <functional>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <Qt>

// THE PREVIEW SURFACE: a letterboxed display of one panel, laid over the
// drawing canvas while the animatic plays or is scrubbed.
//
// Why playback does not simply use the drawing canvas (measured before this
// was built, HANDOFF "Combined workspace"): it could afford to - switching
// the canvas costs 14 ms per cut at 4K - but every switch COMMITS a floating
// paste, a live transform and a pending QuickShape, clears the selection,
// and would play back through the artist's own zoom, rotation, flip, guides
// and onion skin. This widget touches none of that: it is a child of the
// canvas that covers it, so the canvas underneath keeps its panel, its
// selection and its in-flight edit, takes no pointer input while covered,
// and is exactly as it was the moment the preview goes away.
//
// It paints the same Panel::flattenedPixmap() every other consumer reads.
// A cheaper composite straight to preview size was measured (1.7 ms against
// 16 ms at 4K) and NOT used: it would be a second implementation of the
// flatten, which is the kind of duplicate that has drifted here before.
class PanelDisplay : public QWidget
{
public:
    explicit PanelDisplay(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    std::function<void()> onDismissed; // a click anywhere on the preview

    void setPixmap(const QPixmap &pixmap, const QString &caption)
    {
        m_pixmap = pixmap; // implicitly shared — no deep copy
        m_caption = caption;
        update();
    }

    void clear()
    {
        m_pixmap = QPixmap();
        m_caption.clear();
        update();
    }

    QString captionForTest() const { return m_caption; }
    bool hasPixmapForTest() const { return !m_pixmap.isNull(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (!m_pixmap.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            QSize target = m_pixmap.size();
            target.scale(size(), Qt::KeepAspectRatio);
            QRect r(QPoint(0, 0), target);
            r.moveCenter(rect().center());
            painter.drawPixmap(r, m_pixmap);
        }
        // Says what this is and how to leave it. An OPAQUE chip: it sits
        // over artwork of any colour and cannot borrow contrast from it.
        const QString text = m_caption.isEmpty()
            ? QStringLiteral("PREVIEW")
            : QStringLiteral("PREVIEW  \xC2\xB7  %1  \xC2\xB7  click to return to "
                             "drawing").arg(m_caption);
        QFont f = font();
        f.setPixelSize(11);
        painter.setFont(f);
        const QFontMetrics fm(f);
        // Bottom-right: the corner the floating toolbars leave free by
        // default (Layers top-left, Brush top-right, Zoom bottom-left). At
        // top-left it sat underneath the Layers toolbar, unreadable.
        const int chipW = fm.horizontalAdvance(text) + 20;
        const QRect chip(qMax(12, width() - chipW - 12), qMax(12, height() - 36),
                         chipW, 24);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1));
        painter.setBrush(QColor(0x16, 0x16, 0x16));
        painter.drawRoundedRect(chip, 6, 6);
        painter.setPen(QColor(0xdd, 0xdd, 0xdd));
        painter.drawText(chip, Qt::AlignCenter, text);
    }
    // Every pointer event is ACCEPTED so none of it reaches the canvas
    // underneath - an ignored tablet event in particular propagates to the
    // parent, and the parent is a drawing canvas. The preview goes away on
    // the RELEASE, so the whole press-move-release is consumed here and the
    // canvas never sees the tail of a gesture it did not see the start of.
    void mousePressEvent(QMouseEvent *event) override
    {
        event->accept();
        m_pressed = event->button() == Qt::LeftButton;
    }
    void mouseMoveEvent(QMouseEvent *event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        event->accept();
        finishPress();
    }
    void wheelEvent(QWheelEvent *event) override { event->accept(); }
    void tabletEvent(QTabletEvent *event) override
    {
        event->accept();
        if (event->type() == QEvent::TabletPress)
            m_pressed = true;
        else if (event->type() == QEvent::TabletRelease)
            finishPress();
    }

private:
    void finishPress()
    {
        if (!m_pressed)
            return;
        m_pressed = false;
        if (onDismissed)
            onDismissed();
    }

    QPixmap m_pixmap;
    QString m_caption;
    bool m_pressed = false;
};

namespace {

QPushButton *transportButton(const QString &glyph, const QString &tip)
{
    QPushButton *button = new QPushButton(glyph);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(36, 28); // sized for the 40 px header row
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: #1c1c1c; color: #ffffff; border: 1px solid #2a2a2a;"
        "  border-radius: 5px; font-size: 13px;"
        "}"
        "QPushButton:hover { background-color: #262626; }"
        "QPushButton:pressed { background-color: #303030; }"));
    return button;
}

} // namespace

AnimaticPage::AnimaticPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &AnimaticPage::advance);

    // Drives the real-time playhead while playing (50ms cadence).
    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setInterval(50);
    connect(m_playheadTimer, &QTimer::timeout, this, &AnimaticPage::onPlayheadTick);

    // Scratch audio playback (created up front so the volume slider can bind).
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(0.8);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);
    // The audio length is only known once the media metadata has loaded; feed
    // it to the timeline so the waveform can scale.
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        // "Loaded" to the timeline means the project NAMES a track, not
        // that it can be played: hasAudio() here told it a missing track
        // was no track, and its bar read "Right-click to import audio".
        if (m_timeline)
            m_timeline->setAudioLoaded(!m_audioPath.isEmpty(), d);
    });

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // One header row that stays when the section is collapsed, then the
    // timeline. There is no display here any more: the preview is laid over
    // the drawing canvas (attachPreview), and the buttons that have not yet
    // moved to the menus are handed to the workspace (legacyActions).
    m_display = new PanelDisplay;
    m_display->hide();
    m_display->onDismissed = [this] { leavePreview(); };
    m_legacyActions = createLegacyActions();
    // Both are re-parented out of this widget (the preview onto the canvas,
    // the buttons into the workspace's bottom bar), so either may be
    // destroyed before this object is: never keep a pointer to a dead one.
    connect(m_display, &QObject::destroyed, this, [this] { m_display = nullptr; });
    connect(m_legacyActions, &QObject::destroyed, this, [this] {
        m_legacyActions = nullptr;
        m_exportButton = nullptr; // its child goes with it
    });
    root->addWidget(createHeader());
    root->addWidget(createTimingStrip(), 1);

    // NOTHING HERE TAKES KEYBOARD FOCUS. The drawing canvas needs it (the
    // spacebar pan modifier is a key event on the canvas), and a button
    // that took focus on a click would both steal it and start answering
    // Space itself. Which keys the timeline gets is decided by where the
    // POINTER is, in the workspace - never by focus.
    const QList<QWidget *> ours = findChildren<QWidget *>()
        + m_legacyActions->findChildren<QWidget *>();
    for (QWidget *w : ours)
        if (qobject_cast<QAbstractButton *>(w)
            || qobject_cast<QAbstractSlider *>(w))
            w->setFocusPolicy(Qt::NoFocus);
}

AnimaticPage::~AnimaticPage()
{
    // Neither is necessarily in a layout of ours: the preview belongs to the
    // canvas once attached, the legacy bar to the workspace once placed.
    if (m_display && !m_display->parent())
        delete m_display;
    if (m_legacyActions && !m_legacyActions->parent())
        delete m_legacyActions;
}

// --- Not yet moved to the menus --------------------------------------------

// TEMPORARY, and shrinking by design: Import / Remove Audio, Export MP4 and
// Continue to Generation lived on the Animatic screen's top bar. The screen
// is gone; their new homes arrive pass by pass. Until each one moves it
// stays reachable from the workspace's bottom bar, so no pass leaves a
// feature without a way in. Each pass deletes its button here; the last one
// deletes this function.
//   Pass 2 (done): Import / Remove Audio -> the Edit menu and the audio
//                  track's right-click menu. Their buttons are gone.
//   Pass 3 (done): Continue to Generation -> nowhere. The Generation page
//                  is removed from the app; its button went with it.
//   Pass 4: Export MP4 -> File > Export.
QWidget *AnimaticPage::createLegacyActions()
{
    QWidget *bar = new QWidget;
    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_exportButton = new QPushButton(QStringLiteral("Export MP4"));
    m_exportButton->setCursor(Qt::PointingHandCursor);
    m_exportButton->setEnabled(false); // enabled once panels are loaded
    m_exportButton->setStyleSheet(SankoTheme::themed("QPushButton { background-color: %ACCENT%; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 8px 16px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: %ACCENT_HOVER%; }"
        "QPushButton:disabled { background-color: #1c1c1c; color: #555555;"
        " border: 1px solid #2a2a2a; }"));
    connect(m_exportButton, &QPushButton::clicked, this, &AnimaticPage::onExportMp4);
    layout->addWidget(m_exportButton);

    return bar;
}

// --- The preview, over the canvas -------------------------------------------

void AnimaticPage::attachPreview(QWidget *canvasArea)
{
    if (!m_display || !canvasArea)
        return;
    m_previewHost = canvasArea;
    m_display->setParent(canvasArea);
    m_display->setGeometry(canvasArea->rect());
    m_display->hide();
    canvasArea->installEventFilter(this); // follow its size
}

// The workspace is no longer on screen (another page of the window is, or
// the window is minimised): stop. The old Animatic screen paused in its Back
// button; there is no such button now, and a film left playing behind the
// Consistency Board would keep its audio running with nothing to stop it.
void AnimaticPage::hideEvent(QHideEvent *event)
{
    leavePreview();
    QWidget::hideEvent(event);
}

bool AnimaticPage::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_previewHost && event->type() == QEvent::Resize && m_display
        && m_previewHost)
        m_display->setGeometry(m_previewHost->rect());
    return QWidget::eventFilter(object, event);
}

bool AnimaticPage::previewVisible() const
{
    return m_previewShown;
}

void AnimaticPage::enterPreview()
{
    if (!m_display || !m_previewHost || m_items.isEmpty())
        return;
    if (!m_previewShown) {
        m_previewShown = true;
        m_display->setGeometry(m_previewHost->rect());
        m_display->show();
        m_display->raise(); // above the canvas's own child controls
    }
    showPreviewFrame();
}

// Back to drawing. Playback stops, the picture is dropped (a 4K flatten is
// 33 MB - it is not kept behind a hidden widget), and the playhead goes home
// to the SELECTED panel, so the next Play starts from what is on the canvas.
void AnimaticPage::leavePreview()
{
    if (m_playing)
        pause();
    if (!m_previewShown)
        return;
    m_previewShown = false;
    if (m_display) {
        m_display->hide();
        m_display->clear();
    }
    if (m_selected >= 0 && m_selected < m_items.size() && m_selected != m_current)
        seekTo(m_selected);
}

void AnimaticPage::showPreviewFrame()
{
    if (!m_previewShown || !m_display || m_current < 0
        || m_current >= m_items.size())
        return;
    const Item &it = m_items.at(m_current);
    m_display->setPixmap(it.panel->flattenedPixmap(),
                         QString::fromUtf8("Scene %1 \xE2\x80\x94 Panel %2")
                             .arg(it.sceneNumber)
                             .arg(it.panelInScene));
}

QString AnimaticPage::previewCaptionForTest() const
{
    return m_display ? m_display->captionForTest() : QString();
}

bool AnimaticPage::previewHasPictureForTest() const
{
    return m_display && m_display->hasPixmapForTest();
}

QString AnimaticPage::timecodeTextForTest() const
{
    return m_timecodeLabel ? m_timecodeLabel->text() : QString();
}

QWidget *AnimaticPage::previewSurface() const
{
    return m_display;
}

// --- Collapse ---------------------------------------------------------------

void AnimaticPage::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    if (m_body)
        m_body->setVisible(!collapsed);
    if (m_collapseButton) {
        // Down-pointing while open (click to fold away), up while collapsed.
        m_collapseButton->setText(QString::fromUtf8(
            collapsed ? "\xE2\x96\xB4" : "\xE2\x96\xBE"));
        m_collapseButton->setToolTip(collapsed
                                         ? QStringLiteral("Show the timeline")
                                         : QStringLiteral("Hide the timeline"));
    }
    emit collapsedChanged(collapsed);
}

int AnimaticPage::minimumExpandedHeight() const
{
    return kHeaderHeight
        + (m_timeline ? m_timeline->minimumSizeHint().height() : 0);
}

int AnimaticPage::defaultExpandedHeight() const
{
    return kHeaderHeight + (m_timeline ? m_timeline->sizeHint().height() : 0);
}

// --- Timing strip ---------------------------------------------------------

QWidget *AnimaticPage::createTimingStrip()
{
    QWidget *wrap = new QWidget;
    m_body = wrap;
    wrap->setAttribute(Qt::WA_StyledBackground, true);
    wrap->setStyleSheet(QStringLiteral(
        "background-color: #0d0d0d; border-top: 1px solid #1f1f1f;"));
    QVBoxLayout *wrapLayout = new QVBoxLayout(wrap);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->setSpacing(0);

    // Professional NLE timeline (zoom toolbar + multi-track canvas). The
    // transport, loop and speed controls live in the header row above it
    // (createHeader), which stays when this body is collapsed away.
    m_timeline = new AnimaticTimeline;
    m_timeline->setHost(this);
    // The timeline REQUESTS; the workspace decides and pushes state back
    // (setSelectedPanel / refreshStructure / refreshTiming). Nothing below
    // changes a panel, a selection or an order by itself.
    connect(m_timeline, &AnimaticTimeline::panelSeekRequested,
            this, &AnimaticPage::panelSelectRequested);
    connect(m_timeline, &AnimaticTimeline::playheadScrubbed,
            this, &AnimaticPage::onScrubbed);
    connect(m_timeline, &AnimaticTimeline::durationChanged,
            this, &AnimaticPage::onDurationChanged);
    connect(m_timeline, &AnimaticTimeline::panelMoveRequested,
            this, &AnimaticPage::panelMoveRequested);
    connect(m_timeline, &AnimaticTimeline::clipContextMenuRequested,
            this, &AnimaticPage::clipContextMenuRequested);
    connect(m_timeline, &AnimaticTimeline::audioContextMenuRequested,
            this, &AnimaticPage::showAudioMenu);
    wrapLayout->addWidget(m_timeline, 1);

    return wrap;
}

// --- Loop region ----------------------------------------------------------

bool AnimaticPage::loopActive() const
{
    const int n = m_items.size();
    return m_loopStartIndex >= 0 && m_loopEndIndex >= 0
        && m_loopStartIndex < n && m_loopEndIndex < n
        && m_loopStartIndex <= m_loopEndIndex;
}

void AnimaticPage::validateLoop()
{
    const int n = m_items.size();
    if (n == 0) {
        m_loopStartIndex = -1;
        m_loopEndIndex = -1;
        return;
    }
    if (m_loopStartIndex >= 0)
        m_loopStartIndex = qBound(0, m_loopStartIndex, n - 1);
    if (m_loopEndIndex >= 0)
        m_loopEndIndex = qBound(0, m_loopEndIndex, n - 1);
    if (m_loopStartIndex >= 0 && m_loopEndIndex >= 0
        && m_loopStartIndex > m_loopEndIndex) {
        const int t = m_loopStartIndex; // swap so start precedes end
        m_loopStartIndex = m_loopEndIndex;
        m_loopEndIndex = t;
    }
}

void AnimaticPage::setLoopStart()
{
    if (m_current < 0 || m_items.isEmpty())
        return;
    m_loopStartIndex = m_current;
    validateLoop();
    updateLoopUi();
    if (m_timeline)
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
}

void AnimaticPage::setLoopEnd()
{
    if (m_current < 0 || m_items.isEmpty())
        return;
    m_loopEndIndex = m_current;
    validateLoop();
    updateLoopUi();
    if (m_timeline)
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
}

void AnimaticPage::clearLoop()
{
    m_loopStartIndex = -1;
    m_loopEndIndex = -1;
    updateLoopUi();
    if (m_timeline)
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
}

void AnimaticPage::updateLoopUi()
{
    if (m_clearLoopButton)
        m_clearLoopButton->setVisible(loopActive());
    if (m_loopWarningLabel) {
        const bool onlyOne = ((m_loopStartIndex >= 0) != (m_loopEndIndex >= 0));
        m_loopWarningLabel->setVisible(onlyOne);
    }
}

// --- Header row -------------------------------------------------------------

// ONE row, and the only part of the section that survives a collapse: the
// fold button, the transport, the timecode and total, then loop, speed and
// volume. Collapsed, the workspace keeps exactly this - so playback is one
// click away with the timeline folded and the canvas at its tallest.
QWidget *AnimaticPage::createHeader()
{
    QWidget *bar = new QWidget;
    bar->setObjectName(QStringLiteral("animaticHeader"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(kHeaderHeight);
    bar->setStyleSheet(QStringLiteral(
        "QWidget#animaticHeader { background-color: #0d0d0d;"
        " border-top: 1px solid #1f1f1f; }"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(8, 0, 12, 0);
    layout->setSpacing(8);

    m_collapseButton = new QPushButton(QString::fromUtf8("\xE2\x96\xBE"));
    m_collapseButton->setObjectName(QStringLiteral("animaticCollapse"));
    m_collapseButton->setCursor(Qt::PointingHandCursor);
    m_collapseButton->setFocusPolicy(Qt::NoFocus);
    m_collapseButton->setFixedSize(26, 26);
    m_collapseButton->setToolTip(QStringLiteral("Hide the timeline"));
    m_collapseButton->setStyleSheet(SankoTheme::themed(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 13px; }"
        "QPushButton:hover { color: %ACCENT%; border-color: %ACCENT%; }"));
    connect(m_collapseButton, &QPushButton::clicked, this,
            [this] { setCollapsed(!m_collapsed); });
    layout->addWidget(m_collapseButton);

    // --- Transport (icons only) ------------------------------------------
    QPushButton *first = transportButton(QString::fromUtf8("|\xE2\x97\x80"),
                                         QStringLiteral("First panel"));
    connect(first, &QPushButton::clicked, this, &AnimaticPage::goFirst);
    layout->addWidget(first);

    QPushButton *prev = transportButton(QString::fromUtf8("\xE2\x97\x80"),
                                        QStringLiteral("Previous panel"));
    connect(prev, &QPushButton::clicked, this, &AnimaticPage::goPrev);
    layout->addWidget(prev);

    m_playButton = transportButton(QString::fromUtf8("\xE2\x96\xB6"),
                                   QStringLiteral("Play / Pause"));
    m_playButton->setObjectName(QStringLiteral("animaticPlay"));
    connect(m_playButton, &QPushButton::clicked, this, &AnimaticPage::togglePlay);
    layout->addWidget(m_playButton);

    QPushButton *next = transportButton(QString::fromUtf8("\xE2\x96\xB6"),
                                        QStringLiteral("Next panel"));
    connect(next, &QPushButton::clicked, this, &AnimaticPage::goNext);
    layout->addWidget(next);

    QPushButton *last = transportButton(QString::fromUtf8("\xE2\x96\xB6|"),
                                        QStringLiteral("Last panel"));
    connect(last, &QPushButton::clicked, this, &AnimaticPage::goLast);
    layout->addWidget(last);

    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"));
    m_timecodeLabel->setStyleSheet(SankoTheme::themed("color: %ACCENT%; font-family: 'Courier New'; font-size: 13px;"));
    layout->addWidget(m_timecodeLabel);

    m_totalLabel = new QLabel(QStringLiteral("Total: 0:00"));
    m_totalLabel->setStyleSheet(QStringLiteral("color: #aaaaaa; font-size: 13px;"));
    layout->addWidget(m_totalLabel);

    layout->addStretch(1);

    // --- Loop controls (compact) -----------------------------------------
    const QString loopBtn = QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 4px 8px; font-size: 11px; }"
        "QPushButton:hover { color: #4dff91; border-color: #4dff91; }");

    QWidget *loopGroup = new QWidget;
    QHBoxLayout *loopLayout = new QHBoxLayout(loopGroup);
    loopLayout->setContentsMargins(0, 0, 0, 0);
    loopLayout->setSpacing(4);

    QPushButton *setStart = new QPushButton(QStringLiteral("[ Loop"));
    setStart->setCursor(Qt::PointingHandCursor);
    setStart->setToolTip(QStringLiteral("Set loop start to the selected panel"));
    setStart->setStyleSheet(loopBtn);
    connect(setStart, &QPushButton::clicked, this, &AnimaticPage::setLoopStart);
    loopLayout->addWidget(setStart);

    QPushButton *setEnd = new QPushButton(QStringLiteral("Loop ]"));
    setEnd->setCursor(Qt::PointingHandCursor);
    setEnd->setToolTip(QStringLiteral("Set loop end to the selected panel"));
    setEnd->setStyleSheet(loopBtn);
    connect(setEnd, &QPushButton::clicked, this, &AnimaticPage::setLoopEnd);
    loopLayout->addWidget(setEnd);

    m_clearLoopButton = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"));
    m_clearLoopButton->setCursor(Qt::PointingHandCursor);
    m_clearLoopButton->setToolTip(QStringLiteral("Clear the loop region"));
    m_clearLoopButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 4px 8px; font-size: 11px; }"
        "QPushButton:hover { color: #e06c6c; border-color: #e06c6c; }"));
    m_clearLoopButton->setVisible(false);
    connect(m_clearLoopButton, &QPushButton::clicked, this, &AnimaticPage::clearLoop);
    loopLayout->addWidget(m_clearLoopButton);

    m_loopWarningLabel = new QLabel(QStringLiteral("Set both loop points to activate"));
    m_loopWarningLabel->setStyleSheet(SankoTheme::themed("color: %ACCENT%; font-size: 11px;"));
    m_loopWarningLabel->setVisible(false);
    loopLayout->addWidget(m_loopWarningLabel);

    layout->addWidget(loopGroup);

    // --- Speed -------------------------------------------------------------
    QWidget *speedGroup = new QWidget;
    QHBoxLayout *speedLayout = new QHBoxLayout(speedGroup);
    speedLayout->setContentsMargins(0, 0, 0, 0);
    speedLayout->setSpacing(4);

    m_speedHalfButton = new QPushButton(QStringLiteral("0.5x"));
    m_speedHalfButton->setCursor(Qt::PointingHandCursor);
    m_speedHalfButton->setToolTip(QStringLiteral("Play at half speed"));
    connect(m_speedHalfButton, &QPushButton::clicked, this,
            [this] { setPlaybackSpeed(0.5f); });
    speedLayout->addWidget(m_speedHalfButton);

    m_speed1xButton = new QPushButton(QStringLiteral("1x"));
    m_speed1xButton->setCursor(Qt::PointingHandCursor);
    m_speed1xButton->setToolTip(QStringLiteral("Play at normal speed"));
    connect(m_speed1xButton, &QPushButton::clicked, this,
            [this] { setPlaybackSpeed(1.0f); });
    speedLayout->addWidget(m_speed1xButton);

    m_speed2xButton = new QPushButton(QStringLiteral("2x"));
    m_speed2xButton->setCursor(Qt::PointingHandCursor);
    m_speed2xButton->setToolTip(QStringLiteral("Play at double speed"));
    connect(m_speed2xButton, &QPushButton::clicked, this,
            [this] { setPlaybackSpeed(2.0f); });
    speedLayout->addWidget(m_speed2xButton);

    layout->addWidget(speedGroup);
    updateSpeedButtons(); // 1x active by default

    // --- Audio: the track's name and its volume (shown once one is loaded)
    m_audioLabel = new QLabel;
    m_audioLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 12px;"));
    m_audioLabel->setVisible(false);
    layout->addWidget(m_audioLabel);

    m_volumeSlider = new QSlider(Qt::Horizontal);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setFixedWidth(60);
    m_volumeSlider->setToolTip(QStringLiteral("Audio volume"));
    m_volumeSlider->setVisible(false);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_audioOutput)
            m_audioOutput->setVolume(v / 100.0);
    });
    layout->addWidget(m_volumeSlider);

    return bar;
}

// --- Data / display -------------------------------------------------------

// m_items is a DERIVED index - one row per panel, in play order - over the
// workspace's own scenes. It holds Panel pointers, so it must be rebuilt
// whenever the panel lists change (refreshStructure) and emptied before the
// scenes are destroyed (loadScenes({})): a row left pointing at a panel
// whose undo command has since been dropped is a use-after-free the moment
// playback, the preview or an export reads it. When this was a separate
// screen the list was rebuilt on every visit and that could not happen.
void AnimaticPage::rebuildItems()
{
    m_items.clear();
    for (Scene *scene : std::as_const(m_scenes)) {
        for (int pi = 0; pi < scene->panels.size(); ++pi)
            m_items.append({scene, scene->panels.at(pi), scene->number, pi + 1});
    }
}

void AnimaticPage::loadScenes(const QVector<Scene *> &scenes)
{
    leavePreview(); // stops playback; drops the picture of a panel that may be going
    m_scenes = scenes;
    rebuildItems();

    // Loop is a session-only tool; reset it whenever the scene set changes.
    m_loopStartIndex = -1;
    m_loopEndIndex = -1;
    updateLoopUi();

    m_current = -1;
    m_selected = -1;
    m_elapsedMsInCurrentPanel = 0;
    if (m_timeline) {
        m_timeline->setScenes(m_scenes);
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
        m_timeline->setAudioLoaded(!m_audioPath.isEmpty(),
                                   m_player ? m_player->duration() : 0);
        m_timeline->setCurrentPanel(-1);
        m_timeline->setSelectedPanel(-1);
    }
    // The workspace selects a panel right after loading scenes into its
    // strip, and that selection arrives here through setSelectedPanel.
    if (m_exportButton)
        m_exportButton->setEnabled(!m_items.isEmpty());
    updateTotalLabel();
    updateTimecodeLabel();
}

// The same scenes, a different set or order of panels: a panel was added,
// removed, duplicated, pasted or moved in the workspace.
void AnimaticPage::refreshStructure()
{
    leavePreview();
    rebuildItems();
    // Loop points are flat indices; after an insert or a removal they would
    // name different panels than the ones the user marked.
    m_loopStartIndex = -1;
    m_loopEndIndex = -1;
    updateLoopUi();
    const int last = int(m_items.size()) - 1;
    m_current = qMin(m_current, last);
    m_selected = qMin(m_selected, last);
    m_elapsedMsInCurrentPanel = 0;
    if (m_timeline) {
        m_timeline->setScenes(m_scenes);
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
        m_timeline->setCurrentPanel(m_current);
        m_timeline->setSelectedPanel(m_selected);
    }
    if (m_exportButton)
        m_exportButton->setEnabled(!m_items.isEmpty());
    updateTotalLabel();
    updateTimecodeLabel();
}

// A panel's duration changed (the workspace applied it, or undid it).
void AnimaticPage::refreshTiming()
{
    // The running tick was armed with the old duration; rather than guess
    // what "mid-panel" should mean after it changes, playback stops.
    if (m_playing)
        pause();
    if (m_timeline)
        m_timeline->setScenes(m_scenes); // reflow block widths from the new value
    updateTotalLabel();
    updateTimecodeLabel();
}

void AnimaticPage::refreshThumbnails()
{
    if (m_timeline)
        m_timeline->refreshThumbnails();
}

// THE WORKSPACE SELECTED A PANEL (in the strip, by a timeline click it
// granted, by a transport button, by an undo landing somewhere). State in,
// nothing out: this never emits a selection request, which is what keeps the
// two views from answering each other. Selecting moves the playhead to that
// panel - Play starts from what is on the canvas - and, unless something is
// playing, puts the drawing back in front.
void AnimaticPage::setSelectedPanel(int flatIndex)
{
    if (flatIndex < 0 || flatIndex >= m_items.size())
        flatIndex = -1;
    m_selected = flatIndex;
    if (m_timeline)
        m_timeline->setSelectedPanel(flatIndex);
    if (flatIndex < 0)
        return;
    seekTo(flatIndex);
    if (!m_playing)
        leavePreview();
}

// The playhead, and whatever shows it: the timeline, the timecode, and the
// preview if it is up. (This was showPanel(), which also pushed a full
// flatten into the old screen's display on every call.)
void AnimaticPage::movePlayheadTo(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    m_current = index;
    m_elapsedMsInCurrentPanel = 0; // playhead restarts at the new panel's left edge
    if (m_timeline)
        m_timeline->setCurrentPanel(index);
    updateTimecodeLabel();
    showPreviewFrame(); // no-op unless the preview is showing
}

// movePlayheadTo + what a jump needs: the running tick re-armed for the new
// panel and the audio moved to match (seek only, no auto-play).
void AnimaticPage::seekTo(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    movePlayheadTo(index);
    if (m_playing)
        scheduleTick();
    if (hasAudio())
        m_player->setPosition(offsetForPanel(index));
}

// The playhead is being dragged along the ruler: LOOK at that panel. The
// drawing canvas, the selection and the strip are not touched.
void AnimaticPage::onScrubbed(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    if (m_playing)
        pause();
    seekTo(index);
    enterPreview();
}

void AnimaticPage::updateTimecodeLabel()
{
    if (!m_timecodeLabel)
        return;
    // THE PROJECT'S frame rate. This said 24 whatever the project was: the
    // hours, minutes and seconds came out right (it multiplied and divided
    // by the same 24), but the FRAMES field counted 0-23 in a 60 fps
    // project while the ruler beside it, which always used the project's
    // rate, counted 0-59.
    const int fps = m_timeline ? qMax(1, m_timeline->fps()) : 24;
    int frame = 0;
    for (int i = 0; i < m_current && i < m_items.size(); ++i)
        frame += qMax(1, m_items.at(i).panel->duration) * fps;
    frame += static_cast<int>(m_elapsedMsInCurrentPanel / (1000.0 / fps));
    const int ff = frame % fps;
    const int totalSec = frame / fps;
    const int ss = totalSec % 60;
    const int mm = (totalSec / 60) % 60;
    const int hh = totalSec / 3600;
    m_timecodeLabel->setText(QStringLiteral("%1:%2:%3:%4")
                                 .arg(hh, 2, 10, QChar('0'))
                                 .arg(mm, 2, 10, QChar('0'))
                                 .arg(ss, 2, 10, QChar('0'))
                                 .arg(ff, 2, 10, QChar('0')));
}

void AnimaticPage::updateTotalLabel()
{
    int total = 0;
    for (const Item &it : m_items)
        total += it.panel->duration;

    // Show the speed-adjusted duration, with a suffix when not at 1x.
    const int adjusted = qRound(total / static_cast<double>(m_playbackSpeed));
    QString text = QStringLiteral("Total: ") + formatTime(adjusted);
    if (m_playbackSpeed < 0.99f)
        text += QStringLiteral(" (0.5x)");
    else if (m_playbackSpeed > 1.01f)
        text += QStringLiteral(" (2x)");
    m_totalLabel->setText(text);
}

QString AnimaticPage::formatTime(int seconds) const
{
    const int m = seconds / 60;
    const int s = seconds % 60;
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// --- Playback -------------------------------------------------------------

void AnimaticPage::play()
{
    if (m_items.isEmpty())
        return;
    if (loopActive()) {
        movePlayheadTo(m_loopStartIndex); // loop playback always starts at the loop start
        if (hasAudio())
            m_player->setPosition(offsetForPanel(m_loopStartIndex));
    } else if (m_current < 0) {
        movePlayheadTo(0);
    }
    m_playing = true;
    enterPreview(); // playback is watched over the canvas, not on it
    m_elapsedMsInCurrentPanel = 0; // fresh dwell for the current panel
    m_playButton->setText(QString::fromUtf8("\xE2\x8F\xB8")); // pause glyph
    scheduleTick();
    if (m_playheadTimer)
        m_playheadTimer->start();
    if (m_timeline)
        m_timeline->setPlaying(true);
    if (hasAudio())
        m_player->play(); // resumes from current position
}

void AnimaticPage::pause()
{
    m_playing = false;
    if (m_timer)
        m_timer->stop();
    if (m_playheadTimer)
        m_playheadTimer->stop();
    if (m_timeline)
        m_timeline->setPlaying(false);
    if (m_playButton)
        m_playButton->setText(QString::fromUtf8("\xE2\x96\xB6")); // play glyph
    if (hasAudio())
        m_player->pause();
}

void AnimaticPage::togglePlay()
{
    if (m_playing)
        pause();
    else
        play();
}

void AnimaticPage::onPlayheadTick()
{
    // Advance the playhead clock. Scale by speed so the playhead still sweeps
    // the full block in real time at 0.5x / 2x (at 1x this is the spec's 50ms).
    m_elapsedMsInCurrentPanel += qRound(50.0 * m_playbackSpeed);
    if (m_timeline)
        m_timeline->updatePlayhead();
    updateTimecodeLabel();
}

void AnimaticPage::onDurationChanged(int sceneIndex, int panelIndex, int newDuration)
{
    if (sceneIndex < 0 || sceneIndex >= m_scenes.size())
        return;
    Scene *scene = m_scenes.at(sceneIndex);
    if (panelIndex < 0 || panelIndex >= scene->panels.size())
        return;
    const int bounded = qBound(1, newDuration, 30);
    if (scene->panels.at(panelIndex)->duration == bounded) {
        // A drag that ended where it began: nothing to change, nothing to
        // undo - but the clip is still drawn at its dragged width.
        if (m_timeline)
            m_timeline->setScenes(m_scenes);
        return;
    }
    // NOT applied here. A duration used to be written straight to the panel
    // with a documentChanged, which made it the one timeline edit Ctrl+Z
    // could not take back - and, with drawing on the same screen, made
    // Ctrl+Z after a timing drag undo the artist's last STROKE instead. The
    // workspace owns the undo history, so it applies this as a command and
    // calls refreshTiming() when the value lands (and again on undo/redo).
    emit durationChangeRequested(sceneIndex, panelIndex, bounded);
}

void AnimaticPage::setPlaybackSpeed(float speed)
{
    m_playbackSpeed = speed;
    updateSpeedButtons();
    updateTotalLabel();
    // Intentionally do NOT re-arm the timer: a speed change takes effect on the
    // next panel (mid-playback) or when Play resumes (while paused).
}

void AnimaticPage::updateSpeedButtons()
{
    const QString outlined = SankoTheme::themed("QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 5px 11px; font-size: 12px; }"
        "QPushButton:hover { color: %ACCENT%; border-color: %ACCENT%; }");
    const QString active = SankoTheme::themed("QPushButton { background-color: %ACCENT%; color: #0a0a0a; border: 1px solid %ACCENT%;"
        " border-radius: 4px; padding: 5px 11px; font-size: 12px; font-weight: 600; }");

    if (m_speedHalfButton)
        m_speedHalfButton->setStyleSheet(m_playbackSpeed < 0.99f ? active : outlined);
    if (m_speed1xButton)
        m_speed1xButton->setStyleSheet(
            (m_playbackSpeed >= 0.99f && m_playbackSpeed <= 1.01f) ? active : outlined);
    if (m_speed2xButton)
        m_speed2xButton->setStyleSheet(m_playbackSpeed > 1.01f ? active : outlined);
}

void AnimaticPage::scheduleTick()
{
    if (m_current < 0 || m_current >= m_items.size())
        return;
    const int duration = qMax(1, m_items.at(m_current).panel->duration);
    const int ms = qRound(duration * 1000.0 / m_playbackSpeed);
    m_timer->start(ms);
}

void AnimaticPage::advance()
{
    if (m_items.isEmpty())
        return;

    // Loop region: cycle within [start, end] indefinitely.
    if (loopActive()) {
        if (m_current >= m_loopEndIndex) {
            movePlayheadTo(m_loopStartIndex);
            if (hasAudio()) {
                m_player->setPosition(offsetForPanel(m_loopStartIndex));
                if (m_playing)
                    m_player->play(); // restart audio if it had reached its end
            }
        } else {
            movePlayheadTo(m_current + 1);
        }
        if (m_playing)
            scheduleTick();
        return;
    }

    if (m_current >= m_items.size() - 1) {
        // Last panel finished: stop, and go back to drawing. leavePreview
        // returns the playhead to the selected panel (it used to return to
        // the first panel of the film, when there was no selection to go
        // back to); the audio is stopped and parked there with it.
        const int home =
            (m_selected >= 0 && m_selected < m_items.size()) ? m_selected : 0;
        leavePreview();
        movePlayheadTo(home);
        if (hasAudio()) {
            m_player->stop();
            m_player->setPosition(offsetForPanel(home));
        }
        return;
    }
    movePlayheadTo(m_current + 1);
    if (m_playing)
        scheduleTick();
}

// First / previous / next / last are NAVIGATION: they ask the workspace to
// select that panel, exactly as a click on its clip does, and the selection
// comes back through setSelectedPanel (which moves the playhead). Stepping
// is relative to the playhead, so during playback it steps from what is
// being watched.
void AnimaticPage::goFirst()
{
    if (!m_items.isEmpty())
        emit panelSelectRequested(0);
}

void AnimaticPage::goLast()
{
    if (!m_items.isEmpty())
        emit panelSelectRequested(int(m_items.size()) - 1);
}

void AnimaticPage::goPrev()
{
    if (m_items.isEmpty())
        return;
    emit panelSelectRequested(qMax(0, (m_current < 0 ? 0 : m_current) - 1));
}

void AnimaticPage::goNext()
{
    if (m_items.isEmpty())
        return;
    emit panelSelectRequested(qMin(int(m_items.size()) - 1,
                                   (m_current < 0 ? 0 : m_current) + 1));
}

// --- Audio ----------------------------------------------------------------

// A track that can be PLAYED: the project names one and its file is there.
// (The project naming one is audioPath(); see installAudio.)
bool AnimaticPage::hasAudio() const
{
    return m_player && !m_audioPath.isEmpty() && !m_audioMissing;
}

qint64 AnimaticPage::offsetForPanel(int index) const
{
    qint64 ms = 0;
    for (int i = 0; i < index && i < m_items.size(); ++i)
        ms += static_cast<qint64>(qMax(1, m_items.at(i).panel->duration)) * 1000;
    return ms;
}

// IMPORT AND REMOVE ARE REQUESTS. They used to write the track here and emit
// documentChanged, which made them - like a duration drag before them - an
// edit Ctrl+Z could not take back, and on one screen with the drawing that
// means Ctrl+Z right after importing audio undoes the artist's last STROKE.
// The workspace owns the history: it turns the request into a command and
// calls applyAudioPath() when the command runs (and again on undo and redo).
// Both entry points - the Edit menu and the audio track's right-click menu
// - call these two functions, so there is one implementation of each.
void AnimaticPage::importAudio()
{
    if (m_playing)
        pause(); // a dialog is about to open over the film
    const QString path = m_audioPickerForTest
        ? m_audioPickerForTest()
        : QFileDialog::getOpenFileName(
              this, QStringLiteral("Import Audio"), QString(),
              QStringLiteral("Audio (*.wav *.mp3 *.aac *.m4a)"));
    if (path.isEmpty())
        return; // cancelled: nothing changes, nothing joins the history
    if (path == m_audioPath) {
        // The same file again (re-exported, perhaps): reload it. The
        // document already names it, so there is nothing to undo.
        applyAudioPath(path);
        return;
    }
    emit audioChangeRequested(path, QStringLiteral("Import Audio"));
}

void AnimaticPage::removeAudio()
{
    if (m_audioPath.isEmpty())
        return;
    emit audioChangeRequested(QString(), QStringLiteral("Remove Audio"));
}

// The audio track changes - a command running, being undone or redone.
// ANY change to the track pauses playback first, the rule timing and
// structure changes already follow: the player is about to be stopped and
// re-sourced under a film that was in step with it.
void AnimaticPage::applyAudioPath(const QString &path)
{
    if (m_playing)
        pause();
    installAudio(path);
    // Parked where the playhead is, so Play resumes in step.
    if (hasAudio() && m_current >= 0)
        m_player->setPosition(offsetForPanel(m_current));
}

// The one place the track is set, for a command and for a project load
// alike. It announces nothing about the DOCUMENT - a load adopting its
// saved path is not an edit, and an edit is announced by its command.
//
// THE PATH IS KEPT WHETHER OR NOT THE FILE IS THERE. This used to adopt a
// path only if its file existed and clear it otherwise - and save writes
// whatever path is held here. So a project opened while its audio file was
// unavailable (a drive not mounted, a folder renamed) opened clean, said
// nothing, and the next save erased the reference for good. The document's
// path and "a file is loaded" are two facts now: m_audioPath is what the
// project says, m_audioMissing is what the disk says about it.
void AnimaticPage::installAudio(const QString &path)
{
    if (m_player)
        m_player->stop();
    m_audioPath = path;
    m_audioMissing = !path.isEmpty() && !QFileInfo::exists(path);
    if (m_player)
        m_player->setSource(path.isEmpty() || m_audioMissing
                                ? QUrl()
                                : QUrl::fromLocalFile(path));
    updateAudioUi();
}

void AnimaticPage::updateAudioUi()
{
    const bool has = !m_audioPath.isEmpty();
    if (m_volumeSlider)
        m_volumeSlider->setVisible(has && !m_audioMissing);
    if (m_audioLabel) {
        // In the header row, which stays when the timeline is collapsed: a
        // missing track is announced where it cannot be folded away.
        m_audioLabel->setVisible(has);
        if (has) {
            const QString name = QFileInfo(m_audioPath).fileName();
            m_audioLabel->setText(
                m_audioMissing
                    ? name + QString::fromUtf8(" \xE2\x80\x94 missing")
                    : name);
            m_audioLabel->setStyleSheet(
                m_audioMissing
                    ? SankoTheme::themed("color: %WARNING%; font-size: 12px;")
                    : QStringLiteral("color: #888888; font-size: 12px;"));
            m_audioLabel->setToolTip(
                m_audioMissing
                    ? QStringLiteral("This project's audio file was not found "
                                     "at:\n%1\n\nRight-click the audio track, "
                                     "or use Edit > Locate Audio File, to "
                                     "point to it.")
                          .arg(QDir::toNativeSeparators(m_audioPath))
                    : QDir::toNativeSeparators(m_audioPath));
        }
    }
    if (m_timeline) {
        m_timeline->setAudioMissing(m_audioMissing);
        m_timeline->setAudioLoaded(has, m_player ? m_player->duration() : 0);
    }
    emit audioStateChanged(); // the menus enable Remove and Locate from this
}

// POINT THE TRACK AT ITS FILE AGAIN. Choosing a different path changes what
// the project says, so it is a command like Import (undo returns to the
// missing track at the old path). Choosing the SAME path - the file has
// been put back - changes nothing in the document: the file is simply
// loaded, with no command and no unsaved change.
void AnimaticPage::locateAudio()
{
    if (m_audioPath.isEmpty())
        return;
    if (m_playing)
        pause();
    const QFileInfo stored(m_audioPath);
    const QString folder = QDir(stored.absolutePath()).exists()
        ? stored.absolutePath()
        : m_fallbackFolder;
    const QString suggestion = folder.isEmpty()
        ? stored.fileName()
        : folder + QLatin1Char('/') + stored.fileName();
    const QString path = m_audioPickerForTest
        ? m_audioPickerForTest()
        : QFileDialog::getOpenFileName(
              this, QStringLiteral("Locate Audio File"), suggestion,
              QStringLiteral("Audio (*.wav *.mp3 *.aac *.m4a)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).absoluteFilePath().compare(stored.absoluteFilePath(),
                                                   Qt::CaseInsensitive) == 0) {
        applyAudioPath(m_audioPath); // re-read the disk: same document
        return;
    }
    emit audioChangeRequested(path, QStringLiteral("Locate Audio File"));
}

QString AnimaticPage::audioPath() const
{
    return m_audioPath;
}

void AnimaticPage::setAudioPath(const QString &path)
{
    // Used by project load (and New / Close, with an empty path). Silent:
    // no command, no dirty flag.
    installAudio(path);
}

// The audio track's right-click menu: the same two functions the Edit menu
// calls.
void AnimaticPage::showAudioMenu(const QPoint &globalPos)
{
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: #161616; color: #cccccc; border: 1px solid #2a2a2a; }"
        "QMenu::item { padding: 4px 18px; font-size: 11px; }"
        "QMenu::item:selected { background: #262626; color: #ffffff; }"
        "QMenu::item:disabled { color: #555555; }"));
    if (m_audioMissing) {
        // Offered only while there is something to locate.
        QAction *locate = menu.addAction(QStringLiteral("Locate Audio File..."));
        connect(locate, &QAction::triggered, this, [this] { locateAudio(); });
        menu.addSeparator();
    }
    QAction *importAction = menu.addAction(QStringLiteral("Import Audio..."));
    connect(importAction, &QAction::triggered, this, [this] { importAudio(); });
    QAction *removeAction = menu.addAction(QStringLiteral("Remove Audio"));
    removeAction->setEnabled(!m_audioPath.isEmpty());
    connect(removeAction, &QAction::triggered, this, [this] { removeAudio(); });
    if (m_audioMenuHookForTest) {
        m_audioMenuHookForTest(&menu);
        return;
    }
    menu.exec(globalPos);
}

// --- MP4 export -----------------------------------------------------------

namespace {

// The MP4 export's DELIVERY FORMAT — deliberately fixed at 1080p, a product
// decision, NOT a canvas size. Panels of any project resolution are fitted
// (KeepAspectRatio) and letterboxed into this frame; changing the project's
// canvas must never change this constant. (The 960,540 below is this
// frame's centre — 1920/2, 1080/2 — not the old canvas size, despite the
// coincidental digits.)
constexpr QSize kExportFrameSize(1920, 1080);

// Render a panel into the export frame: scaled to fit (KeepAspectRatio),
// centered, black padding around it (matches PanelDisplay behavior).
QImage renderExportFrame(const QPixmap &pixmap)
{
    QImage frame(kExportFrameSize, QImage::Format_RGB32);
    frame.fill(Qt::black);
    if (!pixmap.isNull()) {
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = pixmap.size();
        target.scale(kExportFrameSize, Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(QPoint(kExportFrameSize.width() / 2,
                            kExportFrameSize.height() / 2));
        painter.drawPixmap(r, pixmap);
    }
    return frame;
}

} // namespace

void AnimaticPage::onExportMp4()
{
    if (m_items.isEmpty())
        return;

    pause(); // don't let playback run during export

    // 1. Where to save.
    QString outPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export MP4"),
        QDir::homePath() + QStringLiteral("/animatic.mp4"),
        QStringLiteral("MP4 Video (*.mp4)"));
    if (outPath.isEmpty())
        return;
    if (!outPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
        outPath += QStringLiteral(".mp4");

    // 2. Locate ffmpeg: alongside the exe first, then the system PATH.
    QString ffmpeg;
    const QString localFfmpeg =
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe");
    if (QFile::exists(localFfmpeg))
        ffmpeg = localFfmpeg;
    else
        ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));

    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("ffmpeg not found"),
            QStringLiteral(
                "Could not find ffmpeg.exe.\n\n"
                "Place ffmpeg.exe in the same folder as SankoTV.exe, or install it "
                "somewhere on your system PATH, then try again.\n\n"
                "Windows builds are available at:\n"
                "https://www.gyan.dev/ffmpeg/builds/"));
        return;
    }

    // 3. Prepare a clean temp frames directory.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir framesDir(base + QStringLiteral("/sankotv_frames"));
    if (framesDir.exists())
        framesDir.removeRecursively();
    QDir().mkpath(framesDir.absolutePath());

    constexpr int kFps = 24;
    int totalFrames = 0;
    for (const Item &it : m_items)
        totalFrames += qMax(1, it.panel->duration) * kFps;

    // 4. Write PNG frames with a cancelable progress dialog.
    QProgressDialog progress(QStringLiteral("Preparing frames..."),
                             QStringLiteral("Cancel"), 0, totalFrames, this);
    progress.setWindowTitle(QStringLiteral("Export MP4"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoReset(false);
    progress.setAutoClose(false);
    progress.setValue(0);

    int frameIndex = 0;
    bool cancelled = false;
    for (const Item &it : m_items) {
        const QImage frame = renderExportFrame(it.panel->flattenedPixmap());
        const int copies = qMax(1, it.panel->duration) * kFps;
        for (int c = 0; c < copies; ++c) {
            if (progress.wasCanceled()) {
                cancelled = true;
                break;
            }
            const QString name =
                QStringLiteral("frame_%1.png").arg(frameIndex, 4, 10, QChar('0'));
            frame.save(framesDir.filePath(name), "PNG");
            ++frameIndex;
            progress.setValue(frameIndex);
            progress.setLabelText(QStringLiteral("Preparing frames... %1 / %2")
                                      .arg(frameIndex).arg(totalFrames));
        }
        if (cancelled)
            break;
    }

    if (cancelled) {
        framesDir.removeRecursively(); // clean up partial frames
        return;
    }

    // 5. Encode with ffmpeg (busy/marquee progress).
    progress.setLabelText(QStringLiteral("Encoding video..."));
    progress.setRange(0, 0); // marquee
    QCoreApplication::processEvents();

    QProcess proc;
    proc.setWorkingDirectory(framesDir.absolutePath());

    // Mux in the scratch audio track if one is loaded and still on disk.
    const bool includeAudio = !m_audioPath.isEmpty() && QFileInfo::exists(m_audioPath);

    QStringList args;
    args << QStringLiteral("-framerate") << QStringLiteral("24")
         << QStringLiteral("-i") << QStringLiteral("frame_%04d.png");
    if (includeAudio)
        args << QStringLiteral("-i") << m_audioPath; // second input
    args << QStringLiteral("-c:v") << QStringLiteral("libx264");
    if (includeAudio)
        args << QStringLiteral("-c:a") << QStringLiteral("aac");
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
         << QStringLiteral("-crf") << QStringLiteral("23");
    if (includeAudio)
        args << QStringLiteral("-shortest"); // end with the shorter stream
    args << QStringLiteral("-y") << outPath;

    proc.start(ffmpeg, args);

    if (!proc.waitForStarted(5000)) {
        framesDir.removeRecursively();
        QMessageBox::critical(this, QStringLiteral("Export failed"),
                              QStringLiteral("Could not start ffmpeg."));
        return;
    }

    while (!proc.waitForFinished(100)) {
        QCoreApplication::processEvents();
        if (progress.wasCanceled()) {
            proc.kill();
            proc.waitForFinished(2000);
            framesDir.removeRecursively();
            return;
        }
    }

    const QByteArray stderrOut = proc.readAllStandardError();
    const bool ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);

    // 6. Always clean up temp frames.
    framesDir.removeRecursively();
    progress.close();

    if (ok) {
        QMessageBox::information(
            this, QStringLiteral("Export complete"),
            QStringLiteral("Animatic exported to:\n%1").arg(outPath));
    } else {
        QMessageBox::critical(
            this, QStringLiteral("Export failed"),
            QStringLiteral("ffmpeg exited with code %1.\n\n%2")
                .arg(proc.exitCode())
                .arg(QString::fromLocal8Bit(stderrOut)));
    }
}

void AnimaticPage::setFps(int fps)
{
    if (m_timeline)
        m_timeline->setFps(fps);
    updateTimecodeLabel(); // its frames field counts in the project's rate
}
