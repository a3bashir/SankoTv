#include "AnimaticPage.h"
#include "SankoIcons.h"
#include "SankoTheme.h"
#include "SankoTimelineTheme.h"

#include "AnimaticTimeline.h"
#include "AudioFileCheck.h"
#include "MediaCopyProgress.h"
#include "ProjectMedia.h"
#include "StoryboardModel.h"
#include "camera/ShotCamera.h"

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
#include <QActionGroup>
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
#include <QToolButton>
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
        m_neutral = true;
        update();
    }

    // THE SHOT CAMERA (Phase 1A). Neutral = the paint below exactly as it
    // was before the camera (the neutral pins); otherwise the panel is
    // drawn through shotcam::drawShot, fitted the same way.
    void setCamera(const shotcam::CameraState &camera, bool neutral)
    {
        if (m_neutral == neutral && (neutral || m_camera == camera))
            return;
        m_camera = camera;
        m_neutral = neutral;
        update();
    }

    QString captionForTest() const { return m_caption; }
    bool hasPixmapForTest() const { return !m_pixmap.isNull(); }
    shotcam::CameraState cameraForTest() const { return m_camera; }
    bool neutralForTest() const { return m_neutral; }
    QTransform transformForTest() const
    {
        return m_pixmap.isNull()
            ? QTransform()
            : shotcam::shotTransform(m_camera, m_pixmap.size(), QRectF(rect()));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (!m_pixmap.isNull() && !m_neutral) {
            // The flatten is canvas-sized, so its size IS the canvas.
            shotcam::drawShot(painter, m_pixmap, m_pixmap.size(), m_camera,
                              QRectF(rect()), false);
        } else if (!m_pixmap.isNull()) {
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
    shotcam::CameraState m_camera;
    bool m_neutral = true;
};

namespace {

// The speed dropdown's button: the stylesheet draws the button and its
// text; the chevron (8 x 5, 10 px from the right edge, centred vertically)
// is painted here, grey at rest and the loop buttons' green under the
// pointer, because Qt's stylesheet menu-indicator drew it wrong (see
// kSpeedDropdownSheet).
class SpeedButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *e) override
    {
        QToolButton::paintEvent(e);
        const bool hot = underMouse() || isDown();
        const QPixmap chevron = SankoIcons::renderSvg(
            hot ? QStringLiteral(":/icons/timeline/speed_chevron_hover.svg")
                : QStringLiteral(":/icons/timeline/speed_chevron.svg"),
            QSize(8, 5), devicePixelRatioF());
        QPainter p(this);
        p.drawPixmap(width() - 10 - 8, (height() - 5) / 2, chevron);
    }
};

// A transport button of the redesign: 40 x 28, radius 6, the dark gradient
// (the Play button gets the purple one), an SVG glyph in its natural size.
QPushButton *transportButton(const QString &iconFile, const QSize &iconSize,
                             const QString &tip, const QString &objectName)
{
    QPushButton *button = new QPushButton;
    button->setObjectName(objectName);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(40, 28);
    button->setIconSize(iconSize);
    button->setIcon(SankoIcons::icon(
        QStringLiteral(":/icons/timeline/%1.svg").arg(iconFile), iconSize));
    button->setStyleSheet(SankoTimelineTheme::kTransportSheet);
    return button;
}


// The header row: a plain widget that tells the page when its width
// changes, so the give-way rule and the pill's place are redone.
class HeaderBar : public QWidget
{
public:
    using QWidget::QWidget;
    std::function<void()> onResize;

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        if (onResize)
            onResize();
    }
};

} // namespace

AnimaticPage::AnimaticPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: %1;")
                      .arg(SankoTimelineTheme::kPageBg.name()));

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

    // The shot camera's tick while playing (see m_cameraTimer).
    m_cameraTimer = new QTimer(this);
    m_cameraTimer->setInterval(16);
    connect(m_cameraTimer, &QTimer::timeout, this, &AnimaticPage::onCameraTick);

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
    m_audioOutput->setVolume(1.0); // 100 %: the tracks' default (Part D)
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
    // The body first: the header takes the timeline's zoom and view
    // controls, so the timeline must exist when the header is built.
    QWidget *body = createTimingStrip();
    root->addWidget(createHeader());
    root->addWidget(body, 1);
    layoutHeader();

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
        showPreviewFrame();
        emit previewShownChanged(true);
        return;
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
    emit previewShownChanged(false);
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
    updatePreviewCamera();
}

// --- The shot camera (Phase 1A) ----------------------------------------------

shotcam::CameraState AnimaticPage::cameraAtPlayhead() const
{
    if (m_current < 0 || m_current >= m_items.size())
        return shotcam::CameraState();
    const Item &it = m_items.at(m_current);
    const int fps = m_timeline ? qMax(1, m_timeline->fps()) : 24;
    const int frames = shotcam::panelFrames(*it.panel, fps);
    const int frame = qBound(
        0, shotcam::frameInPanelFromMs(liveElapsedMsInCurrentPanel(), fps), frames - 1);
    return shotcam::evaluateAtPanel(*it.scene, it.panelInScene - 1, frame, fps);
}

