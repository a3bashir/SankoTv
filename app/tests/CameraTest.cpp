// SankoTV CAMERA family - PERMANENT (Phase 1A of the shot camera, 2026-10-25;
// the spec is Documents\SankoTV-Design\camera\phase1-spec.md).
//
// (c1p) THE NEUTRAL PINS. Recorded on the code BEFORE the camera existed
// (commit f46ccbcc6), so they say what a project with no camera moves
// rendered then: the MP4 frame, the PNG file, a PDF page, the playback
// preview and a SHOTS clip thumbnail, for panels at four canvas sizes. A
// project without camera moves must render these bytes for ever; the
// camera checks below prove each pin can see a move (positive controls).
//
// Headless: the widgets are shown with Qt::WA_DontShowOnScreen, so they lay
// out and paint with the real fonts without a window appearing anywhere.
//
// Run: build/<config>/SankoCameraTest.exe (exit code = failure count;
// report in camera_test.txt in the working directory).

#include "AnimaticPage.h"
#include "AnimaticTimeline.h"
#include "ProjectIO.h"
#include "StoryboardModel.h"
#include "camera/ShotCamera.h"
#include "export/MovieEncoder.h"
#include "export/ProjectExport.h"

#include <QApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSet>
#include <QtMath>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <QVariantMap>

#include <cstdio>

namespace {

QTextStream *g_out = nullptr;
int g_failures = 0;
int g_checks = 0;

void check(const QString &label, bool ok, const QString &detail = QString())
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    (*g_out) << (ok ? "PASS " : "FAIL ") << label;
    if (!detail.isEmpty())
        (*g_out) << "   " << detail;
    (*g_out) << "\n";
    g_out->flush();
    fprintf(ok ? stdout : stderr, "%s %s\n", ok ? "PASS" : "FAIL",
            qPrintable(label));
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// SHA-256 of the picture itself: 32-bit ARGB rows, no padding, so the hash
// is of pixels and never of a format's stride.
QString shaOf(const QImage &image)
{
    const QImage a = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (int y = 0; y < a.height(); ++y)
        h.addData(QByteArrayView(reinterpret_cast<const char *>(a.constScanLine(y)),
                                 qsizetype(a.width()) * 4));
    return QString::fromLatin1(h.result().toHex());
}

QString shaOfFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("unreadable");
    return QString::fromLatin1(
        QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex());
}

// A fixture panel: deterministic line work, shapes and a half-opacity
// layer, all placed in proportion to the canvas so every size draws the
// same composition.
Panel *fixturePanel(const QSize &size)
{
    Panel *panel = makeBlankPanel(size);
    const double w = size.width(), h = size.height();
    {
        QPainter p(&panel->layers[1].image);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(20, 20, 20), qMax(1.0, w / 640.0)));
        for (int i = 1; i < 12; ++i)
            p.drawLine(QPointF(w * i / 12.0, h * 0.05), QPointF(w * (12 - i) / 12.0, h * 0.95));
        p.setPen(QPen(QColor(200, 40, 40), qMax(1.0, w / 320.0)));
        p.drawEllipse(QPointF(w * 0.62, h * 0.4), w * 0.18, h * 0.22);
        QPainterPath path;
        path.moveTo(w * 0.1, h * 0.8);
        path.cubicTo(w * 0.3, h * 0.2, w * 0.5, h * 1.0, w * 0.9, h * 0.3);
        p.setPen(QPen(QColor(30, 60, 200), qMax(1.0, w / 400.0)));
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 160, 90, 160));
        p.drawPolygon(QPolygonF({QPointF(w * 0.15, h * 0.15), QPointF(w * 0.35, h * 0.1),
                                 QPointF(w * 0.3, h * 0.45)}));
    }
    Layer tone = makeRasterLayer(QStringLiteral("Tone"), size);
    {
        QPainter p(&tone.image);
        p.fillRect(QRectF(w * 0.55, h * 0.6, w * 0.3, h * 0.25), QColor(90, 90, 90));
    }
    tone.opacity = 0.6;
    panel->layers.append(tone);
    panel->duration = 3;
    return panel;
}

// The four canvas sizes the pins cover: the three presets and an odd one.
const QSize kSizes[4] = {{1920, 1080}, {2048, 1080}, {3840, 2160}, {1001, 777}};

// THE PINS (recorded on f46ccbcc6, Release; identical in Debug). Empty
// strings mean "not recorded yet": the check prints the value to record.
struct Pin
{
    const char *name;
    const char *sha;
};
const Pin kPins[] = {
    {"mp4.1920x1080", "8d8cc2e9c9ba049650b8824f5988b00b5e6860c5201fed67812531a5f4453e2b"},
    {"mp4.2048x1080", "b3e5ef458727fb84b5f7fe52bd2b26fb7157bf01b41f1d7da9516b5a2d6ed05f"},
    {"mp4.3840x2160", "b4728701d4a1da9e28e2ad2485257e6b627a71ea6187f97abdeed89e685a7bcd"},
    {"mp4.1001x777", "2fef0a084f4cb52ae64824907925e2a4ae25c643b02a5a62db9c1ee9f64554cc"},
    {"png.1920x1080", "40a4fc2d48c5b7118bca1a348df0f4488897dd09175547baf8cd5cc2d24f944e"},
    {"png.2048x1080", "516ea162b5c74653da2ca2f7d032d4e027a00f2e50a5ee82a94059ccbbfa28fe"},
    {"png.3840x2160", "2b933bb5f1842d088ba0220a78d5a9e1b80c7e3c18906be88f0712b5a992aad7"},
    {"png.1001x777", "d133429b56cf8ae72391ca821ed1617d443191a2adcf90b2a2acc829757397b3"},
    {"pdf.page0", "4a36b2e8f12da291f43808406b390552d5071801a12e3561e4501f48c81fb322"},
    {"preview.1920x1080", "ea897a327d36a091eb79790b29909ef76b171878f1f5e3996a85556088665e62"},
    {"preview.2048x1080", "606927537aa6e0c6d3892709da1061d1b41858f5dbd148dda484391ede18ab1f"},
    {"preview.3840x2160", "919a13f3f8a94de2a3e9018634836b22d5e89cae8c258fe1c4a9e0ecdf505141"},
    {"preview.1001x777", "5ba3d9dd3b6391b058d63d701c561b19a13460e2efcb0311a26418d68295ed47"},
    {"clip.1920x1080", "bd93c17d4a94a8c799bb32fd993e11bdbc935a3fd417e0e72b62c54b40a100f8"},
    {"clip.2048x1080", "0a6e1eee1051b4aaeed423d50a78bb7c6924c8ed0d781a2cdb61eaeac9edb63b"},
    {"clip.3840x2160", "19dd058ff9b61d8d9addb519fd132bcc1519c316c8cd0aaef5ff8e18ff2f16e5"},
    {"clip.1001x777", "a1af50e90fe2fcd1ce4080a80cd8303aa8113bb1836d848aef9e2764ee0e6b41"},
};

