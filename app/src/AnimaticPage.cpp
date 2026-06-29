#include "AnimaticPage.h"

#include "AnimaticTimeline.h"
#include "StoryboardModel.h"

#include <QAudioOutput>
#include <QCoreApplication>
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

// Letterboxed display of a single panel pixmap (16:9, black bars).
class PanelDisplay : public QWidget
{
public:
    explicit PanelDisplay(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(220);
    }

    void setPixmap(const QPixmap &pixmap)
    {
        m_pixmap = pixmap; // implicitly shared — no deep copy
        update();
    }

    void clear()
    {
        m_pixmap = QPixmap();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (m_pixmap.isNull())
            return;
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = m_pixmap.size();
        target.scale(size(), Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(rect().center());
        painter.drawPixmap(r, m_pixmap);
    }

private:
    QPixmap m_pixmap;
};

namespace {

QPushButton *transportButton(const QString &glyph, const QString &tip)
{
    QPushButton *button = new QPushButton(glyph);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(44, 36);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: #1c1c1c; color: #ffffff; border: 1px solid #2a2a2a;"
        "  border-radius: 5px; font-size: 16px;"
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
        if (m_timeline)
            m_timeline->setAudioLoaded(hasAudio(), d);
    });

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(createTopBar());
    root->addWidget(createDisplay(), 1);
    root->addWidget(createTimingStrip());
    root->addWidget(createControls());
}

// --- Top bar --------------------------------------------------------------

QWidget *AnimaticPage::createTopBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(60);
    bar->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);

    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90"));
    back->setCursor(Qt::PointingHandCursor);
    back->setToolTip(QStringLiteral("Back to Storyboard"));
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: none; font-size: 20px;"
        " padding: 4px 8px; } QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, [this] {
        pause();
        emit backRequested();
    });
    layout->addWidget(back);

    layout->addStretch(1);

    QLabel *title = new QLabel(QStringLiteral("Animatic"));
    title->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 15px; font-weight: 600;"));
    layout->addWidget(title);

    layout->addStretch(1);

    const QString outlined = QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 8px 14px; font-size: 13px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }");

    QPushButton *importAudio = new QPushButton(QStringLiteral("Import Audio"));
    importAudio->setCursor(Qt::PointingHandCursor);
    importAudio->setToolTip(QStringLiteral("Import a scratch audio track (WAV/MP3)"));
    importAudio->setStyleSheet(outlined);
    connect(importAudio, &QPushButton::clicked, this, &AnimaticPage::onImportAudio);
    layout->addWidget(importAudio);

    m_removeAudioButton = new QPushButton(QStringLiteral("Remove Audio"));
    m_removeAudioButton->setCursor(Qt::PointingHandCursor);
    m_removeAudioButton->setStyleSheet(outlined);
    m_removeAudioButton->setVisible(false);
    connect(m_removeAudioButton, &QPushButton::clicked, this, &AnimaticPage::onRemoveAudio);
    layout->addWidget(m_removeAudioButton);

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

    m_exportButton = new QPushButton(QStringLiteral("Export MP4"));
    m_exportButton->setCursor(Qt::PointingHandCursor);
    m_exportButton->setEnabled(false); // enabled once panels are loaded
    m_exportButton->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #f5a623; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 8px 16px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background-color: #ffb733; }"
        "QPushButton:disabled { background-color: #1c1c1c; color: #555555;"
        " border: 1px solid #2a2a2a; }"));
    connect(m_exportButton, &QPushButton::clicked, this, &AnimaticPage::onExportMp4);
    layout->addWidget(m_exportButton);

    return bar;
}

// --- Main display ---------------------------------------------------------

QWidget *AnimaticPage::createDisplay()
{
    QWidget *area = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(area);
    layout->setContentsMargins(24, 18, 24, 12);
    layout->setSpacing(10);

    m_display = new PanelDisplay;
    layout->addWidget(m_display, 1);

    m_caption = new QLabel;
    m_caption->setAlignment(Qt::AlignCenter);
    m_caption->setStyleSheet(QStringLiteral("color: #888888; font-size: 13px;"));
    layout->addWidget(m_caption);

    return area;
}

// --- Timing strip ---------------------------------------------------------

