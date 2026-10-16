#include "AnimaticPage.h"
#include "SankoTheme.h"

#include "AnimaticTimeline.h"
#include "AudioFileCheck.h"
#include "MediaCopyProgress.h"
#include "ProjectMedia.h"
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
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
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
    // PRECISE: a coarse timer may fire up to 5 % late, 150 ms on a 3 s
    // panel, and the sound does not wait for it.
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &AnimaticPage::advance);

    // Drives the real-time playhead while playing (50ms cadence). It only
    // READS the panel's wall clock (onPlayheadTick); nothing accrues per
    // tick, so a late tick costs nothing.
    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setInterval(50);
    m_playheadTimer->setTimerType(Qt::PreciseTimer);
    connect(m_playheadTimer, &QTimer::timeout, this, &AnimaticPage::onPlayheadTick);

    // The cap on the film clock's wait for the sound to begin.
    m_audioStartCap = new QTimer(this);
    m_audioStartCap->setSingleShot(true);
    m_audioStartCap->setTimerType(Qt::PreciseTimer);
    connect(m_audioStartCap, &QTimer::timeout, this, [this] {
        if (m_waitingForAudio && m_playing)
            startFilmClock(-1); // the sound did not come: start anyway
    });

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
    // THE SEEK THE PLAYER COULD NOT TAKE YET is applied the moment the file
    // has loaded (Qt drops a setPosition made before then - measured:
    // after an import with the playhead on panel 5, the sound started from
    // the beginning of the file).
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if ((status == QMediaPlayer::LoadedMedia
                     || status == QMediaPlayer::BufferedMedia)
                    && m_audioWantedMs >= 0) {
                    const qint64 wanted = m_audioWantedMs;
                    m_audioWantedMs = -1;
                    m_player->setPosition(wanted);
                }
            });
    connect(m_player, &QMediaPlayer::positionChanged, this,
            &AnimaticPage::onAudioPosition);
    // A FILE THE PLAYER CANNOT OPEN. Nothing listened for this: the track
    // showed its name with no bar, for ever, and played nothing. Both
    // signals are needed - the status is how "invalid" arrives, the error
    // is where the reason is.
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &text) { onPlayerProblem(text); });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::InvalidMedia)
                    onPlayerProblem(m_player->errorString());
            });
    // THE OUTPUT FOLLOWS THE SYSTEM DEFAULT (see audioOutputName in the
    // header for why). Windows reports a device arriving, leaving or
    // becoming the default through this signal.
    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this,
            [this] { syncAudioOutput(); });

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // One header row that stays when the section is collapsed, then the
    // timeline. There is no display here any more: the preview is laid over
    // the drawing canvas (attachPreview).
    m_display = new PanelDisplay;
    m_display->hide();
    m_display->onDismissed = [this] { leavePreview(); };
    // It is re-parented out of this widget (onto the canvas), so it may be
    // destroyed before this object is: never keep a pointer to a dead one.
    connect(m_display, &QObject::destroyed, this, [this] { m_display = nullptr; });
    root->addWidget(createHeader());
    root->addWidget(createTimingStrip(), 1);

    // NOTHING HERE TAKES KEYBOARD FOCUS. The drawing canvas needs it (the
    // spacebar pan modifier is a key event on the canvas), and a button
    // that took focus on a click would both steal it and start answering
    // Space itself. Which keys the timeline gets is decided by where the
    // POINTER is, in the workspace - never by focus.
    const QList<QWidget *> ours = findChildren<QWidget *>();
    for (QWidget *w : ours)
        if (qobject_cast<QAbstractButton *>(w)
            || qobject_cast<QAbstractSlider *>(w))
            w->setFocusPolicy(Qt::NoFocus);

    syncAudioOutput();
    updateAudioUi(); // the device in use, named in the slider's tooltip
}

AnimaticPage::~AnimaticPage()
{
    // Not necessarily in a layout of ours: the preview belongs to the canvas
    // once attached.
    if (m_display && !m_display->parent())
        delete m_display;
}

// (The old Animatic screen's top-bar buttons stood in the workspace's bottom
// bar between the passes, built by a createLegacyActions() here. The last of
// them, Export MP4, moved to File > Export in Pass 4 - the export itself is
// src/export/ and MainWindow, not this class - and the function went with it.)

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

qint64 AnimaticPage::audioPositionForTest() const
{
    return m_player ? m_player->position() : -1;
}