void AnimaticPage::updatePreviewCamera()
{
    if (!m_display || !m_previewShown || m_current < 0 || m_current >= m_items.size())
        return;
    const Item &it = m_items.at(m_current);
    // No move in the scene, or the CAMERA track hidden (Phase 1C: the eye
    // turns the camera off everywhere): the old paint, the whole panel.
    if (it.scene->camera.isEmpty() || !m_tracks.cameraVisible) {
        m_display->setCamera(shotcam::CameraState(), true); // the old paint
        return;
    }
    const shotcam::CameraState camera = cameraAtPlayhead();
    m_display->setCamera(camera, shotcam::isNeutral(camera, it.panel->canvasSize()));
}

void AnimaticPage::onCameraTick()
{
    updatePreviewCamera();
}

Panel *AnimaticPage::playheadPanel() const
{
    if (m_current < 0 || m_current >= m_items.size())
        return nullptr;
    return m_items.at(m_current).panel;
}

int AnimaticPage::playheadFrameInPanel() const
{
    if (m_current < 0 || m_current >= m_items.size())
        return 0;
    const int fps = projectFps();
    const int frames = shotcam::panelFrames(*m_items.at(m_current).panel, fps);
    return qBound(0, shotcam::frameInPanelFromMs(liveElapsedMsInCurrentPanel(), fps),
                  frames - 1);
}

int AnimaticPage::projectFps() const
{
    return m_timeline ? qMax(1, m_timeline->fps()) : 24;
}

QString AnimaticPage::playheadTimecode() const
{
    return m_timecodeLabel ? m_timecodeLabel->text() : QString();
}

void AnimaticPage::setPlayheadFrameInPanel(int flatIndex, int frameInPanel)
{
    if (flatIndex < 0 || flatIndex >= m_items.size())
        return;
    if (m_playing)
        pause();
    if (flatIndex != m_current)
        movePlayheadTo(flatIndex);
    const int fps = projectFps();
    const int frames = shotcam::panelFrames(*m_items.at(flatIndex).panel, fps);
    m_elapsedMsInCurrentPanel =
        shotcam::msForFrameInPanel(qBound(0, frameInPanel, frames - 1), fps);
    updateTimecodeLabel();
    if (m_timeline)
        m_timeline->updatePlayhead();
    updatePreviewCamera();
    emit playheadMoved();
}

void AnimaticPage::refreshCamera()
{
    updatePreviewCamera();
    if (m_timeline)
        m_timeline->refreshThumbnails(); // the SHOTS clips (and the CAMERA row) show the camera
}

void AnimaticPage::setCameraAnimate(bool on)
{
    if (m_timeline)
        m_timeline->setCameraAnimate(on);
}

bool AnimaticPage::selectedCameraKey(int *sceneIndex, int *keyIndex) const
{
    return m_timeline && m_timeline->selectedCameraKey(sceneIndex, keyIndex);
}

void AnimaticPage::setSelectedCameraKey(int sceneIndex, int keyIndex)
{
    if (m_timeline)
        m_timeline->setSelectedCameraKey(sceneIndex, keyIndex);
}

shotcam::CameraState AnimaticPage::previewCameraForTest() const
{
    return m_display ? m_display->cameraForTest() : shotcam::CameraState();
}

bool AnimaticPage::previewCameraNeutralForTest() const
{
    return !m_display || m_display->neutralForTest();
}

QTransform AnimaticPage::previewTransformForTest() const
{
    return m_display ? m_display->transformForTest() : QTransform();
}

bool AnimaticPage::cameraTimerActiveForTest() const
{
    return m_cameraTimer && m_cameraTimer->isActive();
}

