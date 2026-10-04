#include "ProjectExport.h"

#include "Mp4Probe.h"
#include "StoryboardModel.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <cmath>

namespace sankoexport {

// --- The list every export reads ---------------------------------------------

QVector<ExportPanel> collectPanels(const QVector<Scene *> &scenes)
{
    QVector<ExportPanel> list;
    for (const Scene *scene : scenes) {
        if (!scene)
            continue;
        for (int i = 0; i < scene->panels.size(); ++i) {
            const Panel *panel = scene->panels.at(i);
            if (!panel)
                continue;
            ExportPanel row;
            row.panel = panel;
            row.sceneNumber = scene->number;
            row.panelInScene = i + 1;
            // The same floor the timeline and playback apply.
            row.seconds = qMax(1, panel->duration);
            list.append(row);
        }
    }
    return list;
}

int movieFrameCount(const QVector<ExportPanel> &panels, int fps)
{
    int frames = 0;
    for (const ExportPanel &row : panels)
        frames += row.seconds * qMax(1, fps);
    return frames;
}

QImage renderMovieFrame(const Panel *panel, const QSize &frameSize)
{
    // RGBX8888: the format a video frame is made from without a conversion.
    QImage frame(frameSize, QImage::Format_RGBX8888);
    frame.fill(Qt::black);
    const QImage flat = panel ? panel->flattenedPixmap().toImage() : QImage();
    if (flat.isNull())
        return frame;
    const QSize target = flat.size().scaled(frameSize, Qt::KeepAspectRatio);
    // QImage::scaled, not a scaling drawPixmap: reducing a 4K panel to
    // 1080p through the painter samples it bilinearly and thins fine line
    // work; SmoothTransformation averages the area each output pixel covers.
    const QImage fitted = flat.size() == target
        ? flat
        : flat.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&frame);
    painter.drawImage(QPoint((frameSize.width() - target.width()) / 2,
                             (frameSize.height() - target.height()) / 2),
                      fitted);
    return frame;
}

// --- PNG ------------------------------------------------------------------------

QString fileStem(const QString &projectName)
{
    QString stem = projectName.trimmed();
    for (QChar &c : stem)
        if (c.unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(c))
            c = QLatin1Char('_');
    while (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' ')))
        stem.chop(1); // Windows drops them, and two names would collide
    return stem.isEmpty() ? QStringLiteral("Storyboard") : stem;
}

QString pngFileName(const QString &projectName, const ExportPanel &panel)
{
    return QStringLiteral("%1_S%2_P%3.png")
        .arg(fileStem(projectName))
        .arg(panel.sceneNumber, 2, 10, QLatin1Char('0'))
        .arg(panel.panelInScene, 2, 10, QLatin1Char('0'));
}

FilesResult exportPng(const QVector<ExportPanel> &panels, const QString &folder,
                      const QString &projectName, const Progress &progress)
{
    FilesResult result;
    if (panels.isEmpty()) {
        result.error = QStringLiteral("There are no panels to export.");
        return result;
    }
    if (!QDir().mkpath(folder)) {
        result.error = QStringLiteral("The folder could not be created:\n%1")
                           .arg(QDir::toNativeSeparators(folder));
        return result;
    }
    const QDir dir(folder);
    for (int i = 0; i < panels.size(); ++i) {
        if (progress && !progress(i, int(panels.size()))) {
            result.cancelled = true;
            return result;
        }
        const ExportPanel &row = panels.at(i);
        const QString path = dir.filePath(pngFileName(projectName, row));
        const QImage image = row.panel->flattenedPixmap().toImage();
        // QSaveFile: the PNG is written beside its name and renamed over it
        // on commit, so a failed write leaves no half-file and cannot damage
        // an earlier export of the same panel.
        QSaveFile file(path);
        bool ok = !image.isNull() && file.open(QIODevice::WriteOnly);
        QString why = image.isNull() ? QStringLiteral("the panel has no picture")
                                     : file.errorString();
        if (ok) {
            QImageWriter writer(&file, "png");
            ok = writer.write(image);
            if (!ok)
                why = writer.errorString();
        }
        if (ok) {
            ok = file.commit();
            if (!ok)
                why = file.errorString();
        } else {
            file.cancelWriting();
        }
        if (!ok) {
            result.error =
                QStringLiteral("Scene %1, panel %2 could not be written:\n%3\n\n%4")
                    .arg(row.sceneNumber).arg(row.panelInScene)
                    .arg(QDir::toNativeSeparators(path), why);
            return result;
        }
        result.written.append(path);
    }
    if (progress)
        progress(int(panels.size()), int(panels.size()));
    result.ok = true;
    return result;
}