QString pinned(const QString &name)
{
    for (const Pin &p : kPins)
        if (name == QLatin1String(p.name))
            return QString::fromLatin1(p.sha);
    return QString();
}

void checkPin(const QString &name, const QString &sha)
{
    const QString want = pinned(name);
    check(QStringLiteral("(c1p) %1 renders its pinned bytes").arg(name),
          !want.isEmpty() && sha == want,
          want.isEmpty() ? QStringLiteral("RECORD %1").arg(sha) : sha.left(16));
}

// Stands in front of the real encoder and keeps the SHA of the frames the
// export job actually offered at the indices asked for: what C3 compares
// with the camera's own rendering of those frames.
class CaptureEncoder : public sankoexport::MovieEncoder
{
public:
    explicit CaptureEncoder(const QSet<int> &wanted)
        : m_wanted(wanted), m_inner(sankoexport::createMovieEncoder(this))
    {
        connect(m_inner, &MovieEncoder::videoWanted, this, &MovieEncoder::videoWanted);
        connect(m_inner, &MovieEncoder::audioWanted, this, &MovieEncoder::audioWanted);
        connect(m_inner, &MovieEncoder::finished, this, &MovieEncoder::finished);
    }
    bool begin(const sankoexport::MovieSpec &spec, QString *error) override
    {
        return m_inner->begin(spec, error);
    }
    bool addFrame(const QImage &frame, int index) override
    {
        if (!m_inner->addFrame(frame, index))
            return false;
        if (m_wanted.contains(index) && !shas.contains(index))
            shas.insert(index, shaOf(frame));
        return true;
    }
    bool endVideo() override { return m_inner->endVideo(); }
    bool addAudio(const QByteArray &pcm, qint64 first) override
    {
        return m_inner->addAudio(pcm, first);
    }
    bool endAudio() override { return m_inner->endAudio(); }
    void cancel() override { m_inner->cancel(); }
    QHash<int, QString> shas;

private:
    QSet<int> m_wanted;
    sankoexport::MovieEncoder *m_inner;
};

bool near(double a, double b, double tol = 1e-9)
{
    return qAbs(a - b) <= tol * qMax(1.0, qMax(qAbs(a), qAbs(b)));
}

bool sameState(const shotcam::CameraState &a, const shotcam::CameraState &b,
               double tol = 1e-9)
{
    return near(a.cx, b.cx, tol) && near(a.cy, b.cy, tol)
        && near(a.zoom, b.zoom, tol) && near(a.rotation, b.rotation, tol);
}

QString stateText(const shotcam::CameraState &s)
{
    return QStringLiteral("(%1, %2, z %3, r %4)")
        .arg(s.cx, 0, 'f', 4).arg(s.cy, 0, 'f', 4)
        .arg(s.zoom, 0, 'f', 6).arg(s.rotation, 0, 'f', 4);
}

shotcam::CameraState state(double cx, double cy, double zoom, double rotation)
{
    shotcam::CameraState s;
    s.cx = cx;
    s.cy = cy;
    s.zoom = zoom;
    s.rotation = rotation;
    return s;
}