QVariantMap AnimaticPage::shotCamDevrecState() const
{
    QVariantMap m;
    if (m_current < 0 || m_current >= m_items.size()) {
        m.insert(QStringLiteral("shotCam.scene"), -1);
        return m;
    }
    const Item &it = m_items.at(m_current);
    const int fps = m_timeline ? qMax(1, m_timeline->fps()) : 24;
    const int frames = shotcam::panelFrames(*it.panel, fps);
    const int frameIn = qBound(
        0, shotcam::frameInPanelFromMs(liveElapsedMsInCurrentPanel(), fps), frames - 1);
    const int sceneFrame =
        shotcam::panelStartFrame(*it.scene, it.panelInScene - 1, fps) + frameIn;
    const shotcam::CameraState camera = cameraAtPlayhead();
    bool keyAtPlayhead = false;
    for (const shotcam::CameraKey &key : it.scene->camera.keys) {
        const double f = shotcam::keyFrame(*it.scene, key, fps);
        if (f >= 0.0 && qAbs(f - sceneFrame) < 0.5)
            keyAtPlayhead = true;
    }
    m.insert(QStringLiteral("shotCam.scene"), it.sceneNumber);
    m.insert(QStringLiteral("shotCam.keys"), shotcam::liveKeyCount(*it.scene));
    m.insert(QStringLiteral("shotCam.frame"), sceneFrame);
    m.insert(QStringLiteral("shotCam.x"), camera.cx);
    m.insert(QStringLiteral("shotCam.y"), camera.cy);
    m.insert(QStringLiteral("shotCam.zoom"), camera.zoom);
    m.insert(QStringLiteral("shotCam.rotation"), camera.rotation);
    m.insert(QStringLiteral("shotCam.keyAtPlayhead"), keyAtPlayhead);
    m.insert(QStringLiteral("shotCam.soft"),
             camera.zoom > shotcam::sharpLimit(it.panel->canvasSize()) + 1e-9);
    return m;
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

void AnimaticPage::setPlayheadForTest(int flatIndex, int elapsedMs)
{
    if (flatIndex < 0 || flatIndex >= m_items.size())
        return;
    movePlayheadTo(flatIndex);
    m_elapsedMsInCurrentPanel = qMax(0, elapsedMs);
    updateTimecodeLabel();
    if (m_timeline)
        m_timeline->updatePlayhead();
    updatePreviewCamera();
    emit playheadMoved();
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
        m_collapseButton->setIcon(SankoIcons::icon(
            QStringLiteral(":/icons/timeline/%1.svg")
                .arg(collapsed ? QStringLiteral("collapse_up")
                               : QStringLiteral("collapse_down")),
            QSize(10, 9)));
        m_collapseButton->setToolTip(collapsed
                                         ? QStringLiteral("Show the timeline")
                                         : QStringLiteral("Hide the timeline"));
    }
    // The zoom and view controls go with the body (the user's decision
    // 2026-10-18: the design shows no collapsed state).
    if (m_timeline) {
        if (QWidget *z = m_timeline->zoomControls())
            z->setVisible(!collapsed);
        if (QWidget *v = m_timeline->viewControls())
            v->setVisible(!collapsed);
    }
    layoutHeader();
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
    connect(m_timeline, &AnimaticTimeline::playheadScrubbedTo,
            this, &AnimaticPage::onScrubbedTo);
    connect(m_timeline, &AnimaticTimeline::durationChanged,
            this, &AnimaticPage::onDurationChanged);
    connect(m_timeline, &AnimaticTimeline::panelMoveRequested,
            this, &AnimaticPage::panelMoveRequested);
    connect(m_timeline, &AnimaticTimeline::clipContextMenuRequested,
            this, &AnimaticPage::clipContextMenuRequested);
    connect(m_timeline, &AnimaticTimeline::audioContextMenuRequested,
            this, &AnimaticPage::showAudioMenu);
    // The CAMERA row (Phase 1C): every key edit is the workspace's.
    connect(m_timeline, &AnimaticTimeline::cameraKeyActivated,
            this, &AnimaticPage::cameraKeyActivated);
    connect(m_timeline, &AnimaticTimeline::cameraKeyMoveRequested,
            this, &AnimaticPage::cameraKeyMoveRequested);
    connect(m_timeline, &AnimaticTimeline::cameraKeyContextMenuRequested,
            this, &AnimaticPage::cameraKeyContextMenuRequested);
    connect(m_timeline, &AnimaticTimeline::cameraRowContextMenuRequested,
            this, &AnimaticPage::cameraRowContextMenuRequested);
    connect(m_timeline, &AnimaticTimeline::cameraAnimateToggled,
            this, &AnimaticPage::cameraAnimateToggled);
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
    layoutHeader(); // the loop group's width changed
}

// --- Header row -------------------------------------------------------------