// --- PDF ------------------------------------------------------------------------

int boardPageCount(int panelCount)
{
    return (panelCount + kPdfPanelsPerPage - 1) / kPdfPanelsPerPage;
}

// One page of the board, painted in device pixels at `dpi`: a header line,
// then up to six cells, three across and two down. Each cell is the panel's
// picture with, under it, which panel it is, how long it runs, the shot
// line and the notes. It paints through a QPainter and nothing else, so the
// same code can be pointed at an image to LOOK at a page.
void paintBoardPage(QPainter &painter, const QSize &pageSize, int dpi,
                    const QVector<ExportPanel> &panels, int page,
                    const QString &projectName)
{
    const double mm = dpi / 25.4;
    const QPaintDevice *device = painter.device();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QFont headFont = QGuiApplication::font();
    headFont.setPointSizeF(10);
    QFont labelFont = headFont;
    labelFont.setPointSizeF(9);
    labelFont.setBold(true);
    QFont textFont = headFont;
    textFont.setPointSizeF(8);

    const int pageW = pageSize.width(), pageH = pageSize.height();
    const int headH = int(9 * mm);
    const int gap = int(5 * mm);
    const int textH = int(21 * mm); // label, shot line, three lines of notes
    constexpr int kColumns = 3, kRows = kPdfPanelsPerPage / kColumns;
    const int cellW = (pageW - gap * (kColumns - 1)) / kColumns;
    const int cellH = (pageH - headH - gap * (kRows - 1)) / kRows;

    int totalSeconds = 0;
    for (const ExportPanel &row : panels)
        totalSeconds += row.seconds;

    painter.setFont(headFont);
    painter.setPen(Qt::black);
    const QRect head(0, 0, pageW, headH - int(2 * mm));
    painter.drawText(head, Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("%1   \xC2\xB7   %2 panels   \xC2\xB7   %3:%4")
                         .arg(projectName).arg(panels.size())
                         .arg(totalSeconds / 60)
                         .arg(totalSeconds % 60, 2, 10, QLatin1Char('0')));
    painter.drawText(head, Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("Page %1 of %2")
                         .arg(page + 1).arg(boardPageCount(int(panels.size()))));
    painter.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), 0.25 * mm));
    painter.drawLine(0, head.bottom(), pageW, head.bottom());

    const int first = page * kPdfPanelsPerPage;
    for (int slot = 0; slot < kPdfPanelsPerPage && first + slot < panels.size();
         ++slot) {
        const ExportPanel &row = panels.at(first + slot);
        const QRect cell((slot % kColumns) * (cellW + gap),
                         headH + (slot / kColumns) * (cellH + gap), cellW, cellH);

        // The picture: fitted into the top of the cell, and reduced to the
        // cell's own pixel size first (a 4K panel embedded whole would make
        // a six-panel page some 50 MB for nothing the paper can show).
        QImage image = row.panel->flattenedPixmap().toImage();
        const QRect box(cell.left(), cell.top(), cellW, cellH - textH);
        QRect pictureRect = box;
        if (!image.isNull()) {
            const QSize fitted = image.size().scaled(box.size(), Qt::KeepAspectRatio);
            pictureRect = QRect(box.left() + (box.width() - fitted.width()) / 2,
                                box.top(), fitted.width(), fitted.height());
            if (image.width() > fitted.width())
                image = image.scaled(fitted, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            painter.drawImage(pictureRect, image.convertToFormat(QImage::Format_RGB32));
        }
        painter.setPen(QPen(QColor(0x60, 0x60, 0x60), 0.25 * mm));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(pictureRect);

        // Under it: which panel, how long, the shot, the notes.
        int y = pictureRect.bottom() + int(1.5 * mm);
        painter.setPen(Qt::black);
        painter.setFont(labelFont);
        const int labelH = QFontMetrics(labelFont, device).height();
        painter.drawText(QRect(cell.left(), y, cellW, labelH),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Scene %1  \xC2\xB7  Panel %2")
                             .arg(row.sceneNumber).arg(row.panelInScene));
        painter.drawText(QRect(cell.left(), y, cellW, labelH),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 s").arg(row.seconds));
        y += labelH;
        painter.setFont(textFont);
        const QFontMetrics textMetrics(textFont, device);
        QStringList shot;
        for (const QString &part : {row.panel->shotType, row.panel->cameraAngle,
                                    row.panel->lens, row.panel->mood})
            if (!part.trimmed().isEmpty())
                shot << part.trimmed();
        painter.setPen(QColor(0x50, 0x50, 0x50));
        painter.drawText(QRect(cell.left(), y, cellW, textMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         textMetrics.elidedText(shot.join(QStringLiteral("  \xC2\xB7  ")),
                                                Qt::ElideRight, cellW));
        y += textMetrics.height();
        const QString notes = row.panel->notes.trimmed();
        if (!notes.isEmpty() && cell.bottom() > y) {
            painter.setPen(Qt::black);
            const QRect notesRect(cell.left(), y, cellW, cell.bottom() - y);
            painter.save();
            painter.setClipRect(notesRect);
            painter.drawText(notesRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             notes);
            painter.restore();
        }
    }
}

FilesResult exportPdf(const QVector<ExportPanel> &panels, const QString &path,
                      const QString &projectName, const Progress &progress)
{
    FilesResult result;
    if (panels.isEmpty()) {
        result.error = QStringLiteral("There are no panels to export.");
        return result;
    }
    // The same rule as the PNGs: the PDF appears complete or not at all.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("The PDF could not be written:\n%1\n\n%2")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }

    constexpr int kDpi = 300;
    QPdfWriter pdf(&file);
    pdf.setResolution(kDpi);
    pdf.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape,
                                  QMarginsF(10, 10, 10, 10),
                                  QPageLayout::Millimeter));
    pdf.setTitle(projectName);
    pdf.setCreator(QStringLiteral("SankoTV"));

    QPainter painter;
    if (!painter.begin(&pdf)) {
        file.cancelWriting();
        result.error = QStringLiteral("The PDF could not be started:\n%1")
                           .arg(QDir::toNativeSeparators(path));
        return result;
    }

    const QSize pageSize(pdf.width(), pdf.height()); // the printable area
    const int pages = boardPageCount(int(panels.size()));
    bool ok = true;
    for (int page = 0; ok && page < pages; ++page) {
        if (progress && !progress(page * kPdfPanelsPerPage, int(panels.size()))) {
            painter.end();
            file.cancelWriting();
            result.cancelled = true;
            return result;
        }
        if (page > 0)
            ok = pdf.newPage();
        paintBoardPage(painter, pageSize, kDpi, panels, page, projectName);
    }
    ok = painter.end() && ok;
    if (ok)
        ok = file.commit();
    else
        file.cancelWriting();
    if (!ok) {
        result.error = QStringLiteral("The PDF could not be written:\n%1\n\n%2")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    if (progress)
        progress(int(panels.size()), int(panels.size()));
    result.written.append(path);
    result.ok = true;
    return result;
}

// --- MP4 ------------------------------------------------------------------------

namespace {

// Decoded audio as signed 16-bit, whatever the decoder produced.
QByteArray toInt16(const QAudioBuffer &buffer)
{
    const QAudioFormat format = buffer.format();
    const qsizetype count = qsizetype(buffer.frameCount()) * format.channelCount();
    if (format.sampleFormat() == QAudioFormat::Int16)
        return QByteArray(buffer.constData<char>(), count * 2);
    QByteArray out(count * 2, '\0');
    auto *dst = reinterpret_cast<qint16 *>(out.data());
    switch (format.sampleFormat()) {
    case QAudioFormat::Float: {
        const float *src = buffer.constData<float>();
        for (qsizetype i = 0; i < count; ++i)
            dst[i] = qint16(std::lround(qBound(-1.0f, src[i], 1.0f) * 32767.0f));
        break;
    }
    case QAudioFormat::Int32: {
        const qint32 *src = buffer.constData<qint32>();
        for (qsizetype i = 0; i < count; ++i)
            dst[i] = qint16(src[i] >> 16);
        break;
    }
    case QAudioFormat::UInt8: {
        const quint8 *src = buffer.constData<quint8>();
        for (qsizetype i = 0; i < count; ++i)
            dst[i] = qint16((int(src[i]) - 128) << 8);
        break;
    }
    default:
        return QByteArray();
    }
    return out;
}

} // namespace

MovieExportJob::MovieExportJob(const MovieRequest &request, MovieEncoder *encoder,
                               QObject *parent)
    : QObject(parent)
    , m_request(request)
    , m_encoder(encoder)
{
    if (m_encoder)
        m_encoder->setParent(this);
    m_stall = new QTimer(this);
    m_stall->setSingleShot(true);
    connect(m_stall, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("The export stopped making progress and was "
                            "abandoned. Nothing was exported."));
    });
}

