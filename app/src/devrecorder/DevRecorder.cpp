// TEMPORARY DEVELOPER RECORDER — see DevRecorder.h for the removal recipe.

#include "DevRecorder.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QWindow>

#ifdef SANKOTV_GIT_HEAD_GENERATED
#include "SankoGitHead.h" // GENERATED at build time; see cmake/WriteGitHead.cmake
#endif
#include <QLabel>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QSemaphore>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace devrec {

namespace {

struct Record
{
    qint64 seq = 0;
    double t = 0.0; // ms since session start
    QString type;
    QJsonObject data;
};

QString eventName(QEvent::Type t)
{
    switch (t) {
    case QEvent::MouseButtonPress: return QStringLiteral("mousePress");
    case QEvent::MouseButtonRelease: return QStringLiteral("mouseRelease");
    case QEvent::MouseButtonDblClick: return QStringLiteral("mouseDblClick");
    case QEvent::MouseMove: return QStringLiteral("mouseMove");
    case QEvent::TabletPress: return QStringLiteral("tabletPress");
    case QEvent::TabletRelease: return QStringLiteral("tabletRelease");
    case QEvent::TabletMove: return QStringLiteral("tabletMove");
    case QEvent::Enter: return QStringLiteral("enter");
    case QEvent::Leave: return QStringLiteral("leave");
    case QEvent::FocusIn: return QStringLiteral("focusIn");
    case QEvent::FocusOut: return QStringLiteral("focusOut");
    case QEvent::WindowActivate: return QStringLiteral("windowActivate");
    case QEvent::WindowDeactivate: return QStringLiteral("windowDeactivate");
    case QEvent::GrabMouse: return QStringLiteral("grabMouse");
    case QEvent::UngrabMouse: return QStringLiteral("ungrabMouse");
    case QEvent::Show: return QStringLiteral("show");
    case QEvent::Hide: return QStringLiteral("hide");
    case QEvent::Move: return QStringLiteral("move");
    case QEvent::Resize: return QStringLiteral("resize");
    // A modal dialog blocking/unblocking a top-level: the generic signal
    // for "a modal opened/closed", covering message boxes and dialogs that
    // do not exist yet.
    case QEvent::WindowBlocked: return QStringLiteral("modalOpen");
    case QEvent::WindowUnblocked: return QStringLiteral("modalClose");
    case QEvent::Shortcut: return QStringLiteral("shortcut");
    case QEvent::Wheel: return QStringLiteral("wheel");
    default: return QString();
    }
}

QString widgetLabel(const QObject *o)
{
    QString name = o->objectName();
    if (name.isEmpty())
        name = QString::fromLatin1(o->metaObject()->className());
    else
        name += QLatin1Char(':')
            + QString::fromLatin1(o->metaObject()->className());
    return name;
}

qint64 workingSetMB()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                         sizeof(pmc));
    return qint64(pmc.WorkingSetSize) >> 20;
#else
    return -1;
#endif
}

// Blinking-dot indicator; lives in the menubar corner, never floats.
class RecIndicator : public QWidget
{
public:
    RecIndicator()
    {
        setFixedSize(180, 20);
        setToolTip(QStringLiteral(
            "Developer recorder (TEMP). Ctrl+Shift+R start/stop, "
            "Ctrl+Shift+B = Issue Happened marker."));
        m_tick.setInterval(500);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            m_blink = !m_blink;
            update();
        });
    }
    void setActive(bool on)
    {
        m_active = on;
        m_blink = true;
        if (on) {
            m_started.start();
            m_tick.start();
        } else {
            m_tick.stop();
        }
        update();
    }
    void flashMarker()
    {
        m_flash = true;
        update();
        QTimer::singleShot(600, this, [this] {
            m_flash = false;
            update();
        });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (!m_active)
            return;
        p.setPen(Qt::NoPen);
        p.setBrush(m_flash ? QColor(0xff, 0xa0, 0x20)
                           : (m_blink ? QColor(0xe0, 0x40, 0x40)
                                      : QColor(0x80, 0x30, 0x30)));
        p.drawEllipse(QRectF(4, 5, 10, 10));
        p.setPen(QColor(0xd0, 0xd0, 0xd0));
        const qint64 s = m_started.elapsed() / 1000;
        p.drawText(rect().adjusted(20, 0, 0, 0),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("REC %1:%2  Ctrl+Sh+B=mark")
                       .arg(s / 60, 2, 10, QLatin1Char('0'))
                       .arg(s % 60, 2, 10, QLatin1Char('0')));
    }

private:
    bool m_active = false;
    bool m_blink = true;
    bool m_flash = false;
    QTimer m_tick;
    QElapsedTimer m_started;
};

} // namespace

// ---------------------------------------------------------------------------
// Worker: owns every file. Runs on its own thread; the UI thread only fills
// the bounded queues.
class RecorderWorker : public QObject
{
    Q_OBJECT

public:
    QMutex mutex;
    QVector<Record> queue;                     // bounded by capacity
    QVector<QPair<double, QImage>> shotQueue;  // bounded to 8
    int capacity = 20000;
    std::atomic<qint64> droppedRecords{0};
    std::atomic<qint64> droppedShots{0};