QRect rectOf(const QString &s)
{
    // "x,y wxh" (AnimaticTimeline::clipPartsForTest)
    const QStringList parts = s.split(QLatin1Char(' '));
    if (parts.size() != 2)
        return QRect();
    const QStringList xy = parts.at(0).split(QLatin1Char(','));
    const QStringList wh = parts.at(1).split(QLatin1Char('x'));
    if (xy.size() != 2 || wh.size() != 2)
        return QRect();
    return QRect(xy.at(0).toInt(), xy.at(1).toInt(), wh.at(0).toInt(), wh.at(1).toInt());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QFile outFile(QStringLiteral("camera_test.txt"));
    outFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream ts(&outFile);
    g_out = &ts;
    const QString scratch = QDir::tempPath() + QStringLiteral("/sanko_camera_test");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    // One scene, the four fixture panels, 3 s each.
    Scene *scene = new Scene;
    scene->number = 1;
    for (const QSize &size : kSizes)
        scene->panels.append(fixturePanel(size));
    const QVector<Scene *> scenes{scene};

    // ---- (c1p) the neutral pins -------------------------------------------
    ts << "--- (c1p) neutral pins ---\n";
    {
        const QVector<sankoexport::ExportPanel> rows = sankoexport::collectPanels(scenes);
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            checkPin(QStringLiteral("mp4.") + tag,
                     shaOf(sankoexport::renderMovieFrame(scene->panels.at(i),
                                                         sankoexport::kMovieFrameSize)));
        }
        const QString pngDir = scratch + QStringLiteral("/png");
        const sankoexport::FilesResult png =
            sankoexport::exportPng(rows, pngDir, QStringLiteral("Pin"), {});
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            checkPin(QStringLiteral("png.") + tag,
                     png.ok && png.written.size() == 4 ? shaOfFile(png.written.at(i))
                                                       : QStringLiteral("no file"));
        }
        QImage page(1091, 748, QImage::Format_ARGB32);
        page.fill(Qt::white);
        {
            QPainter p(&page);
            sankoexport::paintBoardPage(p, page.size(), 100, rows, 0, QStringLiteral("Pin"));
        }
        checkPin(QStringLiteral("pdf.page0"), shaOf(page));
    }

    // The preview and the SHOTS clips, through the real page, off screen.
    {
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen, true);
        host.resize(960, 540);
        host.show();
        AnimaticPage page;
        page.setAttribute(Qt::WA_DontShowOnScreen, true);
        page.resize(1600, 320);
        page.setFps(24);
        page.show();
        page.loadScenes(scenes);
        page.attachPreview(&host);
        pump(200);
        AnimaticTimeline *tl = page.timelineForTest();
        tl->setZoomForTest(400.0f);
        pump(200);
        const QImage surface = tl->surfaceForTest()->grab().toImage();
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            const QVariantMap parts = tl->clipPartsForTest(i);
            const QRect thumb = rectOf(parts.value(QStringLiteral("thumb")).toString());
            const bool shown = parts.value(QStringLiteral("thumbShown")).toBool();
            checkPin(QStringLiteral("clip.") + tag,
                     shown && thumb.isValid() ? shaOf(surface.copy(thumb))
                                              : QStringLiteral("no thumb"));
        }
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            page.scrubForTest(i);
            pump(100);
            QWidget *surf = page.previewSurface();
            checkPin(QStringLiteral("preview.") + tag,
                     page.previewVisible() && surf ? shaOf(surf->grab().toImage())
                                                   : QStringLiteral("no preview"));
        }
        page.leavePreview();
        page.loadScenes({});
        pump(100);
    }

    // ---- (c1) positive controls: every pin sees a 0.1 % push-in ------------
    // The same four panels, with ONE key on each panel's first frame that
    // zooms 1.001x: every output the pins cover must change. And the
    // "Full artwork" stills (the user's decision 3) must still be today's.
    ts << "--- (c1) positive controls ---\n";
    Scene *moved = new Scene;
    moved->number = 1;
    for (const QSize &size : kSizes)
        moved->panels.append(fixturePanel(size));
    for (int i = 0; i < 4; ++i) {
        shotcam::CameraState s = shotcam::neutral(kSizes[i]);
        s.zoom = 1.001;
        moved->camera.keys.append(shotcam::makeKey(*moved, i, 0, 24, s));
    }
    {
        const QVector<sankoexport::ExportPanel> rows =
            sankoexport::collectPanels({moved}, 24);
        const QVector<sankoexport::ExportPanel> full =
            sankoexport::collectPanels({moved}, 24, true);
        const sankoexport::FilesResult png = sankoexport::exportPng(
            rows, scratch + QStringLiteral("/png_moved"), QStringLiteral("Pin"), {});
        const sankoexport::FilesResult pngFull = sankoexport::exportPng(
            full, scratch + QStringLiteral("/png_full"), QStringLiteral("Pin"), {});
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            const QString mp4 = shaOf(sankoexport::renderMovieFrame(
                moved->panels.at(i), sankoexport::kMovieFrameSize,
                shotcam::evaluateAtPanel(*moved, i, 0, 24)));
            check(QStringLiteral("(c1) control: a 0.1 %% push-in changes the MP4 frame (%1)")
                      .arg(tag),
                  mp4 != pinned(QStringLiteral("mp4.") + tag), mp4.left(16));
            const QString p = png.ok ? shaOfFile(png.written.at(i)) : QString();
            check(QStringLiteral("(c1) control: ...and the PNG still (%1)").arg(tag),
                  png.ok && p != pinned(QStringLiteral("png.") + tag), p.left(16));
            const QString pf = pngFull.ok ? shaOfFile(pngFull.written.at(i)) : QString();
            check(QStringLiteral("(c1) \"Full artwork\" stills are today's bytes (%1)")
                      .arg(tag),
                  pngFull.ok && pf == pinned(QStringLiteral("png.") + tag), pf.left(16));
        }
        QImage page(1091, 748, QImage::Format_ARGB32);
        page.fill(Qt::white);
        {
            QPainter p(&page);
            sankoexport::paintBoardPage(p, page.size(), 100, rows, 0, QStringLiteral("Pin"));
        }
        check(QStringLiteral("(c1) control: ...and the PDF page"),
              shaOf(page) != pinned(QStringLiteral("pdf.page0")));
    }
    {
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen, true);
        host.resize(960, 540);
        host.show();
        AnimaticPage page;
        page.setAttribute(Qt::WA_DontShowOnScreen, true);
        page.resize(1600, 320);
        page.setFps(24);
        page.show();
        page.loadScenes({moved});
        page.attachPreview(&host);
        pump(200);
        AnimaticTimeline *tl = page.timelineForTest();
        tl->setZoomForTest(400.0f);
        pump(200);
        const QImage surface = tl->surfaceForTest()->grab().toImage();
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            const QRect thumb = rectOf(tl->clipPartsForTest(i)
                                           .value(QStringLiteral("thumb")).toString());
            const QString c = thumb.isValid() ? shaOf(surface.copy(thumb)) : QString();
            check(QStringLiteral("(c1) control: ...and the SHOTS clip (%1)").arg(tag),
                  thumb.isValid() && c != pinned(QStringLiteral("clip.") + tag),
                  c.left(16));
        }
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            page.scrubForTest(i);
            pump(100);
            const QString v = page.previewSurface()
                ? shaOf(page.previewSurface()->grab().toImage()) : QString();
            check(QStringLiteral("(c1) control: ...and the preview (%1)").arg(tag),
                  page.previewVisible() && v != pinned(QStringLiteral("preview.") + tag),
                  v.left(16));
        }
        page.leavePreview();
        page.loadScenes({});
        pump(100);
    }
    delete moved;

    // ---- (c2) the evaluation: closed forms --------------------------------
    ts << "--- (c2) evaluation ---\n";
    {
        Scene s;
        for (int i = 0; i < 3; ++i)
            s.panels.append(makeBlankPanel(QSize(1920, 1080)));
        const int fps = 24; // 72 frames a panel
        const shotcam::CameraState a = state(960, 540, 1.0, 0.0);
        const shotcam::CameraState b = state(1200, 600, 2.0, 30.0);
        const shotcam::Ease eases[] = {shotcam::Ease::Linear, shotcam::Ease::EaseIn,
                                       shotcam::Ease::EaseOut, shotcam::Ease::EaseInOut,
                                       shotcam::Ease::Hold};
        bool all = true;
        QString why;
        for (shotcam::Ease e : eases) {
            s.camera.keys = {shotcam::makeKey(s, 0, 0, fps, a, e),
                             shotcam::makeKey(s, 1, 36, fps, b)}; // frames 0 and 108
            for (double u : {0.25, 0.5, 0.75}) {
                const double w = e == shotcam::Ease::Linear ? u
                    : e == shotcam::Ease::EaseIn ? u * u
                    : e == shotcam::Ease::EaseOut ? 1 - (1 - u) * (1 - u)
                    : e == shotcam::Ease::EaseInOut ? 3 * u * u - 2 * u * u * u
                                                    : 0.0;
                const shotcam::CameraState want = state(
                    960 + 240 * w, 540 + 60 * w, std::exp(std::log(2.0) * w), 30 * w);
                const shotcam::CameraState got =
                    shotcam::evaluate(s, 108 * u, fps, QSize(1920, 1080));
                if (!sameState(got, want)) {
                    all = false;
                    why += QStringLiteral(" %1@%2 %3 vs %4")
                               .arg(shotcam::easeName(e)).arg(u)
                               .arg(stateText(got), stateText(want));
                }
            }
        }
        check(QStringLiteral("(c2) at 1/4, 1/2, 3/4 between two keys every ease "
                             "gives its closed form; the zoom is geometric "
                             "(sqrt 2 half way from 1 to 2)"),
              all, why);
        s.camera.keys = {shotcam::makeKey(s, 0, 0, fps, a, shotcam::Ease::Linear),
                         shotcam::makeKey(s, 1, 36, fps, b)};
        const shotcam::CameraState linMid = shotcam::evaluate(s, 54, fps, QSize(1920, 1080));
        s.camera.keys[0].ease = shotcam::Ease::EaseIn;
        const shotcam::CameraState inMid = shotcam::evaluate(s, 54, fps, QSize(1920, 1080));
        check(QStringLiteral("(c2) control: changing the first key's ease moves "
                             "the half-way framing"),
              !sameState(linMid, inMid), stateText(linMid) + QStringLiteral(" / ")
                                             + stateText(inMid));
        check(QStringLiteral("(c2) before the first key and after the last, the "
                             "camera holds that key"),
              sameState(shotcam::evaluate(s, 0, fps, QSize(1920, 1080)), a)
                  && sameState(shotcam::evaluate(s, 200, fps, QSize(1920, 1080)), b));
        s.camera.keys = {shotcam::makeKey(s, 0, 0, fps, state(960, 540, 1, 170),
                                          shotcam::Ease::Linear),
                         shotcam::makeKey(s, 1, 0, fps, state(960, 540, 1, -170))};
        check(QStringLiteral("(c2) rotation keeps the drawn direction: 170 to -170 "
                             "passes 0, the long way"),
              near(shotcam::evaluate(s, 36, fps, QSize(1920, 1080)).rotation, 0.0));
        s.camera.keys.clear();
        check(QStringLiteral("(c2) no keys: the neutral camera, exactly"),
              shotcam::isNeutral(shotcam::evaluate(s, 50, fps, QSize(1920, 1080)),
                                 QSize(1920, 1080)));
    }

    // ---- (c3) one evaluation path -----------------------------------------
    // A three-panel scene with a move from panel 1 into panel 3. At five
    // frames, the MP4 frame the REAL export job offered, the preview's
    // camera and transform, the clip's camera and the still's camera are
    // the camera evaluate() gives for that frame.
    ts << "--- (c3) one path ---\n";
    Scene *path = new Scene;
    path->number = 1;
    for (int i = 0; i < 3; ++i)
        path->panels.append(fixturePanel(QSize(1920, 1080)));
    path->camera.keys = {
        shotcam::makeKey(*path, 0, 12, 24, shotcam::neutral(QSize(1920, 1080)),
                         shotcam::Ease::Linear),
        shotcam::makeKey(*path, 2, 36, 24, state(1260, 640, 1.6, 8.0))};
    struct Sample
    {
        int panel, frame;
    };
    const Sample samples[] = {{0, 0}, {0, 30}, {1, 10}, {2, 20}, {2, 36}};
    {
        QSet<int> wanted;
        for (const Sample &s : samples)
            wanted.insert(s.panel * 72 + s.frame);
        sankoexport::MovieRequest request;
        request.outPath = scratch + QStringLiteral("/path.mp4");
        request.panels = sankoexport::collectPanels({path}, 24);
        request.fps = 24;
        auto *encoder = new CaptureEncoder(wanted);
        sankoexport::MovieExportJob job(request, encoder);
        job.start();
        QElapsedTimer guard;
        guard.start();
        while (!job.isDone() && guard.elapsed() < 120000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        bool same = job.result().ok;
        QString why = job.result().ok ? QString() : job.result().error;
        for (const Sample &s : samples) {
            const int f = s.panel * 72 + s.frame;
            const shotcam::CameraState cam =
                shotcam::evaluateAtPanel(*path, s.panel, s.frame, 24);
            const QString want = shaOf(sankoexport::renderMovieFrame(
                path->panels.at(s.panel), sankoexport::kMovieFrameSize, cam));
            const bool asked = sameState(sankoexport::movieFrameCamera(request.panels, 24, f), cam);
            if (encoder->shas.value(f) != want || !asked) {
                same = false;
                why += QStringLiteral(" frame %1").arg(f);
            }
        }
        check(QStringLiteral("(c3) the MP4 the real export job wrote: each sampled "
                             "frame is the camera's rendering of that frame"),
              same, why);
        const QSet<QString> distinct(encoder->shas.cbegin(), encoder->shas.cend());
        check(QStringLiteral("(c3) control: the sampled frames differ from each "
                             "other (the camera moved between them)"),
              distinct.size() >= 4, QString::number(distinct.size()));
    }
    {
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen, true);
        host.resize(960, 540);
        host.show();
        AnimaticPage page;
        page.setAttribute(Qt::WA_DontShowOnScreen, true);
        page.resize(1600, 320);
        page.setFps(24);
        page.show();
        page.loadScenes({path});
        page.attachPreview(&host);
        pump(200);
        page.scrubForTest(0);
        pump(50);
        bool previewSame = true;
        QString why;
        for (const Sample &s : samples) {
            page.setPlayheadForTest(s.panel, shotcam::msForFrameInPanel(s.frame, 24));
            const shotcam::CameraState cam =
                shotcam::evaluateAtPanel(*path, s.panel, s.frame, 24);
            const QTransform want = shotcam::shotTransform(
                cam, QSize(1920, 1080), QRectF(page.previewSurface()->rect()));
            const bool neutralPath = shotcam::isNeutral(cam, QSize(1920, 1080));
            if (!sameState(page.previewCameraForTest(), cam) && !neutralPath)
                previewSame = false, why += QStringLiteral(" state@%1/%2").arg(s.panel).arg(s.frame);
            if (page.previewCameraNeutralForTest() != neutralPath)
                previewSame = false, why += QStringLiteral(" neutral@%1/%2").arg(s.panel).arg(s.frame);
            if (!neutralPath && page.previewTransformForTest() != want)
                previewSame = false, why += QStringLiteral(" transform@%1/%2").arg(s.panel).arg(s.frame);
        }
        check(QStringLiteral("(c3) the preview draws each sampled frame with that "
                             "frame's camera and transform (neutral frames take the "
                             "old paint)"),
              previewSame, why);
        page.setPlayheadForTest(2, shotcam::msForFrameInPanel(21, 24));
        check(QStringLiteral("(c3) control: one frame later the preview's camera is "
                             "not that frame's"),
              !sameState(page.previewCameraForTest(),
                         shotcam::evaluateAtPanel(*path, 2, 20, 24)));
        AnimaticTimeline *tl = page.timelineForTest();
        bool clipsSame = true;
        const QVector<sankoexport::ExportPanel> rows = sankoexport::collectPanels({path}, 24);
        for (int i = 0; i < 3; ++i) {
            const shotcam::CameraState first = shotcam::evaluateAtPanel(*path, i, 0, 24);
            clipsSame = clipsSame && sameState(tl->clipCameraForTest(i), first)
                && sameState(rows.at(i).still, first);
        }
        check(QStringLiteral("(c3) every clip thumbnail and every still uses the "
                             "camera at its panel's first frame"),
              clipsSame);

        // ---- (c10) sync: the film's clock, quantised to its frames -----------
        ts << "--- (c10) sync and scrub ---\n";
        page.scrubToForTest(1, 10);
        const shotcam::CameraState at82 = shotcam::evaluateAtPanel(*path, 1, 10, 24);
        check(QStringLiteral("(c10) a scrub to frame 10 of panel 2 shows that frame's "
                             "camera, and the playhead stays there (timecode 00:00:03:10)"),
              sameState(page.previewCameraForTest(), at82)
                  && sameState(page.cameraAtPlayhead(), at82)
                  && page.timecodeTextForTest() == QStringLiteral("00:00:03:10"),
              page.timecodeTextForTest());
        // The real drag on the ruler: dropped on frame 10 of panel 2.
        {
            QWidget *surf = tl->surfaceForTest();
            const double ppf = tl->pxPerFrameForTest();
            // frameAtScreenX rounds to the nearest frame: a quarter frame in.
            const QPointF at(tl->clipRectForTest(1).left() + (10.25 * ppf), 10);
            const QPointF start(tl->clipRectForTest(0).center().x(), 10);
            QMouseEvent press(QEvent::MouseButtonPress, start, surf->mapToGlobal(start),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(surf, &press);
            QMouseEvent move(QEvent::MouseMove, at, surf->mapToGlobal(at),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(surf, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, at, surf->mapToGlobal(at),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(surf, &release);
            pump(100);
            check(QStringLiteral("(c10) a playhead dragged on the ruler and dropped "
                                 "inside panel 2 RESTS on the frame it was dropped on "
                                 "(not the panel's start), camera included"),
                  page.timecodeTextForTest() == QStringLiteral("00:00:03:10")
                      && sameState(page.previewCameraForTest(), at82),
                  page.timecodeTextForTest());
            check(QStringLiteral("(c10) control: that frame's camera is not the "
                                 "panel start's"),
                  !sameState(at82, shotcam::evaluateAtPanel(*path, 1, 0, 24)));
        }
        page.setPlayheadForTest(1, 500);
        page.cameraTickForTest();
        check(QStringLiteral("(c10) the camera tick reads the panel's clock: 500 ms "
                             "into panel 2 is its frame 12"),
              sameState(page.previewCameraForTest(),
                        shotcam::evaluateAtPanel(*path, 1, 12, 24)));
        const bool idle = !page.cameraTimerActiveForTest();
        page.togglePlay();
        pump(60);
        const bool playing = page.cameraTimerActiveForTest();
        page.togglePlay();
        pump(30);
        check(QStringLiteral("(c10) the 60 Hz camera tick runs while the film plays "
                             "and stops when it pauses"),
              idle && playing && !page.cameraTimerActiveForTest());

        // ---- (c11) the Dev Recorder's fields ---------------------------------
        ts << "--- (c11) recorder ---\n";
        page.setPlayheadForTest(2, shotcam::msForFrameInPanel(36, 24));
        const QVariantMap onKey = page.shotCamDevrecState();
        page.setPlayheadForTest(2, shotcam::msForFrameInPanel(35, 24));
        const QVariantMap offKey = page.shotCamDevrecState();
        const QStringList fields{"shotCam.scene", "shotCam.keys", "shotCam.frame",
                                 "shotCam.x", "shotCam.y", "shotCam.zoom",
                                 "shotCam.rotation", "shotCam.keyAtPlayhead",
                                 "shotCam.soft"};
        bool present = true;
        for (const QString &f : fields)
            present = present && onKey.contains(f);
        check(QStringLiteral("(c11) statePoll carries every shotCam. field; on the "
                             "key: keys 2, frame 180, zoom 1.6, keyAtPlayhead"),
              present && onKey.value(QStringLiteral("shotCam.keys")).toInt() == 2
                  && onKey.value(QStringLiteral("shotCam.frame")).toInt() == 180
                  && near(onKey.value(QStringLiteral("shotCam.zoom")).toDouble(), 1.6)
                  && onKey.value(QStringLiteral("shotCam.keyAtPlayhead")).toBool()
                  && onKey.value(QStringLiteral("shotCam.soft")).toBool(),
              QStringList(onKey.keys()).join(QLatin1Char(',')));
        check(QStringLiteral("(c11) control: one frame earlier it is not on a key "
                             "and the framing differs"),
              !offKey.value(QStringLiteral("shotCam.keyAtPlayhead")).toBool()
                  && offKey.value(QStringLiteral("shotCam.zoom")).toDouble()
                         != onKey.value(QStringLiteral("shotCam.zoom")).toDouble());
        page.leavePreview();
        page.loadScenes({});
        pump(100);
    }

    // ---- (c4) retiming: keys move with their panels ----------------------
    ts << "--- (c4) retiming ---\n";
    {
        Scene s;
        for (int i = 0; i < 4; ++i)
            s.panels.append(makeBlankPanel(QSize(64, 36))); // 3 s = 72 frames each
        const shotcam::CameraState k1s = state(20, 10, 1.5, 0);
        const shotcam::CameraState k2s = state(40, 20, 2.0, 0);
        s.camera.keys = {shotcam::makeKey(s, 1, 36, 24, k1s),  // frame 108
                         shotcam::makeKey(s, 3, 0, 24, k2s)};  // frame 216
        const shotcam::CameraKey &k1 = s.camera.keys.at(0);
        const shotcam::CameraKey &k2 = s.camera.keys.at(1);
        auto frames = [&] {
            return QStringLiteral("%1/%2").arg(shotcam::keyFrame(s, k1, 24))
                .arg(shotcam::keyFrame(s, k2, 24));
        };
        const bool base = frames() == QStringLiteral("108/216");
        s.panels.at(0)->duration = 5; // an EARLIER panel lengthens
        const QString earlier = frames();
        s.panels.at(0)->duration = 3;
        s.panels.at(1)->duration = 4; // the key's OWN panel lengthens
        const QString own = frames();
        s.panels.at(1)->duration = 3;
        Panel *inserted = makeBlankPanel(QSize(64, 36));
        s.panels.insert(1, inserted); // a panel inserted before K1
        const QString insert = frames();
        s.panels.removeAt(1);
        delete inserted;
        Panel *k1Panel = s.panels.takeAt(1); // K1's panel deleted ...
        const QString deleted = frames();
        const int liveAfterDelete = shotcam::liveKeyCount(s);
        s.panels.insert(1, k1Panel); // ... and the delete undone (same Panel)
        const QString undone = frames();
        s.panels.move(3, 0); // K2's panel moved to the front
        const QString reordered = frames();
        s.panels.move(0, 3);
        const QString fps60 = QStringLiteral("%1/%2").arg(shotcam::keyFrame(s, k1, 60))
                                  .arg(shotcam::keyFrame(s, k2, 60));
        check(QStringLiteral("(c4) anchored keys: an earlier panel +2 s ripples both "
                             "(156/264); their own panel +1 s keeps K1 half way "
                             "through it (120/240); an insert ripples (180/288); a "
                             "delete makes K1 inert and its undo brings it back "
                             "(-1/144 then 108/216); a reorder carries K2 to the "
                             "front (180/0); 60 fps scales (270/540)"),
              base && earlier == QStringLiteral("156/264")
                  && own == QStringLiteral("120/240")
                  && insert == QStringLiteral("180/288")
                  && deleted == QStringLiteral("-1/144") && liveAfterDelete == 1
                  && undone == QStringLiteral("108/216")
                  && reordered == QStringLiteral("180/0")
                  && fps60 == QStringLiteral("270/540"),
              QStringList{earlier, own, insert, deleted, undone, reordered, fps60}
                  .join(QLatin1Char(' ')));
        // Positive control: fixed scene time (frames stored, not panels)
        // would have answered 108/216 after every one of those edits.
        check(QStringLiteral("(c4) control: those edits really moved the panels - a "
                             "fixed-time key would still say 108/216 for each"),
              earlier != QStringLiteral("108/216") && own != QStringLiteral("108/216")
                  && insert != QStringLiteral("108/216")
                  && reordered != QStringLiteral("108/216"));
        Panel copy(*s.panels.at(1));
        check(QStringLiteral("(c4) a copied panel (duplicate, paste) gets its own "
                             "identity: no key follows it"),
              copy.uid.value() != s.panels.at(1)->uid.value()
                  && shotcam::liveKeysOnPanel(s, s.panels.at(1)) == 1);
    }

    // ---- (c8) the file ----------------------------------------------------
    ts << "--- (c8) save, load, resize ---\n";
    {
        Scene *s = new Scene;
        s->number = 1;
        // As a scene from the Script Editor has it: the loader turns an
        // EMPTY time of day into "UNSPECIFIED" (a rule older than the
        // camera), which a re-save would then carry.
        s->timeOfDay = QStringLiteral("DAY");
        for (int i = 0; i < 3; ++i)
            s->panels.append(fixturePanel(QSize(320, 180)));
        ProjectIO::SaveData data;
        data.projectName = QStringLiteral("Cam");
        data.fps = 24;
        data.canvasSize = QSize(320, 180);
        data.scenes = {s};
        const QString file = scratch + QStringLiteral("/proj/Cam.sankotv");
        QDir().mkpath(scratch + QStringLiteral("/proj"));
        auto scenesJson = [](const QJsonObject &root) {
            return QJsonDocument(root.value(QStringLiteral("scenes")).toArray())
                .toJson(QJsonDocument::Compact);
        };
        // A SCENE's "camera" (each PANEL already writes its camera-angle
        // text under "camera", one level down: not this).
        auto sceneHasCamera = [](const QJsonObject &root) {
            for (const QJsonValue &v : root.value(QStringLiteral("scenes")).toArray())
                if (v.toObject().contains(QStringLiteral("camera")))
                    return true;
            return false;
        };
        const ProjectIO::WriteResult plain = ProjectIO::projectToJson(data, file);
        ProjectIO::LoadedProject back =
            ProjectIO::projectFromJson(plain.root, scratch + QStringLiteral("/proj"));
        bool neutralBack = back.scenes.size() == 1 && back.scenes.at(0)->camera.isEmpty();
        ProjectIO::SaveData again = data;
        again.scenes = back.scenes;
        const ProjectIO::WriteResult plain2 = ProjectIO::projectToJson(again, file);
        const QByteArray j1 = scenesJson(plain.root);
        const QByteArray j2 = scenesJson(plain2.root);
        int firstDiff = -1;
        for (int i = 0; i < qMin(j1.size(), j2.size()); ++i)
            if (j1.at(i) != j2.at(i)) {
                firstDiff = i;
                break;
            }
        check(QStringLiteral("(c8) no camera: no \"camera\" is written, the project "
                             "loads with the neutral camera, and saves the same "
                             "scenes JSON again"),
              plain.ok && plain2.ok && neutralBack && !sceneHasCamera(plain.root)
                  && j1 == j2,
              QStringLiteral("ok %1/%2 neutral %3 camera %4 sizes %5/%6 first diff at %7: [%8] vs [%9]")
                  .arg(plain.ok).arg(plain2.ok).arg(neutralBack)
                  .arg(sceneHasCamera(plain.root)).arg(j1.size()).arg(j2.size())
                  .arg(firstDiff)
                  .arg(QString::fromUtf8(j1.mid(qMax(0, firstDiff - 60), 140)),
                       QString::fromUtf8(j2.mid(qMax(0, firstDiff - 60), 140))));
        for (Scene *sc : back.scenes)
            delete sc;

        s->camera.keys = {
            shotcam::makeKey(*s, 0, 12, 24, state(100.25, 80.5, 1.3333333333333333, -7.5),
                             shotcam::Ease::EaseOut),
            shotcam::makeKey(*s, 2, 50, 24, state(200, 90, 2.0, 12), shotcam::Ease::Hold)};
        const ProjectIO::WriteResult keyed = ProjectIO::projectToJson(data, file);
        // Through the TEXT a file holds, as a real save and open do.
        const QJsonObject keyedText =
            QJsonDocument::fromJson(QJsonDocument(keyed.root).toJson()).object();
        ProjectIO::LoadedProject kb =
            ProjectIO::projectFromJson(keyedText, scratch + QStringLiteral("/proj"));
        bool exact = kb.scenes.size() == 1
            && kb.scenes.at(0)->camera.keys.size() == 2 && kb.cameraKeysDropped == 0;
        for (int i = 0; exact && i < 2; ++i) {
            const shotcam::CameraKey &o = s->camera.keys.at(i);
            const shotcam::CameraKey &n = kb.scenes.at(0)->camera.keys.at(i);
            exact = o.at == n.at && o.state == n.state && o.ease == n.ease
                && shotcam::keyFrame(*s, o, 24) == shotcam::keyFrame(*kb.scenes.at(0), n, 24);
        }
        check(QStringLiteral("(c8) keys round-trip exactly: panel, place, framing "
                             "and ease"),
              keyed.ok && exact);
        check(QStringLiteral("(c8) control: one camera writes \"camera\" and changes "
                             "the scenes JSON"),
              sceneHasCamera(keyed.root) && scenesJson(keyed.root) != j1);
        QJsonObject broken = keyed.root;
        QJsonArray scenesArray = broken.value(QStringLiteral("scenes")).toArray();
        QJsonObject scene0 = scenesArray.at(0).toObject();
        QJsonObject cam = scene0.value(QStringLiteral("camera")).toObject();
        QJsonArray keys = cam.value(QStringLiteral("keys")).toArray();
        QJsonObject bad = keys.at(0).toObject();
        bad[QStringLiteral("panel")] = 99;
        keys.append(bad);
        cam[QStringLiteral("keys")] = keys;
        scene0[QStringLiteral("camera")] = cam;
        scenesArray[0] = scene0;
        broken[QStringLiteral("scenes")] = scenesArray;
        ProjectIO::LoadedProject bb =
            ProjectIO::projectFromJson(broken, scratch + QStringLiteral("/proj"));
        check(QStringLiteral("(c8) a key naming a panel that is not there is left "
                             "out and counted, never guessed"),
              bb.cameraKeysDropped == 1 && bb.scenes.size() == 1
                  && bb.scenes.at(0)->camera.keys.size() == 2);
        for (Scene *sc : kb.scenes)
            delete sc;
        for (Scene *sc : bb.scenes)
            delete sc;

        // Resize (centre-anchored): the framing stays over the same artwork.
        shotcam::SceneCamera r;
        r.keys = {s->camera.keys.at(0)};
        const QSize oldSize(320, 180), newSize(400, 200);
        const QPoint offset((newSize.width() - oldSize.width()) / 2,
                            (newSize.height() - oldSize.height()) / 2);
        const shotcam::CameraState before = r.keys.at(0).state;
        shotcam::remapForResize(r, oldSize, newSize, offset);
        const shotcam::CameraState after = r.keys.at(0).state;
        check(QStringLiteral("(c8) a resize moves the frame with the artwork and "
                             "keeps its pixel width"),
              near(after.cx, before.cx + offset.x()) && near(after.cy, before.cy + offset.y())
                  && near(newSize.width() / after.zoom, oldSize.width() / before.zoom));
        check(QStringLiteral("(c8) control: without the remap the frame's centre is "
                             "over other artwork"),
              !near(before.cx, before.cx + offset.x()));
        delete s;
    }
    delete path;

    // ---- (c12) the canvas tool's limits and key lookups (Phase 1B) -------
    ts << "--- (c12) the canvas tool's limits ---\n";
    {
        const QSize cs(1920, 1080);
        auto onCanvas = [&](const shotcam::CameraState &s) {
            for (const QPointF &p : shotcam::frameQuad(s, cs))
                if (p.x() < -1e-6 || p.y() < -1e-6 || p.x() > cs.width() + 1e-6
                    || p.y() > cs.height() + 1e-6)
                    return false;
            return true;
        };
        check(QStringLiteral("(c12) the zoom a turned frame needs to fit: 1 unturned; "
                             "(W s + H c) / H at 90 degrees"),
              near(shotcam::insideMinZoom(0.0, cs), 1.0)
                  && near(shotcam::insideMinZoom(90.0, cs), 1920.0 / 1080.0, 1e-9)
                  && near(shotcam::insideMinZoom(-90.0, cs), 1920.0 / 1080.0, 1e-9));
        shotcam::CameraState wild{-500.0, 5000.0, 0.3, 37.0};
        const shotcam::CameraState kept = shotcam::clampInside(wild, cs);
        check(QStringLiteral("(c12) clampInside: a frame far off, too wide and turned "
                             "37 degrees comes back with every corner on the canvas, "
                             "at the least zoom that allows (and its turn kept)"),
              onCanvas(kept) && kept.rotation == 37.0
                  && near(kept.zoom, shotcam::insideMinZoom(37.0, cs), 1e-9),
              QStringLiteral("%1,%2 z%3").arg(kept.cx).arg(kept.cy).arg(kept.zoom));
        const shotcam::CameraState loose = shotcam::clampZoom(wild);
        check(QStringLiteral("(c12) control: the zoom caps alone (Stay inside off) keep "
                             "0.5..4 and leave the frame off the canvas"),
              loose.zoom == shotcam::kMinZoom && !onCanvas(loose)
                  && shotcam::clampZoom({0, 0, 9.0, 0}).zoom == shotcam::kMaxZoom);
        const shotcam::CameraState inner{960, 540, 2.0, 0.0};
        check(QStringLiteral("(c12) a frame already inside is left exactly as it is"),
              shotcam::clampInside(inner, cs) == inner);
        check(QStringLiteral("(c12) angles fold into (-180, 180]"),
              shotcam::normalisedAngle(190.0) == -170.0
                  && shotcam::normalisedAngle(-180.0) == 180.0
                  && shotcam::normalisedAngle(540.0) == 180.0
                  && shotcam::normalisedAngle(15.0) == 15.0);
        check(QStringLiteral("(c12) the timecode: HH:MM:SS:FF of a film frame"),
              shotcam::timecodeText(24 * 61 + 5, 24) == QStringLiteral("00:01:01:05")
                  && shotcam::timecodeText(0, 30) == QStringLiteral("00:00:00:00"));

        Scene *k = new Scene;
        for (int i = 0; i < 3; ++i) {
            Panel *p = new Panel;
            p->duration = 2;
            k->panels.append(p);
        }
        const shotcam::CameraState n = shotcam::neutral(cs);
        k->camera.keys = {shotcam::makeKey(*k, 1, 0, 24, n),   // frame 48
                          shotcam::makeKey(*k, 0, 10, 24, n)}; // frame 10
        const QVector<double> frames = shotcam::liveKeyFrames(*k, 24);
        check(QStringLiteral("(c12) key lookups: the live frames in order; the key AT a "
                             "frame; the key whose segment a frame is in"),
              frames == QVector<double>({10.0, 48.0})
                  && shotcam::keyIndexAtFrame(*k, 48.0, 24) == 0
                  && shotcam::keyIndexAtFrame(*k, 47.0, 24) == -1
                  && shotcam::segmentKeyIndex(*k, 47.0, 24) == 1
                  && shotcam::segmentKeyIndex(*k, 9.0, 24) == -1
                  && shotcam::segmentKeyIndex(*k, 100.0, 24) == 0);
        Panel *gone = k->panels.takeAt(1); // its key goes inert
        check(QStringLiteral("(c12) an inert key (its panel deleted) is in no lookup"),
              shotcam::liveKeyFrames(*k, 24) == QVector<double>({10.0})
                  && shotcam::keyIndexAtFrame(*k, 48.0, 24) == -1);
        delete gone;
        delete k;
    }

    delete scene;
    QDir(scratch).removeRecursively();
    ts << "\nRESULT " << (g_failures ? "FAIL" : "PASS") << " (checks=" << g_checks
       << " failures=" << g_failures << ")\n";
    outFile.close();
    return g_failures;
}