QWidget *AnimaticPage::createTimingStrip()
{
    QWidget *wrap = new QWidget;
    wrap->setAttribute(Qt::WA_StyledBackground, true);
    wrap->setStyleSheet(QStringLiteral(
        "background-color: #0d0d0d; border-top: 1px solid #1f1f1f;"));
    QVBoxLayout *wrapLayout = new QVBoxLayout(wrap);
    wrapLayout->setContentsMargins(0, 0, 0, 0);
    wrapLayout->setSpacing(0);

    // Professional NLE timeline (zoom toolbar + multi-track canvas). Loop
    // controls now live in the bottom playback bar (see createControls()).
    m_timeline = new AnimaticTimeline;
    m_timeline->setHost(this);
    connect(m_timeline, &AnimaticTimeline::panelSeekRequested,
            this, &AnimaticPage::jumpTo);
    connect(m_timeline, &AnimaticTimeline::durationChanged,
            this, &AnimaticPage::onDurationChanged);
    wrapLayout->addWidget(m_timeline);

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

// --- Controls -------------------------------------------------------------

QWidget *AnimaticPage::createControls()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(56);
    bar->setStyleSheet(QStringLiteral("background-color: #0d0d0d; border-top: 1px solid #1f1f1f;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    // --- LEFT GROUP: loop controls (compact) -----------------------------
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
    m_loopWarningLabel->setStyleSheet(QStringLiteral("color: #f5a623; font-size: 11px;"));
    m_loopWarningLabel->setVisible(false);
    loopLayout->addWidget(m_loopWarningLabel);

    layout->addWidget(loopGroup);
    layout->addStretch(1);

    // --- CENTRE GROUP: transport (icons only) ----------------------------
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

    layout->addStretch(1);

    // --- RIGHT GROUP: speed, timecode, total -----------------------------
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

    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"));
    m_timecodeLabel->setStyleSheet(QStringLiteral(
        "color: #f5a623; font-family: 'Courier New'; font-size: 13px;"));
    layout->addWidget(m_timecodeLabel);

    m_totalLabel = new QLabel(QStringLiteral("Total: 0:00"));
    m_totalLabel->setStyleSheet(QStringLiteral("color: #aaaaaa; font-size: 13px;"));
    layout->addWidget(m_totalLabel);

    return bar;
}

// --- Data / display -------------------------------------------------------

void AnimaticPage::loadScenes(const QVector<Scene *> &scenes)
{
    pause();
    m_items.clear();
    m_scenes = scenes;

    // Loop is a session-only tool; reset it whenever the scene set changes.
    m_loopStartIndex = -1;
    m_loopEndIndex = -1;
    updateLoopUi();

    for (Scene *scene : scenes) {
        for (int pi = 0; pi < scene->panels.size(); ++pi)
            m_items.append({scene, scene->panels.at(pi), scene->number, pi + 1});
    }

    if (m_timeline) {
        m_timeline->setScenes(m_scenes);
        m_timeline->setLoopRegion(m_loopStartIndex, m_loopEndIndex);
        m_timeline->setAudioLoaded(hasAudio(), m_player ? m_player->duration() : 0);
    }
    m_current = -1;

    if (!m_items.isEmpty()) {
        showPanel(0);
    } else {
        m_display->clear();
        m_caption->clear();
    }
    if (m_exportButton)
        m_exportButton->setEnabled(!m_items.isEmpty());
    updateTotalLabel();
}

void AnimaticPage::showPanel(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    m_current = index;
    m_elapsedMsInCurrentPanel = 0; // playhead restarts at the new panel's left edge
    const Item &it = m_items.at(index);
    m_display->setPixmap(it.panel->pixmap);
    m_caption->setText(QString::fromUtf8("Scene %1 \xE2\x80\x94 Panel %2")
                           .arg(it.sceneNumber)
                           .arg(it.panelInScene));
    if (m_timeline)
        m_timeline->setCurrentPanel(index);
    updateTimecodeLabel();
}

void AnimaticPage::updateTimecodeLabel()
{
    if (!m_timecodeLabel)
        return;
    constexpr int kFps = 24;
    int frame = 0;
    for (int i = 0; i < m_current && i < m_items.size(); ++i)
        frame += qMax(1, m_items.at(i).panel->duration) * kFps;
    frame += static_cast<int>(m_elapsedMsInCurrentPanel / (1000.0 / kFps));
    const int ff = frame % kFps;
    const int totalSec = frame / kFps;
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
        showPanel(m_loopStartIndex); // loop playback always starts at the loop start
        if (hasAudio())
            m_player->setPosition(offsetForPanel(m_loopStartIndex));
    } else if (m_current < 0) {
        showPanel(0);
    }
    m_playing = true;
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
    scene->panels.at(panelIndex)->duration = qBound(1, newDuration, 30);

    if (m_timeline)
        m_timeline->setScenes(m_scenes); // reflow block widths from the new value
    updateTotalLabel();
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
    const QString outlined = QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; padding: 5px 11px; font-size: 12px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }");
    const QString active = QStringLiteral(
        "QPushButton { background-color: #f5a623; color: #0a0a0a; border: 1px solid #f5a623;"
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
            showPanel(m_loopStartIndex);
            if (hasAudio()) {
                m_player->setPosition(offsetForPanel(m_loopStartIndex));
                if (m_playing)
                    m_player->play(); // restart audio if it had reached its end
            }
        } else {
            showPanel(m_current + 1);
        }
        if (m_playing)
            scheduleTick();
        return;
    }

    if (m_current >= m_items.size() - 1) {
        // Last panel finished: stop and return to the first.
        pause();
        showPanel(0);
        if (hasAudio()) {
            m_player->stop();
            m_player->setPosition(0);
        }
        return;
    }
    showPanel(m_current + 1);
    if (m_playing)
        scheduleTick();
}