    QString dir;
    QString imageFormat = QStringLiteral("jpg");
    QString buildConfig;
    int imageQuality = 80;
    int downscale = 2;
    qint64 capBytes = qint64(400) * 1024 * 1024;
    bool capReached = false;

public slots:
    void open(const QString &sessionDir)
    {
        dir = sessionDir;
        m_file.setFileName(dir + QStringLiteral("/events.jsonl"));
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text))
            qWarning("devrecorder: cannot open %s",
                     qPrintable(m_file.fileName()));
        m_bytes = 0;
        capReached = false;
        m_counts.clear();
        m_markers.clear();
        m_recent.clear();
        m_warnings.clear();
        m_worstGaps.clear();
        m_drainTimer = new QTimer(this);
        m_drainTimer->setInterval(25);
        connect(m_drainTimer, &QTimer::timeout, this, &RecorderWorker::drain);
        m_drainTimer->start();
    }

    void drain()
    {
        QVector<Record> batch;
        QVector<QPair<double, QImage>> shots;
        {
            QMutexLocker lock(&mutex);
            batch.swap(queue);
            shots.swap(shotQueue);
        }
        for (const Record &r : batch)
            write(r);
        if (!m_file.isOpen())
            return;
        m_file.flush();
        for (const auto &s : shots)
            saveShot(s.first, s.second);
    }

    void finish(double durationMs, QSemaphore *done)
    {
        drain(); // everything buffered lands on disk — nothing is lost
        if (m_drainTimer)
            m_drainTimer->stop();
        writeSummary(durationMs);
        if (m_file.isOpen())
            m_file.close();
        done->release();
    }

private:
    void write(const Record &r)
    {
        // Bookkeeping for the summary.
        m_counts[r.type]++;
        m_recent.append(r);
        if (m_recent.size() > 250)
            m_recent.removeFirst();
        if (r.type == QLatin1String("marker")) {
            Marker m;
            m.t = r.t;
            m.note = r.data.value(QLatin1String("note")).toString();
            const int n = qMin(21, m_recent.size());
            for (int i = m_recent.size() - n; i < m_recent.size() - 1; ++i)
                m.context.append(line(m_recent.at(i)));
            m_markers.append(m);
        } else if (r.type == QLatin1String("qtMessage")) {
            if (m_warnings.size() < 50)
                m_warnings.append(line(r));
        } else if (r.type == QLatin1String("perf")) {
            const double gap =
                r.data.value(QLatin1String("maxGapMs")).toDouble();
            m_worstGaps.append({gap, r.t});
            std::sort(m_worstGaps.begin(), m_worstGaps.end(),
                      [](const QPair<double, double> &a,
                         const QPair<double, double> &b) {
                          return a.first > b.first;
                      });
            if (m_worstGaps.size() > 5)
                m_worstGaps.resize(5);
        }
        if (!m_file.isOpen())
            return;
        QJsonObject o = r.data;
        o.insert(QLatin1String("seq"), r.seq);
        o.insert(QLatin1String("t"), r.t);
        o.insert(QLatin1String("type"), r.type);
        m_file.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        m_file.write("\n");
    }

    static QString line(const Record &r)
    {
        return QStringLiteral("%1  #%2  %3  %4")
            .arg(r.t, 10, 'f', 2)
            .arg(r.seq)
            .arg(r.type, -14)
            .arg(QString::fromUtf8(
                QJsonDocument(r.data).toJson(QJsonDocument::Compact)));
    }

    void saveShot(double t, const QImage &img)
    {
        if (capReached)
            return;
        QImage out = img;
        if (downscale > 1)
            out = img.scaled(img.size() / downscale, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        const QString name = dir
            + QStringLiteral("/screenshots/t%1.%2")
                  .arg(qint64(t), 8, 10, QLatin1Char('0'))
                  .arg(imageFormat);
        out.save(name, imageFormat.toLatin1().constData(),
                 imageFormat == QLatin1String("jpg") ? imageQuality : -1);
        m_bytes += QFile(name).size();
        if (m_bytes >= capBytes) {
            // Defined cap behaviour: SCREENSHOTS STOP, events (tiny)
            // continue; the summary reports the cap was hit.
            capReached = true;
            Record r;
            r.type = QStringLiteral("capReached");
            r.t = t;
            write(r);
        }
    }

    void writeSummary(double durationMs)
    {
        QFile f(dir + QStringLiteral("/summary.txt"));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return;
        QTextStream ts(&f);
        ts << "SANKOTV DEV RECORDER SESSION SUMMARY\n";
        ts << "BUILD: " << buildConfig
           << "  (read timings against this profile)" << Qt::endl;
        ts << "folder: " << dir << "\n";
        ts << "duration: " << QString::number(durationMs / 1000.0, 'f', 1)
           << " s\n\n";
        ts << "-- event counts --\n";
        for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it)
            ts << QStringLiteral("%1 %2\n").arg(it.key(), -16).arg(it.value());
        ts << "\n-- dropped (bounded-queue overflow, drop-newest) --\n";
        ts << "records: " << droppedRecords.load()
           << "   screenshots: " << droppedShots.load() << "\n";
        if (capReached)
            ts << "\nSESSION SIZE CAP REACHED: screenshot capture stopped; "
                  "events continued.\n";
        ts << "\n-- ISSUE MARKERS (" << m_markers.size() << ") --\n";
        for (const Marker &m : m_markers) {
            ts << "\n=== marker at t=" << QString::number(m.t, 'f', 2)
               << " ms";
            if (!m.note.isEmpty())
                ts << "  note: " << m.note;
            ts << "\n    screenshot: the marker triggered an immediate "
                  "capture — see the screenshots/ file with the smallest t"
                  " >= this marker's t; find the log line by searching"
                  " events.jsonl for \"marker\" at this t\n";
            ts << "    last events before the marker:\n";
            for (const QString &l : m.context)
                ts << "      " << l << "\n";
        }
        ts << "\n-- worst UI-thread event-loop stalls --\n";
        for (const auto &g : m_worstGaps)
            ts << QStringLiteral("%1 ms at t=%2 ms\n")
                      .arg(g.first, 0, 'f', 1)
                      .arg(g.second, 0, 'f', 0);
        ts << "\n-- warnings / errors (" << m_warnings.size()
           << " captured, first 50) --\n";
        for (const QString &w : m_warnings)
            ts << w << "\n";
    }

    struct Marker
    {
        double t = 0.0;
        QString note;
        QStringList context;
    };

    QFile m_file;
    qint64 m_bytes = 0;
    QTimer *m_drainTimer = nullptr;
    QMap<QString, qint64> m_counts;
    QVector<Marker> m_markers;
    QVector<Record> m_recent;
    QStringList m_warnings;
    QVector<QPair<double, double>> m_worstGaps; // gapMs, t
};

