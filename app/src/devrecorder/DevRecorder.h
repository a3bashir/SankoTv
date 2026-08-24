#pragma once

// ============================================================================
// TEMPORARY DEVELOPER RECORDER — remove before release.
// Removal: delete app/src/devrecorder/ and the single
// "Developer recorder (TEMP)" block in app/CMakeLists.txt, plus the one
// guarded block in MainWindow.cpp. CMake option: SANKOTV_DEV_RECORDER.
// ============================================================================
//
// Self-contained bug-capture tool for intermittent, hard-to-reproduce UI
// issues (motivating case: the floating-toolbar Grab Control hang). While
// recording it captures, into ONE session folder per run:
//   screenshots/   app-window grabs (true screen content, so partial
//                  repaints and floating tool windows are visible)
//   events.jsonl   machine-readable log: mouse/tablet input, enter/leave,
//                  focus, activation, mouse grab/ungrab, top-level
//                  show/hide/move/resize, shortcuts, host state snapshots,
//                  Qt warnings/criticals, perf samples, Issue markers
//   system.txt     OS / Qt / screens / build info
//   summary.txt    human-readable digest: counts, markers with the last
//                  events before each, worst UI stalls, dropped counts
//
// MODULARITY: no dependency on any host-app type. The host provides a
// window to capture and (optionally) a state-snapshot callback returning
// key/value pairs. Depends only on QtCore/QtGui/QtWidgets — portable to any
// Qt6 widgets app.
//
// Host integration surface (all one-liners):
//   auto *rec = devrec::Recorder::instance();
//   rec->initialize(mainWindow, {"MyApp", "1.0", "Debug", "abc123"});
//   rec->setStateProvider([]{ return QVariantMap{...}; });
//   menu->addAction(rec->toggleAction());   // Ctrl+Shift+R
//   menu->addAction(rec->markAction());     // Ctrl+Shift+B (Issue Happened)
//   menuBar->setCornerWidget(rec->indicatorWidget());
//
// THREADING: the UI thread only enqueues small records and performs the
// (unavoidable, GUI-thread-only) QScreen::grabWindow; downscale, encoding,
// disk I/O, and summary generation run on a dedicated worker thread.
// Queues are bounded; overflow drops the NEWEST record (history before an
// issue is what matters), and every drop is counted and reported.
//
// PRIVACY: captures the application window's rect only — never the desktop
// or other screens — and skips capture while the app is not the active
// window (so other applications' windows cannot be caught overlapping it).
// Session-size cap: when reached, screenshot capture stops (events, which
// are tiny, continue) and the summary says so.

#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtLogging>

#include <functional>

class QWidget;

namespace devrec {

struct HostInfo
{
    QString appName;
    QString appVersion;
    QString buildConfig; // "Debug" / "Release"
    QString gitHead;     // optional; captured at BUILD time, and suffixed
                         // "+dirty" when built from uncommitted work
    // When the BINARY was built. A build from a dirty tree is not any
    // commit, so the hash alone cannot tell you whether a recording
    // predates the work being investigated; this can.
    QString buildStamp;
};

class RecorderPrivate;

class Recorder : public QObject
{
    Q_OBJECT

public:
    static Recorder *instance();

    // Call once at startup. window = the top-level widget to capture.
    void initialize(QWidget *window, const HostInfo &info);

    // Optional: key/value snapshot of host state (current tool, brush
    // settings, ...), polled at the screenshot cadence and on markers.
    void setStateProvider(std::function<QVariantMap()> provider);

    // Optional: the host's CAMERA/VIEW state. Polled every 150 ms and
    // re-checked after every input event; a "cameraChange" record with
    // before/after values is emitted WHENEVER IT CHANGES, attributed to the
    // most recent input event ("lastInput" - a change with stale/no input
    // is programmatic). Degenerate states are flagged in the record itself:
    // non-finite/zero "scalePx" or "zoom", an empty visible rect
    // (visW/visH), or a visible rect (visX..) that does not intersect
    // canvasW/canvasH.
    void setCameraProvider(std::function<QVariantMap()> provider);

    // Optional: classifies a mouse press on a host widget (e.g. "grab
    // accepted on the floating-toolbar strip" vs "child button" vs
    // "outside"). Merged into every mousePress record; the matching
    // mouseRelease record gains dragMoved/frameDx/frameDy computed from the
    // pressed window's frame position, turning a motionless press-and-hold
    // into an unambiguous "grab accepted, hand never moved".
    void setPressClassifier(
        std::function<QVariantMap(QWidget *, const QPoint &)> classifier);

    bool isRecording() const;
    QString sessionDir() const; // empty when not recording

    // Ready-made UI: a checkable start/stop action (Ctrl+Shift+R), an
    // Issue Happened action (Ctrl+Shift+B, ApplicationShortcut so it works
    // mid-interaction without stealing focus), and a small menubar-corner
    // indicator (never a floating window, so it cannot interfere with the
    // floating toolbars under test).
    QAction *toggleAction();
    QAction *markAction();
    QWidget *indicatorWidget();

    // Configuration (persisted in QSettings under devrecorder/*; the
    // output directory can also be forced with SANKOTV_DEVREC_DIR).
    void setOutputRoot(const QString &dir);
    void setCaptureIntervalMs(int ms);      // default 500 (~2/second)
    void setImageFormat(const QString &fm); // "jpg" (default) or "png"
    void setDownscaleFactor(int f);         // default 2 (half resolution)
    void setSessionCapMB(int mb);           // default 400
    void setQueueCapacity(int records);     // default 20000 (tests use tiny)

public slots:
    void startRecording();
    void stopRecording(); // drains every buffered record before returning
    void toggleRecording();
    // Timestamped Issue Happened marker + immediate capture + state
    // snapshot. Safe mid-interaction: changes no focus, grabs nothing.
    void markIssue(const QString &note = QString());

    // Internal bridge for the installed Qt message handler (a free
    // function); not part of the host integration surface.
    void enqueueFromHandler(const QJsonObject &data);
    QtMessageHandler d_previousHandler() const;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    void captureNow();
    void logState(const QString &type);
    // Visible top-levels with geometry, modal overlap and z-order.
    QJsonArray topLevelAudit(const QRect &modalRect) const;
    void checkCamera();
    void enqueue(const QString &type, const QJsonObject &data);
    void writeSystemInfo();

    RecorderPrivate *d;
};

} // namespace devrec