bool AnimaticPage::audioPlayingForTest() const
{
    return m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
}

QString AnimaticPage::audioLabelTextForTest() const
{
    return m_audioLabel && !m_audioLabel->isHidden() ? m_audioLabel->text()
                                                     : QString();
}

QString AnimaticPage::audioLabelToolTipForTest() const
{
    return m_audioLabel ? m_audioLabel->toolTip() : QString();
}

QString AnimaticPage::volumeToolTipForTest() const
{
    return m_volumeSlider ? m_volumeSlider->toolTip() : QString();
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
    if (hasAudio()) {
        seekAudio(offsetForPanel(index));
        // Jumping back into a track the film had run past: sound again.
        if (m_playing && m_player->playbackState() != QMediaPlayer::PlayingState
            && (m_player->duration() <= 0 || offsetForPanel(index) < m_player->duration()))
            m_player->play();
    }
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
            seekAudio(offsetForPanel(m_loopStartIndex));
    } else if (m_current < 0) {
        movePlayheadTo(0);
    }
    m_playing = true;
    enterPreview(); // playback is watched over the canvas, not on it
    // NOT reset here: a Play after a pause continues the panel from where
    // it was paused (movePlayheadTo resets it for every fresh start).
    m_playButton->setText(QString::fromUtf8("\xE2\x8F\xB8")); // pause glyph
    if (m_timeline)
        m_timeline->setPlaying(true);
    // Asked again at every Play, not only when Windows reports a change:
    // the cost of being wrong is a film that plays in silence with nothing
    // to say why.
    syncAudioOutput();
    if (!hasAudio()) {
        startFilmClock(-1); // nothing to wait for
        return;
    }
    // WHERE THE SOUND MUST START: where the film is. Checked here as well
    // as when it was asked, because a seek made before the file had loaded
    // was dropped - and because after a pause the two are compared once
    // more rather than trusted.
    const qint64 wanted = filmPositionMs();
    // A track shorter than the film, with the film past its end: there is
    // no sound here to start, and asking the player to play past its end
    // would start it from the beginning.
    if (m_player->duration() > 0 && wanted >= m_player->duration()) {
        startFilmClock(-1);
        return;
    }
    if (m_audioWantedMs >= 0 || qAbs(m_player->position() - wanted) > 200)
        seekAudio(wanted);
    m_panelSpeed = m_playbackSpeed;
    m_player->setPlaybackRate(m_panelSpeed); // the sound at the picture's speed
    // THE PICTURE WAITS FOR THE SOUND. The panel timers start when the
    // player's position first moves on from where it was asked to start
    // (onAudioPosition), or when the cap says start anyway.
    m_waitingForAudio = true;
    m_audioStartFromMs = wanted;
    m_audioStartCap->start(m_audioStartCapMsForTest > 0 ? m_audioStartCapMsForTest
                                                        : 1000);
    m_player->play();
}

// The player's position has moved. While the film is waiting for its
// sound, a position that has moved on from where the sound was asked to
// start IS the sound beginning: the picture starts from the player's own
// position, so the two begin together.
void AnimaticPage::onAudioPosition(qint64 ms)
{
    if (!m_waitingForAudio || !m_playing || m_audioNeverBeginsForTest)
        return;
    // Moved on from the start, and not a stale report from before a
    // pending seek took effect (the file's beginning while 12 s in was
    // wanted, say).
    if (ms > m_audioStartFromMs && ms - m_audioStartFromMs < 2000)
        startFilmClock(ms);
}

// The picture's clock begins. With the sound's position given, the panel's
// elapsed time is taken from it; otherwise the picture starts from where
// it was. Every panel start and every resume runs the clock from here.
void AnimaticPage::startFilmClock(qint64 audioMs)
{
    m_waitingForAudio = false;
    m_audioStartCap->stop();
    if (audioMs >= 0 && m_current >= 0 && m_current < m_items.size()) {
        const qint64 duration =
            qMax(1, m_items.at(m_current).panel->duration) * qint64(1000);
        m_elapsedMsInCurrentPanel =
            int(qBound<qint64>(0, audioMs - offsetForPanel(m_current), duration - 1));
    }
    scheduleTick();
    if (m_playheadTimer)
        m_playheadTimer->start();
}