// ---------------------------------------------------------------------------
class RecorderPrivate
{
public:
    QWidget *window = nullptr;
    HostInfo info;
    std::function<QVariantMap()> stateProvider;
    std::function<QVariantMap()> cameraProvider;
    std::function<QVariantMap(QWidget *, const QPoint &)> pressClassifier;
    QVariantMap lastCamera;
    QTimer cameraTimer;
    QJsonObject lastInput; // most recent input event, for attribution
    QPointer<QWidget> pressedWindow;
    QPoint pressedFramePos;

    bool recording = false;
    QString sessionDir;
    QString outputRoot;
    QElapsedTimer clock;
    std::atomic<qint64> seq{0};

    QThread thread;
    RecorderWorker *worker = nullptr;

    QTimer shotTimer;
    QTimer perfSampleTimer;
    QTimer probeTimer;
    QElapsedTimer probeClock;
    qint64 probeLast = 0;
    double probeMaxGap = 0.0;
    qint64 paintCount = 0;

    QAction *toggle = nullptr;
    QAction *mark = nullptr;
    // QPointer, NOT a raw pointer: QMenuBar::setCornerWidget takes
    // OWNERSHIP, so the indicator dies with the menu bar that holds it. In
    // the app there is one MainWindow per process and this never mattered;
    // the lifecycle gate constructs MainWindow repeatedly, and the second
    // window's setCornerWidget(indicatorWidget()) handed the menu bar a
    // DANGLING widget (crash in QMenuBar::setCornerWidget). The QPointer
    // nulls on deletion and indicatorWidget() recreates.
    QPointer<RecIndicator> indicator;

    int intervalMs = 500;
    QString format = QStringLiteral("jpg");
    int downscale = 2;
    int capMB = 400;
    int queueCap = 20000;

    QtMessageHandler previousHandler = nullptr;
};

static Recorder *s_instance = nullptr;

Recorder *Recorder::instance()
{
    if (!s_instance)
        s_instance = new Recorder(qApp);
    return s_instance;
}

static void messageHandler(QtMsgType type, const QMessageLogContext &ctx,
                           const QString &message);