// ONE row, and the only part of the section that survives a collapse: the
// fold button, the transport, the timecode and total, then loop, speed and
// volume. Collapsed, the workspace keeps exactly this - so playback is one
// click away with the timeline folded and the canvas at its tallest.
// THE HEADER ROW, as the redesign draws it at 1920 px (every x is the
// design's): collapse at 12; Fit 61, - 108, the slider 149..309, + 323 (the
// timeline's own controls, in its container); the transport pill 846..1074
// (centred on the window, placed by layoutHeader); timecode 1127, separator
// 1258, total 1287; then the speed buttons, the audio label and volume
// (hidden until a track exists); the loop buttons 1655..1735; Frames 1764,
// Seconds 1810, Snap 1856; right margin 32. The groups to the right of the
// pill are right-aligned, the groups to its left are left-aligned, and the
// stretch between them is what the pill is centred over.
QWidget *AnimaticPage::createHeader()
{
    using namespace SankoTimelineTheme;
    auto *bar = new HeaderBar;
    m_header = bar;
    bar->setObjectName(QStringLiteral("animaticHeader"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(kHeaderHeight);
    bar->setStyleSheet(QStringLiteral(
        "QWidget#animaticHeader { background-color: %1;"
        " border-bottom: 1px solid %2; }")
                           .arg(kHeaderBg.name(), kHairline.name()));
    bar->onResize = [this] { layoutHeader(); };

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 0, 32, 0);
    layout->setSpacing(0);

    // --- Left group: collapse + the timeline's zoom controls ---------------
    m_leftGroup = new QWidget;
    m_leftGroup->setObjectName(QStringLiteral("animaticHeaderLeft"));
    // The page's background sheet cascades to plain children; with the
    // controls flat (Part D) a container would show as a dark box, so each
    // one is made transparent by name (the children keep their own sheets).
    auto transparent = [](QWidget *w) {
        w->setStyleSheet(QStringLiteral("QWidget#%1 { background: transparent; }").arg(w->objectName()));
    };
    transparent(m_leftGroup);
    QHBoxLayout *left = new QHBoxLayout(m_leftGroup);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(0);

    m_collapseButton = new QPushButton;
    m_collapseButton->setObjectName(QStringLiteral("animaticCollapse"));
    m_collapseButton->setCursor(Qt::PointingHandCursor);
    m_collapseButton->setFocusPolicy(Qt::NoFocus);
    m_collapseButton->setFixedSize(26, 26);
    m_collapseButton->setIconSize(QSize(10, 9));
    m_collapseButton->setIcon(SankoIcons::icon(
        QStringLiteral(":/icons/timeline/collapse_down.svg"), QSize(10, 9)));
    m_collapseButton->setToolTip(QStringLiteral("Hide the timeline"));
    m_collapseButton->setStyleSheet(kCollapseSheet);
    connect(m_collapseButton, &QPushButton::clicked, this,
            [this] { setCollapsed(!m_collapsed); });
    left->addWidget(m_collapseButton);
    left->addSpacing(20); // the design of 2026-10-23: Fit at x 58
    if (m_timeline && m_timeline->zoomControls())
        left->addWidget(m_timeline->zoomControls());
    layout->addWidget(m_leftGroup, 0, Qt::AlignVCenter);

    layout->addStretch(1);

    // --- Right group: audio .. view controls (the design of 2026-10-23
    // moved the timecode and the total into the centre cluster, below) ----
    m_rightGroup = new QWidget;
    m_rightGroup->setObjectName(QStringLiteral("animaticHeaderRight"));
    transparent(m_rightGroup);
    QHBoxLayout *right = new QHBoxLayout(m_rightGroup);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(0);

    // --- The audio warning (Part D, 2026-10-23; the user's rule, drawn as
    // Figma proposal frame 425:390): a 16 px warning glyph on a flat 28 x 28
    // hit area, shown ONLY while the track is missing, cannot be played or
    // there is no audio output device, with the message as its tooltip; 20
    // px before the loop group. The header's audio name and volume slider
    // are gone: the audio bar names the track, the Audio track's speaker
    // opens the volume.
    m_warningGroup = new QWidget;
    m_warningGroup->setObjectName(QStringLiteral("animaticAudioWarningGroup"));
    transparent(m_warningGroup);
    QHBoxLayout *warnLayout = new QHBoxLayout(m_warningGroup);
    warnLayout->setContentsMargins(0, 0, 20, 0);
    warnLayout->setSpacing(0);
    m_audioWarning = new QToolButton;
    m_audioWarning->setObjectName(QStringLiteral("animaticAudioWarning"));
    m_audioWarning->setFixedSize(28, 28);
    m_audioWarning->setIconSize(QSize(16, 16));
    m_audioWarning->setIcon(SankoIcons::icon(QStringLiteral(":/icons/timeline/audio_warning.svg"), QSize(16, 16)));
    m_audioWarning->setAutoRaise(true);
    m_audioWarning->setFocusPolicy(Qt::NoFocus);
    m_audioWarning->setCursor(Qt::ArrowCursor);
    m_audioWarning->setStyleSheet(kWarningButtonSheet);
    warnLayout->addWidget(m_audioWarning);
    m_warningGroup->setVisible(false);
    right->addWidget(m_warningGroup, 0, Qt::AlignVCenter);

    // --- Loop: icon buttons, the clear button when a loop is set, the
    // warning while one point is set -------------------------------------------
    m_loopGroup = new QWidget;
    m_loopGroup->setObjectName(QStringLiteral("animaticLoop"));
    transparent(m_loopGroup);
    QHBoxLayout *loopLayout = new QHBoxLayout(m_loopGroup);
    loopLayout->setContentsMargins(0, 0, 0, 0); // right-anchored; the 105 px gap was to the total, now in the cluster
    loopLayout->setSpacing(6);

    m_loopWarningLabel = new QLabel(QStringLiteral("Set both loop points to activate"));
    m_loopWarningLabel->setObjectName(QStringLiteral("animaticLoopWarning"));
    m_loopWarningLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(kTimecode.name()));
    m_loopWarningLabel->setVisible(false);
    loopLayout->addWidget(m_loopWarningLabel);

    const struct { const char *file; const char *tip; const char *name; void (AnimaticPage::*slot)(); } loops[] = {
        {"loop_start", "Set loop start to the selected panel", "animaticLoopStart", &AnimaticPage::setLoopStart},
        {"loop_end", "Set loop end to the selected panel", "animaticLoopEnd", &AnimaticPage::setLoopEnd},
    };
    for (const auto &l : loops) {
        QPushButton *b = new QPushButton;
        b->setObjectName(QLatin1String(l.name));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(QLatin1String(l.tip));
        b->setFixedSize(36, 32);
        b->setIconSize(QSize(15, 14));
        b->setIcon(SankoIcons::icon(
            QStringLiteral(":/icons/timeline/%1.svg").arg(QLatin1String(l.file)), QSize(15, 14)));
        b->setStyleSheet(kLoopButtonSheet);
        connect(b, &QPushButton::clicked, this, l.slot);
        loopLayout->addWidget(b);
    }
    m_clearLoopButton = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"));
    m_clearLoopButton->setObjectName(QStringLiteral("animaticLoopClear"));
    m_clearLoopButton->setCursor(Qt::PointingHandCursor);
    m_clearLoopButton->setToolTip(QStringLiteral("Clear the loop region"));
    m_clearLoopButton->setFixedSize(36, 32);
    m_clearLoopButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #292b35; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; font-size: 13px; }"
        "QPushButton:hover { color: #e06c6c; border-color: #e06c6c; }"));
    m_clearLoopButton->setVisible(false);
    connect(m_clearLoopButton, &QPushButton::clicked, this, &AnimaticPage::clearLoop);
    loopLayout->addWidget(m_clearLoopButton);
    right->addWidget(m_loopGroup, 0, Qt::AlignVCenter);

    // --- Frames / Seconds / Snap: the timeline's view controls ------------------
    right->addSpacing(31); // the design: loop end at 1733, Frames at 1764
    if (m_timeline && m_timeline->viewControls())
        right->addWidget(m_timeline->viewControls(), 0, Qt::AlignVCenter);
    layout->addWidget(m_rightGroup, 0, Qt::AlignVCenter);

    // --- The centre cluster (the design of 2026-10-23): the timecode, a
    // separator, the transport pill, a second separator, the total and the
    // speed dropdown, one widget placed by layoutHeader so the PILL is
    // centred on the window. Gaps from the design: timecode box 132, 6,
    // separator, 68, pill 228, 68, separator, 34, total, 28, speed 60.
    m_centreGroup = new QWidget(bar);
    m_centreGroup->setObjectName(QStringLiteral("animaticCentre"));
    transparent(m_centreGroup);
    m_centreGroup->setFixedHeight(kHeaderHeight);
    QHBoxLayout *centre = new QHBoxLayout(m_centreGroup);
    centre->setContentsMargins(0, 0, 0, 0);
    centre->setSpacing(0);

    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"));
    m_timecodeLabel->setObjectName(QStringLiteral("animaticTimecode"));
    m_timecodeLabel->setFixedSize(132, kHeaderHeight);
    m_timecodeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timecodeLabel->setStyleSheet(
        QStringLiteral("color: %1; font-family: 'Courier New'; font-size: 16px;"
                       " font-weight: bold; background: transparent;")
            .arg(kTimecode.name()));
    centre->addWidget(m_timecodeLabel);
    centre->addSpacing(6);

    auto separator = [&](const QString &name) {
        QWidget *s = new QWidget;
        s->setObjectName(name);
        s->setFixedSize(1, 22);
        s->setAttribute(Qt::WA_StyledBackground, true);
        s->setStyleSheet(QStringLiteral("background-color: %1;").arg(kTimecodeSeparator.name()));
        centre->addWidget(s, 0, Qt::AlignVCenter);
        return s;
    };
    m_timecodeSeparator = separator(QStringLiteral("animaticTimecodeSeparator"));
    centre->addSpacing(68);

    // --- The transport pill: five buttons, inside the cluster ----------------
    m_transportPill = new QWidget(m_centreGroup);
    m_transportPill->setObjectName(QStringLiteral("animaticTransport"));
    transparent(m_transportPill);
    m_transportPill->setFixedSize(228, 32);
    QHBoxLayout *pill = new QHBoxLayout(m_transportPill);
    pill->setContentsMargins(0, 2, 0, 2);
    pill->setSpacing(7);
    QPushButton *first = transportButton(QStringLiteral("tr_first"), QSize(13, 13),
                                         QStringLiteral("First panel"),
                                         QStringLiteral("animaticFirst"));
    connect(first, &QPushButton::clicked, this, &AnimaticPage::goFirst);
    pill->addWidget(first);
    QPushButton *prev = transportButton(QStringLiteral("tr_prev"), QSize(10, 13),
                                        QStringLiteral("Previous panel"),
                                        QStringLiteral("animaticPrev"));
    connect(prev, &QPushButton::clicked, this, &AnimaticPage::goPrev);
    pill->addWidget(prev);
    m_playButton = transportButton(QStringLiteral("tr_play"), QSize(11, 13),
                                   QStringLiteral("Play / Pause"),
                                   QStringLiteral("animaticPlay"));
    m_playButton->setStyleSheet(kPlaySheet);
    connect(m_playButton, &QPushButton::clicked, this, &AnimaticPage::togglePlay);
    pill->addWidget(m_playButton);
    QPushButton *next = transportButton(QStringLiteral("tr_next"), QSize(10, 13),
                                        QStringLiteral("Next panel"),
                                        QStringLiteral("animaticNext"));
    connect(next, &QPushButton::clicked, this, &AnimaticPage::goNext);
    pill->addWidget(next);
    QPushButton *last = transportButton(QStringLiteral("tr_last"), QSize(13, 13),
                                        QStringLiteral("Last panel"),
                                        QStringLiteral("animaticLast"));
    connect(last, &QPushButton::clicked, this, &AnimaticPage::goLast);
    pill->addWidget(last);
    centre->addWidget(m_transportPill, 0, Qt::AlignVCenter);
    centre->addSpacing(68);
    m_totalSeparator = separator(QStringLiteral("animaticTotalSeparator"));
    centre->addSpacing(34);

    m_totalLabel = new QLabel(QStringLiteral("Total: 00:00:00:00"));
    m_totalLabel->setObjectName(QStringLiteral("animaticTotal"));
    m_totalLabel->setFixedHeight(kHeaderHeight);
    m_totalLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_totalLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; background: transparent;")
            .arg(kTotal.name()));
    centre->addWidget(m_totalLabel);
    centre->addSpacing(28);

    // --- Speed: one dropdown (the user's choice, 2026-10-23; Figma frame
    // 417:390), 60 x 28 at y 6, the current speed shown, a menu of the same
    // three speeds; the behaviour is setPlaybackSpeed's, unchanged ------------
    m_speedButton = new SpeedButton;
    m_speedButton->setObjectName(QStringLiteral("animaticSpeed"));
    m_speedButton->setCursor(Qt::PointingHandCursor);
    m_speedButton->setFocusPolicy(Qt::NoFocus);
    m_speedButton->setToolTip(QStringLiteral("Playback speed (takes effect on the next panel)"));
    m_speedButton->setFixedSize(60, 28);
    m_speedButton->setPopupMode(QToolButton::InstantPopup);
    m_speedButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_speedButton->setStyleSheet(kSpeedDropdownSheet);
    m_speedMenu = new QMenu(m_speedButton);
    m_speedMenu->setObjectName(QStringLiteral("animaticSpeedMenu"));
    m_speedMenu->setStyleSheet(kSpeedMenuSheet);
    auto *speedGroup = new QActionGroup(m_speedMenu);
    speedGroup->setExclusive(true);
    const struct { const char *text; float speed; } speeds[] = {
        {"0.5x", 0.5f}, {"1x", 1.0f}, {"2x", 2.0f}};
    for (int i = 0; i < 3; ++i) {
        QAction *a = m_speedMenu->addAction(QLatin1String(speeds[i].text));
        a->setCheckable(true);
        a->setObjectName(QStringLiteral("animaticSpeed%1").arg(QLatin1String(speeds[i].text)));
        speedGroup->addAction(a);
        const float speed = speeds[i].speed;
        connect(a, &QAction::triggered, this, [this, speed] { setPlaybackSpeed(speed); });
        m_speedActions[i] = a;
    }
    m_speedButton->setMenu(m_speedMenu);
    centre->addWidget(m_speedButton, 0, Qt::AlignVCenter);
    updateSpeedControl(); // 1x checked by default

    return bar;
}