MovieExportJob::~MovieExportJob()
{
    // Never leave the partial file behind - not when destroyed mid-export
    // (the window closing), and not when an encoder that had stopped
    // answering still held the file open at the time conclude() tried.
    // The encoder goes first so that the file is closed.
    delete m_encoder;
    m_encoder = nullptr;
    if (!m_result.ok && !m_partial.isEmpty())
        QFile::remove(m_partial);
}

void MovieExportJob::kick()
{
    if (!m_done)
        m_stall->start(m_stallMs);
}

void MovieExportJob::start()
{
    const int fps = qMax(1, m_request.fps);
    m_endFrame.clear();
    m_totalFrames = 0;
    for (const ExportPanel &row : m_request.panels) {
        m_totalFrames += row.seconds * fps;
        m_endFrame.append(m_totalFrames);
    }
    m_result = MovieResult();
    m_result.framesExpected = m_totalFrames;
    if (!m_encoder) {
        fail(QStringLiteral("MP4 export is not available on this system."));
        return;
    }
    if (m_totalFrames <= 0) {
        fail(QStringLiteral("There are no panels to export."));
        return;
    }
    // A name of its own, beside the destination (same volume, so the final
    // rename is a rename and not a copy). Still ".mp4": the recorder picks
    // the container from the extension.
    const QFileInfo out(m_request.outPath);
    m_partial = out.absolutePath() + QLatin1Char('/') + out.completeBaseName()
        + QStringLiteral(".partial-")
        + QUuid::createUuid().toString(QUuid::Id128).left(8) + QStringLiteral(".mp4");
    kick();
    if (m_request.audioPath.isEmpty())
        beginEncode();
    else
        beginAudioDecode();
}