Recorder::Recorder(QObject *parent) : QObject(parent), d(new RecorderPrivate)
{
    const QSettings s(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    d->outputRoot = qEnvironmentVariable(
        "SANKOTV_DEVREC_DIR",
        s.value(QStringLiteral("devrecorder/outputDir"),
                QStandardPaths::writableLocation(
                    QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/SankoTV-DevRecordings"))
            .toString());
    d->intervalMs =
        s.value(QStringLiteral("devrecorder/intervalMs"), 500).toInt();
    d->format = s.value(QStringLiteral("devrecorder/format"),
                        QStringLiteral("jpg"))
                    .toString();
    d->downscale =
        s.value(QStringLiteral("devrecorder/downscale"), 2).toInt();
    d->capMB = s.value(QStringLiteral("devrecorder/capMB"), 400).toInt();

    d->worker = new RecorderWorker;
    d->worker->moveToThread(&d->thread);
    d->thread.setObjectName(QStringLiteral("devrecWorker"));
    d->thread.start(QThread::LowPriority);

    d->toggle = new QAction(QStringLiteral("Start Recording (TEMP dev tool)"),
                            this);
    d->toggle->setCheckable(true);
    d->toggle->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    d->toggle->setShortcutContext(Qt::ApplicationShortcut);
    connect(d->toggle, &QAction::triggered, this,
            &Recorder::toggleRecording);

    d->mark = new QAction(QStringLiteral("Issue Happened!"), this);
    d->mark->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    d->mark->setShortcutContext(Qt::ApplicationShortcut);
    d->mark->setEnabled(false);
    connect(d->mark, &QAction::triggered, this, [this] { markIssue(); });

    d->shotTimer.setInterval(d->intervalMs);
    connect(&d->shotTimer, &QTimer::timeout, this, [this] {
        // Privacy guard: only grab while WE have focus, so an overlapping
        // foreign window can never be captured. A modal dialog of OURS
        // takes activation away from the main window, which is why this
        // used to skip capture AND the state poll for a modal's entire
        // lifetime — the recorder went blind exactly when a dialog bug was
        // on screen. Our own active modal counts as us; a foreign app with
        // focus still suppresses capture, because then neither the main
        // window nor our modal is the active window.
        if (!d->window)
            return;
        QWidget *modal = QApplication::activeModalWidget();
        const bool weHaveFocus = d->window->isActiveWindow()
            || (modal && modal->isActiveWindow());
        if (!weHaveFocus)
            return;
        captureNow();
        logState(QStringLiteral("statePoll"));
    });

    d->probeTimer.setInterval(10);
    connect(&d->probeTimer, &QTimer::timeout, this, [this] {
        const qint64 now = d->probeClock.elapsed();
        d->probeMaxGap = qMax(d->probeMaxGap, double(now - d->probeLast));
        d->probeLast = now;
    });
    d->perfSampleTimer.setInterval(1000);
    connect(&d->perfSampleTimer, &QTimer::timeout, this, [this] {
        QJsonObject o;
        o.insert(QLatin1String("maxGapMs"), d->probeMaxGap);
        o.insert(QLatin1String("workingSetMB"), workingSetMB());
        o.insert(QLatin1String("paints"), d->paintCount);
        d->probeMaxGap = 0.0;
        d->paintCount = 0;
        enqueue(QStringLiteral("perf"), o);
    });
    d->cameraTimer.setInterval(150); // cheap: a handful of doubles
    connect(&d->cameraTimer, &QTimer::timeout, this,
            [this] { checkCamera(); });
}

Recorder::~Recorder()
{
    if (d->recording)
        stopRecording();
    d->thread.quit();
    d->thread.wait(3000);
    delete d->worker;
    delete d;
}

void Recorder::initialize(QWidget *window, const HostInfo &info)
{
    d->window = window;
    d->info = info;
}

void Recorder::setStateProvider(std::function<QVariantMap()> provider)
{
    d->stateProvider = std::move(provider);
}

void Recorder::setCameraProvider(std::function<QVariantMap()> provider)
{
    d->cameraProvider = std::move(provider);
}

void Recorder::setPressClassifier(
    std::function<QVariantMap(QWidget *, const QPoint &)> classifier)
{
    d->pressClassifier = std::move(classifier);
}

// Compare the live camera snapshot to the last one; on ANY change emit a
// cameraChange record carrying before/after, the most recent input event
// (stale or absent input = programmatic change), and explicit DEGENERATE
// flags so a broken transform is visible in the log, not inferred.
void Recorder::checkCamera()
{
    if (!d->recording || !d->cameraProvider)
        return;
    const QVariantMap now = d->cameraProvider();
    if (now == d->lastCamera)
        return;
    QJsonObject o;
    o.insert(QLatin1String("before"),
             QJsonObject::fromVariantMap(d->lastCamera));
    o.insert(QLatin1String("after"), QJsonObject::fromVariantMap(now));
    o.insert(QLatin1String("lastInput"), d->lastInput);
    QStringList degenerate;
    const double zoom = now.value(QStringLiteral("zoom")).toDouble();
    const double scalePx =
        now.value(QStringLiteral("scalePx"), 1.0).toDouble();
    if (!(zoom > 0.0) || !qIsFinite(zoom))
        degenerate << QStringLiteral("zoom<=0-or-nonfinite");
    if (!(scalePx > 0.0) || !qIsFinite(scalePx))
        degenerate << QStringLiteral("scale<=0-or-nonfinite");
    if (now.contains(QStringLiteral("visW"))) {
        const QRect vis(now.value(QStringLiteral("visX")).toInt(),
                        now.value(QStringLiteral("visY")).toInt(),
                        now.value(QStringLiteral("visW")).toInt(),
                        now.value(QStringLiteral("visH")).toInt());
        const QRect canvasR(0, 0,
                            now.value(QStringLiteral("canvasW")).toInt(),
                            now.value(QStringLiteral("canvasH")).toInt());
        if (vis.isEmpty())
            degenerate << QStringLiteral("empty-visible-rect");
        else if (!canvasR.isEmpty() && !vis.intersects(canvasR))
            degenerate << QStringLiteral("visible-rect-misses-canvas");
    }
    if (now.value(QStringLiteral("paperOnScreen")).isValid()
        && !now.value(QStringLiteral("paperOnScreen")).toBool())
        degenerate << QStringLiteral("paper-off-screen");
    if (!degenerate.isEmpty())
        o.insert(QLatin1String("DEGENERATE"),
                 degenerate.join(QLatin1Char(',')));
    d->lastCamera = now;
    enqueue(QStringLiteral("cameraChange"), o);
}

bool Recorder::isRecording() const { return d->recording; }
QString Recorder::sessionDir() const { return d->sessionDir; }
QAction *Recorder::toggleAction() { return d->toggle; }
QAction *Recorder::markAction() { return d->mark; }

QWidget *Recorder::indicatorWidget()
{
    if (!d->indicator)
        d->indicator = new RecIndicator;
    return d->indicator;
}

void Recorder::setOutputRoot(const QString &dir) { d->outputRoot = dir; }
void Recorder::setCaptureIntervalMs(int ms)
{
    d->intervalMs = qMax(50, ms);
    d->shotTimer.setInterval(d->intervalMs);
}
void Recorder::setImageFormat(const QString &fm) { d->format = fm; }
void Recorder::setDownscaleFactor(int f) { d->downscale = qMax(1, f); }
void Recorder::setSessionCapMB(int mb) { d->capMB = qMax(1, mb); }
void Recorder::setQueueCapacity(int records)
{
    d->queueCap = qMax(16, records);
}

void Recorder::toggleRecording()
{
    if (d->recording)
        stopRecording();
    else
        startRecording();
}

void Recorder::startRecording()
{
    if (d->recording || !d->window)
        return;
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    d->sessionDir = d->outputRoot + QLatin1Char('/') + stamp;
    QDir().mkpath(d->sessionDir + QStringLiteral("/screenshots"));

    d->worker->imageFormat = d->format;
    d->worker->buildConfig = d->info.buildConfig;
    d->worker->downscale = d->downscale;
    d->worker->capBytes = qint64(d->capMB) * 1024 * 1024;
    d->worker->capacity = d->queueCap;
    d->worker->droppedRecords = 0;
    d->worker->droppedShots = 0;
    QMetaObject::invokeMethod(d->worker, "open", Qt::QueuedConnection,
                              Q_ARG(QString, d->sessionDir));

    writeSystemInfo();

    d->clock.start();
    d->seq = 0;
    d->recording = true;
    d->probeClock.start();
    d->probeLast = 0;
    d->probeMaxGap = 0.0;
    d->paintCount = 0;
    qApp->installEventFilter(this);
    d->previousHandler = qInstallMessageHandler(messageHandler);
    d->shotTimer.start();
    d->probeTimer.start();
    d->perfSampleTimer.start();
    d->lastCamera.clear();
    d->lastInput = QJsonObject();
    if (d->cameraProvider) {
        d->lastCamera = d->cameraProvider();
        enqueue(QStringLiteral("cameraInitial"),
                QJsonObject::fromVariantMap(d->lastCamera));
        d->cameraTimer.start();
    }
    d->toggle->setChecked(true);
    d->toggle->setText(QStringLiteral("Stop Recording (TEMP dev tool)"));
    d->mark->setEnabled(true);
    if (d->indicator)
        d->indicator->setActive(true);
    enqueue(QStringLiteral("sessionStart"), {});
    logState(QStringLiteral("stateInitial"));
    captureNow();
}

void Recorder::stopRecording()
{
    if (!d->recording)
        return;
    enqueue(QStringLiteral("sessionEnd"), {});
    d->recording = false;
    qApp->removeEventFilter(this);
    qInstallMessageHandler(d->previousHandler);
    d->previousHandler = nullptr;
    d->shotTimer.stop();
    d->probeTimer.stop();
    d->perfSampleTimer.stop();
    d->cameraTimer.stop();
    d->toggle->setChecked(false);
    d->toggle->setText(QStringLiteral("Start Recording (TEMP dev tool)"));
    d->mark->setEnabled(false);
    if (d->indicator)
        d->indicator->setActive(false);
    // Drain EVERYTHING buffered before returning: no record is lost on a
    // graceful stop.
    QSemaphore done;
    RecorderWorker *worker = d->worker;
    const double duration = double(d->clock.elapsed());
    QMetaObject::invokeMethod(
        d->worker, [worker, duration, &done] { worker->finish(duration, &done); },
        Qt::QueuedConnection);
    done.acquire();
    d->sessionDir.clear();
}

void Recorder::markIssue(const QString &note)
{
    if (!d->recording)
        return;
    QJsonObject o;
    if (!note.isEmpty())
        o.insert(QLatin1String("note"), note);
    // Markers bypass the queue bound: they must never be dropped.
    Record r;
    r.seq = ++d->seq;
    r.t = double(d->clock.nsecsElapsed()) / 1e6;
    r.type = QStringLiteral("marker");
    r.data = o;
    {
        QMutexLocker lock(&d->worker->mutex);
        d->worker->queue.append(r);
    }
    logState(QStringLiteral("stateAtMarker"));
    captureNow(); // immediate capture, regardless of the interval
    if (d->indicator)
        d->indicator->flashMarker();
}

void Recorder::captureNow()
{
    if (!d->window)
        return;
    QScreen *screen = d->window->screen();
    if (!screen)
        return;
    // Grab the SCREEN REGION the application window occupies (not the
    // window's own surface: on Windows a per-window grab excludes the
    // app's OTHER top-level windows — the floating tool bars this tool
    // exists to observe). Bounded to the app window's frame, never the
    // desktop or other screens; the active-window gate above keeps foreign
    // windows out of the region in practice (an always-on-top foreign
    // window overlapping the app is the one residual caveat).
    QRect fg = d->window->frameGeometry();
    // A modal dialog is normally centred ON the app window, so the app
    // frame already contains it and the captured region — and therefore the
    // privacy bound — is unchanged. Only when a dialog has been dragged
    // clear of the window (now possible: the frameless dialogs drag by
    // their header, and can cross monitors) is the region extended to
    // include it, because a capture that cannot see the dialog is useless
    // for the dialog bugs this exists to catch. Extending costs nothing:
    // the grab is a fixed ~16.6 ms regardless of region size (measured).
    // A modal on ANOTHER screen is captured only as far as this screen
    // reaches — noted in the record via "modalClipped".
    bool modalOutside = false, modalClipped = false;
    if (QWidget *modal = QApplication::activeModalWidget()) {
        const QRect mf = modal->frameGeometry();
        if (!fg.contains(mf)) {
            modalOutside = true;
            fg = fg.united(mf);
            modalClipped = modal->screen() != screen;
        }
    }
    const QRect sg = screen->geometry();
    fg = fg.intersected(sg); // never grab beyond this screen
    if (fg.isEmpty())
        return;
    const QPixmap shot =
        screen->grabWindow(0, fg.x() - sg.x(), fg.y() - sg.y(), fg.width(),
                           fg.height());
    if (modalOutside) {
        QJsonObject o;
        o.insert(QLatin1String("region"),
                 QStringLiteral("%1,%2 %3x%4").arg(fg.x()).arg(fg.y())
                     .arg(fg.width()).arg(fg.height()));
        o.insert(QLatin1String("modalClipped"), modalClipped);
        enqueue(QStringLiteral("captureRegionExtended"), o);
    }
    if (shot.isNull())
        return;
    const double t = double(d->clock.nsecsElapsed()) / 1e6;
    QMutexLocker lock(&d->worker->mutex);
    if (d->worker->shotQueue.size() >= 8) {
        ++d->worker->droppedShots;
        return;
    }
    d->worker->shotQueue.append({t, shot.toImage()});
}

void Recorder::logState(const QString &type)
{
    QJsonObject o;
    if (d->stateProvider)
        o = QJsonObject::fromVariantMap(d->stateProvider());
    // MODAL CONTEXT. A recording could show that a dialog interaction ran
    // but never what it was SEEDED with or what it would paint, which is
    // exactly what a "the values look blank" report needs. The identity and
    // geometry below need no host knowledge; the values come from an
    // OPTIONAL invokable the dialog may implement
    // (Q_INVOKABLE QVariantMap devrecState() const), so the recorder stays
    // host-agnostic and any dialog opts in with a few lines.
    if (QWidget *modal = QApplication::activeModalWidget()) {
        const QRect mf = modal->frameGeometry();
        o.insert(QLatin1String("modal.class"),
                 QString::fromLatin1(modal->metaObject()->className()));
        o.insert(QLatin1String("modal.name"), modal->objectName());
        o.insert(QLatin1String("modal.title"), modal->windowTitle());
        o.insert(QLatin1String("modal.geom"),
                 QStringLiteral("%1,%2 %3x%4").arg(mf.x()).arg(mf.y())
                     .arg(mf.width()).arg(mf.height()));
        o.insert(QLatin1String("modal.appModal"),
                 modal->windowModality() == Qt::ApplicationModal);
        if (d->window)
            o.insert(QLatin1String("modal.insideApp"),
                     d->window->frameGeometry().contains(mf));
        QVariantMap hostState;
        if (QMetaObject::invokeMethod(modal, "devrecState",
                                      Qt::DirectConnection,
                                      Q_RETURN_ARG(QVariantMap, hostState))) {
            for (auto it = hostState.cbegin(); it != hostState.cend(); ++it)
                o.insert(QLatin1String("modal.") + it.key(),
                         QJsonValue::fromVariant(it.value()));
        }
    } else if (!d->stateProvider) {
        return; // nothing to say
    }
    enqueue(type, o);
}

// Every visible top-level of OURS, with geometry, whether it overlaps the
// modal, and its z-order — the record that would have named an obscuring
// floating panel instead of costing an investigation. Overlap alone only
// proves adjacency; the z-index says which one is actually in front.
QJsonArray Recorder::topLevelAudit(const QRect &modalRect) const
{
    QHash<WId, int> zIndex; // lower = closer to the front
#ifdef Q_OS_WIN
    int idx = 0;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        zIndex.insert(reinterpret_cast<WId>(h), idx++);
#endif
    QJsonArray arr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (!w->isVisible())
            continue;
        const QRect g = w->frameGeometry();
        QJsonObject o;
        o.insert(QLatin1String("w"), widgetLabel(w));
        o.insert(QLatin1String("geom"),
                 QStringLiteral("%1,%2 %3x%4").arg(g.x()).arg(g.y())
                     .arg(g.width()).arg(g.height()));
        o.insert(QLatin1String("overlapsModal"),
                 !modalRect.isNull() && g.intersects(modalRect));
        // windowHandle() rather than winId(): asking a non-native widget
        // for winId() would CREATE a native window — the recorder must not
        // change what it measures. A shown top-level already has one.
        const int z = w->windowHandle()
            ? zIndex.value(w->windowHandle()->winId(), -1) : -1;
        o.insert(QLatin1String("z"), z);
        arr.append(o);
    }
    return arr;
}

void Recorder::enqueue(const QString &type, const QJsonObject &data)
{
    Record r;
    r.seq = ++d->seq;
    r.t = d->clock.isValid() ? double(d->clock.nsecsElapsed()) / 1e6 : 0.0;
    r.type = type;
    r.data = data;
    QMutexLocker lock(&d->worker->mutex);
    if (d->worker->queue.size() >= d->worker->capacity) {
        ++d->worker->droppedRecords; // drop-NEWEST, counted, never silent
        return;
    }
    d->worker->queue.append(r);
}

bool Recorder::eventFilter(QObject *object, QEvent *event)
{
    if (!d->recording || !object->isWidgetType())
        return false;
    const QEvent::Type t = event->type();
    if (t == QEvent::Paint) {
        if (object == d->window)
            ++d->paintCount;
        return false;
    }
    const QString name = eventName(t);
    if (name.isEmpty())
        return false;
    auto *w = static_cast<QWidget *>(object);
    // WINDOW-SCOPED events belong to the WINDOW, but Qt delivers them to
    // every widget inside it — so without this filter one modal dialog
    // opening produced 388 "modalOpen" records (one per button, label and
    // scrollbar), each running a full desktop z-order walk, and one window
    // activation produced a comparable burst. Geometry and visibility churn
    // were already filtered here; blocking and activation were not, and the
    // activation noise sat in HANDOFF for two days being read as noise
    // rather than as this same defect.
    if ((t == QEvent::Move || t == QEvent::Resize || t == QEvent::Show
         || t == QEvent::Hide || t == QEvent::WindowBlocked
         || t == QEvent::WindowUnblocked || t == QEvent::WindowActivate
         || t == QEvent::WindowDeactivate)
        && !w->isWindow())
        return false;

    QJsonObject o;
    o.insert(QLatin1String("w"), widgetLabel(w));
    switch (t) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: {
        auto *e = static_cast<QMouseEvent *>(event);
        o.insert(QLatin1String("x"), e->position().x());
        o.insert(QLatin1String("y"), e->position().y());
        o.insert(QLatin1String("gx"), e->globalPosition().x());
        o.insert(QLatin1String("gy"), e->globalPosition().y());
        o.insert(QLatin1String("btn"), int(e->button()));
        o.insert(QLatin1String("btns"), int(e->buttons()));
        o.insert(QLatin1String("mods"), int(e->modifiers()));
        if (t == QEvent::MouseButtonPress && w->isWindow()) {
            // Grab-acceptance classification + frame tracking, so a
            // motionless press-and-hold is unambiguous at release.
            if (d->pressClassifier) {
                const QVariantMap c =
                    d->pressClassifier(w, e->position().toPoint());
                for (auto it = c.cbegin(); it != c.cend(); ++it)
                    o.insert(it.key(), QJsonValue::fromVariant(it.value()));
            }
            d->pressedWindow = w;
            d->pressedFramePos = w->frameGeometry().topLeft();
        } else if (t == QEvent::MouseButtonRelease
                   && d->pressedWindow == w) {
            const QPoint delta =
                w->frameGeometry().topLeft() - d->pressedFramePos;
            o.insert(QLatin1String("frameDx"), delta.x());
            o.insert(QLatin1String("frameDy"), delta.y());
            o.insert(QLatin1String("dragMoved"), !delta.isNull());
            d->pressedWindow = nullptr;
        }
        break;
    }
    case QEvent::Wheel: {
        auto *e = static_cast<QWheelEvent *>(event);
        o.insert(QLatin1String("x"), e->position().x());
        o.insert(QLatin1String("y"), e->position().y());
        o.insert(QLatin1String("angleX"), e->angleDelta().x());
        o.insert(QLatin1String("angleY"), e->angleDelta().y());
        o.insert(QLatin1String("pixelX"), e->pixelDelta().x());
        o.insert(QLatin1String("pixelY"), e->pixelDelta().y());
        o.insert(QLatin1String("inverted"), e->inverted());
        o.insert(QLatin1String("mods"), int(e->modifiers()));
        break;
    }
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletMove: {
        auto *e = static_cast<QTabletEvent *>(event);
        o.insert(QLatin1String("x"), e->position().x());
        o.insert(QLatin1String("y"), e->position().y());
        o.insert(QLatin1String("pressure"), e->pressure());
        o.insert(QLatin1String("tiltX"), e->xTilt());
        o.insert(QLatin1String("tiltY"), e->yTilt());
        o.insert(QLatin1String("rotation"), e->rotation());
        o.insert(QLatin1String("btns"), int(e->buttons()));
        break;
    }
    // Move/Resize/Show carry the FULL frame rect, not just the half each
    // event happens to be about: a panel that moves while a modal is open
    // is otherwise untraceable after the fact — you can see THAT it moved
    // but never to where, so overlap can never be reconstructed. Show is
    // included so a window that never moves still has a geometry on record.
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show: {
        const QRect g = w->frameGeometry();
        o.insert(QLatin1String("x"), w->x());
        o.insert(QLatin1String("y"), w->y());
        o.insert(QLatin1String("wd"), w->width());
        o.insert(QLatin1String("ht"), w->height());
        o.insert(QLatin1String("geom"),
                 QStringLiteral("%1,%2 %3x%4").arg(g.x()).arg(g.y())
                     .arg(g.width()).arg(g.height()));
        break;
    }
    case QEvent::WindowBlocked: {
        // A modal just blocked this window: enumerate every visible
        // top-level with geometry, modal overlap and z-order, and snapshot
        // the modal's own state in the same breath.
        QRect modalRect;
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modalRect = modal->frameGeometry();
            o.insert(QLatin1String("modal"),
                     QString::fromLatin1(modal->metaObject()->className()));
            o.insert(QLatin1String("modalGeom"),
                     QStringLiteral("%1,%2 %3x%4").arg(modalRect.x())
                         .arg(modalRect.y()).arg(modalRect.width())
                         .arg(modalRect.height()));
        }
        o.insert(QLatin1String("topLevels"), topLevelAudit(modalRect));
        break;
    }
    default:
        break;
    }
    // Remember the input for camera-change attribution, then re-check the
    // camera right after inputs that can move it (wheel, press, release).
    if (t != QEvent::MouseMove && t != QEvent::TabletMove) {
        QJsonObject li;
        li.insert(QLatin1String("type"), name);
        li.insert(QLatin1String("w"), o.value(QLatin1String("w")));
        li.insert(QLatin1String("t"),
                  double(d->clock.nsecsElapsed()) / 1e6);
        d->lastInput = li;
    }
    enqueue(name, o);
    // A modal just opened: capture and poll state IMMEDIATELY rather than
    // waiting up to a full interval — the dialog's opening appearance and
    // its seeded values are the evidence a dialog report needs.
    if (t == QEvent::WindowBlocked && w == d->window) {
        captureNow();
        logState(QStringLiteral("statePoll"));
    }
    if (t == QEvent::Wheel || t == QEvent::MouseButtonRelease
        || t == QEvent::MouseButtonPress || t == QEvent::Shortcut)
        checkCamera();
    return false; // observe only — never consume, never perturb routing
}