void AnimaticPage::goFirst()
{
    showPanel(0);
    if (m_playing)
        scheduleTick();
    if (hasAudio())
        m_player->setPosition(0);
}

void AnimaticPage::goLast()
{
    showPanel(m_items.size() - 1);
    if (m_playing)
        scheduleTick();
    if (hasAudio())
        m_player->setPosition(offsetForPanel(m_items.size() - 1));
}

void AnimaticPage::goPrev()
{
    if (m_items.isEmpty())
        return;
    const int target = qMax(0, (m_current < 0 ? 0 : m_current) - 1);
    jumpTo(target);
}

void AnimaticPage::goNext()
{
    if (m_items.isEmpty())
        return;
    const int target = qMin(m_items.size() - 1, (m_current < 0 ? 0 : m_current) + 1);
    jumpTo(target);
}

void AnimaticPage::jumpTo(int index)
{
    showPanel(index);
    if (m_playing)
        scheduleTick();
    if (hasAudio())
        m_player->setPosition(offsetForPanel(index)); // seek only, no auto-play
}

// --- Audio ----------------------------------------------------------------

bool AnimaticPage::hasAudio() const
{
    return m_player && !m_audioPath.isEmpty();
}

qint64 AnimaticPage::offsetForPanel(int index) const
{
    qint64 ms = 0;
    for (int i = 0; i < index && i < m_items.size(); ++i)
        ms += static_cast<qint64>(qMax(1, m_items.at(i).panel->duration)) * 1000;
    return ms;
}

void AnimaticPage::onImportAudio()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Audio"), QString(),
        QStringLiteral("Audio (*.wav *.mp3 *.aac *.m4a)"));
    if (path.isEmpty())
        return;
    loadAudioFile(path); // does not auto-play
}

void AnimaticPage::loadAudioFile(const QString &path)
{
    m_audioPath = path;
    m_player->setSource(QUrl::fromLocalFile(path));
    updateAudioUi();
}

void AnimaticPage::onRemoveAudio()
{
    m_audioPath.clear();
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
    updateAudioUi();
}

void AnimaticPage::updateAudioUi()
{
    const bool has = !m_audioPath.isEmpty();
    if (m_removeAudioButton)
        m_removeAudioButton->setVisible(has);
    if (m_volumeSlider)
        m_volumeSlider->setVisible(has);
    if (m_audioLabel) {
        m_audioLabel->setVisible(has);
        if (has)
            m_audioLabel->setText(QFileInfo(m_audioPath).fileName());
    }
    if (m_timeline)
        m_timeline->setAudioLoaded(has, m_player ? m_player->duration() : 0);
}

QString AnimaticPage::audioPath() const
{
    return m_audioPath;
}

void AnimaticPage::setAudioPath(const QString &path)
{
    // Used by project load. Adopt the path only if the file still exists.
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        loadAudioFile(path);
    } else {
        m_audioPath.clear();
        if (m_player) {
            m_player->stop();
            m_player->setSource(QUrl());
        }
        updateAudioUi();
    }
}

// --- MP4 export -----------------------------------------------------------

namespace {

// Render a panel into a 1920x1080 frame: scaled to fit (KeepAspectRatio),
// centered, black padding around it (matches PanelDisplay behavior).
QImage renderExportFrame(const QPixmap &pixmap)
{
    QImage frame(1920, 1080, QImage::Format_RGB32);
    frame.fill(Qt::black);
    if (!pixmap.isNull()) {
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = pixmap.size();
        target.scale(1920, 1080, Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(QPoint(960, 540));
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
        const QImage frame = renderExportFrame(it.panel->pixmap);
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