// The audio is decoded BEFORE the encoder is started, into memory, and only
// as far as the film runs. That is what makes "the video's length wins" a
// fact rather than a muxer option: the encoder is never offered a sample
// past the last frame. (16-bit stereo at 48 kHz is 11.5 MB a minute.)
void MovieExportJob::beginAudioDecode()
{
    emit progress(QStringLiteral("Reading audio..."), 0, 0);
    m_decoder = new QAudioDecoder(this);
    QAudioFormat wanted;
    wanted.setSampleRate(48000);
    wanted.setChannelCount(2);
    wanted.setSampleFormat(QAudioFormat::Int16);
    m_decoder->setAudioFormat(wanted);
    m_decoder->setSource(QUrl::fromLocalFile(m_request.audioPath));
    connect(m_decoder, &QAudioDecoder::bufferReady, this,
            &MovieExportJob::onAudioBuffer);
    connect(m_decoder, &QAudioDecoder::finished, this, [this] {
        if (m_done || m_encoderRunning)
            return;
        if (m_pcm.isEmpty()) {
            // Exporting a silent film without saying so would be the old
            // export's habit of dropping what it could not handle.
            fail(QStringLiteral("No sound could be read from the audio file:\n%1")
                     .arg(QDir::toNativeSeparators(m_request.audioPath)));
            return;
        }
        beginEncode();
    });
    connect(m_decoder,
            QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), this,
            [this](QAudioDecoder::Error) {
        if (m_done || m_encoderRunning)
            return;
        fail(QStringLiteral("The audio file could not be read:\n%1\n\n%2")
                 .arg(QDir::toNativeSeparators(m_request.audioPath),
                      m_decoder->errorString()));
    });
    m_decoder->start();
}

