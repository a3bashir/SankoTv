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
#include "NewProjectDialog.h"
#include "ProjectIO.h"
#include "StageMarginDialog.h"
#include "brushlib/StudioControls.h"
#include "StoryboardModel.h"
#include "camera/ShotCamera.h"
#include "export/MovieEncoder.h"
#include "export/ProjectExport.h"

#include <QApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMap>
#include <QMouseEvent>
#include <QSet>
#include <QtMath>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
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

// THE DEPTH FIXTURE (Phase 2B, dp): the Figma 2B-3 scene - Sky 0 %,
// Mountains 50 %, House on the picture plane, Bush 150 % - each a layer with
// line work and one solid 40 px MARKER square in its own colour, so where a
// layer went can be read off a pixel. Index: 0 paper, 1 Sky, 2 Mountains,
// 3 House, 4 Bush.
struct DepthMarker
{
    QPointF centre;
    QColor colour;
};
const DepthMarker kDepthMarkers[5] = {{{0, 0}, Qt::white},
                                      {{300, 260}, QColor(220, 30, 30)},
                                      {{900, 500}, QColor(30, 170, 60)},
                                      {{1400, 620}, QColor(30, 60, 220)},
                                      {{620, 820}, QColor(200, 30, 200)}};
const double kDepths[5] = {1.0, 0.0, 0.5, 1.0, 1.5};

Panel *depthPanel(const QSize &size = QSize(1920, 1080), const QPoint &offset = QPoint())
{
    Panel *panel = makeBlankPanel(size);
    panel->layers.removeLast();
    const char *names[] = {"", "Sky", "Mountains", "House", "Bush"};
    for (int i = 1; i < 5; ++i) {
        Layer layer = makeRasterLayer(QString::fromLatin1(names[i]), size);
        QPainter p(&layer.image);
        p.translate(offset);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(kDepthMarkers[i].colour.darker(150), 3));
        for (int k = 0; k < 6; ++k) // line work in a band below every marker
            p.drawLine(QPointF(160 * i + 90 * k, 960), QPointF(160 * i + 90 * k + 300, 1060));
        p.fillRect(QRectF(kDepthMarkers[i].centre - QPointF(20, 20), QSizeF(40, 40)),
                   kDepthMarkers[i].colour);
        p.end();
        layer.parallax = kDepths[i];
        panel->layers.append(layer);
    }
    panel->activeLayerIndex = 1;
    panel->duration = 3;
    return panel;
}

// Where layer `i`'s marker centre shows in a shot of `out` size.
QPointF markerInShot(const Panel &panel, int i, const shotcam::CameraState &cam,
                     const shotcam::Stage &stage, const QSize &out, const QPoint &offset = QPoint())
{
    const double a = panel.effectiveParallax(panel.layers.at(i));
    return (shotcam::layerToStage(a, cam, stage)
            * shotcam::shotTransform(cam, stage, QRectF(QPointF(0, 0), out)))
        .map(kDepthMarkers[i].centre + QPointF(offset));
}

bool colourNear(const QColor &a, const QColor &b, int tol = 12)
{
    return qAbs(a.red() - b.red()) <= tol && qAbs(a.green() - b.green()) <= tol
        && qAbs(a.blue() - b.blue()) <= tol;
}

// The four canvas sizes the pins cover: the three presets and an odd one.
const QSize kSizes[4] = {{1920, 1080}, {2048, 1080}, {3840, 2160}, {1001, 777}};

// A STAGE MARGIN fixture (Phase 2A, mg1): the fixture panel of `frame`,
// every layer placed whole at the centre of a stage `margin` px larger on
// each side, with other art (filled bands) drawn in the margin only. Its
// shot at the base framing must be the frame-sized fixture's, byte for byte.
Panel *marginPanel(const QSize &frame, const QSize &margin)
{
    Panel *src = fixturePanel(frame);
    const QSize stage(frame.width() + 2 * margin.width(), frame.height() + 2 * margin.height());
    Panel *panel = new Panel;
    panel->duration = src->duration;
    for (int i = 0; i < src->layers.size(); ++i) {
        Layer copy = src->layers.at(i);
        QImage big(stage, QImage::Format_ARGB32_Premultiplied);
        big.fill(copy.type == QLatin1String("background") ? QColor(Qt::white)
                                                           : QColor(Qt::transparent));
        QPainter p(&big);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(margin.width(), margin.height(), src->layers.at(i).image);
        if (i == 1) { // the margin's own art, outside the frame
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            const QColor band(200, 0, 120, 220);
            p.fillRect(QRect(0, 0, margin.width(), stage.height()), band);
            p.fillRect(QRect(stage.width() - margin.width(), 0, margin.width(), stage.height()),
                       band);
            p.fillRect(QRect(0, 0, stage.width(), margin.height()), band);
            p.fillRect(QRect(0, stage.height() - margin.height(), stage.width(), margin.height()),
                       band);
        }
        p.end();
        copy.image = big;
        panel->layers.append(copy);
    }
    panel->activeLayerIndex = src->activeLayerIndex;
    delete src;
    return panel;
}

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

// THE MOVING PINS (mp): recorded on 32763eb6b, before Phase 2A's stage
// margin rewrote every camera function's canvas into a frame and a stage
// (the Phase 2 spec, 8.1). What a project WITH camera moves rendered then:
// a project without a margin must render these bytes for ever.
const Pin kMovingPins[] = {
    {"clip.1001x777.p1", "2accf11ac555f4470e935a14207fb33102a3361d4186b25f959b189fc4ee2348"},
    {"clip.1001x777.p2", "c9c608dbd0eefb67221a6d7f941c5fbcff18e5f1e3739a699d617b4f435c3683"},
    {"clip.1920x1080.p1", "152733aa11048019c847cee7962ad3ab4e621f75d68530a45b4f98b8096b8263"},
    {"clip.1920x1080.p2", "11241632deac9b256df1fedc25cd6074fe446eb83a94f84a7acfa5b4c265a970"},
    {"clip.2048x1080.p1", "865a0214de00157cee8e27fdb4cfd2729ab741dd553a86482dc60cdcaa881c1c"},
    {"clip.2048x1080.p2", "faa8fa41434e3177a12dec533f9cf040e75d5cab36c659ce4b4a067cb4076607"},
    {"clip.3840x2160.p1", "f7c28c2bb02319805b8a434816ee13e2a4cc418cd0c5768285921ef7da35baf5"},
    {"clip.3840x2160.p2", "5ea62a720bfb6a8fea6323df780315c4c7fd7dabdcf1dcf9a04b00a85b202a51"},
    {"geometry", "7491456e39c08683270894b0e979b30f0f88e9d9c21a0469ca41fc55815323c6"},
    {"mp4.1001x777.f102", "e822b3637cea41129d9f7b7d998cf5eb5c54f338c2fb81f6efa204b611b24e3c"},
    {"mp4.1001x777.f35", "a228adfa196dd487cd114cbbbb6035299acf766fa9bc1c158dc6f9cfcf184be4"},
    {"mp4.1001x777.f50", "4cc088e8fde0de7f8643e14a98292a94eba9e9bc7c8051b537fc9cf1a4092f94"},
    {"mp4.1001x777.f64", "9aeb6c943dd904aad7a4049bdc726a8da9c8cdc2c72905e43cb5025829a54d86"},
    {"mp4.1001x777.f7", "8ecedd5d24a8a52daa33d20046eeec31af3eaa2fe07c2d19cf64c69e9c69fbe3"},
    {"mp4.1920x1080.f102", "e2f0d413726867a5bd3828f7aa22ae11597720a293857e4334da9ffad16654a6"},
    {"mp4.1920x1080.f35", "ca72016dbdc4d62fd26a3b2fb6da28d36e6360653323b1de33f2bb0fee817fb6"},
    {"mp4.1920x1080.f50", "abc98ad4503ddabe9d9c310c25ddec35342ea72567f0652db09f0a1caa8e4db5"},
    {"mp4.1920x1080.f64", "779b999217d6896de571edb96d0e4c0cb70a050c3b4b80bdf9624a239dab83a5"},
    {"mp4.1920x1080.f7", "791a7f39c948ae9db025b81e49372064437e448c03b004608a154113d7fb4460"},
    {"mp4.2048x1080.f102", "729316275cc03774407f6dddf69cba8e753c6ef4d829244ce07c1b79a12624f2"},
    {"mp4.2048x1080.f35", "a4343276036b04db7a721cd8100e901e87e50f976a21ce28bdf16ba70796b01b"},
    {"mp4.2048x1080.f50", "31080745b39b6920fc12fb6d157ee4003a50f95d23906460a97e647f73fe5db3"},
    {"mp4.2048x1080.f64", "c153c77013f365c27eaa10fc4e759396dca4c89ac4bf71307483387804feec8b"},
    {"mp4.2048x1080.f7", "99fbd33c5a87a832a4078e0ca4a18114a19e6bbb90af47cc7d70ac120d58eb61"},
    {"mp4.3840x2160.f102", "da08563f2b9a143dea8c21859a97ce58eccf942f031a78325ae3b74c5fbd0c8b"},
    {"mp4.3840x2160.f35", "65da98376bdc3d7b4f754a1e84ae4b50bc870e5b0f43cdc1329aa91bfe724941"},
    {"mp4.3840x2160.f50", "b9db097e61146dae3d582111ce378baf517dea149563fa34edf9649426d4f235"},
    {"mp4.3840x2160.f64", "c1f0e6f9e65a703d90631340ffe8787b69420d844fc985a3f524d90cfd0540ad"},
    {"mp4.3840x2160.f7", "ba36988fecc7dece976702ab1557ba1aaac1f8fc871691c0464860b364a50158"},
    {"pdf.page0", "26dac732de30bd69b3db0f3b3dd8ada74df0351bbafc97594c6cde9829b7cad0"},
    {"png.1001x777.p1", "d133429b56cf8ae72391ca821ed1617d443191a2adcf90b2a2acc829757397b3"},
    {"png.1001x777.p2", "de9ee4c7aa8b6d4754be5da75f6df4ee4f80008bc88fa9b6bfe7963056957321"},
    {"png.1920x1080.p1", "40a4fc2d48c5b7118bca1a348df0f4488897dd09175547baf8cd5cc2d24f944e"},
    {"png.1920x1080.p2", "5a603de51d74dbc1b6abd59b275db78de591a8cc6269d857af08bc027f1d452b"},
    {"png.2048x1080.p1", "516ea162b5c74653da2ca2f7d032d4e027a00f2e50a5ee82a94059ccbbfa28fe"},
    {"png.2048x1080.p2", "e850bb8385904be35d23b58170e7e5e15163139c451b0e0e1d42924afc37c23b"},
    {"png.3840x2160.p1", "2b933bb5f1842d088ba0220a78d5a9e1b80c7e3c18906be88f0712b5a992aad7"},
    {"png.3840x2160.p2", "958c12bf319fbf7ff8e976aa0d3b5e6e54d3e82b21f66e2effbbdf62efc63aad"},
    {"preview.1001x777.f50", "78286c3d49e1acce4dfa1c0736acbc5332e39cf48ac46f32916aae6c14f3ee54"},
    {"preview.1920x1080.f50", "ca07a614060b69d8bbc1ac7272872d245da9393ad5ae6d029d6cf0d3a3021ae6"},
    {"preview.2048x1080.f50", "a913f1a95fc39bbb81388c4cb1c88242bf3c56610f4fee67363d13a5a2c5ee9a"},
    {"preview.3840x2160.f50", "c9ad14cd60f502785bec5be4f81b6c1e3333f59cae6f088c3a6655036cdd821a"},
};

QString movingPinned(const QString &name)
{
    for (const Pin &p : kMovingPins)
        if (name == QLatin1String(p.name))
            return QString::fromLatin1(p.sha);
    return QString();
}

void checkMovingPin(const QString &name, const QString &sha)
{
    const QString want = movingPinned(name);
    check(QStringLiteral("(mp) %1 renders its pinned bytes").arg(name),
          !want.isEmpty() && sha == want,
          want.isEmpty() ? QStringLiteral("RECORD {\"%1\", \"%2\"},").arg(name, sha)
                         : sha.left(16));
}

// THE MARGIN PINS (mq): recorded on 35ea9ab05, before Phase 2B's depth (the
// Phase 2 spec, 8.1). The moving pins' scenes inside a stage margin of
// 480 x 270 px each side (25 % of a 1080p frame; one project, one margin):
// what a margin project with camera moves renders after 2A. A project with
// a margin and no depth must render these bytes for ever. (A KEYLESS
// margin scene is pinned by (mg1): it renders the 17 neutral pins' bytes.)
const QSize kMarginPinMargin(480, 270);
const QMap<QString, QString> kMarginPins = {
    {"clip.1001x777.p1", "2accf11ac555f4470e935a14207fb33102a3361d4186b25f959b189fc4ee2348"},
    {"clip.1001x777.p2", "4c601adadcffa1acf4e3575545f5db1c3db20547b6134de76b26f59d7d31ad39"},
    {"clip.1920x1080.p1", "152733aa11048019c847cee7962ad3ab4e621f75d68530a45b4f98b8096b8263"},
    {"clip.1920x1080.p2", "326402d420a05a7c1417e894a7e27a35359d1d541756f9505ad5438a2cdea45a"},
    {"clip.2048x1080.p1", "865a0214de00157cee8e27fdb4cfd2729ab741dd553a86482dc60cdcaa881c1c"},
    {"clip.2048x1080.p2", "9294042f04f9f754bdcc8f03c93bc5153d9ba4cfce3a90487cd5cd873a26f6dc"},
    {"clip.3840x2160.p1", "f7c28c2bb02319805b8a434816ee13e2a4cc418cd0c5768285921ef7da35baf5"},
    {"clip.3840x2160.p2", "39d701b46b7fb791bba9b37f35a1722088448d814bf807b7f1c545ccf796a15a"},
    {"geometry", "94c6580cedecbf1882cd30b5ad84ce346726a87ae0d5f8e0a6e966b9d2a51fa2"},
    {"mp4.1001x777.f102", "e822b3637cea41129d9f7b7d998cf5eb5c54f338c2fb81f6efa204b611b24e3c"},
    {"mp4.1001x777.f35", "a228adfa196dd487cd114cbbbb6035299acf766fa9bc1c158dc6f9cfcf184be4"},
    {"mp4.1001x777.f50", "4cc088e8fde0de7f8643e14a98292a94eba9e9bc7c8051b537fc9cf1a4092f94"},
    {"mp4.1001x777.f64", "9aeb6c943dd904aad7a4049bdc726a8da9c8cdc2c72905e43cb5025829a54d86"},
    {"mp4.1001x777.f7", "8ecedd5d24a8a52daa33d20046eeec31af3eaa2fe07c2d19cf64c69e9c69fbe3"},
    {"mp4.1920x1080.f102", "e2f0d413726867a5bd3828f7aa22ae11597720a293857e4334da9ffad16654a6"},
    {"mp4.1920x1080.f35", "ca72016dbdc4d62fd26a3b2fb6da28d36e6360653323b1de33f2bb0fee817fb6"},
    {"mp4.1920x1080.f50", "abc98ad4503ddabe9d9c310c25ddec35342ea72567f0652db09f0a1caa8e4db5"},
    {"mp4.1920x1080.f64", "779b999217d6896de571edb96d0e4c0cb70a050c3b4b80bdf9624a239dab83a5"},
    {"mp4.1920x1080.f7", "791a7f39c948ae9db025b81e49372064437e448c03b004608a154113d7fb4460"},
    {"mp4.2048x1080.f102", "729316275cc03774407f6dddf69cba8e753c6ef4d829244ce07c1b79a12624f2"},
    {"mp4.2048x1080.f35", "a4343276036b04db7a721cd8100e901e87e50f976a21ce28bdf16ba70796b01b"},
    {"mp4.2048x1080.f50", "31080745b39b6920fc12fb6d157ee4003a50f95d23906460a97e647f73fe5db3"},
    {"mp4.2048x1080.f64", "c153c77013f365c27eaa10fc4e759396dca4c89ac4bf71307483387804feec8b"},
    {"mp4.2048x1080.f7", "99fbd33c5a87a832a4078e0ca4a18114a19e6bbb90af47cc7d70ac120d58eb61"},
    {"mp4.3840x2160.f102", "da08563f2b9a143dea8c21859a97ce58eccf942f031a78325ae3b74c5fbd0c8b"},
    {"mp4.3840x2160.f35", "65da98376bdc3d7b4f754a1e84ae4b50bc870e5b0f43cdc1329aa91bfe724941"},
    {"mp4.3840x2160.f50", "b9db097e61146dae3d582111ce378baf517dea149563fa34edf9649426d4f235"},
    {"mp4.3840x2160.f64", "c1f0e6f9e65a703d90631340ffe8787b69420d844fc985a3f524d90cfd0540ad"},
    {"mp4.3840x2160.f7", "ba36988fecc7dece976702ab1557ba1aaac1f8fc871691c0464860b364a50158"},
    {"pdf.page0", "26dac732de30bd69b3db0f3b3dd8ada74df0351bbafc97594c6cde9829b7cad0"},
    {"png.1001x777.p1", "d133429b56cf8ae72391ca821ed1617d443191a2adcf90b2a2acc829757397b3"},
    {"png.1001x777.p2", "de9ee4c7aa8b6d4754be5da75f6df4ee4f80008bc88fa9b6bfe7963056957321"},
    {"png.1920x1080.p1", "40a4fc2d48c5b7118bca1a348df0f4488897dd09175547baf8cd5cc2d24f944e"},
    {"png.1920x1080.p2", "5a603de51d74dbc1b6abd59b275db78de591a8cc6269d857af08bc027f1d452b"},
    {"png.2048x1080.p1", "516ea162b5c74653da2ca2f7d032d4e027a00f2e50a5ee82a94059ccbbfa28fe"},
    {"png.2048x1080.p2", "e850bb8385904be35d23b58170e7e5e15163139c451b0e0e1d42924afc37c23b"},
    {"png.3840x2160.p1", "2b933bb5f1842d088ba0220a78d5a9e1b80c7e3c18906be88f0712b5a992aad7"},
    {"png.3840x2160.p2", "958c12bf319fbf7ff8e976aa0d3b5e6e54d3e82b21f66e2effbbdf62efc63aad"},
    {"preview.1001x777.f50", "78286c3d49e1acce4dfa1c0736acbc5332e39cf48ac46f32916aae6c14f3ee54"},
    {"preview.1920x1080.f50", "ca07a614060b69d8bbc1ac7272872d245da9393ad5ae6d029d6cf0d3a3021ae6"},
    {"preview.2048x1080.f50", "a913f1a95fc39bbb81388c4cb1c88242bf3c56610f4fee67363d13a5a2c5ee9a"},
    {"preview.3840x2160.f50", "c9ad14cd60f502785bec5be4f81b6c1e3333f59cae6f088c3a6655036cdd821a"},
};