qint64 AnimaticPage::filmPositionMs() const
{
    if (m_current < 0 || m_current >= m_items.size())
        return 0;
    return offsetForPanel(m_current) + m_elapsedMsInCurrentPanel;
}

// A seek the player may not be able to take yet (it drops one made before
// the file has loaded): remembered, and applied when it loads.
void AnimaticPage::seekAudio(qint64 ms)
{
    if (!m_player)
        return;
    const QMediaPlayer::MediaStatus status = m_player->mediaStatus();
    const bool loaded = status == QMediaPlayer::LoadedMedia
        || status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::EndOfMedia
        || status == QMediaPlayer::BufferingMedia;
    if (loaded) {
        m_audioWantedMs = -1;
        m_player->setPosition(ms);
    } else {
        m_audioWantedMs = ms;
    }
}

qreal AnimaticPage::audioRateForTest() const
{
    return m_player ? m_player->playbackRate() : 0.0;
}

void AnimaticPage::pause()
{
    m_playing = false;
    // The panel's clock FREEZES where it is; Play continues from here.
    if (m_waitingForAudio) {
        m_waitingForAudio = false; // the sound never began: nothing elapsed
        m_audioStartCap->stop();
    } else if (m_panelClock.isValid()) {
        onPlayheadTick();
    }
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
    // Read the panel's wall clock, scaled by the speed the panel was started
    // at (so the playhead still sweeps the full block in real time at 0.5x
    // / 2x). It used to ADD 50 ms per tick, and a late tick was time lost.
    if (m_panelClock.isValid())
        m_elapsedMsInCurrentPanel =
            m_panelBaseMs + qRound(m_panelClock.elapsed() * double(m_panelSpeed));
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
    // The REMAINDER of the panel, at the speed in force now - which is the
    // whole panel for a fresh start (movePlayheadTo zeroes the elapsed
    // time) and what is left for a resume. The sound is given the same
    // speed, so a speed change reaches both at the next panel.
    const int duration = qMax(1, m_items.at(m_current).panel->duration);
    const int remaining = qMax(1, duration * 1000 - m_elapsedMsInCurrentPanel);
    m_panelSpeed = m_playbackSpeed;
    m_panelBaseMs = m_elapsedMsInCurrentPanel;
    m_panelClock.start();
    if (m_player && hasAudio() && !qFuzzyCompare(m_player->playbackRate(), qreal(m_panelSpeed)))
        m_player->setPlaybackRate(m_panelSpeed);
    m_timer->start(qRound(remaining / double(m_panelSpeed)));
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
                seekAudio(offsetForPanel(m_loopStartIndex));
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
            seekAudio(offsetForPanel(home));
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

namespace {
// Two paths to one file, as Windows compares names.
bool samePath(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty()
        && QDir::cleanPath(QFileInfo(a).absoluteFilePath())
               .compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()),
                        Qt::CaseInsensitive) == 0;
}
} // namespace

// A track that can be PLAYED: the project names one and its file is there.
// (The project naming one is audioPath(); see installAudio.)
bool AnimaticPage::hasAudio() const
{
    return m_player && !m_audioPath.isEmpty() && !m_audioMissing
        && !m_audioUnplayable;
}

// --- Where the sound goes -------------------------------------------------

QString AnimaticPage::audioOutputName() const
{
    return m_noAudioOutput || !m_audioOutput ? QString()
                                             : m_audioOutput->device().description();
}

// Put the output on the system's default device, whatever that is NOW.
// QAudioOutput does not do this itself: it takes the default when it is
// constructed and keeps it, so an app started before the speaker was
// connected plays into the old device - successfully, as far as Qt and
// Windows are concerned - and is silent. No default at all (nothing
// connected, or the audio service not running) is a state of its own,
// which the track says.
void AnimaticPage::syncAudioOutput()
{
    if (!m_audioOutput)
        return;
    const QAudioDevice wanted = m_defaultOutputForTest
        ? m_defaultOutputForTest()
        : QMediaDevices::defaultAudioOutput();
    const bool none = wanted.isNull();
    bool changed = none != m_noAudioOutput;
    if (!none && m_audioOutput->device() != wanted) {
        m_audioOutput->setDevice(wanted);
        changed = true;
    }
    m_noAudioOutput = none;
    if (changed)
        updateAudioUi();
}