void MovieExportJob::onAudioBuffer()
{
    if (m_done || m_encoderRunning || !m_decoder)
        return;
    kick();
    const int fps = qMax(1, m_request.fps);
    while (m_decoder->bufferAvailable()) {
        const QAudioBuffer buffer = m_decoder->read();
        if (!buffer.isValid() || buffer.frameCount() <= 0)
            continue;
        const QAudioFormat format = buffer.format();
        if (m_audioRate == 0) {
            m_audioRate = format.sampleRate();
            m_audioChannels = format.channelCount();
        } else if (format.sampleRate() != m_audioRate
                   || format.channelCount() != m_audioChannels) {
            fail(QStringLiteral("The audio file changes format part-way "
                                "through and cannot be exported:\n%1")
                     .arg(QDir::toNativeSeparators(m_request.audioPath)));
            return;
        }
        const QByteArray pcm = toInt16(buffer);
        if (pcm.isEmpty() || m_audioRate <= 0 || m_audioChannels <= 0) {
            fail(QStringLiteral("The audio file is in a form that cannot be "
                                "exported:\n%1")
                     .arg(QDir::toNativeSeparators(m_request.audioPath)));
            return;
        }
        // The film's length in sample frames; nothing past it is kept - and
        // one block less than that. AAC is written in blocks of 1024
        // samples and the last one is padded out (measured: fed 1,440,000
        // samples, the track holds 1,440,768), so sound fed right up to
        // the film's end would make the FILE run past its last frame. One
        // block held back keeps the track inside the film whatever the
        // rounding, at the cost of the last 0.02 s of sound when the audio
        // is at least as long as the film.
        constexpr qint64 kHeldBackFrames = 1024;
        const qint64 limitBytes =
            qMax<qint64>(0, qint64(m_totalFrames) * m_audioRate / fps - kHeldBackFrames)
            * m_audioChannels * 2;
        m_pcm.append(pcm.constData(),
                     qsizetype(qMin<qint64>(pcm.size(), limitBytes - m_pcm.size())));
        if (m_pcm.size() >= limitBytes) {
            m_decoder->stop(); // the audio runs past the film: enough
            beginEncode();
            return;
        }
    }
}

void MovieExportJob::beginEncode()
{
    if (m_done || m_encoderRunning)
        return;
    MovieSpec spec;
    spec.path = m_partial;
    spec.frameSize = m_request.frameSize;
    spec.fps = qMax(1, m_request.fps);
    spec.audio = !m_pcm.isEmpty();
    if (spec.audio) {
        spec.audioSampleRate = m_audioRate;
        spec.audioChannels = m_audioChannels;
        spec.audioBitRate = kMovieAudioBitRate;
    }
    m_hadAudio = spec.audio;
    connect(m_encoder, &MovieEncoder::videoWanted, this, &MovieExportJob::pumpVideo);
    connect(m_encoder, &MovieEncoder::audioWanted, this, &MovieExportJob::pumpAudio);
    connect(m_encoder, &MovieEncoder::finished, this,
            &MovieExportJob::onEncoderFinished);
    emit progress(QStringLiteral("Exporting video..."), 0, m_totalFrames);
    QString error;
    m_encoderRunning = true;
    if (!m_encoder->begin(spec, &error)) {
        m_encoderRunning = false;
        fail(error.isEmpty() ? QStringLiteral("The video encoder could not start.")
                             : error);
        return;
    }
    kick();
}