// THE GIVE-WAY RULE (the user's, 2026-10-18) and the pill's place. The
// header's layout holds the left and right groups with a stretch between
// them; the centre cluster (timecode | pill | total | speed, since the
// design of 2026-10-23) is not in the layout: it is placed so the pill is
// centred on the window, and pushed inward only when a group would
// otherwise come within 16 px of the cluster. Nothing gives way (Part D
// took the slider and the audio name out of the header): the header's
// minimum width stops the window, so nothing overlaps and no button is
// cut off.
void AnimaticPage::layoutHeader()
{
    if (!m_header || !m_leftGroup || !m_rightGroup || !m_transportPill)
        return;
    constexpr int kPillW = 228, kPillGap = 16;
    const int available = m_header->width();

    // A changed size is seen by the widget's own layout item at once, but
    // the CONTAINER's item in the layout above keeps its cached size hint
    // (QWidgetItemV2) until the container itself reports a geometry
    // change. So every container on the path is told (updateGeometry) and
    // its layout invalidated, bottom up, before the row's layout is
    // activated and measured. (Measured 2026-10-18.)
    auto refresh = [&] {
        for (QWidget *w : {m_warningGroup, m_centreGroup, m_loopGroup,
                           m_timeline ? m_timeline->zoomControls() : nullptr,
                           m_timeline ? m_timeline->viewControls() : nullptr,
                           m_leftGroup, m_rightGroup}) {
            if (!w)
                continue;
            if (w->layout())
                w->layout()->invalidate();
            w->updateGeometry();
        }
        m_header->layout()->invalidate();
        m_header->layout()->activate();
    };
    refresh();

    // THE FLOOR (Part D, 2026-10-23): nothing gives way any more - the zoom
    // slider and the audio name are gone from the header - so the floor is
    // every group at its size, the cluster, and the 16 px it keeps from
    // each group. The window's minimum width follows it (MainWindow).
    const QMargins m = m_header->layout()->contentsMargins();
    const int floorWidth = m.left() + m_leftGroup->minimumSizeHint().width() + kPillGap
        + m_centreGroup->sizeHint().width() + kPillGap
        + m_rightGroup->minimumSizeHint().width() + m.right();
    if (m_header->minimumWidth() != floorWidth)
        m_header->setMinimumWidth(floorWidth);

    // The pill: centred on the window, kept out of both groups.
    // The centre cluster is placed so that the PILL is centred on the window;
    // the cluster is pushed inward only when it would come within 16 px of
    // a group. The pill sits `before` px into the cluster (timecode, 6,
    // separator, 68) and `after` px of cluster follow it (68, separator,
    // 34, total, 28, speed).
    const int leftRight = m_leftGroup->geometry().right() + 1;
    const int rightLeft = m_rightGroup->geometry().left();
    m_centreGroup->layout()->activate();
    const int clusterW = m_centreGroup->sizeHint().width();
    m_centreGroup->resize(clusterW, kHeaderHeight);
    const int before = m_transportPill->geometry().x();
    const int after = clusterW - before - kPillW;
    int x = (available - kPillW) / 2;
    x = qMin(x, rightLeft - kPillGap - kPillW - after);
    x = qMax(x, leftRight + kPillGap + before);
    m_centreGroup->move(x - before, 0);
    m_centreGroup->raise();
}