// THE ROW PINS' EVALUATION HALF (rp; the Phase 2 spec's 8.1, before 2D):
// evaluate() at EVERY frame of the moving fixtures, with and without the
// margin, as text with 12 significant digits, hashed. Recorded on
// 2d8116226 before any 2D code; 2D (the waveform) and 2E (pinned keys,
// which gain the scene's film start) must give these numbers for a
// project without pinned keys.
const QMap<QString, QString> kEvaluatePins = {
    {"evaluate.moving", "e43a30f9fcc11db6550911a6d4aa20cedb4aa6fa17afc2ed15ca7723d9817007"},
    {"evaluate.margin", "7bd38237a37a75794c03e0ad80232a85129c1e730dff60ba927e1bb8b9b0a846"},
};

void checkEvaluatePin(const QString &name, const QString &sha)
{
    const QString want = kEvaluatePins.value(name);
    check(QStringLiteral("(rp) %1: evaluate() at every frame of the moving fixtures gives its "
                         "pinned numbers").arg(name),
          !want.isEmpty() && sha == want,
          want.isEmpty() ? QStringLiteral("RECORD {\"%1\", \"%2\"},").arg(name, sha)
                         : sha.left(16));
}

// The text the evaluate pin hashes: every scene, every frame of its 144.
QString evaluateText(const QVector<Scene *> &list)
{
    QString g;
    QTextStream gs(&g);
    gs.setRealNumberPrecision(12);
    for (int i = 0; i < list.size(); ++i) {
        const Scene *s = list.at(i);
        const QSize cs = s->panels.first()->canvasSize();
        for (int f = 0; f < 144; ++f) {
            const shotcam::CameraState st = shotcam::evaluate(*s, f, 24, cs);
            gs << i << ' ' << f << ' ' << st.cx << ' ' << st.cy << ' ' << st.zoom << ' '
               << st.rotation << '\n';
        }
    }
    gs.flush();
    return QString::fromLatin1(
        QCryptographicHash::hash(g.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void checkMarginPin(const QString &name, const QString &sha)
{
    const QString want = kMarginPins.value(name);
    check(QStringLiteral("(mq) %1 renders its pinned bytes").arg(name),
          !want.isEmpty() && sha == want,
          want.isEmpty() ? QStringLiteral("RECORD {\"%1\", \"%2\"},").arg(name, sha)
                         : sha.left(16));
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

    // ---- (qm) the Quick Moves, as pure functions (Phase 1D) ------------------
    // Each move against its closed form at the four canvas sizes; then the
    // user's decisions 3 and 4 (no room: the START gives way), each with the
    // Stay-inside-off control that shows the clamp is what made the
    // difference.
    ts << "--- (qm) the quick moves ---\n";
    {
        using shotcam::CameraState;
        using shotcam::QuickMove;
        auto near = [](double a, double b, double tol = 0.5) { return qAbs(a - b) <= tol; };
        const shotcam::QuickAmounts amt; // 1.25x, 25 %, 10 deg
        QStringList bad;
        int checked = 0;
        for (const QSize &cs : kSizes) {
            const double W = cs.width(), H = cs.height();
            const CameraState z2{W / 2, H / 2, 2.0, 0.0}; // room to move every way
            auto mv = [&](QuickMove m) { return shotcam::quickMove(m, z2, amt, cs, true); };
            struct Want { QuickMove m; double cx, cy, zoom, rot; };
            const Want wants[] = {
                {QuickMove::PushIn, W / 2, H / 2, 2.5, 0.0},
                {QuickMove::PullOut, W / 2, H / 2, 1.6, 0.0},
                {QuickMove::PanLeft, W / 2 - 0.25 * W / 2, H / 2, 2.0, 0.0},
                {QuickMove::PanRight, W / 2 + 0.25 * W / 2, H / 2, 2.0, 0.0},
                {QuickMove::TiltUp, W / 2, H / 2 - 0.25 * H / 2, 2.0, 0.0},
                {QuickMove::TiltDown, W / 2, H / 2 + 0.25 * H / 2, 2.0, 0.0},
                {QuickMove::RotateLeft, W / 2, H / 2, 2.0, -10.0},
                {QuickMove::RotateRight, W / 2, H / 2, 2.0, 10.0},
            };
            for (const Want &w : wants) {
                ++checked;
                const shotcam::QuickFrames f = mv(w.m);
                if (!(f.moves && f.start == z2 && near(f.end.cx, w.cx) && near(f.end.cy, w.cy)
                      && near(f.end.zoom, w.zoom, 1e-9) && near(f.end.rotation, w.rot, 1e-9)))
                    bad << QStringLiteral("%1 %2x%3: %4,%5 %6x %7deg")
                               .arg(shotcam::quickMoveLabel(w.m)).arg(cs.width()).arg(cs.height())
                               .arg(f.end.cx).arg(f.end.cy).arg(f.end.zoom).arg(f.end.rotation);
            }
        }
        check(QStringLiteral("(qm1) each of the eight moves from a 2x framing lands on its closed "
                             "form (Zoom 1.25x / its inverse, Move 25 % of the frame, Turn 10 deg), "
                             "the START unchanged, at the four canvas sizes"),
              checked == 32 && bad.isEmpty(), bad.join(QStringLiteral("; ")));
        {
            const QSize cs = kSizes[1];
            const CameraState z2{cs.width() / 2.0, cs.height() / 2.0, 2.0, 0.0};
            shotcam::QuickAmounts other;
            other.zoom = 1.5;
            other.move = 0.1;
            other.turn = 5.0;
            const auto p = shotcam::quickMove(QuickMove::PushIn, z2, other, cs, true);
            const auto t = shotcam::quickMove(QuickMove::PanRight, z2, other, cs, true);
            const auto r = shotcam::quickMove(QuickMove::RotateRight, z2, other, cs, true);
            check(QStringLiteral("(qm1) control: other amounts give other ends (Zoom 1.5x, Move "
                                 "10 %, Turn 5 deg)"),
                  near(p.end.zoom, 3.0, 1e-9) && near(t.end.cx, cs.width() / 2.0 + 0.1 * cs.width() / 2)
                      && near(r.end.rotation, 5.0, 1e-9));
            // A turned frame pans along its own axis.
            const CameraState turned{cs.width() / 2.0, cs.height() / 2.0, 2.5, 30.0};
            const auto pr = shotcam::quickMove(QuickMove::PanRight, turned, shotcam::QuickAmounts(), cs, true);
            const double d = 0.25 * cs.width() / 2.5;
            check(QStringLiteral("(qm1) a frame turned 30 deg pans along its own x axis"),
                  near(pr.end.cx, turned.cx + d * std::cos(qDegreesToRadians(30.0)))
                      && near(pr.end.cy, turned.cy + d * std::sin(qDegreesToRadians(30.0))));
            const CameraState full{cs.width() / 2.0, cs.height() / 2.0, 4.0, 0.0};
            check(QStringLiteral("(qm1) Push In at the 4x limit is no move"),
                  !shotcam::quickMove(QuickMove::PushIn, full, shotcam::QuickAmounts(), cs, true).moves);
        }
        // Decisions 3 and 4, at the whole canvas.
        QStringList bad3;
        for (const QSize &cs : kSizes) {
            const double W = cs.width(), H = cs.height();
            const CameraState whole = shotcam::neutral(cs);
            const auto pan = shotcam::quickMove(QuickMove::PanRight, whole, amt, cs, true);
            const auto tilt = shotcam::quickMove(QuickMove::TiltUp, whole, amt, cs, true);
            const auto pull = shotcam::quickMove(QuickMove::PullOut, whole, amt, cs, true);
            const auto turn = shotcam::quickMove(QuickMove::RotateRight, whole, amt, cs, true);
            // The pan at 1.25x: from the left edge to the right edge (0.2 W = 25 % of 0.8 W).
            if (!(pan.moves && near(pan.start.zoom, 1.25, 1e-3) && near(pan.start.cx, W / 2 - 0.1 * W, 1.0)
                  && near(pan.end.cx, W / 2 + 0.1 * W, 1.0) && near(pan.end.zoom, pan.start.zoom, 1e-12)))
                bad3 << QStringLiteral("pan %1x%2: %3x %4 -> %5").arg(cs.width()).arg(cs.height())
                            .arg(pan.start.zoom).arg(pan.start.cx).arg(pan.end.cx);
            if (!(tilt.moves && near(tilt.start.zoom, 1.25, 1e-3) && near(tilt.start.cy, H / 2 + 0.1 * H, 1.0)
                  && near(tilt.end.cy, H / 2 - 0.1 * H, 1.0)))
                bad3 << QStringLiteral("tilt %1x%2").arg(cs.width()).arg(cs.height());
            if (!(pull.moves && near(pull.start.zoom, 1.25, 1e-9) && near(pull.end.zoom, 1.0, 1e-9)))
                bad3 << QStringLiteral("pull %1x%2: %3 -> %4").arg(cs.width()).arg(cs.height())
                            .arg(pull.start.zoom).arg(pull.end.zoom);
            const double need = shotcam::insideMinZoom(10.0, cs);
            if (!(turn.moves && near(turn.end.rotation, 10.0, 1e-9) && near(turn.start.rotation, 0.0, 1e-9)
                  && near(turn.start.zoom, need, 1e-9) && near(turn.end.zoom, need, 1e-9)
                  && near(turn.start.cx, W / 2) && near(turn.start.cy, H / 2)
                  && near(turn.end.cx, W / 2) && near(turn.end.cy, H / 2)))
                bad3 << QStringLiteral("turn %1x%2: %3x -> %4x").arg(cs.width()).arg(cs.height())
                            .arg(turn.start.zoom).arg(turn.end.zoom);
        }
        check(QStringLiteral("(qm3) at the whole canvas with Stay inside on, Pan / Tilt push the "
                             "START in to 1.25x and slide it back so the whole 25 % fits; Pull "
                             "Out starts at 1.25x and ends at the whole canvas; Rotate pushes the "
                             "START in just enough and turns at that zoom about the centre "
                             "(decisions 3 and 4)"),
              bad3.isEmpty(), bad3.join(QStringLiteral("; ")));
        {
            const QSize cs = kSizes[1];
            const CameraState whole = shotcam::neutral(cs);
            const auto pan = shotcam::quickMove(QuickMove::PanRight, whole, amt, cs, false);
            const auto pull = shotcam::quickMove(QuickMove::PullOut, whole, amt, cs, false);
            const auto turn = shotcam::quickMove(QuickMove::RotateRight, whole, amt, cs, false);
            check(QStringLiteral("(qm3) control: with Stay inside off nothing gives way - the pan "
                                 "starts where it is and runs off the artwork, the pull-out ends "
                                 "at 0.8x, the turn stays at 1x"),
                  pan.start == whole && near(pan.end.cx, cs.width() * 0.75)
                      && pull.start == whole && near(pull.end.zoom, 0.8, 1e-9)
                      && turn.start == whole && near(turn.end.zoom, 1.0, 1e-9)
                      && near(turn.end.rotation, 10.0, 1e-9));
        }
    }

    // ---- (mp) the moving pins (recorded before Phase 2A) -------------------
    // One scene per pin size, two panels of 3 s: panel 1 holds six keys, one
    // of each ease, pushing in and turning; panel 2 a Quick Move (Pan Right
    // from a turned 1.5x framing, Stay inside on). Pinned: five MP4 frames a
    // size as the REAL export job offered them, both PNG stills, a PDF page,
    // a preview grab, both SHOTS clips, and the numbers the frame functions
    // give (frame quad, shot transform, the Stay inside clamp, every Quick
    // Move) - the geometry the canvas draws its frame from.
    ts << "--- (mp) moving pins ---\n";
    {
        // `margin` (the margin pins, mq): the same scenes, each panel the
        // fixture inside a stage with that margin, every key and the Quick
        // Move over the same art (offset by the margin). 0 x 0: the (mp) pins.
        auto buildMoving = [&](double nudge, const QSize &margin = QSize(0, 0)) {
            QVector<Scene *> list;
            for (int i = 0; i < 4; ++i) {
                const QSize cs = kSizes[i];
                const double W = cs.width(), H = cs.height();
                const double mx = margin.width(), my = margin.height();
                const shotcam::Stage stage = shotcam::stageWithMargin(
                    QSize(cs.width() + 2 * margin.width(), cs.height() + 2 * margin.height()),
                    margin);
                Scene *s = new Scene;
                s->number = i + 1;
                if (margin.isEmpty())
                    s->panels << fixturePanel(cs) << fixturePanel(cs);
                else
                    s->panels << marginPanel(cs, margin) << marginPanel(cs, margin);
                const struct
                {
                    int frame;
                    double x, y, z, r;
                    shotcam::Ease e;
                } k[] = {{0, 0.5, 0.5, 1.0, 0.0, shotcam::Ease::Linear},
                         {14, 0.42, 0.47, 1.35, 4.0, shotcam::Ease::EaseIn},
                         {28, 0.6, 0.55, 1.8, -6.0, shotcam::Ease::EaseOut},
                         {42, 0.55, 0.4, 1.5, 12.0, shotcam::Ease::EaseInOut},
                         {56, 0.48, 0.52, 2.2, 20.0, shotcam::Ease::Hold},
                         {70, 0.5, 0.5, 1.2, 0.0, shotcam::Ease::EaseInOut}};
                for (const auto &key : k)
                    s->camera.keys.append(shotcam::makeKey(
                        *s, 0, key.frame, 24,
                        state(mx + W * key.x + nudge, my + H * key.y, key.z, key.r), key.e));
                const shotcam::QuickFrames q = shotcam::quickMove(
                    shotcam::QuickMove::PanRight,
                    state(mx + W * 0.4 + nudge, my + H * 0.5, 1.5, 5.0),
                    shotcam::QuickAmounts(), stage, true);
                s->camera.keys.append(shotcam::makeKey(*s, 1, 0, 24, q.start));
                s->camera.keys.append(shotcam::makeKey(*s, 1, 72, 24, q.end));
                list.append(s);
            }
            return list;
        };
        auto tagOf = [](int i) {
            return QStringLiteral("%1x%2").arg(kSizes[i].width()).arg(kSizes[i].height());
        };
        auto renderAll = [&](const QVector<Scene *> &list, const QString &run,
                             const QSize &margin = QSize(0, 0)) {
            QMap<QString, QString> out;
            const QVector<sankoexport::ExportPanel> rows = margin.isEmpty()
                ? sankoexport::collectPanels(list, 24)
                : sankoexport::collectPanels(list, 24, false, false, margin);
            // The MP4: the real export job; each scene is 144 frames.
            const int sampleFrames[] = {7, 35, 50, 64, 72 + 30};
            QSet<int> wanted;
            for (int i = 0; i < 4; ++i)
                for (int f : sampleFrames)
                    wanted.insert(i * 144 + f);
            sankoexport::MovieRequest request;
            request.outPath = scratch + QStringLiteral("/mp_%1.mp4").arg(run);
            request.panels = rows;
            request.fps = 24;
            auto *encoder = new CaptureEncoder(wanted);
            sankoexport::MovieExportJob job(request, encoder);
            job.start();
            QElapsedTimer guard;
            guard.start();
            while (!job.isDone() && guard.elapsed() < 180000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            for (int i = 0; i < 4; ++i)
                for (int f : sampleFrames)
                    out.insert(QStringLiteral("mp4.%1.f%2").arg(tagOf(i)).arg(f),
                               job.result().ok ? encoder->shas.value(i * 144 + f,
                                                                     QStringLiteral("missing"))
                                               : QStringLiteral("export failed"));
            // The PNG stills (the shot at each panel's first frame).
            const sankoexport::FilesResult png = sankoexport::exportPng(
                rows, scratch + QStringLiteral("/mp_png_%1").arg(run), QStringLiteral("Pin"), {});
            for (int i = 0; i < 8; ++i)
                out.insert(QStringLiteral("png.%1.p%2").arg(tagOf(i / 2)).arg(i % 2 + 1),
                           png.ok && png.written.size() == 8 ? shaOfFile(png.written.at(i))
                                                             : QStringLiteral("no file"));
            QImage page(1091, 748, QImage::Format_ARGB32);
            page.fill(Qt::white);
            {
                QPainter p(&page);
                sankoexport::paintBoardPage(p, page.size(), 100, rows, 0, QStringLiteral("Pin"));
            }
            out.insert(QStringLiteral("pdf.page0"), shaOf(page));
            // The preview and the SHOTS clips, through the real page, off screen.
            {
                QWidget host;
                host.setAttribute(Qt::WA_DontShowOnScreen, true);
                host.resize(960, 540);
                host.show();
                AnimaticPage apage;
                apage.setAttribute(Qt::WA_DontShowOnScreen, true);
                apage.resize(1600, 320);
                apage.setFps(24);
                if (!margin.isEmpty())
                    apage.setStageMargin(margin);
                apage.show();
                apage.loadScenes(list);
                apage.attachPreview(&host);
                pump(200);
                AnimaticTimeline *tl = apage.timelineForTest();
                tl->setZoomForTest(200.0f);
                pump(200);
                const QImage surface = tl->surfaceForTest()->grab().toImage();
                for (int i = 0; i < 8; ++i) {
                    const QVariantMap parts = tl->clipPartsForTest(i);
                    const QRect thumb = rectOf(parts.value(QStringLiteral("thumb")).toString());
                    out.insert(QStringLiteral("clip.%1.p%2").arg(tagOf(i / 2)).arg(i % 2 + 1),
                               parts.value(QStringLiteral("thumbShown")).toBool() && thumb.isValid()
                                   ? shaOf(surface.copy(thumb))
                                   : QStringLiteral("no thumb"));
                }
                apage.scrubForTest(0);
                pump(50);
                for (int i = 0; i < 4; ++i) {
                    apage.setPlayheadForTest(2 * i, shotcam::msForFrameInPanel(50, 24));
                    pump(50);
                    QWidget *surf = apage.previewSurface();
                    out.insert(QStringLiteral("preview.%1.f50").arg(tagOf(i)),
                               apage.previewVisible() && surf ? shaOf(surf->grab().toImage())
                                                              : QStringLiteral("no preview"));
                }
                apage.leavePreview();
                apage.loadScenes({});
                pump(100);
            }
            // The geometry, as text with 12 significant digits.
            QString g;
            QTextStream gs(&g);
            gs.setRealNumberPrecision(12);
            for (int i = 0; i < 4; ++i) {
                // Without a margin the stage IS the size (the (mp) numbers).
                const shotcam::Stage cs = margin.isEmpty()
                    ? shotcam::Stage(kSizes[i])
                    : shotcam::stageWithMargin(
                          QSize(kSizes[i].width() + 2 * margin.width(),
                                kSizes[i].height() + 2 * margin.height()),
                          margin);
                const double mx = margin.width(), my = margin.height();
                QVector<shotcam::CameraState> states;
                for (const shotcam::CameraKey &k : list.at(i)->camera.keys)
                    states << k.state;
                states << state(mx + kSizes[i].width() * 0.3, my + kSizes[i].height() * 0.7,
                                0.7, 30.0)
                       << state(mx + kSizes[i].width() * 0.9, my + kSizes[i].height() * 0.1,
                                3.0, -45.0);
                gs << tagOf(i) << " sharp " << shotcam::sharpLimit(cs) << '\n';
                for (const shotcam::CameraState &st : states) {
                    for (const QPointF &pt : shotcam::frameQuad(st, cs))
                        gs << pt.x() << ',' << pt.y() << ' ';
                    for (bool cover : {false, true}) {
                        const QTransform t =
                            shotcam::shotTransform(st, cs, QRectF(0, 0, 1600, 900), cover);
                        gs << t.m11() << ' ' << t.m12() << ' ' << t.m21() << ' ' << t.m22()
                           << ' ' << t.dx() << ' ' << t.dy() << ' ';
                    }
                    const shotcam::CameraState c = shotcam::clampInside(st, cs);
                    gs << c.cx << ' ' << c.cy << ' ' << c.zoom << ' ' << c.rotation << ' '
                       << shotcam::insideMinZoom(st.rotation, cs) << ' '
                       << shotcam::isNeutral(st, cs.canvas) << '\n';
                }
                for (int m = 0; m < 8; ++m)
                    for (bool inside : {true, false}) {
                        const shotcam::QuickFrames q = shotcam::quickMove(
                            shotcam::QuickMove(m), states.at(3), shotcam::QuickAmounts(), cs, inside);
                        gs << m << inside << ' ' << q.start.cx << ' ' << q.start.cy << ' '
                           << q.start.zoom << ' ' << q.start.rotation << ' ' << q.end.cx << ' '
                           << q.end.cy << ' ' << q.end.zoom << ' ' << q.end.rotation << ' '
                           << q.moves << '\n';
                    }
            }
            gs.flush();
            out.insert(QStringLiteral("geometry"),
                       QString::fromLatin1(QCryptographicHash::hash(
                           g.toUtf8(), QCryptographicHash::Sha256).toHex()));
            return out;
        };
        const QVector<Scene *> moving = buildMoving(0.0);
        const QMap<QString, QString> got = renderAll(moving, QStringLiteral("a"));
        for (auto it = got.cbegin(); it != got.cend(); ++it)
            checkMovingPin(it.key(), it.value());
        // Control: every key 1 px further across changes every pinned output.
        const QVector<Scene *> nudged = buildMoving(1.0);
        // (rp) the evaluation at every frame, pinned (the Phase 2 spec's 8.1
        // row pins, before 2D); control: the nudge changes it.
        checkEvaluatePin(QStringLiteral("evaluate.moving"), evaluateText(moving));
        check(QStringLiteral("(rp) control: every key moved 1 px across changes the evaluation "
                             "text"),
              evaluateText(nudged) != evaluateText(moving));
        const QMap<QString, QString> other = renderAll(nudged, QStringLiteral("b"));
        QStringList unchanged;
        for (auto it = got.cbegin(); it != got.cend(); ++it)
            if (other.value(it.key()) == it.value())
                unchanged << it.key();
        check(QStringLiteral("(mp) control: every key moved 1 px across changes every pinned "
                             "output - each MP4 frame, still, the PDF page, preview, clip and the "
                             "geometry (%1 outputs)").arg(got.size()),
              got.size() == 42 && other.size() == got.size() && unchanged.isEmpty(),
              unchanged.isEmpty() ? QString() : QStringLiteral("unchanged: ") + unchanged.join(' '));
        qDeleteAll(moving);
        qDeleteAll(nudged);

        // ---- (mq) the margin pins (recorded before Phase 2B) ----------------
        ts << "--- (mq) margin pins ---\n";
        const QVector<Scene *> inMargin = buildMoving(0.0, kMarginPinMargin);
        const QMap<QString, QString> mgot =
            renderAll(inMargin, QStringLiteral("ma"), kMarginPinMargin);
        for (auto it = mgot.cbegin(); it != mgot.cend(); ++it)
            checkMarginPin(it.key(), it.value());
        checkEvaluatePin(QStringLiteral("evaluate.margin"), evaluateText(inMargin));
        // Control: the same 1 px nudge changes every margin output too.
        const QVector<Scene *> mNudged = buildMoving(1.0, kMarginPinMargin);
        const QMap<QString, QString> mother =
            renderAll(mNudged, QStringLiteral("mb"), kMarginPinMargin);
        QStringList mSame;
        for (auto it = mgot.cbegin(); it != mgot.cend(); ++it)
            if (mother.value(it.key()) == it.value())
                mSame << it.key();
        check(QStringLiteral("(mq) control: every key moved 1 px across changes every margin "
                             "output (%1 outputs)").arg(mgot.size()),
              mgot.size() == 42 && mother.size() == mgot.size() && mSame.isEmpty(),
              mSame.isEmpty() ? QString() : QStringLiteral("unchanged: ") + mSame.join(' '));
        // Control, and 2A's own promise: the frame is the same size and the
        // margin whole pixels, so every shot that sees only the frame's art
        // (each MP4 frame, still, the PDF page, the preview, the base
        // framing's clip) is the no-margin pin's bytes. Only the geometry
        // (stage px) and the MOVING clips (drawn from the 512 px mip of the
        // whole, larger stage) differ.
        QStringList mDiffer, wantDiffer{QStringLiteral("geometry")};
        for (auto it = mgot.cbegin(); it != mgot.cend(); ++it)
            if (got.value(it.key()) != it.value())
                mDiffer << it.key();
        for (int i = 0; i < 4; ++i)
            wantDiffer << QStringLiteral("clip.%1.p2").arg(tagOf(i));
        mDiffer.sort();
        wantDiffer.sort();
        check(QStringLiteral("(mq) control: inside the margin the shots that see only the frame's "
                             "art are the no-margin pins' bytes; only the geometry and the 4 "
                             "moving clips differ"),
              mDiffer == wantDiffer, mDiffer.join(' '));
        qDeleteAll(inMargin);
        qDeleteAll(mNudged);
    }

    // ---- (mg) THE STAGE MARGIN (Phase 2A) -------------------------------------
    ts << "--- (mg) the stage margin ---\n";
    const QSize kMargin(160, 90); // one project margin, px each side
    {
        // (mg1) a keyless margin project: the four pin fixtures, each inside
        // a stage 160 / 90 px larger with other art in the margin, give the
        // 17 NEUTRAL PINS' bytes - the shot is the frame, cut out and drawn
        // by the code that drew the frame-sized panel.
        Scene *ms = new Scene;
        ms->number = 1;
        for (const QSize &size : kSizes)
            ms->panels.append(marginPanel(size, kMargin));
        const QVector<sankoexport::ExportPanel> rows =
            sankoexport::collectPanels({ms}, 24, false, false, kMargin);
        QStringList off;
        {
            const QSet<int> wanted{0, 72, 144, 216};
            sankoexport::MovieRequest request;
            request.outPath = scratch + QStringLiteral("/mg1.mp4");
            request.panels = rows;
            request.fps = 24;
            auto *encoder = new CaptureEncoder(wanted);
            sankoexport::MovieExportJob job(request, encoder);
            job.start();
            QElapsedTimer guard;
            guard.start();
            while (!job.isDone() && guard.elapsed() < 120000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            for (int i = 0; i < 4; ++i) {
                const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                        .arg(kSizes[i].height());
                if (encoder->shas.value(i * 72) != pinned(QStringLiteral("mp4.") + tag))
                    off << QStringLiteral("mp4.") + tag;
            }
        }
        const sankoexport::FilesResult png = sankoexport::exportPng(
            rows, scratch + QStringLiteral("/mg1_png"), QStringLiteral("Pin"), {});
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            if (!png.ok || png.written.size() != 4
                || shaOfFile(png.written.at(i)) != pinned(QStringLiteral("png.") + tag))
                off << QStringLiteral("png.") + tag;
        }
        {
            QImage page(1091, 748, QImage::Format_ARGB32);
            page.fill(Qt::white);
            QPainter p(&page);
            sankoexport::paintBoardPage(p, page.size(), 100, rows, 0, QStringLiteral("Pin"));
            p.end();
            if (shaOf(page) != pinned(QStringLiteral("pdf.page0")))
                off << QStringLiteral("pdf.page0");
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
            page.setStageMargin(kMargin);
            page.show();
            page.loadScenes({ms});
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
                if (!thumb.isValid() || shaOf(surface.copy(thumb)) != pinned(QStringLiteral("clip.") + tag))
                    off << QStringLiteral("clip.") + tag;
            }
            for (int i = 0; i < 4; ++i) {
                const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                        .arg(kSizes[i].height());
                page.scrubForTest(i);
                pump(100);
                QWidget *surf = page.previewSurface();
                if (!surf || shaOf(surf->grab().toImage()) != pinned(QStringLiteral("preview.") + tag))
                    off << QStringLiteral("preview.") + tag;
            }
            page.leavePreview();
            page.loadScenes({});
            pump(100);
        }
        check(QStringLiteral("(mg1) A KEYLESS MARGIN PROJECT GIVES THE 17 NEUTRAL PINS' BYTES: the "
                             "MP4 the real export job wrote, the PNG files, the PDF page, the SHOTS "
                             "clips and the preview, at the four pin sizes inside a 160 / 90 px "
                             "margin full of other art"),
              off.isEmpty(), off.isEmpty() ? QStringLiteral("17 of 17") : off.join(' '));
        // Control: the base framing 1 px across is no longer the frame's crop.
        Scene *shifted = new Scene;
        shifted->number = 1;
        for (const QSize &size : kSizes)
            shifted->panels.append(marginPanel(size, kMargin));
        for (int i = 0; i < 4; ++i) {
            shotcam::CameraState s = shotcam::neutral(shifted->panels.at(i)->canvasSize());
            s.cx += 1.0;
            shifted->camera.keys.append(shotcam::makeKey(*shifted, i, 0, 24, s));
        }
        const QVector<sankoexport::ExportPanel> srows =
            sankoexport::collectPanels({shifted}, 24, false, false, kMargin);
        int changed = 0;
        for (int i = 0; i < 4; ++i) {
            const QString tag = QStringLiteral("%1x%2").arg(kSizes[i].width())
                                    .arg(kSizes[i].height());
            const shotcam::Stage st = shotcam::stageWithMargin(
                shifted->panels.at(i)->canvasSize(), kMargin);
            changed += shaOf(sankoexport::renderMovieFrame(
                           shifted->panels.at(i), sankoexport::kMovieFrameSize,
                           shotcam::evaluateAtPanel(*shifted, i, 0, 24), st))
                    != pinned(QStringLiteral("mp4.") + tag);
            changed += shaOf(sankoexport::stillImage(srows.at(i))) != shaOf(sankoexport::stillImage(rows.at(i)));
        }
        const QVector<sankoexport::ExportPanel> full =
            sankoexport::collectPanels({ms}, 24, true, false, kMargin);
        const QImage whole = sankoexport::stillImage(full.at(0));
        check(QStringLiteral("(mg1) control: the camera 1 px across changes every MP4 frame and "
                             "still; Full Artwork is the whole stage, margin art and all"),
              changed == 8 && whole.size() == ms->panels.at(0)->canvasSize()
                  && whole.pixelColor(kMargin.width() / 2, kMargin.height() / 2) != QColor(Qt::white),
              QStringLiteral("%1 of 8 changed; full artwork %2x%3").arg(changed)
                  .arg(whole.width()).arg(whole.height()));
        delete shifted;
        delete ms;
    }
    {
        // (mg2) without a margin a stage is the canvas: every frame function
        // given Stage(cs, cs) answers what the canvas alone answers.
        bool same = true;
        bool differs = true;
        for (const QSize &cs : kSizes) {
            const shotcam::Stage plain(cs, cs), narrow(cs, cs - QSize(2, 2));
            const shotcam::CameraState st = state(cs.width() * 0.45, cs.height() * 0.55, 1.4, 12.0);
            same = same && shotcam::frameQuad(st, plain) == shotcam::frameQuad(st, cs)
                && shotcam::shotTransform(st, plain, QRectF(0, 0, 800, 450))
                       == shotcam::shotTransform(st, cs, QRectF(0, 0, 800, 450))
                && shotcam::clampInside(st, plain) == shotcam::clampInside(st, cs)
                && shotcam::insideMinZoom(12.0, plain) == shotcam::insideMinZoom(12.0, cs)
                && shotcam::sharpLimit(plain) == shotcam::sharpLimit(cs)
                && !plain.hasMargin();
            differs = differs && shotcam::frameQuad(st, narrow) != shotcam::frameQuad(st, cs);
        }
        check(QStringLiteral("(mg2) a stage without a margin is the canvas: frame quad, shot "
                             "transform, Stay inside, its minimum zoom and the sharp limit all "
                             "equal the canvas's own"),
              same);
        check(QStringLiteral("(mg2) control: a frame 2 px smaller than the canvas changes the "
                             "frame quad"),
              differs);
    }
    {
        // (mg3) - (mg5) on a 1080p shot with a 25 % margin.
        const shotcam::Stage st(QSize(2880, 1620), QSize(1920, 1080));
        const shotcam::CameraState base = shotcam::neutral(st.canvas);
        const QPolygonF quad = shotcam::frameQuad(base, st);
        check(QStringLiteral("(mg3) the zoom is the frame's: at 1x the frame is 1920 x 1080 "
                             "centred on the 2880 x 1620 stage; the sharp limit is 1.00x; Stay "
                             "inside lets it pull out to 0.667x (the largest framing)"),
              quad.boundingRect() == QRectF(480, 270, 1920, 1080)
                  && near(shotcam::sharpLimit(st), 1.0)
                  && near(shotcam::insideMinZoom(0.0, st), 1920.0 / 2880.0),
              QStringLiteral("%1; sharp %2; min %3")
                  .arg(QStringLiteral("%1,%2 %3x%4").arg(quad.boundingRect().x())
                           .arg(quad.boundingRect().y()).arg(quad.boundingRect().width())
                           .arg(quad.boundingRect().height()))
                  .arg(shotcam::sharpLimit(st)).arg(shotcam::insideMinZoom(0.0, st)));
        check(QStringLiteral("(mg3) control: without the margin the same 1080p canvas cannot "
                             "pull out (minimum 1x)"),
              near(shotcam::insideMinZoom(0.0, QSize(1920, 1080)), 1.0));
        const shotcam::CameraState pan = shotcam::clampInside(state(1920, 810, 1.0, 0.0), st);
        check(QStringLiteral("(mg4) a pan at 1x to the margin's edge stays where it is and is "
                             "not soft; a 1.01x push-in is"),
              pan == state(1920, 810, 1.0, 0.0) && !(pan.zoom > shotcam::sharpLimit(st) + 1e-9)
                  && 1.01 > shotcam::sharpLimit(st) + 1e-9,
              stateText(pan));
        check(QStringLiteral("(mg4) control: without the margin Stay inside pulls the same pan "
                             "back to the canvas's centre"),
              shotcam::clampInside(state(1920, 540, 1.0, 0.0), QSize(1920, 1080)).cx == 960.0);
        const shotcam::QuickAmounts amt;
        const shotcam::QuickFrames pr = shotcam::quickMove(shotcam::QuickMove::PanRight, base,
                                                           amt, st, true);
        const shotcam::Stage st10(QSize(2304, 1296), QSize(1920, 1080));
        const shotcam::QuickFrames po = shotcam::quickMove(
            shotcam::QuickMove::PullOut, shotcam::neutral(st10.canvas), amt, st10, true);
        check(QStringLiteral("(mg5) Quick Moves on the margin: Pan Right at 1x has room (starts "
                             "where it is, travels 25 % of the FRAME, 480 px); Pull Out at 10 % "
                             "ends at the largest framing the stage holds (0.833x)"),
              pr.moves && pr.start == base && near(pr.end.cx, 1440 + 480) && near(pr.end.zoom, 1.0)
                  && po.moves && near(po.end.zoom, 1920.0 / 2304.0) && near(po.start.zoom, 1.0),
              QStringLiteral("pan %1 -> %2; pull %3 -> %4").arg(stateText(pr.start), stateText(pr.end),
                                                             stateText(po.start), stateText(po.end)));
        const shotcam::QuickFrames pr0 = shotcam::quickMove(
            shotcam::QuickMove::PanRight, shotcam::neutral(QSize(1920, 1080)), amt, QSize(1920, 1080), true);
        check(QStringLiteral("(mg5) control: without the margin the pan has no room at 1x and "
                             "pushes the start in (Phase 1D's decision 3)"),
              pr0.start.zoom > 1.0, stateText(pr0.start));
    }
    {
        // (mg6) a margin change keeps every key over the same artwork: the
        // centre moves with the offset and the zoom stays (the frame did not
        // change). Control: the Phase 1 rule - the zoom scaled by the canvas -
        // moves the frame off its art.
        const QSize oldCanvas(1920, 1080), newCanvas(2880, 1620);
        const shotcam::Stage oldStage(oldCanvas), newStage(newCanvas, QSize(1920, 1080));
        const QPoint offset(480, 270);
        shotcam::SceneCamera cam;
        shotcam::CameraKey key;
        key.state = state(1100, 500, 1.6, 10.0);
        cam.keys = {key};
        shotcam::SceneCamera old = cam, phase1 = cam;
        shotcam::remapForResize(cam, oldStage, newStage, offset);
        shotcam::remapForResize(phase1, oldCanvas, newCanvas, offset);
        const QPolygonF before = shotcam::frameQuad(old.keys.at(0).state, oldStage)
                                     .translated(offset);
        const QPolygonF after = shotcam::frameQuad(cam.keys.at(0).state, newStage);
        bool over = before.size() == after.size();
        for (int i = 0; over && i < after.size(); ++i)
            over = near(before.at(i).x(), after.at(i).x()) && near(before.at(i).y(), after.at(i).y());
        const QPolygonF wrong = shotcam::frameQuad(phase1.keys.at(0).state, newStage);
        check(QStringLiteral("(mg6) adding a margin keeps a key's frame over the same pixels "
                             "(centre + offset, zoom unchanged)"),
              over && near(cam.keys.at(0).state.zoom, 1.6), stateText(cam.keys.at(0).state));
        check(QStringLiteral("(mg6) control: the canvas-scaled zoom (no stage) puts it off its art"),
              !near(wrong.boundingRect().width(), after.boundingRect().width()),
              QString::number(wrong.boundingRect().width()));
    }
    {
        // (mg7) the file: frameWidth / frameHeight only with a margin, read
        // back; a frame that cannot be one is repaired, not cropped.
        Scene *fs = new Scene;
        fs->number = 1;
        fs->panels.append(marginPanel(QSize(1920, 1080), QSize(480, 270)));
        ProjectIO::SaveData data;
        data.projectName = QStringLiteral("Margin");
        data.fps = 24;
        data.canvasSize = QSize(2880, 1620);
        data.stageMargin = QSize(480, 270);
        data.scenes = {fs};
        const QString dir = scratch + QStringLiteral("/mg7");
        QDir().mkpath(dir);
        const ProjectIO::WriteResult w = ProjectIO::projectToJson(data, dir + QStringLiteral("/m.sankotv"));
        ProjectIO::LoadedProject back = ProjectIO::projectFromJson(w.root, dir, dir + QStringLiteral("/m.sankotv"));
        const bool written = w.root.value(QStringLiteral("frameWidth")).toInt() == 1920
            && w.root.value(QStringLiteral("frameHeight")).toInt() == 1080;
        check(QStringLiteral("(mg7) a margin project writes its shot's size and reads its margin "
                             "back (480 / 270 px each side)"),
              w.ok && written && back.stageMargin == QSize(480, 270) && !back.stageMarginRepaired,
              QStringLiteral("frame %1x%2, margin %3x%4").arg(w.root.value(QStringLiteral("frameWidth")).toInt())
                  .arg(w.root.value(QStringLiteral("frameHeight")).toInt())
                  .arg(back.stageMargin.width()).arg(back.stageMargin.height()));
        qDeleteAll(back.scenes);
        data.stageMargin = QSize(0, 0);
        const ProjectIO::WriteResult w0 = ProjectIO::projectToJson(data, dir + QStringLiteral("/m.sankotv"));
        check(QStringLiteral("(mg7) control: without a margin neither key is written (the manifest "
                             "every older project has)"),
              w0.ok && !w0.root.contains(QStringLiteral("frameWidth"))
                  && !w0.root.contains(QStringLiteral("frameHeight")));
        QJsonObject bad = w.root;
        bad[QStringLiteral("frameWidth")] = 1921; // not centred on whole pixels
        ProjectIO::LoadedProject repaired = ProjectIO::projectFromJson(bad, dir, dir + QStringLiteral("/m.sankotv"));
        QJsonObject big = w.root;
        big[QStringLiteral("frameWidth")] = 4000; // larger than the stage
        ProjectIO::LoadedProject repaired2 = ProjectIO::projectFromJson(big, dir, dir + QStringLiteral("/m.sankotv"));
        check(QStringLiteral("(mg7) a frame the artwork cannot have (off-centre, or larger than "
                             "the stage) opens with no margin and says so; the artwork keeps its "
                             "size"),
              repaired.stageMarginRepaired && repaired.stageMargin == QSize(0, 0)
                  && repaired2.stageMarginRepaired && repaired2.stageMargin == QSize(0, 0)
                  && !repaired.scenes.isEmpty()
                  && repaired.scenes.first()->panels.first()->canvasSize() == QSize(2880, 1620));
        qDeleteAll(repaired.scenes);
        qDeleteAll(repaired2.scenes);
        delete fs;
    }
    {
        // (mg10) the dialogs' rules: the presets are shares of the shot, the
        // note says the stage and its weight, amber past a 4K layer.
        NewProjectDialog::setDefaultLocationForTest(scratch);
        NewProjectDialog np;
        np.marginDropdown()->choose(2); // 25 %
        const bool at25 = np.stageMargin() == QSize(480, 270) && np.canvasWidth() == 2880
            && np.canvasHeight() == 1620
            && np.stageNoteForTest() == QStringLiteral("Stage 2880 × 1620 · 18.7 MB a layer")
            && !np.stageNoteHeavyForTest() && !np.acrossField()->fieldEnabled();
        np.presetDropdown()->choose(2); // 4K: the margin follows the shot
        const bool at4k = np.stageMargin() == QSize(960, 540)
            && np.stageNoteForTest() == QStringLiteral("Stage 5760 × 3240 · 74.6 MB a layer")
            && np.stageNoteHeavyForTest();
        np.presetDropdown()->choose(0);
        np.marginDropdown()->choose(0);
        const bool none = np.stageMargin() == QSize(0, 0) && np.canvasWidth() == 1920
            && np.stageNoteForTest() == QStringLiteral("No margin: the stage is the frame")
            && !np.stageNoteHeavyForTest();
        check(QStringLiteral("(mg10) New Project: 25 % at 1080p is 480 / 270 px each side, a 2880 x "
                             "1620 stage, 18.7 MB a layer, fields locked; at 4K it follows the shot "
                             "and the note turns amber (74.6 MB); None is today's project"),
              at25 && at4k && none,
              QStringLiteral("25 %1, 4K %2, none %3").arg(at25).arg(at4k).arg(none));
        np.marginDropdown()->choose(stagemargin::kCustom);
        np.acrossField()->setText(QStringLiteral("4000"));
        Q_EMIT np.acrossField()->textEdited(QStringLiteral("4000"));
        check(QStringLiteral("(mg10) control: a Custom margin past the 8192 px stage is refused "
                             "and Create is off"),
              np.acrossField()->fieldEnabled() && !np.validationReason().isEmpty()
                  && !np.createButton()->isEnabled(),
              np.validationReason());
        NewProjectDialog::setDefaultLocationForTest(QString());

        // The Stage Margin dialog: grow, shrink with nothing there, shrink
        // that cuts art (the probe says what lies in the band), too big.
        int asked = 0;
        StageMarginDialog::Band answer;
        StageMarginDialog md(QSize(1920, 1080), QSize(192, 108),
                             [&](const QSize &) { ++asked; return answer; });
        const QString current = md.currentText();
        md.presetDropdown()->choose(2); // grow to 25 %
        const QString grow = md.effectText();
        const bool growOk = grow.startsWith(QStringLiteral("The stage grows to 2880 × 1620"))
            && !md.effectWarns() && asked == 0;
        StageMarginDialog sd(QSize(1920, 1080), QSize(480, 270),
                             [&](const QSize &) { ++asked; return answer; });
        sd.presetDropdown()->choose(1); // shrink to 10 %, nothing drawn there
        const bool quiet = sd.effectText().contains(QStringLiteral("Nothing is drawn in the part "
                                                                    "that goes (288 px each side, "
                                                                    "162 px top and bottom)"))
            && !sd.effectWarns();
        answer.panelsWithArt = 3;
        answer.keysInBand = 2;
        sd.presetDropdown()->choose(0);
        sd.presetDropdown()->choose(1); // the same shrink, art there
        const bool warns = sd.effectWarns()
            && sd.effectText().contains(QStringLiteral("Art on 3 panels"))
            && sd.effectText().contains(QStringLiteral("2 camera keys frame part of it."));
        check(QStringLiteral("(mg10) Stage Margin: the current line reads \"current stage\"; "
                             "growing says nothing is lost (no band asked about); shrinking with "
                             "nothing drawn there says so plainly; art cut is amber, naming the "
                             "panels and the camera keys (Q2)"),
              current == QStringLiteral("Frame 1920 × 1080 · current stage 2304 × 1296 (10 %)")
                  && growOk && quiet && warns,
              QStringLiteral("current '%1' grow %2 quiet %3 warns %4").arg(current).arg(growOk)
                  .arg(quiet).arg(warns));
        sd.presetDropdown()->choose(stagemargin::kCustom);
        sd.acrossField()->setText(QStringLiteral("4000"));
        // The dialog hears the field's own line edit, as Resize Project does.
        Q_EMIT sd.acrossField()->edit()->textEdited(QStringLiteral("4000"));
        // The whole project's memory (the user's item, 2026-10-31): 46
        // layers of a 4K project, 25 % around the shot = a 5760 x 3240
        // stage, 74,649,600 bytes a layer, about 3.2 GB; amber when it
        // passes 75 % of (free + held).
        StageMarginDialog mem(QSize(3840, 2160), QSize(0, 0), {});
        const qint64 held = qint64(46) * 3840 * 2160 * 4; // about 1.4 GB held now
        mem.setProjectMemory(46, 11, held, qint64(16) * 1024 * 1024 * 1024, true);
        mem.presetDropdown()->choose(2); // 25 %
        const QString roomy = mem.memoryText();
        const bool roomyOk = roomy.startsWith(QStringLiteral("Whole project: about 3.2 GB (46 layers "
                                                             "in 11 panels)"))
            && roomy.endsWith(QStringLiteral("16.0 GB free now")) && !mem.memoryWarns();
        mem.setProjectMemory(46, 11, held, qint64(2) * 1024 * 1024 * 1024, true);
        const bool tight = mem.memoryWarns(); // 3.2 GB > 0.75 x (2 GB + 1.4 GB)
        check(QStringLiteral("(mg10) the Stage Margin dialog says the WHOLE project's memory at the "
                             "chosen stage (layers x stage) and what is free; with 16 GB free it is "
                             "plain, with 2 GB free it is amber"),
              roomyOk && tight, roomy);
        mem.presetDropdown()->choose(0); // back to no margin: 1.5 GB
        check(QStringLiteral("(mg10) control: at the stage the project already has, the same "
                             "2 GB free is not amber (1.4 GB < 0.75 x 3.4 GB)"),
              !mem.memoryWarns() && mem.memoryText().startsWith(QStringLiteral("Whole project: about 1.4 GB")),
              mem.memoryText());

        check(QStringLiteral("(mg10) control: past the cap Set Margin is off with the reason; "
                             "Same share both ways filled the other field"),
              !sd.setButton()->isEnabled() && sd.validationReason().startsWith(
                  QStringLiteral("The stage can be at most 8192"))
                  && sd.downField()->intValue() == qRound(4000.0 * 1080 / 1920),
              sd.validationReason());
    }

    // ---- (dp) DEPTH (Phase 2B) ----------------------------------------------
    // The 2B-3 scene: Sky 0 %, Mountains 50 %, House 100 %, Bush 150 %. Every
    // depth consumer draws each layer through shotcam::layerToStage; these
    // checks prove the placement's closed forms, then that each real
    // consumer's pixels are that placement.
    ts << "--- (dp) depth ---\n";
    {
        const QSize out(1920, 1080);
        const shotcam::Stage st(out);
        const shotcam::CameraState base = shotcam::neutral(out);
        const shotcam::CameraState pan = state(960 + 400, 540, 1.0, 0.0);
        auto at = [](const QTransform &t, const QPointF &p) { return t.map(p); };

        // (dp2) the closed forms.
        bool planeId = true, baseId = true;
        for (const shotcam::CameraState &cam :
             {state(1100, 500, 1.6, 10), state(700, 620, 0.8, -20), state(960, 540, 2.5, 0)})
            planeId = planeId && shotcam::layerToStage(1.0, cam, st).isIdentity();
        for (double a : {0.0, 0.5, 1.5, 2.0})
            for (double r : {0.0, 25.0})
                baseId = baseId && shotcam::layerToStage(a, state(960, 540, 1.0, r), st).isIdentity();
        check(QStringLiteral("(dp2) a = 1 (the picture plane) is the identity for every camera, "
                             "and the base framing is the identity for every depth, turned or not "
                             "(every layer lines up as drawn, D8)"),
              planeId && baseId);
        QStringList panText;
        bool panOk = true;
        for (int i = 1; i < 5; ++i) {
            const double a = kDepths[i];
            const QPointF p = kDepthMarkers[i].centre;
            const QPointF q0 = at(shotcam::layerToStage(a, base, st)
                                      * shotcam::shotTransform(base, st, QRectF(QPointF(), out)), p);
            const QPointF q1 = at(shotcam::layerToStage(a, pan, st)
                                      * shotcam::shotTransform(pan, st, QRectF(QPointF(), out)), p);
            panOk = panOk && qAbs((q1.x() - q0.x()) + a * 400.0) < 1e-6 && qAbs(q1.y() - q0.y()) < 1e-6;
            panText << QStringLiteral("%1 %2").arg(a).arg(q1.x() - q0.x());
        }
        check(QStringLiteral("(dp2) a pan of 400 px at 1x moves each layer a x 400 px on screen: "
                             "the sky 0, the mountains 200, the house 400, the bush 600"),
              panOk, panText.join(QStringLiteral(", ")));
        const shotcam::CameraState push = state(960, 540, 2.0, 0.0);
        auto magnification = [&](double a, const shotcam::CameraState &cam) {
            return std::sqrt(qAbs((shotcam::layerToStage(a, cam, st)
                                   * shotcam::shotTransform(cam, st, QRectF(QPointF(), out)))
                                      .determinant()));
        };
        const double m0 = magnification(0.0, push), mHalf = magnification(0.5, push),
                     mNear = magnification(1.5, push), mPlane = magnification(1.0, push);
        check(QStringLiteral("(dp2) a push-in to 2x is a dolly: each layer magnified z / d - the "
                             "sky 1x, the mountains 1.33x, the picture plane 2x, the bush 4x"),
              qAbs(m0 - 1.0) < 1e-9 && qAbs(mHalf - 2.0 / 1.5) < 1e-9 && qAbs(mPlane - 2.0) < 1e-9
                  && qAbs(mNear - 4.0) < 1e-9,
              QStringLiteral("%1 %2 %3 %4").arg(m0).arg(mHalf).arg(mPlane).arg(mNear));

        // (dp4) the MP4's pixels are that placement: every marker where
        // layerToStage puts it; the flat (no depth) picture has them as drawn.
        Panel *dp = depthPanel();
        const QImage shot = shotcam::renderShot(*dp, pan, st, out);
        const QImage flatShot = shotcam::renderShot(dp->flattenedImage(), pan, st, out);
        QStringList probe;
        bool placed = true, flatOff = true;
        for (int i = 1; i < 5; ++i) {
            const QPoint q = markerInShot(*dp, i, pan, st, out).toPoint();
            const QColor got = shot.pixelColor(q);
            placed = placed && colourNear(got, kDepthMarkers[i].colour);
            if (kDepths[i] != 1.0)
                flatOff = flatOff && !colourNear(flatShot.pixelColor(q), kDepthMarkers[i].colour);
            probe << QStringLiteral("%1@%2,%3 %4").arg(dp->layers.at(i).name).arg(q.x()).arg(q.y())
                         .arg(got.name());
        }
        check(QStringLiteral("(dp4) THE SHOT, 400 px pan: each layer's marker is exactly where "
                             "layerToStage puts it"),
              placed, probe.join(QStringLiteral(", ")));
        check(QStringLiteral("(dp4) control: without depth (the flat shot) the three off-plane "
                             "markers are not there"),
              flatOff);
        Panel *swapped = depthPanel();
        swapped->layers[1].parallax = 1.5;
        swapped->layers[4].parallax = 0.0;
        check(QStringLiteral("(dp2) control: swapping two layers' depths (Sky <-> Bush) changes "
                             "the frame"),
              shaOf(shotcam::renderShot(*swapped, pan, st, out)) != shaOf(shot));
        delete swapped;

        // (dp1) every layer at 100 %: the Phase 1 path, the same bytes.
        Panel *plane = depthPanel();
        for (Layer &l : plane->layers)
            l.parallax = 1.0;
        const QString planeSha = shaOf(sankoexport::renderMovieFrame(plane, out, pan));
        const QString oldSha = shaOf(shotcam::renderShot(plane->flattenedImage(), pan, out));
        plane->layers[2].parallax = 0.95;
        const QString nearlySha = shaOf(sankoexport::renderMovieFrame(plane, out, pan));
        check(QStringLiteral("(dp1) every layer at 100 %: no depth, and the movie frame is the "
                             "Phase 1 picture byte for byte (the moving and margin pins above "
                             "hold on this code)"),
              planeSha == oldSha, planeSha.left(16));
        check(QStringLiteral("(dp1) control: one layer at 95 % changes the moving frame"),
              nearlySha != planeSha);
        delete plane;

        // (dp5) the folder rule.
        Panel *fp = depthPanel();
        Layer folder;
        folder.id = QStringLiteral("folder-far");
        folder.name = QStringLiteral("Far things");
        folder.type = QStringLiteral("group");
        folder.parallax = 0.4;
        Layer member = makeRasterLayer(QStringLiteral("Clouds"), out);
        member.groupId = folder.id;
        member.parallax = 1.5;
        fp->layers.append(member);
        fp->layers.append(folder);
        fp->layers[0].parallax = 0.3; // the paper, given a depth it cannot have
        const double inFolder = fp->effectiveParallax(fp->layers.at(5));
        const double paper = fp->effectiveParallax(fp->layers.at(0));
        Layer left = fp->layers.at(5);
        left.groupId.clear();
        const double outOfFolder = fp->effectiveParallax(left);
        check(QStringLiteral("(dp5) THE FOLDER RULE: a member uses its folder's depth (40 %) and "
                             "keeps its own (150 %) for when it leaves; the paper is always 100 %"),
              inFolder == 0.4 && outOfFolder == 1.5 && paper == 1.0,
              QStringLiteral("%1 / %2 / %3").arg(inFolder).arg(outOfFolder).arg(paper));
        check(QStringLiteral("(dp5) control: a root layer uses its own (Mountains 50 %)"),
              fp->effectiveParallax(fp->layers.at(2)) == 0.5);

        // (dp8) the file: "parallax" only off the picture plane, exact, repaired.
        {
            Scene *ds = new Scene;
            ds->number = 1;
            ds->panels.append(fp);
            ProjectIO::SaveData data;
            data.projectName = QStringLiteral("Depth");
            data.fps = 24;
            data.canvasSize = out;
            data.scenes = {ds};
            const QString dir = scratch + QStringLiteral("/dp8");
            QDir().mkpath(dir);
            const QString path = dir + QStringLiteral("/d.sankotv");
            const ProjectIO::WriteResult w = ProjectIO::projectToJson(data, path);
            const QJsonArray layers = w.root.value(QStringLiteral("scenes")).toArray().at(0)
                .toObject().value(QStringLiteral("panels")).toArray().at(0).toObject()
                .value(QStringLiteral("layers")).toArray();
            QStringList written;
            for (const QJsonValue &v : layers)
                if (v.toObject().contains(QStringLiteral("parallax")))
                    written << QStringLiteral("%1=%2").arg(v.toObject().value(QStringLiteral("name")).toString())
                                   .arg(v.toObject().value(QStringLiteral("parallax")).toDouble());
            const QStringList wantWritten{QStringLiteral("Sky=0"), QStringLiteral("Mountains=0.5"),
                                          QStringLiteral("Bush=1.5"), QStringLiteral("Clouds=1.5"),
                                          QStringLiteral("Far things=0.4")};
            ProjectIO::LoadedProject back = ProjectIO::projectFromJson(w.root, dir, path);
            QStringList read;
            if (!back.scenes.isEmpty() && !back.scenes.first()->panels.isEmpty())
                for (const Layer &l : back.scenes.first()->panels.first()->layers)
                    read << QString::number(l.parallax);
            QStringList wantRead; // the paper (index 0) is left out: its depth is never used
            for (const Layer &l : fp->layers)
                wantRead << QString::number(l.parallax);
            check(QStringLiteral("(dp8) the save writes \"parallax\" only on the layers off the "
                             "picture plane (and the folder's own and its member's own), and the "
                             "load reads every one back exactly"),
                  w.ok && written.size() >= 5 && read.size() == fp->layers.size()
                      && read.mid(1) == wantRead.mid(1)
                      && [&] { for (const QString &s : wantWritten) if (!written.contains(s)) return false; return true; }(),
                  written.join(QLatin1Char(' ')) + QStringLiteral(" | ") + read.join(QLatin1Char(' ')));
            qDeleteAll(back.scenes);
            QJsonObject bad = w.root;
            QJsonArray scenesA = bad.value(QStringLiteral("scenes")).toArray();
            QJsonObject s0 = scenesA.at(0).toObject();
            QJsonArray panelsA = s0.value(QStringLiteral("panels")).toArray();
            QJsonObject p0 = panelsA.at(0).toObject();
            QJsonArray layersA = p0.value(QStringLiteral("layers")).toArray();
            QJsonObject l1 = layersA.at(1).toObject();
            l1[QStringLiteral("parallax")] = 3.0; // past 200 %
            layersA[1] = l1;
            QJsonObject l2 = layersA.at(2).toObject();
            l2[QStringLiteral("parallax")] = QStringLiteral("far"); // not a number
            layersA[2] = l2;
            p0[QStringLiteral("layers")] = layersA;
            panelsA[0] = p0;
            s0[QStringLiteral("panels")] = panelsA;
            scenesA[0] = s0;
            bad[QStringLiteral("scenes")] = scenesA;
            ProjectIO::LoadedProject fixed = ProjectIO::projectFromJson(bad, dir, path);
            const Panel *fixedPanel = fixed.scenes.isEmpty() || fixed.scenes.first()->panels.isEmpty()
                ? nullptr : fixed.scenes.first()->panels.first();
            check(QStringLiteral("(dp8) a depth that cannot be one (300 %, a word) opens on the "
                                 "picture plane and is counted"),
                  fixed.parallaxRepaired == 2 && fixedPanel && fixedPanel->layers.at(1).parallax == 1.0
                      && fixedPanel->layers.at(2).parallax == 1.0
                      && fixedPanel->layers.at(4).parallax == 1.5,
                  QStringLiteral("repaired %1").arg(fixed.parallaxRepaired));
            qDeleteAll(fixed.scenes);
            // Control: a project without depth writes no "parallax" at all.
            for (Layer &l : fp->layers)
                l.parallax = 1.0;
            const ProjectIO::WriteResult w0 = ProjectIO::projectToJson(data, path);
            check(QStringLiteral("(dp8) control: with every layer on the picture plane no "
                                 "\"parallax\" is written (the JSON every older project saves)"),
                  w0.ok && !QJsonDocument(w0.root).toJson().contains("parallax"));
            delete ds; // owns fp
        }

        // (dp6) a layer the camera reaches is not drawn.
        Panel *bp = depthPanel();
        {
            QPainter p(&bp->layers[4].image);
            p.fillRect(QRect(860, 440, 200, 200), kDepthMarkers[4].colour); // round the centre
        }
        Panel *bpHidden = depthPanel();
        bpHidden->layers[4].visible = false;
        const shotcam::CameraState z285 = state(960, 540, 2.85, 0.0), z27 = state(960, 540, 2.7, 0.0);
        check(QStringLiteral("(dp6) the camera reaches a near layer: at 2.85x the 150 % bush has "
                             "d = 0.075 < 0.1 and is not drawn (the frame is the bush-less one)"),
              !shotcam::layerShown(1.5, z285)
                  && qAbs(shotcam::depthDivisor(1.5, z285) - 0.075) < 1e-12
                  && shaOf(shotcam::renderShot(*bp, z285, st, out))
                      == shaOf(shotcam::renderShot(*bpHidden, z285, st, out)));
        check(QStringLiteral("(dp6) control: at 2.7x (d = 0.15) it is drawn"),
              shotcam::layerShown(1.5, z27)
                  && shaOf(shotcam::renderShot(*bp, z27, st, out))
                      != shaOf(shotcam::renderShot(*bpHidden, z27, st, out)));
        delete bp;
        delete bpHidden;

        // (dp7) the roll is shared: a turn alone places every layer alike.
        const shotcam::CameraState turned = state(960, 540, 1.0, 20.0);
        const shotcam::CameraState turnedPan = state(960 + 200, 540, 1.0, 20.0);
        bool rollAlike = !shotcam::layeredShot(*dp, turned, st);
        for (double a : {0.0, 0.5, 1.5})
            rollAlike = rollAlike && shotcam::layerToStage(a, turned, st).isIdentity();
        check(QStringLiteral("(dp7) a 20 degree turn at the base framing moves every layer alike: "
                             "no parallax, the Phase 1 path"),
              rollAlike);
        check(QStringLiteral("(dp7) control: a 200 px pan under the same turn shows parallax (the "
                             "sky and the picture plane placed differently)"),
              shotcam::layeredShot(*dp, turnedPan, st)
                  && shotcam::layerToStage(0.0, turnedPan, st)
                      != shotcam::layerToStage(1.0, turnedPan, st));

        // (dp3) ONE GEOMETRY, through the real consumers: a scene whose
        // camera holds the 400 px pan on frame 0 and drifts after it.
        Scene *ps = new Scene;
        ps->number = 1;
        ps->panels.append(depthPanel());
        ps->panels.append(fixturePanel(out)); // where the playhead parks for the clip grab
        ps->camera.keys = {shotcam::makeKey(*ps, 0, 0, 24, pan, shotcam::Ease::Linear),
                           shotcam::makeKey(*ps, 0, 71, 24, state(960 - 300, 560, 1.4, 0.0))};
        const shotcam::CameraState f0 = shotcam::evaluateAtPanel(*ps, 0, 0, 24);
        const shotcam::CameraState f1 = shotcam::evaluateAtPanel(*ps, 0, 1, 24);
        const QString want0 = shaOf(shotcam::renderShot(*ps->panels.first(), f0, st, out));
        const QString want1 = shaOf(shotcam::renderShot(*ps->panels.first(), f1, st, out));
        const QVector<sankoexport::ExportPanel> rows = sankoexport::collectPanels({ps}, 24);
        QString mp4Sha0, mp4Sha1;
        {
            sankoexport::MovieRequest request;
            request.outPath = scratch + QStringLiteral("/dp3.mp4");
            request.panels = rows;
            request.fps = 24;
            auto *encoder = new CaptureEncoder({0, 1});
            sankoexport::MovieExportJob job(request, encoder);
            job.start();
            QElapsedTimer guard;
            guard.start();
            while (!job.isDone() && guard.elapsed() < 120000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            mp4Sha0 = encoder->shas.value(0);
            mp4Sha1 = encoder->shas.value(1);
        }
        check(QStringLiteral("(dp3) THE MP4 the real export job offered is the layered shot of "
                             "its frame, byte for byte"),
              mp4Sha0 == want0 && mp4Sha1 == want1, mp4Sha0.left(16));
        check(QStringLiteral("(dp3) control: one frame later is a different picture"),
              want0 != want1);
        const sankoexport::FilesResult png = sankoexport::exportPng(
            rows, scratch + QStringLiteral("/dp3_png"), QStringLiteral("Depth"), {});
        check(QStringLiteral("(dp3) THE PNG still is the layered shot of the panel's first frame"),
              png.ok && png.written.size() == 2
                  && shaOf(QImage(png.written.value(0))) == want0);
        {
            QWidget host;
            host.setAttribute(Qt::WA_DontShowOnScreen, true);
            host.resize(out);
            host.show();
            AnimaticPage apage;
            apage.setAttribute(Qt::WA_DontShowOnScreen, true);
            apage.resize(1600, 320);
            apage.setFps(24);
            apage.show();
            apage.loadScenes({ps});
            apage.attachPreview(&host);
            pump(200);
            AnimaticTimeline *tl = apage.timelineForTest();
            tl->setZoomForTest(200.0f);
            pump(200);
            // The SHOTS clip: the layered shot of the first frame, cover -
            // grabbed with the playhead on panel 2, so its line is not over it.
            apage.scrubForTest(1);
            pump(80);
            const QVariantMap parts = tl->clipPartsForTest(0);
            const QRect thumb = rectOf(parts.value(QStringLiteral("thumb")).toString());
            const QImage surface = tl->surfaceForTest()->grab().toImage();
            const QImage clipWant = shotcam::shotThumb(*ps->panels.first(), f0, st, thumb.size(), true);
            const QRect inner = QRect(QPoint(0, 0), thumb.size()).adjusted(5, 5, -5, -5);
            const QImage gotClip = surface.copy(inner.translated(thumb.topLeft()))
                                       .convertToFormat(QImage::Format_ARGB32);
            const QImage wantClip = clipWant.copy(inner).convertToFormat(QImage::Format_ARGB32);
            // The clip's panel number is printed over its top-left corner.
            const QRect number = rectOf(parts.value(QStringLiteral("number")).toString())
                                     .translated(-thumb.topLeft() - inner.topLeft());
            int clipDiff = 0;
            QString firstDiff;
            for (int y = 0; y < gotClip.height() && gotClip.size() == wantClip.size(); ++y)
                for (int x = 0; x < gotClip.width(); ++x)
                    if (!number.contains(x, y) && gotClip.pixel(x, y) != wantClip.pixel(x, y)) {
                        if (clipDiff < 30)
                            firstDiff += QStringLiteral("%1,%2 %3/%4 ").arg(x).arg(y)
                                            .arg(QColor(gotClip.pixel(x, y)).name())
                                            .arg(QColor(wantClip.pixel(x, y)).name());
                        ++clipDiff;
                    }
            check(QStringLiteral("(dp3) THE SHOTS CLIP is drawShot of the first frame at the "
                                 "clip's size, pixel for pixel (inside its rounded border, its "
                                 "panel number aside)"),
                  thumb.isValid() && number.isValid() && gotClip.size() == wantClip.size()
                      && clipDiff == 0,
                  QStringLiteral("%1x%2, %3 px differ %4").arg(thumb.width()).arg(thumb.height())
                      .arg(clipDiff).arg(firstDiff));
            const QImage flatClip = shotcam::shotThumb(*ps->panels.first(), base, st, thumb.size(), true);
            check(QStringLiteral("(dp3) control: the clip is not the base framing's picture"),
                  shaOf(flatClip.copy(inner)) != shaOf(clipWant.copy(inner)));
            // The preview at the MP4's size: the same bytes above its chip.
            apage.scrubForTest(0);
            pump(50);
            apage.setPlayheadForTest(0, 0);
            pump(80);
            QWidget *surf = apage.previewSurface();
            const QImage grab0 = surf ? surf->grab().toImage() : QImage();
            const QRect above(0, 0, out.width(), out.height() - 40);
            check(QStringLiteral("(dp3) THE PREVIEW at 1920 x 1080 is the MP4's frame, byte for "
                                 "byte (above its PREVIEW chip)"),
                  apage.previewVisible() && grab0.size() == out
                      && shaOf(grab0.copy(above))
                          == shaOf(shotcam::renderShot(*ps->panels.first(), f0, st, out).copy(above)),
                  QStringLiteral("%1x%2").arg(grab0.width()).arg(grab0.height()));
            apage.setPlayheadForTest(0, shotcam::msForFrameInPanel(1, 24));
            pump(80);
            const QImage grab1 = surf ? surf->grab().toImage() : QImage();
            check(QStringLiteral("(dp3) control: the preview one frame later differs"),
                  !grab1.isNull() && shaOf(grab1.copy(above)) != shaOf(grab0.copy(above)));
            const QVariantMap recState = apage.shotCamDevrecState();
            check(QStringLiteral("(dp) the recorder's statePoll counts the playhead panel's "
                                 "depth layers (3) and the passed ones (0)"),
                  recState.value(QStringLiteral("shotCam.depthLayers")).toInt() == 3
                      && recState.value(QStringLiteral("shotCam.passedLayers")).toInt() == 0);
            apage.leavePreview();
            apage.loadScenes({});
            pump(100);
        }
        delete ps;
        delete dp;

        // (4.8) THE COST, measured and reported - never a check.
        {
            auto timeIt = [](const std::function<void()> &f, int n) {
                QElapsedTimer t;
                t.start();
                for (int i = 0; i < n; ++i)
                    f();
                return t.nsecsElapsed() / 1e6 / n;
            };
            for (const QSize &stage : {QSize(2880, 1620), QSize(3840, 2160)}) {
                for (int layersN : {4, 8}) {
                    Panel *cp = makeBlankPanel(stage);
                    cp->layers.removeLast();
                    for (int i = 0; i < layersN; ++i) {
                        Layer l = makeRasterLayer(QStringLiteral("L%1").arg(i), stage);
                        QPainter p(&l.image);
                        p.fillRect(QRect(i * 50, i * 40, stage.width() / 3, stage.height() / 3),
                                   QColor(20 * i, 100, 200 - 20 * i));
                        p.end();
                        l.parallax = 0.2 + 0.2 * i;
                        cp->layers.append(l);
                    }
                    const shotcam::Stage cst = shotcam::stageWithMargin(
                        stage, stage == QSize(2880, 1620) ? QSize(480, 270) : QSize(0, 0));
                    const shotcam::CameraState mv =
                        state(stage.width() / 2.0 + 120, stage.height() / 2.0 - 40, 1.25, 3.0);
                    QImage preview(960, 540, QImage::Format_ARGB32_Premultiplied);
                    const double previewMs = timeIt([&] {
                        QPainter p(&preview);
                        shotcam::drawShot(p, *cp, cst, mv, QRectF(0, 0, 960, 540), false);
                    }, 10);
                    const double mp4Ms = timeIt([&] {
                        shotcam::renderShot(*cp, mv, cst, QSize(1920, 1080));
                    }, 5);
                    shotcam::clearDepthCachesForTest();
                    const double stripMs = timeIt([&] {
                        shotcam::clearDepthCachesForTest();
                        shotcam::stageThumb(*cp, mv, cst, QSize(192, 108));
                    }, 3);
                    ts << QStringLiteral("INFO (dp cost) %1x%2, %3 depth layers: preview paint "
                                         "%4 ms, MP4 frame %5 ms (%6 s a minute of film at 24 "
                                         "fps), strip thumbnail from cold %7 ms\n")
                              .arg(stage.width()).arg(stage.height()).arg(layersN)
                              .arg(previewMs, 0, 'f', 1).arg(mp4Ms, 0, 'f', 1)
                              .arg(mp4Ms * 24 * 60 / 1000.0, 0, 'f', 0).arg(stripMs, 0, 'f', 1);
                    delete cp;
                }
            }
            shotcam::clearDepthCachesForTest();
        }
    }

    // ---- (dt) DEPTH TOOLS (Phase 2C) -----------------------------------------
    // Stay inside with depth (D12, Figma 2C-3), Quick Moves through it (2C-4),
    // the passed layers (D10, 2C-2). A 25 % stage, 2880 x 1620 around the
    // 1920 x 1080 frame: Sky 0 % and Mountains 50 % reach the side edges,
    // the Ground (150 %) the sides and the bottom, the Bush (150 %) no edge.
    ts << "--- (dt) depth tools ---\n";
    {
        const QSize big(2880, 1620);
        const shotcam::Stage st = shotcam::stageWithMargin(big, QSize(480, 270));
        auto layerOf = [&](const char *name, double a, const QRect &art) {
            Layer l = makeRasterLayer(QString::fromLatin1(name), big);
            QPainter p(&l.image);
            p.fillRect(art, QColor(90, 120, 80));
            p.end();
            l.parallax = a;
            return l;
        };
        auto scenePanel = [&](bool ground, bool bushToEdge) {
            Panel *p = makeBlankPanel(big);
            p->layers.removeLast();
            p->layers.append(layerOf("Sky", 0.0, QRect(0, 0, 2880, 500)));
            p->layers.append(layerOf("Mountains", 0.5, QRect(0, 500, 2880, 400)));
            if (ground)
                p->layers.append(layerOf("Ground", 1.5, QRect(0, 1300, 2880, 320)));
            p->layers.append(layerOf("Bush", 1.5, bushToEdge ? QRect(600, 1100, 2280, 200)
                                                             : QRect(600, 1100, 300, 200)));
            return p;
        };
        Panel *gp = scenePanel(true, false);
        const QVector<shotcam::DepthEdge> edges = shotcam::depthEdges(*gp);
        QStringList found;
        for (const shotcam::DepthEdge &e : edges)
            found << QStringLiteral("%1 %2%3%4%5").arg(e.name).arg(e.left ? "L" : "")
                         .arg(e.top ? "T" : "").arg(e.right ? "R" : "").arg(e.bottom ? "B" : "");
        check(QStringLiteral("(dt) the layers that hold the frame in: the Mountains (50 %, both "
                             "sides) and the Ground (150 %, sides and bottom); not the Sky (0 %, "
                             "its footprint is always the base framing) nor the Bush (no edge)"),
              found == QStringList({QStringLiteral("Mountains LR"), QStringLiteral("Ground LRB")}),
              found.join(QStringLiteral(", ")));
        // (dt1) a pan right at 1x: the Ground stops it at 320 px, its
        // footprint's right edge exactly on the stage's.
        int by = -2;
        const shotcam::CameraState asked = state(1440 + 480, 810, 1.0, 0.0);
        const shotcam::CameraState held = shotcam::clampInside(asked, st, edges, &by);
        const double groundRight = 1440 + 1.5 * (held.cx - 1440) + 1.0 * 960;
        check(QStringLiteral("(dt1) STAY INSIDE WITH DEPTH: a 480 px pan at 1x stops at 320 px, "
                             "where the Ground's footprint meets the stage's right edge, and says "
                             "the Ground stopped it"),
              qAbs(held.cx - 1760) < 1e-6 && held.cy == 810 && held.zoom == 1.0
                  && by >= 0 && edges.at(by).name == QStringLiteral("Ground")
                  && qAbs(groundRight - 2880) < 1e-6,
              QStringLiteral("cx %1, by %2").arg(held.cx).arg(by));
        const shotcam::CameraState loose = shotcam::clampZoom(asked);
        check(QStringLiteral("(dt1) control: with Stay inside off the same pan keeps its 480 px "
                             "and the Ground's footprint crosses the edge (its end shows)"),
              loose.cx == 1920 && 1440 + 1.5 * 480 + 960 > 2880);
        // Every edge-reaching footprint inside, over turned and zoomed frames.
        QStringList outside;
        const shotcam::CameraState tries[] = {state(2600, 1500, 1.3, 12.0), state(100, 1500, 1.1, -25.0),
                                              state(2800, 100, 2.0, 40.0), state(1440, 1610, 0.9, 5.0)};
        for (const shotcam::CameraState &t : tries) {
            const shotcam::CameraState c = shotcam::clampInside(t, st, edges);
            const double rr = qDegreesToRadians(c.rotation);
            const double bw = 1920 / c.zoom * qAbs(std::cos(rr)) + 1080 / c.zoom * qAbs(std::sin(rr));
            const double bh = 1920 / c.zoom * qAbs(std::sin(rr)) + 1080 / c.zoom * qAbs(std::cos(rr));
            for (const shotcam::DepthEdge &e : edges) {
                const double d = shotcam::depthDivisor(e.parallax, c);
                const double fx = 1440 + e.parallax * (c.cx - 1440), fy = 810 + e.parallax * (c.cy - 810);
                if ((e.left && fx - d * bw / 2 < -1e-6) || (e.right && fx + d * bw / 2 > 2880 + 1e-6)
                    || (e.top && fy - d * bh / 2 < -1e-6) || (e.bottom && fy + d * bh / 2 > 1620 + 1e-6))
                    outside << QStringLiteral("%1 at %2").arg(e.name, stateText(c));
            }
            const QPolygonF q = shotcam::frameQuad(c, st); // and the stage's own rule
            for (const QPointF &pt : q)
                if (pt.x() < -1e-6 || pt.y() < -1e-6 || pt.x() > 2880 + 1e-6 || pt.y() > 1620 + 1e-6)
                    outside << QStringLiteral("frame at %1").arg(stateText(c));
        }
        check(QStringLiteral("(dt1) over four turned and zoomed framings every edge-reaching layer's "
                             "footprint and the frame itself stay inside the stage"),
              outside.isEmpty(), outside.join(QStringLiteral("; ")));
        bool sameAs2A = true;
        for (const shotcam::CameraState &t : tries)
            sameAs2A = sameAs2A && shotcam::clampInside(t, st, {}) == shotcam::clampInside(t, st);
        check(QStringLiteral("(dt) with no edge-reaching depth layer the clamp IS 2A's, number for "
                             "number"),
              sameAs2A);
        // (dt) no centre fits at the zoom: pushed in by 1D's search.
        Panel *near2 = scenePanel(true, false);
        for (Layer &l : near2->layers)
            if (l.name == QStringLiteral("Ground"))
                l.parallax = 2.0;
        const QVector<shotcam::DepthEdge> edges2 = shotcam::depthEdges(*near2);
        const shotcam::CameraState wide = state(1440, 810, 0.7, 0.0);
        const shotcam::CameraState pushed = shotcam::clampInside(wide, st, edges2);
        check(QStringLiteral("(dt) a 200 % Ground that cannot fit a 0.7x framing pushes the zoom in "
                             "to the smallest that fits (0.8x: (2 - z) 960 / z = 1440)"),
              qAbs(pushed.zoom - 0.8) < 1e-6, QString::number(pushed.zoom, 'f', 9));
        check(QStringLiteral("(dt) control: without it 0.7x stands (the stage holds 0.67x)"),
              shotcam::clampInside(wide, st).zoom == 0.7);
        // (dt2) a layer that reaches no edge constrains nothing.
        Panel *bushOnly = scenePanel(false, false);
        int byB = -2;
        const shotcam::CameraState free = shotcam::clampInside(asked, st, shotcam::depthEdges(*bushOnly), &byB);
        Panel *bushEdge = scenePanel(false, true);
        const shotcam::CameraState heldB = shotcam::clampInside(asked, st, shotcam::depthEdges(*bushEdge));
        check(QStringLiteral("(dt2) THE BUSH REACHES NO EDGE: it constrains nothing - the pan goes "
                             "its 480 px to the stage's edge, and nothing is named"),
              free.cx == 1920 && byB == -1, QStringLiteral("cx %1").arg(free.cx));
        check(QStringLiteral("(dt2) control: the same bush running to the right edge holds the "
                             "frame in (320 px)"),
              qAbs(heldB.cx - 1760) < 1e-6, QString::number(heldB.cx));
        // Q6: a faint speck at the edge is not "reaching" it.
        auto speckPanel = [&](int alpha) {
            Panel *p = makeBlankPanel(big);
            p->layers.removeLast();
            Layer l = makeRasterLayer(QStringLiteral("Speck"), big);
            QPainter sp(&l.image);
            sp.fillRect(QRect(2878, 800, 2, 4), QColor(0, 0, 0, alpha));
            sp.end();
            l.parallax = 1.5;
            p->layers.append(l);
            return p;
        };
        Panel *faint = speckPanel(20), *seen = speckPanel(30);
        const QVector<shotcam::DepthEdge> faintEdges = shotcam::depthEdges(*faint);
        const QVector<shotcam::DepthEdge> seenEdges = shotcam::depthEdges(*seen);
        check(QStringLiteral("(dt) A FAINT SPECK (alpha 20, under 10 %) at the edge does not count as "
                             "reaching it (your Q6)"),
              faintEdges.isEmpty());
        check(QStringLiteral("(dt) control: the same speck at alpha 30 (over 10 %) reaches the right "
                             "edge"),
              seenEdges.size() == 1 && seenEdges.first().right && !seenEdges.first().left);
        // (dt3) Quick Moves respect depth: the START gives way.
        const shotcam::CameraState base2 = shotcam::neutral(big);
        const shotcam::QuickFrames qd = shotcam::quickMove(shotcam::QuickMove::PanRight, base2,
                                                           shotcam::QuickAmounts(), st, true, edges);
        const shotcam::QuickFrames q2 = shotcam::quickMove(shotcam::QuickMove::PanRight, base2,
                                                           shotcam::QuickAmounts(), st, true);
        check(QStringLiteral("(dt3) QUICK MOVES RESPECT DEPTH: Pan Right 480 px from the base framing "
                             "with the Ground - the START slides back 160 px, the END stops at 320, "
                             "the whole 480 px kept, and the Ground is named"),
              qAbs(qd.start.cx - 1280) < 0.5 && qAbs(qd.end.cx - 1760) < 0.5
                  && qd.placedStart == base2 && qd.stoppedBy == QStringLiteral("Ground")
                  && qd.stoppedByDepth == 1.5,
              QStringLiteral("start %1, end %2, by '%3'").arg(qd.start.cx).arg(qd.end.cx).arg(qd.stoppedBy));
        check(QStringLiteral("(dt3) control: with no depth layer the same move is 1D's (start 1440, "
                             "end 1920, nothing named), and quickMove with an empty list is "
                             "quickMove itself"),
              q2.start.cx == 1440 && q2.end.cx == 1920 && q2.stoppedBy.isEmpty()
                  && shotcam::quickMove(shotcam::QuickMove::PanRight, base2, shotcam::QuickAmounts(),
                                        st, true, {}).end == q2.end);
        // Passed (D10, Q1).
        Panel *pp = scenePanel(true, false);
        const QStringList at285 = shotcam::passedLayers(*pp, state(1440, 810, 2.85, 0.0));
        const QStringList at27 = shotcam::passedLayers(*pp, state(1440, 810, 2.7, 0.0));
        const QChar L(0x2018), R(0x2019);
        check(QStringLiteral("(dt) PASSED: at 2.85x the camera has passed both 150 % layers (the "
                             "Ground and the Bush), and the chip's words name them; 1, 2 and 3 "
                             "names read as in Figma 2C-2"),
              at285 == QStringList({QStringLiteral("Ground"), QStringLiteral("Bush")})
                  && shotcam::passedText(at285) == QStringLiteral("Passed ") + L + "Ground" + R
                         + QStringLiteral(", ") + L + "Bush" + R
                  && shotcam::passedText({QStringLiteral("Bush")}) == QStringLiteral("Passed ") + L + "Bush" + R
                  && shotcam::passedText({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
                         == QStringLiteral("Passed 3 layers"),
              at285.join(QLatin1Char(',')));
        check(QStringLiteral("(dt) control: at 2.7x nothing is passed"), at27.isEmpty());
        for (Panel *p : {gp, near2, bushOnly, bushEdge, faint, seen, pp})
            delete p;
    }

    // ---- (pk) PINNED KEYS (Phase 2E) ----------------------------------------
    // The model: a key pinned to the sound holds its film TIME (spec 5.2); its
    // scene frame is time x fps - the scene's film start; STRANDED outside the
    // span (D17): kept, saved, not evaluated; the file (5.5). pk1, pk2, pk3's
    // model half and pk9, each with its control. The window halves (the row,
    // the menus, the notes, the bar) are the Lifecycle family's (pk).
    ts << "--- (pk) pinned keys ---\n";
    {
        using shotcam::CameraKey;
        using shotcam::CameraState;
        // Two scenes of 4 panels, 3 s each at 24 fps: 72 frames a panel, 288 a
        // scene; scene 2 starts at film frame 288.
        Scene a, b;
        for (int i = 0; i < 4; ++i) {
            a.panels.append(makeBlankPanel(QSize(64, 36)));
            b.panels.append(makeBlankPanel(QSize(64, 36)));
        }
        auto startB = [&] { return shotcam::sceneFrameCount(a, 24); };
        const CameraState pA = state(20, 10, 1.5, 0), pB = state(40, 20, 2.0, 0);
        // Scene 2: an anchored key on panel 2 at frame 36 of it (scene frame
        // 108) and a pinned key at film frame 288 + 230 = 518 (scene frame
        // 230, over panel 4).
        b.camera.keys = {shotcam::makeKey(b, 1, 36, 24, pA), shotcam::makePinnedKey(518, 24, pB)};
        const CameraKey &anchored = b.camera.keys.at(0);
        const CameraKey &pinned = b.camera.keys.at(1);
        auto frames = [&] {
            return QStringLiteral("%1/%2")
                .arg(shotcam::keyFrame(b, anchored, 24, startB()))
                .arg(shotcam::keyFrame(b, pinned, 24, startB()));
        };
        auto filmOf = [&](const CameraKey &k) {
            const double f = shotcam::keyFrame(b, k, 24, startB());
            return f < 0.0 ? -1.0 : startB() + f;
        };
        const bool base = frames() == QStringLiteral("108/230") && filmOf(pinned) == 518.0;
        // (pk1) the matrix 5.3.
        b.panels.at(0)->duration = 5; // an EARLIER panel in its scene +2 s
        const QString earlierOwn = frames();
        const double filmEarlierOwn = filmOf(pinned);
        b.panels.at(0)->duration = 3;
        b.panels.at(3)->duration = 4; // the panel the pinned key is OVER +1 s
        const QString over = frames();
        b.panels.at(3)->duration = 3;
        a.panels.at(0)->duration = 5; // an EARLIER SCENE +2 s: scene 2 starts at 336
        const QString earlierScene = frames();
        const double filmEarlierScene = filmOf(pinned);
        a.panels.at(0)->duration = 3;
        Panel *ins = makeBlankPanel(QSize(64, 36));
        b.panels.insert(1, ins); // a panel inserted before both
        const QString inserted = frames();
        b.panels.removeAt(1);
        delete ins;
        b.panels.move(3, 0); // the panel under the pinned key moved to the front
        const QString reordered = frames();
        b.panels.move(0, 3);
        Panel *last = b.panels.takeAt(3); // the panel under it DELETED: the scene is 216 long
        const QString deleted = frames();
        const int liveAfterDelete = shotcam::liveKeyCount(b, 24, startB());
        const int strandedAfterDelete = shotcam::strandedCount(b, 24, startB());
        const bool strandedFlag = shotcam::isStranded(b, pinned, 24, startB());
        const int keysKept = b.camera.keys.size();
        const QJsonObject savedStranded = shotcam::toJson(b);
        const CameraState atEnd = shotcam::evaluate(b, 216, 24, QSize(64, 36), startB());
        b.panels.insert(3, last); // ...and the delete undone
        const QString undone = frames();
        Panel *earlier = a.panels.takeAt(0); // an earlier SCENE's panel deleted: start 216
        const QString earlierSceneDeleted = frames();
        a.panels.insert(0, earlier);
        const QString fps60 = QStringLiteral("%1/%2")
                                  .arg(shotcam::keyFrame(b, anchored, 60, shotcam::sceneFrameCount(a, 60)))
                                  .arg(shotcam::keyFrame(b, pinned, 60, shotcam::sceneFrameCount(a, 60)));
        check(QStringLiteral("(pk1) THE RETIMING MATRIX (5.3): an earlier panel +2 s ripples the "
                             "anchored key to 156 and leaves the pinned at 230 (film 518); the "
                             "panel under it +1 s changes neither (108/230); an earlier SCENE +2 s "
                             "keeps the anchored at 108 and the pinned at film 518, now scene frame "
                             "182; an insert before them ripples the anchored only (180/230); the "
                             "panel under it moved to the front, the same (180/230); that panel "
                             "deleted strands the pinned key (108/-1: 1 live, 1 stranded), its undo "
                             "brings it back (108/230); an earlier scene's panel deleted strands it "
                             "too; 60 fps keeps the seconds (270/575)"),
              base && earlierOwn == QStringLiteral("156/230") && filmEarlierOwn == 518.0
                  && over == QStringLiteral("108/230")
                  && earlierScene == QStringLiteral("108/182") && filmEarlierScene == 518.0
                  && inserted == QStringLiteral("180/230") && reordered == QStringLiteral("180/230")
                  && deleted == QStringLiteral("108/-1") && liveAfterDelete == 1
                  && strandedAfterDelete == 1 && strandedFlag
                  && undone == QStringLiteral("108/230")
                  && earlierSceneDeleted == QStringLiteral("108/-1")
                  && fps60 == QStringLiteral("270/575"),
              QStringList{frames(), earlierOwn, over, earlierScene, inserted, reordered, deleted,
                          undone, earlierSceneDeleted, fps60}
                  .join(QLatin1Char(' ')));
        // Control: the anchored expectation for the same key - a twin
        // anchored on panel 4 at frame 14 of it, scene frame 230 today - is
        // another number under each of those edits.
        const CameraKey twin = shotcam::makeKey(b, 3, 14, 24, pB);
        b.panels.at(0)->duration = 5;
        const double twinEarlier = shotcam::keyFrame(b, twin, 24, startB());
        b.panels.at(0)->duration = 3;
        a.panels.at(0)->duration = 5;
        const double twinFilmEarlierScene = startB() + shotcam::keyFrame(b, twin, 24, startB());
        a.panels.at(0)->duration = 3;
        check(QStringLiteral("(pk1) control: an anchored key in the same place answers 278 to the "
                             "earlier panel's +2 s and film 566 to the earlier scene's - the "
                             "pinned key's 230 and 518 are the other rule"),
              twinEarlier == 278.0 && twinFilmEarlierScene == 566.0,
              QStringLiteral("%1 %2").arg(twinEarlier).arg(twinFilmEarlierScene));

        // (pk2) EXACT FRAMES at 24 / 25 / 30 / 60 over a whole film: a key
        // pinned at frame f evaluates at f exactly, and at f - 1 it does not.
        {
            int exact = 0, inexact = 0, total = 0, offByOne = 0;
            QString first;
            for (int fps : {24, 25, 30, 60}) {
                const int filmStart = shotcam::sceneFrameCount(a, fps);
                const int count = shotcam::sceneFrameCount(b, fps);
                for (int f = 1; f <= count; ++f) {
                    Scene t;
                    t.panels = b.panels; // the same panels, not owned
                    t.camera.keys = {shotcam::makeKey(t, 0, 0, fps, pA),
                                     shotcam::makePinnedKey(filmStart + f, fps, pB, shotcam::Ease::Linear)};
                    const double kf = shotcam::keyFrame(t, t.camera.keys.at(1), fps, filmStart);
                    const bool atF = shotcam::evaluate(t, f, fps, QSize(64, 36), filmStart) == pB;
                    const bool before = shotcam::evaluate(t, f - 1, fps, QSize(64, 36), filmStart) != pB;
                    ++total;
                    if (kf == double(f) && atF && before)
                        ++exact;
                    else {
                        ++inexact;
                        if (first.isEmpty())
                            first = QStringLiteral("fps %1 f %2 -> %3 at %4 before %5")
                                        .arg(fps).arg(f).arg(kf, 0, 'f', 9).arg(atF).arg(before);
                    }
                    // The control: the time of one frame later answers f + 1.
                    const CameraKey next = shotcam::makePinnedKey(filmStart + f + 1, fps, pB);
                    if (shotcam::keyFrame(t, next, fps, filmStart) == double(f + 1)
                        || f + 1 > count)
                        ++offByOne;
                    t.panels.clear();
                }
            }
            check(QStringLiteral("(pk2) EXACT FRAMES: at 24, 25, 30 and 60 fps a key pinned at every "
                                 "frame f of scene 2 (its time f / fps from the film's start) has "
                                 "scene frame f exactly and evaluates to its own framing at f and "
                                 "not at f - 1"),
                  total > 0 && inexact == 0, QStringLiteral("%1 of %2 exact; first miss: %3").arg(exact).arg(total).arg(first));
            check(QStringLiteral("(pk2) control: the time one frame later answers f + 1 every time"),
                  offByOne == total);
        }

        // (pk3) STRANDED: kept, saved, not evaluated (the model half).
        check(QStringLiteral("(pk3) a stranded key is KEPT (2 keys), SAVED (the file holds its "
                             "pinned time), and NOT EVALUATED (the scene's end shows the anchored "
                             "key's framing); the stranding text names it"),
              keysKept == 2 && savedStranded.value(QStringLiteral("keys")).toArray().size() == 2
                  && savedStranded.value(QStringLiteral("keys")).toArray().at(1).toObject()
                             .value(QStringLiteral("pinned")).toBool()
                  && atEnd == pA
                  && shotcam::strandingText(0, 1, 2) == QStringLiteral(" (1 pinned camera key now outside scene 2)")
                  && shotcam::strandingText(2, 0, 3) == QStringLiteral(" (2 pinned camera keys back inside scene 3)"),
              QStringLiteral("kept %1, saved %2, end zoom %3").arg(keysKept)
                  .arg(savedStranded.value(QStringLiteral("keys")).toArray().size()).arg(atEnd.zoom));
        check(QStringLiteral("(pk3) control: a retime that keeps it inside names nothing, and a "
                             "live pinned key is evaluated (zoom 2 at its frame)"),
              shotcam::strandingText(1, 1, 2).isEmpty()
                  && shotcam::evaluate(b, 230, 24, QSize(64, 36), startB()) == pB);
        // The sort (5.2): a pinned and an anchored key meeting at one frame -
        // the pinned sorts after, so its framing shows from that frame.
        {
            Scene t;
            t.panels = b.panels;
            t.camera.keys = {shotcam::makePinnedKey(startB() + 108, 24, pB), shotcam::makeKey(t, 1, 36, 24, pA)};
            const bool pinnedWins = shotcam::evaluate(t, 108, 24, QSize(64, 36), startB()) == pB
                && shotcam::keyIndexAtFrame(t, 108, 24, startB()) == 1
                && shotcam::segmentKeyIndex(t, 108, 24, startB()) == 0;
            t.panels.clear();
            check(QStringLiteral("(pk3) two keys on one frame after a retime: the pinned one sorts "
                                 "after the anchored, its framing shows from the frame, keyIndexAtFrame "
                                 "names the anchored and the segment is the pinned's"),
                  pinnedWins);
        }

        // (pk9) THE FILE: pinned / time only for pinned keys; a project without
        // them re-saves byte-identical; a bad time drops the key, counted.
        {
            Scene *s = new Scene;
            s->number = 1;
            s->timeOfDay = QStringLiteral("DAY");
            for (int i = 0; i < 3; ++i)
                s->panels.append(fixturePanel(QSize(320, 180)));
            ProjectIO::SaveData data;
            data.projectName = QStringLiteral("Pin");
            data.fps = 24;
            data.canvasSize = QSize(320, 180);
            data.scenes = {s};
            const QString file = scratch + QStringLiteral("/pin/Pin.sankotv");
            QDir().mkpath(scratch + QStringLiteral("/pin"));
            auto scenesJson = [](const QJsonObject &root) {
                return QJsonDocument(root.value(QStringLiteral("scenes")).toArray())
                    .toJson(QJsonDocument::Compact);
            };
            s->camera.keys = {shotcam::makeKey(*s, 0, 12, 24, pA, shotcam::Ease::EaseOut)};
            const QByteArray anchoredOnly = scenesJson(ProjectIO::projectToJson(data, file).root);
            s->camera.keys.append(shotcam::makePinnedKey(130, 24, pB, shotcam::Ease::Linear));
            const ProjectIO::WriteResult withPin = ProjectIO::projectToJson(data, file);
            const QByteArray pinnedJson = scenesJson(withPin.root);
            const QJsonObject keyObj = withPin.root.value(QStringLiteral("scenes")).toArray().at(0)
                                           .toObject().value(QStringLiteral("camera")).toObject()
                                           .value(QStringLiteral("keys")).toArray().at(1).toObject();
            const QJsonObject text = QJsonDocument::fromJson(QJsonDocument(withPin.root).toJson()).object();
            ProjectIO::LoadedProject back = ProjectIO::projectFromJson(text, scratch + QStringLiteral("/pin"));
            const bool loaded = back.scenes.size() == 1 && back.scenes.at(0)->camera.keys.size() == 2
                && back.scenes.at(0)->camera.keys.at(1).pinned
                && back.scenes.at(0)->camera.keys.at(1).time == 130.0 / 24.0
                && back.scenes.at(0)->camera.keys.at(1).state == pB
                && back.scenes.at(0)->camera.keys.at(1).ease == shotcam::Ease::Linear
                && !back.scenes.at(0)->camera.keys.at(0).pinned
                && shotcam::keyFrame(*back.scenes.at(0), back.scenes.at(0)->camera.keys.at(1), 24) == 130.0;
            for (Scene *sc : back.scenes)
                delete sc;
            s->camera.keys.removeLast();
            const QByteArray anchoredAgain = scenesJson(ProjectIO::projectToJson(data, file).root);
            check(QStringLiteral("(pk9) THE FILE: a pinned key writes pinned and time and no panel / at, "
                                 "loads back pinned at its time with its framing and ease, and the "
                                 "anchored key beside it is written as before; with the pinned key "
                                 "removed the project re-saves byte-identical to the one that never "
                                 "had it"),
                  keyObj.value(QStringLiteral("pinned")).toBool() && keyObj.contains(QStringLiteral("time"))
                      && !keyObj.contains(QStringLiteral("panel")) && !keyObj.contains(QStringLiteral("at"))
                      && loaded && anchoredAgain == anchoredOnly && pinnedJson != anchoredOnly,
                  QString::fromUtf8(QJsonDocument(keyObj).toJson(QJsonDocument::Compact)));
            // Control: pinning one adds the words; a negative time drops the
            // key, counted as Phase 1's drops are.
            QJsonObject broken = text;
            {
                QJsonArray scenes = broken.value(QStringLiteral("scenes")).toArray();
                QJsonObject sc = scenes.at(0).toObject();
                QJsonObject cam = sc.value(QStringLiteral("camera")).toObject();
                QJsonArray keys = cam.value(QStringLiteral("keys")).toArray();
                QJsonObject k = keys.at(1).toObject();
                k[QStringLiteral("time")] = -1.0;
                keys[1] = k;
                cam[QStringLiteral("keys")] = keys;
                sc[QStringLiteral("camera")] = cam;
                scenes[0] = sc;
                broken[QStringLiteral("scenes")] = scenes;
            }
            ProjectIO::LoadedProject dropped = ProjectIO::projectFromJson(broken, scratch + QStringLiteral("/pin"));
            check(QStringLiteral("(pk9) control: the pinned key's words are in the file only while it "
                                 "is there, and a pinned key with a negative time is dropped on load "
                                 "and counted"),
                  pinnedJson.contains("\"pinned\":true") && !anchoredOnly.contains("\"pinned\"")
                      && dropped.scenes.size() == 1 && dropped.scenes.at(0)->camera.keys.size() == 1
                      && dropped.cameraKeysDropped == 1,
                  QStringLiteral("keys %1 dropped %2")
                      .arg(dropped.scenes.isEmpty() ? -1 : dropped.scenes.at(0)->camera.keys.size())
                      .arg(dropped.cameraKeysDropped));
            for (Scene *sc : dropped.scenes)
                delete sc;
            delete s;
        }
        // a and b delete their own panels.
    }

    // ---- (mv) THE MOVEMENT GLYPHS (2026-11-06, Figma CAM-2) ----------------
    // What a move does, by its end key against its start key: the canvas
    // draws it as glyphs on the frame (an editing guide; nothing that renders
    // the shot does - the neutral and moving pins above prove those
    // unchanged). Pure functions, so every kind of move is checked here,
    // with the thresholds and a Hold as the controls.
    ts << "--- (mv) the movement glyphs ---\n";
    {
        using shotcam::CameraState;
        using shotcam::Motion;
        const shotcam::Stage stage = shotcam::stageWithMargin(QSize(2880, 1620), QSize(480, 270));
        const CameraState base{1440, 810, 1.0, 0.0};
        auto mo = [&](const CameraState &to) { return shotcam::motionBetween(base, to, stage); };
        auto says = [](const Motion &m) {
            return QStringLiteral("zoom %1 pan (%2, %3) rotate %4").arg(m.zoom).arg(m.pan.x())
                .arg(m.pan.y()).arg(m.rotate);
        };
        const Motion in = mo({1440, 810, 1.5, 0.0}), out = mo({1440, 810, 0.8, 0.0});
        const Motion right = mo({1640, 810, 1.0, 0.0}), up = mo({1440, 700, 1.0, 0.0});
        const Motion both = mo({1640, 810, 1.5, 0.0}), cw = mo({1440, 810, 1.0, 5.0}),
                     ccw = mo({1440, 810, 1.0, -5.0}), none = mo(base);
        check(QStringLiteral("(mv1) EACH KIND OF MOVE gives its glyph: Push In zoom +1, Pull Out -1, "
                             "a pan right its travel (+200, 0), a tilt up (0, -110), push in and pan "
                             "both, a turn +1 / -1, and the same framing nothing"),
              in.zoom == 1 && in.pan.isNull() && in.rotate == 0 && out.zoom == -1 && out.pan.isNull()
                  && right.zoom == 0 && right.pan == QPointF(200, 0) && up.pan == QPointF(0, -110)
                  && both.zoom == 1 && both.pan == QPointF(200, 0) && cw.rotate == 1 && ccw.rotate == -1
                  && cw.zoom == 0 && cw.pan.isNull() && !none.any(),
              QStringLiteral("in [%1] out [%2] right [%3] both [%4] cw [%5]").arg(says(in), says(out),
                                                                             says(right), says(both), says(cw)));
        // The thresholds (zoom 1 %, travel 2 % of the 1920 frame = 38.4 px,
        // turn 0.5 deg): just under shows nothing, just over the glyph.
        const bool under = !mo({1440, 810, 1.009, 0.0}).any() && !mo({1440 + 38.0, 810, 1.0, 0.0}).any()
            && !mo({1440, 810, 1.0, 0.4}).any();
        const bool over = mo({1440, 810, 1.011, 0.0}).zoom == 1 && !mo({1440 + 39.0, 810, 1.0, 0.0}).pan.isNull()
            && mo({1440, 810, 1.0, 0.6}).rotate == 1;
        check(QStringLiteral("(mv1) control: the thresholds - a 0.9 % zoom, a 38 px travel (under 2 % "
                             "of the frame) and a 0.4 deg turn show nothing; 1.1 %, 39 px and 0.6 deg "
                             "show their glyphs"),
              under && over, QStringLiteral("under %1 over %2").arg(under).arg(over));

        // The segment at a scene frame: three panels, keys at the start of
        // each (a push in, then a pan) and at the end of the last (a pull
        // out). Placed by frames of each panel, whatever its length.
        Scene *s = new Scene;
        for (int p = 0; p < 3; ++p)
            s->panels.append(makeBlankPanel(QSize(2880, 1620)));
        const int fps = 24;
        const int f0 = shotcam::panelFrames(*s->panels.at(0), fps), f1 = shotcam::panelFrames(*s->panels.at(1), fps),
                  f2 = shotcam::panelFrames(*s->panels.at(2), fps);
        const CameraState pushed{1440, 810, 1.5, 0.0}, panned{1740, 810, 1.5, 0.0}, pulled{1740, 810, 0.9, 0.0};
        s->camera.keys = {shotcam::makeKey(*s, 0, 0, fps, base), shotcam::makeKey(*s, 1, 0, fps, pushed),
                          shotcam::makeKey(*s, 2, 0, fps, panned), shotcam::makeKey(*s, 2, f2, fps, pulled)};
        auto at = [&](double frame) { return shotcam::segmentMotion(*s, frame, fps, stage); };
        const Motion m1 = at(f0 / 2.0), m2 = at(f0 + f1 / 2.0), m3 = at(f0 + f1 + f2 / 2.0),
                     mStart = at(0.0), mAfter = at(f0 + f1 + f2 + 10.0);
        check(QStringLiteral("(mv2) THE SEGMENT AT THE PLAYHEAD: the push in mid panel 1, the pan mid "
                             "panel 2, the pull out mid panel 3; on the first key's own frame its move "
                             "already shows; after the last key nothing"),
              m1.zoom == 1 && m1.pan.isNull() && m2.zoom == 0 && m2.pan == QPointF(300, 0) && m3.zoom == -1
                  && m3.pan.isNull() && mStart.zoom == 1 && !mAfter.any(),
              QStringLiteral("p1 [%1] p2 [%2] p3 [%3] after [%4]").arg(says(m1), says(m2), says(m3), says(mAfter)));
        s->camera.keys[1].ease = shotcam::Ease::Hold;
        const Motion held = at(f0 + f1 / 2.0);
        s->camera.keys[1].ease = shotcam::Ease::EaseInOut;
        const Motion unheld = at(f0 + f1 / 2.0);
        check(QStringLiteral("(mv2) control: the same segment as a HOLD shows nothing; eased again, "
                             "its pan"),
              !held.any() && unheld.pan == QPointF(300, 0),
              QStringLiteral("held [%1] eased [%2]").arg(says(held), says(unheld)));
        // A pinned key ends a segment like any other (2E): pinned at panel
        // 3's start, the pan into it is still drawn.
        s->camera.keys[2] = shotcam::makePinnedKey(double(f0 + f1), fps, panned);
        const Motion toPinned = at(f0 + f1 / 2.0);
        check(QStringLiteral("(mv2) a PINNED key ends a segment as an anchored one does: the pan into "
                             "it is drawn"),
              toPinned.pan == QPointF(300, 0) && toPinned.zoom == 0, says(toPinned));
        delete s;

        // THE GLYPHS' STROKES (CAM-2 as adjusted in Figma, 2026-11-06):
        // shotcam::motionGlyphPath - what the canvas strokes, 1 px wide - on
        // a 480 x 270 frame at (100, 100), at the Camera tool's scale. The
        // numbers are the Figma frame's.
        const QPolygonF frame({QPointF(100, 100), QPointF(580, 100), QPointF(580, 370), QPointF(100, 370)});
        auto glyph = [&](const Motion &m, const QPointF &dir, double scale = 1.0,
                         const QPolygonF &q = QPolygonF()) {
            return shotcam::motionGlyphPath(q.isEmpty() ? frame : q, m, dir, scale).toSubpathPolygons();
        };
        auto same2 = [](const QPointF &a, const QPointF &b) { return QLineF(a, b).length() < 0.02; };
        auto has = [&](const QList<QPolygonF> &subs, const QPointF &a, const QPointF &b) { // a stroke a -> b
            for (const QPolygonF &p : subs)
                if (p.size() >= 2 && same2(p.first(), a) && same2(p.last(), b))
                    return true;
            return false;
        };
        auto chev = [&](const QList<QPolygonF> &subs, const QPointF &a, const QPointF &tip, const QPointF &b) {
            for (const QPolygonF &p : subs)
                if (p.size() == 3 && same2(p.at(0), a) && same2(p.at(1), tip) && same2(p.at(2), b))
                    return true;
            return false;
        };
        auto pts = [](const QList<QPolygonF> &subs) {
            QStringList t;
            for (const QPolygonF &p : subs) {
                QStringList q;
                for (const QPointF &pt : p)
                    q << QStringLiteral("%1,%2").arg(pt.x(), 0, 'f', 2).arg(pt.y(), 0, 'f', 2);
                t << (p.size() > 4 ? QStringLiteral("%1 points %2 .. %3").arg(p.size()).arg(q.first(), q.last())
                                   : q.join(QLatin1Char(' ')));
            }
            return t.join(QStringLiteral(" | "));
        };
        Motion zin, zout, pan, cwTurn, ccwTurn, inPan;
        zin.zoom = 1;
        zout.zoom = -1;
        pan.pan = QPointF(200, 0); // the travel's size and way: the direction comes from the screen
        cwTurn.rotate = 1;
        ccwTurn.rotate = -1;
        inPan.zoom = 1;
        inPan.pan = QPointF(200, 0);
        const QPointF corners[4] = {QPointF(100, 100), QPointF(580, 100), QPointF(580, 370), QPointF(100, 370)};
        const QPointF inward[4] = {QPointF(1, 1), QPointF(-1, 1), QPointF(-1, -1), QPointF(1, -1)};
        auto cornerArrows = [&](const QList<QPolygonF> &subs, bool pointingIn, double from, double to) {
            for (int i = 0; i < 4; ++i) {
                const QPointF a = corners[i] + inward[i] * from, b = corners[i] + inward[i] * to;
                if (!(pointingIn ? has(subs, a, b) : has(subs, b, a)))
                    return false;
            }
            return true;
        };

        const QList<QPolygonF> pIn = glyph(zin, QPointF()), pOut = glyph(zout, QPointF());
        double shaft = 0.0, barb1 = 0.0, barb2 = 0.0, angle1 = 0.0, angle2 = 0.0;
        if (pIn.size() == 12 && pIn.at(0).size() == 2 && pIn.at(1).size() == 2 && pIn.at(2).size() == 2) {
            const QLineF s0(pIn.at(0).first(), pIn.at(0).last());
            const QLineF back(s0.p2(), s0.p1()), b1(pIn.at(1).first(), pIn.at(1).last()),
                b2(pIn.at(2).first(), pIn.at(2).last());
            shaft = s0.length();
            barb1 = b1.length();
            barb2 = b2.length();
            angle1 = back.angleTo(b1);
            angle2 = back.angleTo(b2);
            angle1 = qMin(angle1, 360.0 - angle1);
            angle2 = qMin(angle2, 360.0 - angle2);
        }
        check(QStringLiteral("(mv3) PUSH IN AND PULL OUT AS CAM-2 DRAWS THEM: at each corner a 14 px arrow on "
                             "the 45 degree line from 6 px in from both edges, pointing in (Push In) or at "
                             "the corner (Pull Out); its head two 6.3 px barbs at 35 degrees; nothing else"),
              pIn.size() == 12 && pOut.size() == 12 && cornerArrows(pIn, true, 6.0, 15.9)
                  && cornerArrows(pOut, false, 6.0, 15.9) && qAbs(shaft - 14.0) < 0.01 && qAbs(barb1 - 6.3) < 0.01
                  && qAbs(barb2 - 6.3) < 0.01 && qAbs(angle1 - 35.0) < 0.1 && qAbs(angle2 - 35.0) < 0.1,
              QStringLiteral("shaft %1 barbs %2 / %3 at %4 / %5 deg; in: %6").arg(shaft).arg(barb1).arg(barb2)
                  .arg(angle1).arg(angle2).arg(pts(pIn)));
        const double k = 10.0 / 14.0;
        const QList<QPolygonF> pSmall = glyph(zin, QPointF(), k);
        check(QStringLiteral("(mv3) control: a drawing tool's glyphs are 10/14 of the Camera tool's (a 10 px "
                             "arrow from 4.3 px in), and no move - a Hold - gives no stroke at all"),
              pSmall.size() == 12 && cornerArrows(pSmall, true, 6.0 * k, 15.9 * k)
                  && glyph(Motion(), QPointF(1, 0)).isEmpty(),
              pts(pSmall));

        const QList<QPolygonF> pRight = glyph(pan, QPointF(1, 0)), pLeft = glyph(pan, QPointF(-1, 0)),
                               pUp = glyph(pan, QPointF(0, -1)), pDown = glyph(pan, QPointF(0, 1));
        const bool goesRight = pRight.size() == 2 && chev(pRight, {343, 104}, {348, 100}, {343, 96})
            && chev(pRight, {343, 374}, {348, 370}, {343, 366});
        const bool goesLeft = pLeft.size() == 2 && chev(pLeft, {337, 96}, {332, 100}, {337, 104})
            && chev(pLeft, {337, 366}, {332, 370}, {337, 374});
        const bool goesUp = pUp.size() == 2 && chev(pUp, {104, 232}, {100, 227}, {96, 232})
            && chev(pUp, {584, 232}, {580, 227}, {576, 232});
        const bool goesDown = pDown.size() == 2 && chev(pDown, {96, 238}, {100, 243}, {104, 238})
            && chev(pDown, {576, 238}, {580, 243}, {584, 238});
        check(QStringLiteral("(mv4) A PAN AND A TILT: a chevron (5 deep, 8 wide) on each edge along the move, "
                             "3 to 8 px past the edge's middle the way it goes - a pan right on the top and "
                             "bottom edges pointing right, left pointing left, a tilt up / down on the two "
                             "sides - and no other stroke"),
              goesRight && goesLeft && goesUp && goesDown,
              QStringLiteral("right [%1] up [%2]").arg(pts(pRight), pts(pUp)));
        auto way = [](double deg) {
            return QPointF(std::cos(qDegreesToRadians(deg)), std::sin(qDegreesToRadians(deg)));
        };
        const QList<QPolygonF> p20 = glyph(pan, way(20.0)), p25 = glyph(pan, way(25.0));
        bool diagonal = p25.size() == 12;
        for (const QPointF &c : corners)
            diagonal = diagonal && has(p25, c - way(25.0) * 22.0, c - way(25.0) * 7.0);
        check(QStringLiteral("(mv4) A DIAGONAL PAN (25 degrees, the Figma frame's own): at each corner a 15 px "
                             "arrow along the travel, its tip 7 px short of the corner"),
              diagonal, pts(p25));
        check(QStringLiteral("(mv4) control: within 22.5 degrees of level (20) it is still a pan's chevrons, "
                             "not the corner arrows"),
              p20.size() == 2 && chev(p20, {343, 104}, {348, 100}, {343, 96}), pts(p20));
        // NOTHING IN THE MIDDLE: every point of every glyph lies within 23 px of
        // the frame's outline (a point's distance to the nearest edge line).
        auto fromOutline = [](const QPointF &p) {
            return qMin(qMin(qAbs(p.x() - 100.0), qAbs(p.x() - 580.0)), qMin(qAbs(p.y() - 100.0), qAbs(p.y() - 370.0)));
        };
        double farthest = 0.0;
        for (const QList<QPolygonF> &subs : {pIn, pOut, pRight, pUp, p25, glyph(inPan, QPointF(1, 0)),
                                             glyph(cwTurn, QPointF())})
            for (const QPolygonF &p : subs)
                for (const QPointF &pt : p)
                    farthest = qMax(farthest, fromOutline(pt));
        // The control: the earlier glyph's double arrow, 30 px through the
        // middle, measured the same way.
        const double oldMiddle = qMin(fromOutline(QPointF(340 - 15, 235)), fromOutline(QPointF(340 + 15, 235)));
        check(QStringLiteral("(mv4) NOTHING IN THE MIDDLE: every point of every glyph (push, pull, pan, tilt, "
                             "diagonal, push and pan, turn) is within 23 px of the frame's outline"),
              farthest <= 23.0, QStringLiteral("farthest %1 px").arg(farthest));
        check(QStringLiteral("(mv4) control: the earlier glyph's double arrow through the middle measures 135 px "
                             "from the outline - the measure finds it"),
              oldMiddle > 100.0, QStringLiteral("%1 px").arg(oldMiddle));

        const QList<QPolygonF> pBoth = glyph(inPan, QPointF(1, 0));
        check(QStringLiteral("(mv5) PUSH IN + PAN: the four corner arrows and the two edge chevrons, nothing "
                             "else"),
              pBoth.size() == 14 && cornerArrows(pBoth, true, 6.0, 15.9)
                  && chev(pBoth, {343, 104}, {348, 100}, {343, 96}) && chev(pBoth, {343, 374}, {348, 370}, {343, 366}),
              pts(pBoth));
        // A TURN: a half circle of radius 12 centred 3.6 px above the top
        // edge's middle (340, 96.4), wholly above the edge, the head at the
        // end it turns to, pointing down at the frame.
        auto arcFrom = [&](const QList<QPolygonF> &subs, const QPointF &from, const QPointF &to) {
            if (subs.size() != 3 || subs.at(0).size() < 8 || !same2(subs.at(0).first(), from)
                || !same2(subs.at(0).last(), to))
                return false;
            double top = 1e9;
            for (const QPointF &p : subs.at(0)) {
                if (p.y() > 96.4 + 0.02 || qAbs(QLineF(p, QPointF(340, 96.4)).length() - 12.0) > 0.05)
                    return false;
                top = qMin(top, p.y());
            }
            return qAbs(top - 84.4) < 0.05;
        };
        const QPointF lEnd(328, 96.4), rEnd(352, 96.4);
        const QList<QPolygonF> pCw = glyph(cwTurn, QPointF()), pCcw = glyph(ccwTurn, QPointF());
        const bool turnsCw = arcFrom(pCw, lEnd, rEnd) && has(pCw, rEnd, QPointF(348.39, 91.24))
            && has(pCw, rEnd, QPointF(355.61, 91.24));
        const bool turnsCcw = arcFrom(pCcw, rEnd, lEnd) && has(pCcw, lEnd, QPointF(331.61, 91.24))
            && has(pCcw, lEnd, QPointF(324.39, 91.24));
        check(QStringLiteral("(mv5) A TURN: a half circle (radius 12) above the top edge, outside the frame, its "
                             "centre 3.6 px out from the edge's middle; clockwise the head at the right end, "
                             "anticlockwise at the left, pointing down at the frame"),
              turnsCw && turnsCcw, QStringLiteral("cw [%1] ccw [%2]").arg(pts(pCw), pts(pCcw)));
        // The frame's own axes: turned 30 degrees about its centre the glyphs
        // turn with it; mirrored (a flipped view) the clockwise head is at
        // the frame's own right end - on screen, the left.
        QTransform turn;
        turn.translate(340, 235);
        turn.rotate(30.0);
        turn.translate(-340, -235);
        const QPolygonF turned = turn.map(frame);
        const QPointF ex = (turned.at(1) - turned.at(0)) / 480.0, ey = (turned.at(3) - turned.at(0)) / 270.0;
        const QList<QPolygonF> tIn = glyph(zin, QPointF(), 1.0, turned), tCw = glyph(cwTurn, QPointF(), 1.0, turned);
        const QPointF tTop = (turned.at(0) + turned.at(1)) / 2.0 - ey * 3.6;
        const bool turnedOk = has(tIn, turned.at(0) + (ex + ey) * 6.0, turned.at(0) + (ex + ey) * 15.9)
            && tCw.size() == 3 && same2(tCw.at(0).first(), tTop - ex * 12.0) && same2(tCw.at(0).last(), tTop + ex * 12.0);
        const QPolygonF mirrored({QPointF(580, 100), QPointF(100, 100), QPointF(100, 370), QPointF(580, 370)});
        const QList<QPolygonF> mCw = glyph(cwTurn, QPointF(), 1.0, mirrored);
        const bool mirroredOk = mCw.size() == 3 && same2(mCw.at(0).last(), lEnd) && has(mCw, lEnd, QPointF(331.61, 91.24));
        check(QStringLiteral("(mv5) control: the glyphs are in the frame's own axes - turned 30 degrees they "
                             "turn with it (the push-in arrow 6 px in along the turned edges, the arc over the "
                             "turned top edge); mirrored, the clockwise head is at the screen's left end"),
              turnedOk && mirroredOk,
              QStringLiteral("turned in [%1] cw [%2]; mirrored cw [%3]").arg(pts(tIn.mid(0, 3)), pts(tCw), pts(mCw)));
    }

    delete scene;
    QDir(scratch).removeRecursively();
    ts << "\nRESULT " << (g_failures ? "FAIL" : "PASS") << " (checks=" << g_checks
       << " failures=" << g_failures << ")\n";
    outFile.close();
    return g_failures;
}