void MovieExportJob::pumpVideo()
{
    if (m_done || !m_encoderRunning || m_cancelRequested || !m_result.error.isEmpty())
        return;
    kick();
    while (m_frame < m_totalFrames) {
        int panel = qMax(0, m_panelOfImage);
        while (panel < m_endFrame.size() - 1 && m_frame >= m_endFrame.at(panel))
            ++panel;
        if (panel != m_panelOfImage) {
            // Rendered once per panel, however many frames it is held for.
            m_image = renderMovieFrame(m_request.panels.at(panel).panel,
                                       m_request.frameSize);
            m_panelOfImage = panel;
        }
        if (!m_encoder->addFrame(m_image, m_frame))
            break; // refused: the SAME frame is offered again next time
        ++m_frame;
    }
    if (m_frame >= m_totalFrames && !m_videoEnded && m_encoder->endVideo()) {
        m_videoEnded = true;
        m_image = QImage();
        emit progress(QStringLiteral("Finishing the file..."), 0, 0);
        return;
    }
    emit progress(QStringLiteral("Exporting video..."), m_frame, m_totalFrames);
}

void MovieExportJob::pumpAudio()
{
    if (m_done || !m_encoderRunning || m_cancelRequested || !m_result.error.isEmpty()
        || m_pcm.isEmpty())
        return;
    kick();
    const int frameBytes = m_audioChannels * 2;
    const qint64 totalFrames = m_pcm.size() / frameBytes;
    while (m_audioFrame < totalFrames) {
        const qint64 count = qMin<qint64>(4096, totalFrames - m_audioFrame);
        if (!m_encoder->addAudio(m_pcm.mid(m_audioFrame * frameBytes,
                                           count * frameBytes),
                                 m_audioFrame))
            return;
        m_audioFrame += count;
    }
    if (!m_audioEnded && m_encoder->endAudio())
        m_audioEnded = true;
}

void MovieExportJob::cancel()
{
    if (m_done || m_cancelRequested)
        return;
    m_cancelRequested = true;
    if (m_encoderRunning) {
        m_encoder->cancel(); // its finished() arrives; conclude from there
        kick();
        return;
    }
    if (m_decoder)
        m_decoder->stop();
    conclude();
}

void MovieExportJob::fail(const QString &error)
{
    if (m_done)
        return;
    if (m_result.error.isEmpty())
        m_result.error = error;
    if (m_encoderRunning) {
        // Let the encoder close the file before it is deleted - but not for
        // ever: an encoder that has stopped answering is why we may be here.
        m_encoder->cancel();
        QTimer::singleShot(qMin(m_stallMs, 5000), this, [this] { conclude(); });
        return;
    }
    if (m_decoder)
        m_decoder->stop();
    conclude();
}