int AnimaticPage::headerMinimumWidthForTest() const
{
    return m_header ? m_header->minimumWidth() : 0;
}

QRect AnimaticPage::transportPillRectForTest() const
{
    // In the header's coordinates (the pill lives inside the centre cluster).
    return m_transportPill && m_header
        ? QRect(m_transportPill->mapTo(m_header, QPoint(0, 0)), m_transportPill->size())
        : QRect();
}




bool AnimaticPage::audioMutedForTest() const
{
    return m_audioOutput && m_audioOutput->isMuted();
}



// --- The tracks' lock / visibility / mute (Part B) ---------------------------

void AnimaticPage::setTracks(const TimelineTracks &tracks)
{
    m_tracks = tracks; // a load: silent
    applyTracks();
}

void AnimaticPage::setTrackLocked(TimelineTrack track, bool locked)
{
    if (m_tracks.locked(track) == locked)
        return;
    m_tracks.setLocked(track, locked);
    applyTracks();
    emit tracksChanged();
}

void AnimaticPage::setTrackVisible(TimelineTrack track, bool visible)
{
    if (m_tracks.visible(track) == visible)
        return;
    m_tracks.setVisible(track, visible);
    applyTracks();
    emit tracksChanged();
}


// Mute is the player's own flag (QAudioOutput::setMuted): the volume
// slider's value is kept, so unmuting plays at the volume it had.
void AnimaticPage::applyTracks()
{
    if (m_timeline)
        m_timeline->setTracks(m_tracks);
    updatePreviewCamera(); // the CAMERA eye reaches the preview at once
    if (m_audioOutput) {
        m_audioOutput->setVolume(audioGainForExport()); // the same gain the export uses
        m_audioOutput->setMuted(m_tracks.audioMuted());
    }
}