void Recorder::writeSystemInfo()
{
    QFile f(d->sessionDir + QStringLiteral("/system.txt"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts << "app: " << d->info.appName << " " << d->info.appVersion << "\n";
    ts << "build: " << d->info.buildConfig << "\n";
    ts << "git: " << d->info.gitHead << "\n";
    ts << "built: " << d->info.buildStamp << "\n";
    ts << "os: " << QSysInfo::prettyProductName() << " ("
       << QSysInfo::kernelVersion() << ")\n";
    ts << "qt: " << QLatin1String(qVersion()) << "\n";
    const auto screens = QGuiApplication::screens();
    ts << "screens: " << screens.size() << "\n";
    for (QScreen *s : screens)
        ts << "  " << s->name() << " " << s->geometry().width() << "x"
           << s->geometry().height() << " @" << s->geometry().x() << ","
           << s->geometry().y() << " dpr=" << s->devicePixelRatio() << "\n";
#ifdef Q_OS_WIN
    QProcess gpu;
    gpu.start(QStringLiteral("wmic"),
              {QStringLiteral("path"), QStringLiteral("win32_VideoController"),
               QStringLiteral("get"), QStringLiteral("name")});
    if (gpu.waitForFinished(1500))
        ts << "gpu: "
           << QString::fromLocal8Bit(gpu.readAllStandardOutput())
                  .simplified()
           << "\n";
#endif
}

// Message handler: forwards to the previous handler after recording.
static void messageHandler(QtMsgType type, const QMessageLogContext &ctx,
                           const QString &message)
{
    if (s_instance && s_instance->isRecording()
        && (type == QtWarningMsg || type == QtCriticalMsg
            || type == QtFatalMsg)) {
        QJsonObject o;
        o.insert(QLatin1String("level"),
                 type == QtWarningMsg
                     ? QStringLiteral("warning")
                     : (type == QtCriticalMsg ? QStringLiteral("critical")
                                              : QStringLiteral("fatal")));
        o.insert(QLatin1String("msg"), message);
        if (ctx.file)
            o.insert(QLatin1String("src"),
                     QStringLiteral("%1:%2").arg(QLatin1String(ctx.file))
                         .arg(ctx.line));
        // Route through the public-ish enqueue via a queued call is
        // unnecessary: the handler can run on any thread, and enqueue only
        // touches the mutex-guarded queue and atomics — thread-safe here.
        static_cast<Recorder *>(s_instance)->enqueueFromHandler(o);
    }
    if (s_instance && s_instance->d_previousHandler())
        s_instance->d_previousHandler()(type, ctx, message);
}

void Recorder::enqueueFromHandler(const QJsonObject &o)
{
    enqueue(QStringLiteral("qtMessage"), o);
}

QtMessageHandler Recorder::d_previousHandler() const
{
    return d->previousHandler;
}

} // namespace devrec

#include "DevRecorder.moc"
