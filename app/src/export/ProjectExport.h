#pragma once

#include "MovieEncoder.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QAudioDecoder;
class QPainter;
class QTimer;

struct Panel;
struct Scene;

// FILE > EXPORT: the film as an MP4, the panels as PNG files, the board as
// a PDF. Three outputs of one list - the project's panels in play order -
// and one picture of each panel: Panel::flattenedPixmap(), the same flatten
// the canvas, the strip, the preview and Save read. Nothing here has its
// own idea of what a panel looks like.
//
// No dialogs and no widgets in this file: MainWindow asks the questions and
// shows the progress; this does the work and says what happened.
namespace sankoexport {

// The MP4's DELIVERY FORMAT - fixed at 1080p, a product decision, not a
// canvas size. A panel of any project resolution is fitted into it
// (aspect kept) on black. Changing the project's canvas never changes this.
inline constexpr QSize kMovieFrameSize(1920, 1080);

// The MP4's SOUND: AAC-LC at this rate (the top rate Windows' encoder
// documents). Not a hope: the export reads the finished file back and
// refuses it if the sound track is any other kind or rate.
inline constexpr int kMovieAudioBitRate = 192000;

inline constexpr int kPdfPanelsPerPage = 6; // A4 landscape, 3 across, 2 down

// One panel of the film, in play order.
struct ExportPanel
{
    const Panel *panel = nullptr;
    int sceneNumber = 0;
    int panelInScene = 0; // 1-based
    int seconds = 1;      // on screen; never less than 1
};

QVector<ExportPanel> collectPanels(const QVector<Scene *> &scenes);

// seconds x fps, summed. A panel's duration is whole seconds and the frame
// rate a whole number, so this is exact: no frame is ever rounded away.
int movieFrameCount(const QVector<ExportPanel> &panels, int fps);

// The panel fitted into the movie frame, smooth-scaled, on black.
QImage renderMovieFrame(const Panel *panel, const QSize &frameSize);

// Return false to cancel. Called on the caller's thread, between files.
using Progress = std::function<bool(int done, int total)>;

struct FilesResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;       // names the file that could not be written
    QStringList written; // full paths, in order, of files that are complete
};

// The project's name with the characters a file name cannot hold replaced:
// what every exported file's name starts with.
QString fileStem(const QString &projectName);

// "<Project>_S01_P03.png".
QString pngFileName(const QString &projectName, const ExportPanel &panel);

// One PNG per panel at the panel's own size (the project's canvas size).
// EVERY write is checked, and a file either exists complete or not at all
// (written beside its name, then renamed). Stops at the first failure.
FilesResult exportPng(const QVector<ExportPanel> &panels, const QString &folder,
                      const QString &projectName, const Progress &progress);

// The board on A4 landscape, six panels to a page: picture, scene / panel
// number, duration, the shot line and the notes.
int boardPageCount(int panelCount);
// One page of it (0-based), in device pixels of a page `pageSize` at `dpi`.
// exportPdf paints every page through this; pointing it at a QImage is how
// a page is looked at without a PDF reader.
void paintBoardPage(QPainter &painter, const QSize &pageSize, int dpi,
                    const QVector<ExportPanel> &panels, int page,
                    const QString &projectName);
FilesResult exportPdf(const QVector<ExportPanel> &panels, const QString &path,
                      const QString &projectName, const Progress &progress);

struct MovieRequest
{
    QString outPath;
    QVector<ExportPanel> panels;
    int fps = 24;
    QString audioPath; // empty: a silent film
    QSize frameSize = kMovieFrameSize;
};

struct MovieResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    int framesExpected = 0;
    int framesInFile = 0;      // read back from the file, not assumed
    bool constantTiming = false;
    double videoSeconds = 0.0; // as the file says
    bool audioTrack = false;
    double audioSeconds = 0.0;
    int audioObjectType = 0;    // 2 = AAC-LC; read back
    double audioBitRate = 0.0;  // bits per second, MEASURED from the file
    bool colourTagged = false;  // the file says its colour is BT.709
    qint64 bytes = 0;
};

// THE MP4 EXPORT. Renders each panel once, hands the encoder one frame per
// 1/fps for as long as the panel is on screen, and audio beside it.
//
// What the old export got wrong, and what this does instead:
// - 24 fps whatever the project said -> the project's frame rate.
// - "-shortest": audio shorter than the film CUT THE FILM -> the video's
//   length is the film's length. Audio is fed only up to it; shorter audio
//   simply ends.
// - one fixed temp folder of PNG frames, shared by every export on the
//   machine -> no frame files at all; the MP4 is written under a name no
//   other export can have, beside its destination, and renamed when - and
//   only when - it is complete.
// - not one frame write checked -> every frame offered is either accepted
//   or offered again; an encoder error stops the export; and the finished
//   file is READ BACK: it is kept only if it holds every frame - and, when
//   there is sound, only if the sound track is AAC-LC at the rate asked.
//   (The first encoder built here wrote it at 16 kbit/s and reported
//   success; nothing checked.)
class MovieExportJob : public QObject
{
    Q_OBJECT

public:
    // Takes ownership of the encoder.
    MovieExportJob(const MovieRequest &request, MovieEncoder *encoder,
                   QObject *parent = nullptr);
    ~MovieExportJob() override;

    void start();
    void cancel();
    bool isDone() const { return m_done; }
    const MovieResult &result() const { return m_result; }

    // How long the encoder may say nothing before the export gives up.
    void setStallTimeoutForTest(int ms) { m_stallMs = ms; }

signals:
    void progress(const QString &stage, int done, int total);
    void finished(); // exactly once, never from inside start()

private:
    void beginAudioDecode();
    void onAudioBuffer();
    void beginEncode();
    void pumpVideo();
    void pumpAudio();
    void onEncoderFinished(bool ok, const QString &error);
    void fail(const QString &error);
    void conclude();
    void kick();

    MovieRequest m_request;
    MovieEncoder *m_encoder = nullptr;
    MovieResult m_result;
    QString m_partial;
    bool m_done = false;
    bool m_cancelRequested = false;
    bool m_encoderRunning = false;

    int m_totalFrames = 0;
    QVector<int> m_endFrame; // per panel: the frame after its last
    int m_frame = 0;
    int m_panelOfImage = -1;
    QImage m_image;
    bool m_videoEnded = false;

    QAudioDecoder *m_decoder = nullptr;
    QByteArray m_pcm; // 16-bit interleaved, never longer than the film
    bool m_hadAudio = false; // the encoder was given sound to write
    int m_audioRate = 0;
    int m_audioChannels = 0;
    qint64 m_audioFrame = 0;
    bool m_audioEnded = false;

    QTimer *m_stall = nullptr;
    int m_stallMs = 60000;
};

} // namespace sankoexport