// The player could not open the track's file. Only that: an error after
// the file has loaded (it has a length) is not a statement about the file,
// and is not allowed to take a working track away.
void AnimaticPage::onPlayerProblem(const QString &reason)
{
    if (!m_player || m_audioPath.isEmpty() || m_audioMissing)
        return; // clearing the source, or no file to have a problem with
    // A report that belongs to a source already replaced: setSource has
    // reset the player's error by the time it arrives.
    if (m_player->error() == QMediaPlayer::NoError
        && m_player->mediaStatus() != QMediaPlayer::InvalidMedia)
        return;
    if (m_player->mediaStatus() != QMediaPlayer::InvalidMedia
        && m_player->duration() > 0)
        return;
    const QString why = reason.simplified().isEmpty()
        ? QStringLiteral("It could not be opened as audio.")
        : reason.simplified();
    if (m_audioUnplayable && m_audioProblem == why)
        return;
    m_audioUnplayable = true;
    m_audioProblem = why;
    updateAudioUi();
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
    // CAN IT BE READ AT ALL? Asked before anything is copied or changed.
    if (!readableAsAudio(path, QStringLiteral("Import Audio")))
        return;
    // INTO THE PROJECT FIRST. What the track will name is the project's own
    // copy, so the file picked can be moved, renamed or unplugged a moment
    // later. (A project with no file yet has nowhere to copy to: the track
    // names the file where it is, and the first save brings it in.)
    QString file = path;
    if (!m_projectFile.isEmpty()
        && !adoptIntoProject(path, QStringLiteral("Import Audio"), &file))
        return; // failed (and said so) or cancelled: nothing changes
    if (!m_audioLinked && samePath(file, m_audioPath)) {
        // The file the track already plays, picked again: reload it. The
        // document already names it, so there is nothing to undo. (A file
        // of the same NAME with different contents - a re-export - is not
        // this: it arrives as "name (2)", a new track, and undo returns to
        // the earlier one.)
        applyAudioPath(m_audioPath, false);
        return;
    }
    emit audioChangeRequested(file, false, QStringLiteral("Import Audio"));
}

void AnimaticPage::removeAudio()
{
    if (m_audioPath.isEmpty())
        return;
    // The file stays in the project's folder: undo needs it, and nothing in
    // that folder is ever deleted by the app.
    emit audioChangeRequested(QString(), false, QStringLiteral("Remove Audio"));
}

// A LINKED track becomes the project's own: the deliberate step an old
// project's absolute path is converted by (a save never does it). A command
// like Import - undo returns to the linked track, and the copy stays.
void AnimaticPage::copyAudioIntoProject()
{
    if (m_audioPath.isEmpty() || !m_audioLinked || m_audioMissing
        || m_projectFile.isEmpty())
        return;
    if (m_playing)
        pause();
    QString file;
    if (!adoptIntoProject(m_audioPath, QStringLiteral("Copy Audio Into Project"),
                          &file))
        return;
    emit audioChangeRequested(file, false,
                              QStringLiteral("Copy Audio Into Project"));
}

// A picked file that SankoTV cannot read as audio is REFUSED HERE, in words,
// before it is copied or the track is touched. It used to be accepted: the
// track then showed its name and no bar, played nothing, and only an MP4
// export ever said why. (A file that can be read but is decoded WRONGLY is
// not caught - nothing here can hear.)
bool AnimaticPage::readableAsAudio(const QString &picked, const QString &title)
{
    const AudioFileCheck::Result read = AudioFileCheck::check(picked);
    if (read.ok)
        return true;
    QMessageBox::warning(
        this, title,
        QStringLiteral("SankoTV cannot read this file as audio, so it was NOT "
                       "imported.\n\n%1\n\n%2\n\nNothing has been copied, and "
                       "the project's audio track has not been changed.")
            .arg(QDir::toNativeSeparators(picked), read.reason));
    return false;
}

// The copy, with its progress window and its one way of saying no. `title`
// is the command the artist chose, so the message is headed by what they
// were doing.
bool AnimaticPage::adoptIntoProject(const QString &picked, const QString &title,
                                    QString *file)
{
    ProjectMedia::Adoption copy;
    {
        MediaCopyProgress progress(
            this, QStringLiteral("Copying %1 into the project...")
                      .arg(QFileInfo(picked).fileName()));
        copy = ProjectMedia::adopt(picked, m_projectFile, progress.callback());
    }
    if (copy.ok()) {
        *file = copy.file;
        return true;
    }
    if (copy.outcome == ProjectMedia::Adoption::Failed)
        QMessageBox::warning(
            this, title,
            QStringLiteral("The audio file was NOT copied into the project, "
                           "and the project's audio track has not been "
                           "changed.\n\n%1\n\n%2")
                .arg(copy.failedFile, copy.reason));
    return false;
}