void MovieExportJob::onEncoderFinished(bool ok, const QString &error)
{
    if (m_done)
        return;
    m_encoderRunning = false;
    if (m_cancelRequested || !m_result.error.isEmpty()) {
        conclude();
        return;
    }
    if (!ok) {
        m_result.error = error.isEmpty()
            ? QStringLiteral("The video encoder failed.")
            : error;
        conclude();
        return;
    }

    // READ IT BACK. The encoder says the file is complete; count the frames.
    const Mp4Info info = readMp4(m_partial);
    const Mp4Track *video = info.video();
    const Mp4Track *audio = info.audio();
    m_result.framesInFile = video ? int(video->samples) : 0;
    m_result.constantTiming = video && video->timing.size() == 1;
    m_result.videoSeconds = video ? video->seconds() : 0.0;
    m_result.audioTrack = audio != nullptr;
    m_result.audioSeconds = audio ? audio->seconds() : 0.0;
    m_result.audioObjectType = audio ? audio->audioObjectType : 0;
    m_result.audioBitRate = audio ? audio->bitsPerSecond() : 0.0;
    m_result.colourTagged = video && video->colourMatrix == 1;
    m_result.bytes = QFileInfo(m_partial).size();
    const double expectedSeconds = double(m_totalFrames) / qMax(1, m_request.fps);
    if (!info.readable || !video) {
        m_result.error = QStringLiteral("The encoder reported success, but the "
                                        "file it wrote has no video in it. "
                                        "Nothing was exported.");
    } else if (m_result.framesInFile != m_totalFrames) {
        m_result.error = QStringLiteral("The exported file holds %1 of the "
                                        "film's %2 frames, so it was not kept.")
                             .arg(m_result.framesInFile).arg(m_totalFrames);
    } else if (std::abs(m_result.videoSeconds - expectedSeconds)
               > 0.5 / qMax(1, m_request.fps)) {
        m_result.error = QStringLiteral("The exported file runs %1 s where the "
                                        "film runs %2 s, so it was not kept.")
                             .arg(m_result.videoSeconds, 0, 'f', 3)
                             .arg(expectedSeconds, 0, 'f', 3);
    } else if (m_hadAudio && !audio) {
        m_result.error = QStringLiteral("The exported file has no sound track "
                                        "although the project has audio, so it "
                                        "was not kept.");
    } else if (m_hadAudio
               && (m_result.audioObjectType != 2
                   || std::abs(m_result.audioBitRate - kMovieAudioBitRate)
                          > 0.1 * kMovieAudioBitRate)) {
        // THE CHECK THAT WAS MISSING. A sound track of the wrong kind or a
        // fraction of the rate plays, and reports nothing - the first
        // encoder here wrote HE-AAC at 16 kbit/s under a request for 192.
        m_result.error =
            QStringLiteral("The exported file's sound track is %1 at %2 kbit/s "
                           "where AAC-LC at %3 kbit/s was asked for, so it was "
                           "not kept.")
                .arg(m_result.audioObjectType == 2 ? QStringLiteral("AAC-LC")
                     : m_result.audioObjectType == 5 ? QStringLiteral("HE-AAC")
                     : QStringLiteral("an unexpected kind of audio"))
                .arg(m_result.audioBitRate / 1000.0, 0, 'f', 0)
                .arg(kMovieAudioBitRate / 1000);
    }
    if (m_result.error.isEmpty()) {
        // Only now does the destination change. The artist chose this name
        // in the file dialog, which asked about replacing it.
        if (QFile::exists(m_request.outPath) && !QFile::remove(m_request.outPath)) {
            m_result.error =
                QStringLiteral("The existing file could not be replaced (is it "
                               "open in a player?):\n%1")
                    .arg(QDir::toNativeSeparators(m_request.outPath));
        } else if (!QFile::rename(m_partial, m_request.outPath)) {
            m_result.error = QStringLiteral("The finished film could not be "
                                            "moved into place:\n%1")
                                 .arg(QDir::toNativeSeparators(m_request.outPath));
        } else {
            m_result.ok = true;
        }
    }
    conclude();
}

void MovieExportJob::conclude()
{
    if (m_done)
        return;
    m_done = true;
    m_stall->stop();
    m_pcm.clear();
    m_image = QImage();
    if (!m_result.ok) {
        m_result.cancelled = m_cancelRequested && m_result.error.isEmpty();
        if (!m_partial.isEmpty())
            QFile::remove(m_partial);
    }
    // Queued: start() may be what got us here, and its caller has not begun
    // waiting yet.
    QMetaObject::invokeMethod(this, [this] { emit finished(); },
                              Qt::QueuedConnection);
}

} // namespace sankoexport