// THE VOLUME (Part D, the user's rules): 0..100 on a PERCEPTUAL scale - the
// slider's value goes through the power curve in audioGainForExport, so
// 50 % sounds half as loud - applied to the player at once (QAudioOutput
// takes a new volume mid-stream without a restart), 0 mutes. Not an undo
// command: tracksChanged marks the project unsaved. The AUDIO lock does
// not touch it.
void AnimaticPage::setAudioVolume(int percent)
{
    percent = qBound(0, percent, 100);
    if (m_tracks.audioVolume == percent)
        return;
    m_tracks.audioVolume = percent;
    applyTracks();
    emit tracksChanged();
}

// THE VOLUME CURVE (the user's choice, 2026-10-23): a power curve,
// gain = (v / 100) ^ (5/3), so 50 % is -10 dB (about half as loud, after
// Stevens' loudness law), 25 % -20.1 dB, 75 % -4.2 dB; 0 % is silence and
// 100 % full volume. The ONE function the player and the export read.
// (Qt's LogarithmicVolumeScale was tried first and measured -16.4 dB at
// 50 %, far quieter than half loudness.)
double AnimaticPage::audioGainForExport() const
{
    const double v = qBound(0, m_tracks.audioVolume, 100) / 100.0;
    return v <= 0.0 ? 0.0 : std::pow(v, 5.0 / 3.0);
}

QString AnimaticPage::audioWarningForTest() const
{
    return audioWarningShownForTest() ? m_audioWarning->toolTip() : QString();
}

bool AnimaticPage::audioWarningShownForTest() const
{
    return m_warningGroup && m_audioWarning && !m_warningGroup->isHidden();
}

QString AnimaticPage::volumeToolTipForTest() const
{
    return m_timeline && m_timeline->volumePopupForTest()
        ? m_timeline->volumePopupForTest()->toolTip()
        : QString();
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
    if (!m_playing)
        emit playheadMoved();
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
    // WITH THE CAMERA TOOL (the user's rule, 2026-10-26) a scrub moves the
    // playhead ON THE CANVAS: the panel under it is SELECTED (the workspace
    // decides, as for a clip click) and no preview opens, so the camera
    // frame is drawn - and Animate keys - at any frame, not only a panel's
    // start. Every other tool keeps "scrubbing looks without selecting".
    if (m_scrubShowsCanvas) {
        if (index != m_selected)
            emit panelSelectRequested(index); // -> setSelectedPanel -> seekTo
        if (index != m_current)
            seekTo(index); // the workspace did not select it: look anyway
        leavePreview();
        return;
    }
    seekTo(index);
    enterPreview();
}

void AnimaticPage::setScrubShowsCanvas(bool on)
{
    m_scrubShowsCanvas = on;
}

// FRAME-ACCURATE SCRUB (Phase 1A): the timeline reports the frame inside
// the panel as the playhead is dragged, and the playhead STAYS on it - a
// camera move is seen frame by frame, and the panel is no longer the only
// place a scrub can land. The sound is not sought here: Play seeks it to
// where the film is (filmPositionMs) before it starts, as it always has.
void AnimaticPage::onScrubbedTo(int index, int frameInPanel)
{
    if (index < 0 || index >= m_items.size())
        return;
    if (m_playing)
        pause();
    if (index != m_current)
        onScrubbed(index);
    const int fps = m_timeline ? qMax(1, m_timeline->fps()) : 24;
    const int frames = shotcam::panelFrames(*m_items.at(index).panel, fps);
    m_elapsedMsInCurrentPanel =
        shotcam::msForFrameInPanel(qBound(0, frameInPanel, frames - 1), fps);
    updateTimecodeLabel();
    if (m_timeline)
        m_timeline->updatePlayhead();
    updatePreviewCamera();
    emit playheadMoved();
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

    // Show the speed-adjusted duration as HH:MM:SS:FF in the project's rate
    // (the redesign's format, the user's decision 2026-10-18), with a suffix
    // when not at 1x.
    const int fps = m_timeline ? qMax(1, m_timeline->fps()) : 24;
    const int frames = qRound(total * fps / static_cast<double>(m_playbackSpeed));
    const int ff = frames % fps, totalSec = frames / fps;
    QString text = QStringLiteral("Total: %1:%2:%3:%4")
                       .arg(totalSec / 3600, 2, 10, QChar('0'))
                       .arg((totalSec / 60) % 60, 2, 10, QChar('0'))
                       .arg(totalSec % 60, 2, 10, QChar('0'))
                       .arg(ff, 2, 10, QChar('0'));
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
    m_playButton->setIcon(SankoIcons::icon( // the pause bars
        QStringLiteral(":/icons/timeline/tr_pause.svg"), QSize(11, 13)));
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
    if (m_cameraTimer)
        m_cameraTimer->start();
}