// The audio track changes - a command running, being undone or redone.
// ANY change to the track pauses playback first, the rule timing and
// structure changes already follow: the player is about to be stopped and
// re-sourced under a film that was in step with it.
void AnimaticPage::applyAudioPath(const QString &path, bool linked)
{
    if (m_playing)
        pause();
    installAudio(path, linked);
    // Parked where the film is, so Play resumes in step. The file has not
    // loaded yet, so this is the seek Qt used to drop: seekAudio keeps it.
    if (hasAudio() && m_current >= 0)
        seekAudio(filmPositionMs());
}

// A Save As has just made the new project its own copy of the track, and
// that is the file this project plays from now on. Not an edit - the
// document says what it said - and when the track is already there (every
// ordinary save) nothing is touched, so Ctrl+S never interrupts playback.
void AnimaticPage::relocateAudio(const QString &path)
{
    if (path.isEmpty() || m_audioPath.isEmpty() || m_audioLinked
        || samePath(path, m_audioPath))
        return;
    applyAudioPath(path, false);
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
void AnimaticPage::installAudio(const QString &path, bool linked)
{
    if (m_player)
        m_player->stop();
    m_audioPath = path;
    m_audioLinked = linked && !path.isEmpty();
    m_audioUnplayable = false; // a new file: the player has not judged it yet
    m_audioProblem.clear();
    m_audioWantedMs = -1; // a seek meant for the file before
    m_audioMissing = !path.isEmpty() && !QFileInfo::exists(path);
    if (m_player)
        m_player->setSource(path.isEmpty() || m_audioMissing
                                ? QUrl()
                                : QUrl::fromLocalFile(path));
    syncAudioOutput(); // a new track: make sure of where it will be heard
    updateAudioUi();
}

void AnimaticPage::updateAudioUi()
{
    const bool has = !m_audioPath.isEmpty();
    // WHICH DEVICE, in the volume slider's tooltip: the one question "why
    // can I not hear it" needs answered first, and until 2026-10-05 nothing
    // in the app could answer it.
    const QString playingThrough = m_noAudioOutput
        ? QStringLiteral("No audio output device: Windows has nothing to play "
                         "through. Connect speakers or headphones and SankoTV "
                         "will use them.")
        : QStringLiteral("Playing through: %1").arg(audioOutputName());
    if (m_volumeSlider) {
        m_volumeSlider->setVisible(has && !m_audioMissing && !m_audioUnplayable);
        m_volumeSlider->setToolTip(QStringLiteral("Audio volume\n") + playingThrough);
    }
    if (m_audioLabel && has && !m_audioMissing && (m_audioUnplayable || m_noAudioOutput)) {
        // THE TWO WAYS A TRACK THAT IS THERE MAKES NO SOUND, said on the
        // label - which stays when the timeline is collapsed - in the
        // warning colour, because silence with no explanation is the
        // failure this exists to end. A file the player cannot open
        // outranks a missing output device: it is the one that needs the
        // artist to do something to the project.
        const QString name = QFileInfo(m_audioPath).fileName();
        m_audioLabel->setVisible(true);
        m_audioLabel->setStyleSheet(
            SankoTheme::themed("color: %WARNING%; font-size: 12px;"));
        if (m_audioUnplayable) {
            m_audioLabel->setText(name + QString::fromUtf8(" \xE2\x80\x94 cannot be played"));
            m_audioLabel->setToolTip(
                QStringLiteral("SankoTV cannot play this audio file:\n%1\n\n%2\n\n"
                               "The film plays without sound. Right-click the "
                               "audio track, or use Edit > Import Audio, to "
                               "choose another file.")
                    .arg(QDir::toNativeSeparators(m_audioPath), m_audioProblem));
        } else {
            m_audioLabel->setText(
                name
                + (m_audioLinked ? QString::fromUtf8(" \xE2\x80\x94 linked") : QString())
                + QString::fromUtf8(" \xE2\x80\x94 no audio output"));
            m_audioLabel->setToolTip(
                QStringLiteral("%1\n\n%2").arg(QDir::toNativeSeparators(m_audioPath),
                                               playingThrough));
        }
    } else if (m_audioLabel) {
        // In the header row, which stays when the timeline is collapsed: a
        // missing track is announced where it cannot be folded away.
        m_audioLabel->setVisible(has);
        if (has) {
            const QString name = QFileInfo(m_audioPath).fileName();
            // LINKED IS SAID ON THE LABEL, not left to the tooltip (the
            // user's decision, 2026-10-04): a linked track is the one that
            // loses its sound when the project folder moves, and that must
            // be visible without hovering. Missing outranks it - a track
            // that is not there at all is the more urgent thing to say.
            m_audioLabel->setText(
                m_audioMissing
                    ? name + QString::fromUtf8(" \xE2\x80\x94 missing")
                    : m_audioLinked
                          ? name + QString::fromUtf8(" \xE2\x80\x94 linked")
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
                    : m_audioLinked
                          ? QStringLiteral(
                                "This audio file is outside the project's "
                                "folder:\n%1\n\nThe project will lose its "
                                "sound if it is moved or copied without this "
                                "file. Right-click the audio track, or use "
                                "Edit > Copy Audio Into Project, to make the "
                                "project carry it.")
                                .arg(QDir::toNativeSeparators(m_audioPath))
                          : QDir::toNativeSeparators(m_audioPath));
        }
    }
    if (m_timeline) {
        m_timeline->setAudioMissing(m_audioMissing);
        m_timeline->setAudioUnplayable(m_audioUnplayable);
        m_timeline->setAudioLoaded(has, m_player ? m_player->duration() : 0);
    }
    emit audioStateChanged(); // the menus enable Remove and Locate from this
}

// POINT THE TRACK AT ITS FILE AGAIN. The file chosen is COPIED INTO THE
// PROJECT, like an import - so Locate on an old project's linked track is
// also what makes that project carry its sound (the user's decision,
// 2026-10-04: a linked track converts by Copy Audio Into Project or by
// this, never by a save).
//
// For the project's OWN track the copy usually lands under the very name
// the project already says - the file was missing, so the name is free; or
// the artist put it back and picked it where it belongs. Then nothing in
// the document changes: the file is simply loaded, with no command and no
// unsaved change. Anything else - another name, or a linked track - changes
// what the project says, so it is a command like Import (undo returns to
// the missing track at the old path).
void AnimaticPage::locateAudio()
{
    if (m_audioPath.isEmpty())
        return;
    if (m_playing)
        pause();
    const QFileInfo stored(m_audioPath);
    const QString folder = QDir(stored.absolutePath()).exists()
        ? stored.absolutePath()
        : QFileInfo(m_projectFile).absolutePath();
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
    if (!readableAsAudio(path, QStringLiteral("Locate Audio File")))
        return;
    if (m_projectFile.isEmpty()) {
        // No project file, so nowhere to copy to: as it was before the
        // media policy. The same path again just re-reads the disk.
        if (samePath(path, m_audioPath))
            applyAudioPath(m_audioPath, m_audioLinked);
        else
            emit audioChangeRequested(path, false,
                                      QStringLiteral("Locate Audio File"));
        return;
    }
    QString file;
    if (!adoptIntoProject(path, QStringLiteral("Locate Audio File"), &file))
        return;
    if (!m_audioLinked && samePath(file, m_audioPath)) {
        applyAudioPath(m_audioPath, false); // re-read the disk: same document
        return;
    }
    emit audioChangeRequested(file, false, QStringLiteral("Locate Audio File"));
}

QString AnimaticPage::audioPath() const
{
    return m_audioPath;
}

void AnimaticPage::setAudioPath(const QString &path, bool linked)
{
    // Used by project load (and New / Close, with an empty path). Silent:
    // no command, no dirty flag.
    installAudio(path, linked);
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
    } else if (m_audioLinked && !m_projectFile.isEmpty()) {
        // Offered only for a linked track whose file is there to copy.
        QAction *copyIn =
            menu.addAction(QStringLiteral("Copy Audio Into Project"));
        connect(copyIn, &QAction::triggered, this,
                [this] { copyAudioIntoProject(); });
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

void AnimaticPage::setFps(int fps)
{
    if (m_timeline)
        m_timeline->setFps(fps);
    updateTimecodeLabel(); // its frames field counts in the project's rate
}
