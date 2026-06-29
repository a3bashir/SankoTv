#include "AnimaticPage.h"

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

constexpr int kThumbW = 80;
constexpr int kThumbH = 45;

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

    // Scratch audio playback (created up front so the volume slider can bind).
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(0.8);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);

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
    QScrollArea *strip = new QScrollArea;
    strip->setWidgetResizable(true);
    strip->setFixedHeight(100);
    strip->setFrameShape(QFrame::NoFrame);
    strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    strip->setStyleSheet(QStringLiteral(
        "QScrollArea { background-color: #111111; border-top: 1px solid #1f1f1f; }"));

    QWidget *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    m_stripLayout = new QHBoxLayout(container);
    m_stripLayout->setContentsMargins(12, 6, 12, 6);
    m_stripLayout->setSpacing(10);
    m_stripLayout->addStretch(1);

    strip->setWidget(container);
    return strip;
}

void AnimaticPage::rebuildTimingStrip()
{
    m_thumbs.clear();
    m_spins.clear();
    while (QLayoutItem *item = m_stripLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    const QString spinStyle = QStringLiteral(
        "QSpinBox { background-color: #1a1a1a; color: #ffffff; border: 1px solid #2a2a2a;"
        " border-radius: 3px; padding: 1px 3px; font-size: 11px; }"
        "QSpinBox::up-button, QSpinBox::down-button { width: 12px; }");

    for (int i = 0; i < m_items.size(); ++i) {
        Panel *panel = m_items.at(i).panel;

        QWidget *cell = new QWidget;
        QVBoxLayout *cl = new QVBoxLayout(cell);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(4);

        QLabel *thumb = new QLabel;
        thumb->setObjectName(QStringLiteral("animaticThumb"));
        thumb->setProperty("itemIndex", i);
        thumb->setFixedSize(kThumbW, kThumbH);
        thumb->setScaledContents(true);
        thumb->setCursor(Qt::PointingHandCursor);
        thumb->setPixmap(panel->pixmap.scaled(kThumbW, kThumbH,
                                              Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        thumb->installEventFilter(this);
        cl->addWidget(thumb, 0, Qt::AlignHCenter);

        QSpinBox *spin = new QSpinBox;
        spin->setRange(1, 30);
        spin->setValue(panel->duration);
        spin->setSuffix(QStringLiteral("s"));
        spin->setFixedWidth(58);
        spin->setStyleSheet(spinStyle);
        connect(spin, &QSpinBox::valueChanged, this, [this, panel](int v) {
            panel->duration = v; // takes effect after the current panel finishes
            updateTotalLabel();
        });
        cl->addWidget(spin, 0, Qt::AlignHCenter);

        m_stripLayout->addWidget(cell);
        m_thumbs.append(thumb);
        m_spins.append(spin);
    }

    m_stripLayout->addStretch(1);
    updateStripHighlight();
}

void AnimaticPage::updateStripHighlight()
{
    for (int i = 0; i < m_thumbs.size(); ++i) {
        const bool current = (i == m_current);
        m_thumbs.at(i)->setStyleSheet(
            current
                ? QStringLiteral("QLabel#animaticThumb { border: 1px solid #f5a623; }")
                : QStringLiteral("QLabel#animaticThumb { border: 1px solid #2a2a2a; }"));
    }
}

// --- Controls -------------------------------------------------------------

QWidget *AnimaticPage::createControls()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(60);
    bar->setStyleSheet(QStringLiteral("background-color: #0d0d0d; border-top: 1px solid #1f1f1f;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);
    layout->setSpacing(10);

    layout->addStretch(1);

    QPushButton *first = transportButton(QString::fromUtf8("|\xE2\x97\x80"),
                                         QStringLiteral("First panel"));
    connect(first, &QPushButton::clicked, this, &AnimaticPage::goFirst);
    layout->addWidget(first);

    m_playButton = transportButton(QString::fromUtf8("\xE2\x96\xB6"),
                                   QStringLiteral("Play / Pause"));
    connect(m_playButton, &QPushButton::clicked, this, &AnimaticPage::togglePlay);
    layout->addWidget(m_playButton);

    QPushButton *last = transportButton(QString::fromUtf8("\xE2\x96\xB6|"),
                                        QStringLiteral("Last panel"));
    connect(last, &QPushButton::clicked, this, &AnimaticPage::goLast);
    layout->addWidget(last);

    layout->addStretch(1);

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

    for (Scene *scene : scenes) {
        for (int pi = 0; pi < scene->panels.size(); ++pi)
            m_items.append({scene, scene->panels.at(pi), scene->number, pi + 1});
    }

    rebuildTimingStrip();
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
    const Item &it = m_items.at(index);
    m_display->setPixmap(it.panel->pixmap);
    m_caption->setText(QString::fromUtf8("Scene %1 \xE2\x80\x94 Panel %2")
                           .arg(it.sceneNumber)
                           .arg(it.panelInScene));
    updateStripHighlight();
}

void AnimaticPage::updateTotalLabel()
{
    int total = 0;
    for (const Item &it : m_items)
        total += it.panel->duration;
    m_totalLabel->setText(QStringLiteral("Total: ") + formatTime(total));
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
    if (m_current < 0)
        showPanel(0);
    m_playing = true;
    m_playButton->setText(QString::fromUtf8("\xE2\x8F\xB8")); // pause glyph
    scheduleTick();
    if (hasAudio())
        m_player->play(); // resumes from current position (0 on a fresh start)
}

void AnimaticPage::pause()
{
    m_playing = false;
    if (m_timer)
        m_timer->stop();
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

void AnimaticPage::scheduleTick()
{
    if (m_current < 0 || m_current >= m_items.size())
        return;
    const int seconds = qMax(1, m_items.at(m_current).panel->duration);
    m_timer->start(seconds * 1000);
}

void AnimaticPage::advance()
{
    if (m_items.isEmpty())
        return;
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

bool AnimaticPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        const QVariant idx = object->property("itemIndex");
        if (idx.isValid()) {
            jumpTo(idx.toInt());
            return false;
        }
    }
    return QWidget::eventFilter(object, event);
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