int AnimaticPage::liveElapsedMsInCurrentPanel() const
{
    if (m_playing && !m_waitingForAudio && m_panelClock.isValid())
        return m_panelBaseMs + qRound(m_panelClock.elapsed() * double(m_panelSpeed));
    return m_elapsedMsInCurrentPanel;
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
    if (m_cameraTimer)
        m_cameraTimer->stop();
    updatePreviewCamera(); // the frame it stopped on
    if (m_timeline)
        m_timeline->setPlaying(false);
    if (m_playButton)
        m_playButton->setIcon(SankoIcons::icon( // the play triangle
            QStringLiteral(":/icons/timeline/tr_play.svg"), QSize(11, 13)));
    if (hasAudio())
        m_player->pause();
    emit playheadMoved(); // the frame it stopped on
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
    updateSpeedControl();
    updateTotalLabel();
    // Intentionally do NOT re-arm the timer: a speed change takes effect on the
    // next panel (mid-playback) or when Play resumes (while paused).
}

void AnimaticPage::updateSpeedControl()
{
    const int which = m_playbackSpeed < 0.99f ? 0 : m_playbackSpeed > 1.01f ? 2 : 1;
    for (int i = 0; i < 3; ++i)
        if (m_speedActions[i])
            m_speedActions[i]->setChecked(i == which);
    if (m_speedButton && m_speedActions[which])
        m_speedButton->setText(m_speedActions[which]->text());
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
    if (m_tracks.audioLocked)
        return; // the menus say "(locked)"; this is the belt to their braces
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
    if (m_audioPath.isEmpty() || m_tracks.audioLocked)
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
        || m_projectFile.isEmpty() || m_tracks.audioLocked)
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
    // WHICH DEVICE, in the volume popup's tooltip: the one question "why
    // can I not hear it" needs answered first, and until 2026-10-05 nothing
    // in the app could answer it.
    const QString playingThrough = m_noAudioOutput
        ? QStringLiteral("No audio output device: Windows has nothing to play "
                         "through. Connect speakers or headphones and SankoTV "
                         "will use them.")
        : QStringLiteral("Playing through: %1").arg(audioOutputName());
    if (m_timeline)
        m_timeline->setVolumeTooltip(QStringLiteral("Audio volume\n") + playingThrough);

    // THE WARNING ICON (Part D, the user's rule): shown only while the
    // track is MISSING, CANNOT BE PLAYED, or there is NO AUDIO OUTPUT -
    // the three ways a track makes no sound - with the message the
    // header's name used to carry as its tooltip. Nothing shows when the
    // audio is fine; "linked" is the audio bar's to say. A file the player
    // cannot open outranks a missing output device: it is the one that
    // needs the artist to do something to the project.
    const QString native = QDir::toNativeSeparators(m_audioPath);
    QString warning;
    if (has && m_audioMissing)
        warning = QStringLiteral("This project's audio file was not found at:\n%1\n\n"
                                 "Right-click the audio track, or use Edit > Locate "
                                 "Audio File, to point to it.")
                      .arg(native);
    else if (has && m_audioUnplayable)
        warning = QStringLiteral("SankoTV cannot play this audio file:\n%1\n\n%2\n\n"
                                 "The film plays without sound. Right-click the audio "
                                 "track, or use Edit > Import Audio, to choose another "
                                 "file.")
                      .arg(native, m_audioProblem);
    else if (has && m_noAudioOutput)
        warning = QStringLiteral("%1\n\n%2").arg(native, playingThrough);
    if (m_audioWarning)
        m_audioWarning->setToolTip(warning);
    if (m_warningGroup)
        m_warningGroup->setVisible(!warning.isEmpty());

    if (m_timeline) {
        m_timeline->setAudioMissing(m_audioMissing);
        m_timeline->setAudioUnplayable(m_audioUnplayable);
        m_timeline->setAudioLoaded(has, m_player ? m_player->duration() : 0);
    }
    layoutHeader(); // the warning came or went
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
    if (m_audioPath.isEmpty() || m_tracks.audioLocked)
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
    // A LOCKED AUDIO track (Part B): every entry stays, disabled and saying
    // why, so the lock is seen rather than guessed at.
    if (m_tracks.audioLocked) {
        for (QAction *a : menu.actions()) {
            if (a->isSeparator())
                continue;
            a->setEnabled(false);
            a->setText(a->text() + QStringLiteral(" (locked)"));
        }
    }
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
