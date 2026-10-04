// Linked into every gate family by CMake; no test source refers to it. It
// installs itself: part before main(), part when the application object is
// created. Two jobs, both about a gate that runs UNATTENDED on a machine
// someone is working at.
//
// 1. A TEST THAT CANNOT START MUST SAY WHY AND EXIT - NOT ASK.
//
//    Measured 2026-10-02: with its platform plugin missing, a Release test
//    blocked on Qt's own Windows message box ("no Qt platform plugin could
//    be initialized") with NOTHING on stderr. Qt raises that box only when
//    the process has no console window - which is every launch from Claude
//    - so an unattended gate did not fail, it waited for a click, eight
//    times in a row.
//
//    So a watchdog thread starts before main() and lives only until the
//    application object exists. A dialog owned by this process in that
//    window of time is a startup failure by definition: the watchdog prints
//    its text as the reason and exits with code 3. After startup it is
//    gone - dialogs raised by a running test are the test's business.
//
//    The C runtime's own dialogs (abort, assert, the debug heap's
//    corruption report that once hung a Debug run) go to stderr through the
//    documented switches, and the crash box is turned off.
//
//    NOT REACHABLE FROM HERE: a missing DLL. Windows reports that before a
//    single instruction of the executable runs. Only the LAUNCHER can
//    prevent that dialog (SetErrorMode, which children inherit) -
//    tools/run-gate.ps1 does it; the shells Claude launches from already do.
//
// 2. TEST WINDOWS OPEN ON ONE CHOSEN SCREEN.
//
//    Qt places an unpositioned top-level window on whichever screen holds
//    the mouse cursor (QPlatformWindow::initialGeometry; measured: created
//    on the Cintiq with the cursor there, on the Dell with it there). The
//    families never position their root windows, so a gate run threw
//    windows at whichever monitor the user was working on.
//
//    Which screen, in order:
//      SANKO_TEST_SCREEN   a screen name or part of one ("DELL"), a 1-based
//                          index, "primary", or "cursor" (the old behaviour:
//                          no pinning)
//      THE TEST SCREEN     the screen whose name contains kTestScreen below
//                          - the user's Cintiq. A FIXED default, their
//                          decision (2026-10-04): every run, theirs from
//                          PowerShell and Claude's, uses that one screen and
//                          the other stays free for their work.
//      the primary screen  when that screen is not connected - and the line
//                          says so.
//    One line on stderr says which was chosen and why.
//
//    WHAT THIS REPLACED, so nobody rebuilds it: the default used to be "the
//    launcher's screen" - this process's console window if visible, else
//    the nearest parent process owning a window. It could not keep its
//    promise. From Claude it found the Claude window only on a DIRECT
//    launch; the gate is launched through a bash script that runs a bash
//    script, the chain of parents ends at a process that no longer exists,
//    and every gate fell through to the primary screen. From a console it
//    put the windows on top of the output the person was watching. A fixed
//    screen has neither problem and nothing to detect.
//
//    HOW IT MOVES A WINDOW WITHOUT CHANGING WHAT A TEST MEASURES: only a
//    ROOT window (no parent) that nobody positioned is touched, once,
//    before it becomes visible, and it keeps its offset within the screen -
//    so the only thing that changes is which monitor. A window already on
//    the target screen is not touched at all. Dialogs and tool windows
//    position themselves against their parent and follow it.
//
// 3. A TEST WINDOW THAT IS MINIMISED, OR LOSES THE FOREGROUND, IS SAID SO.
//
//    No test minimises a window. If one is minimised during a run, someone
//    at the machine did it (or something else did), and checks that read
//    the screen or the floating bars fail for that reason alone: a
//    minimised main window hides the bars and the studio by design.
//
//    And no test gives the foreground away. If ANOTHER PROGRAM is activated
//    while a test window is on screen - a click in any other window does it
//    - the canvas loses focus, and the product then bakes a pending
//    QuickShape (DrawingCanvas::focusOutEvent; right for an artist who
//    clicks away). The QuickShape family's "button" checks fail from that
//    point on, at a different check each time (2026-10-04, with the machine
//    in use: 14 failures, then 0, 0, 9).
//
//    For both, a line is printed where it happens and another at exit, so
//    such a failure names its own cause instead of looking like a defect.
//    The lines report; they change nothing a test measures.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <crtdbg.h>

#ifdef QT_WIDGETS_LIB
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QVector>
#include <QWidget>
#include <QWindow>
#include <QWindowStateChangeEvent>
#endif

namespace {

// ---------------------------------------------------------------------------
// 1. Startup failures
// ---------------------------------------------------------------------------

std::atomic<bool> g_applicationExists{false};

struct DialogText
{
    DWORD pid = 0;
    bool found = false;
    wchar_t caption[256] = {0};
    wchar_t body[1536] = {0};
};

BOOL CALLBACK collectChildText(HWND child, LPARAM lp)
{
    auto *d = reinterpret_cast<DialogText *>(lp);
    wchar_t cls[32] = {0};
    GetClassNameW(child, cls, 32);
    if (lstrcmpiW(cls, L"Static") != 0)
        return TRUE; // the message is in static controls; skip the buttons
    wchar_t text[1024] = {0};
    if (GetWindowTextW(child, text, 1024) > 0) {
        if (d->body[0])
            wcsncat_s(d->body, L" ", _TRUNCATE);
        wcsncat_s(d->body, text, _TRUNCATE);
    }
    return TRUE;
}

BOOL CALLBACK findOwnDialog(HWND hwnd, LPARAM lp)
{
    auto *d = reinterpret_cast<DialogText *>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != d->pid || !IsWindowVisible(hwnd))
        return TRUE;
    wchar_t cls[32] = {0};
    GetClassNameW(hwnd, cls, 32);
    if (lstrcmpW(cls, L"#32770") != 0) // the system dialog class
        return TRUE;
    d->found = true;
    GetWindowTextW(hwnd, d->caption, 256);
    EnumChildWindows(hwnd, collectChildText, lp);
    return FALSE;
}

void startupWatchdog()
{
    DialogText d;
    d.pid = GetCurrentProcessId();
    while (!g_applicationExists.load()) {
        EnumWindows(findOwnDialog, reinterpret_cast<LPARAM>(&d));
        if (d.found) {
            // Newlines out: one failure, one line a gate log can grep.
            for (wchar_t *c = d.body; *c; ++c)
                if (*c == L'\r' || *c == L'\n')
                    *c = L' ';
            std::fprintf(stderr,
                         "STARTUP FAILED (exit 3, no dialog left waiting): "
                         "[%ls] %ls\n",
                         d.caption, d.body);
            std::fflush(stderr);
            std::fflush(stdout);
            // TerminateProcess, not exit(): the thread that raised the
            // dialog is blocked inside it and an orderly shutdown would
            // wait on that thread.
            TerminateProcess(GetCurrentProcess(), 3);
        }
        Sleep(25);
    }
}

struct BeforeMain
{
    BeforeMain()
    {
        // No "has stopped working" box, no "disk not ready" box. Keeps
        // whatever the launcher already set.
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                     | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // abort(): no message box, and no hand-off to error reporting.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // The CRT's own reports (asserts, the debug heap) to stderr. These
        // are no-ops in a Release CRT and the whole story in a Debug one.
        for (int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
        }
        std::thread(startupWatchdog).detach();
    }
} g_beforeMain;

// ---------------------------------------------------------------------------
// 2. One screen for every root window
// ---------------------------------------------------------------------------

#ifdef QT_WIDGETS_LIB

// THE TEST SCREEN: part of the name of the one screen every gate run uses
// unless SANKO_TEST_SCREEN says otherwise. The user's Cintiq 22HD - their
// decision; the Dell beside it is kept free for their other work.
const char kTestScreen[] = "Cintiq";

struct Choice
{
    QScreen *screen = nullptr; // null = do not pin (follow the cursor)
    QString why;
};

Choice chooseScreen()
{
    Choice c;
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QString wanted =
        QString::fromLocal8Bit(qgetenv("SANKO_TEST_SCREEN")).trimmed();
    QString note;
    if (!wanted.isEmpty()) {
        if (wanted.compare(QStringLiteral("cursor"), Qt::CaseInsensitive) == 0) {
            c.why = QStringLiteral("SANKO_TEST_SCREEN=cursor");
            return c;
        }
        if (wanted.compare(QStringLiteral("primary"), Qt::CaseInsensitive) == 0) {
            c.screen = QGuiApplication::primaryScreen();
            c.why = QStringLiteral("SANKO_TEST_SCREEN=primary");
            return c;
        }
        bool isIndex = false;
        const int index = wanted.toInt(&isIndex);
        if (isIndex && index >= 1 && index <= screens.size()) {
            c.screen = screens.at(index - 1);
            c.why = QStringLiteral("SANKO_TEST_SCREEN=%1").arg(index);
            return c;
        }
        for (QScreen *s : screens) {
            if (s->name().contains(wanted, Qt::CaseInsensitive)) {
                c.screen = s;
                c.why = QStringLiteral("SANKO_TEST_SCREEN=%1").arg(wanted);
                return c;
            }
        }
        // Named, and not there: say so and carry on. A typo in a
        // convenience setting must not fail a gate.
        note = QStringLiteral("SANKO_TEST_SCREEN=\"%1\" matches no screen; ")
                   .arg(wanted);
    }
    for (QScreen *s : screens) {
        if (s->name().contains(QLatin1String(kTestScreen), Qt::CaseInsensitive)) {
            c.screen = s;
            c.why = note + QStringLiteral("the fixed test screen");
            return c;
        }
    }
    c.screen = QGuiApplication::primaryScreen();
    c.why = note
        + QStringLiteral("the test screen \"%1\" is NOT CONNECTED, so the "
                         "primary screen").arg(QLatin1String(kTestScreen));
    return c;
}

class ScreenPin : public QObject
{
public:
    ScreenPin(QScreen *target, QObject *parent)
        : QObject(parent), m_target(target)
    {
    }
    ~ScreenPin() override
    {
        std::fprintf(stderr,
                     "TESTSCREEN: %d root window(s) placed; %d moved to the "
                     "chosen screen, %d already on it, %d ended up "
                     "elsewhere, %d never painted\n",
                     m_moved + m_alreadyThere + m_elsewhere
                         + int(m_unconfirmed.size()),
                     m_moved, m_alreadyThere, m_elsewhere,
                     int(m_unconfirmed.size()));
        std::fflush(stderr);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Paint && !m_unconfirmed.isEmpty()
            && object->isWidgetType()) {
            // CONFIRMED BY WHERE IT ACTUALLY IS, at its first paint - not by
            // where it was asked to go. The window system has had its say
            // by then.
            auto *painted = static_cast<QWidget *>(object);
            for (int i = m_unconfirmed.size() - 1; i >= 0; --i) {
                QWidget *root = m_unconfirmed.at(i).data();
                if (!root) {
                    m_unconfirmed.removeAt(i);
                } else if (root == painted) {
                    m_unconfirmed.removeAt(i);
                    if (QGuiApplication::screenAt(
                            root->frameGeometry().center())
                        == m_target)
                        ++m_moved;
                    else
                        ++m_elsewhere;
                }
            }
            return false;
        }
        if (event->type() != QEvent::Show || !object->isWidgetType())
            return false;
        auto *w = static_cast<QWidget *>(object);
        // Roots only. A window with a parent (a dialog, a tool window) is
        // placed against that parent by its own code or by Qt, and a window
        // somebody positioned keeps the position it was given.
        if (!w->isWindow() || w->parentWidget()
            || w->testAttribute(Qt::WA_Moved)
            || w->testAttribute(Qt::WA_DontShowOnScreen)
            || (w->windowType() != Qt::Window && w->windowType() != Qt::Dialog))
            return false;
        if (w->property("sankoTestScreenSeen").toBool())
            return false; // a root shown again keeps where it was put
        w->setProperty("sankoTestScreenSeen", true);
        QScreen *current =
            w->windowHandle() ? w->windowHandle()->screen() : nullptr;
        if (!current || !m_target)
            return false;
        if (current == m_target) {
            ++m_alreadyThere; // untouched: exactly what an unpinned run does
            return false;
        }
        // The Show event arrives BEFORE the native window is made visible,
        // so this move is never seen. Same offset within the screen; then
        // kept inside the target's usable area in case the screens differ.
        const QRect from = current->availableGeometry();
        const QRect to = m_target->availableGeometry();
        const QRect frame = w->frameGeometry();
        QPoint pos = to.topLeft() + (frame.topLeft() - from.topLeft());
        pos.setX(qMax(to.left(), qMin(pos.x(), to.right() - frame.width() + 1)));
        pos.setY(qMax(to.top(), qMin(pos.y(), to.bottom() - frame.height() + 1)));
        w->move(pos);
        m_unconfirmed.append(QPointer<QWidget>(w));
        return false;
    }

private:
    QScreen *m_target;
    QVector<QPointer<QWidget>> m_unconfirmed;
    int m_moved = 0;
    int m_alreadyThere = 0;
    int m_elsewhere = 0;
};

// ---------------------------------------------------------------------------
// 3. A test window that is minimised, or loses the foreground, is reported
// ---------------------------------------------------------------------------

// A test's own window, on screen now: a visible root that is not minimised.
bool rootWindowOnScreen()
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *w : tops) {
        if (w->parentWidget() || !w->isVisible() || w->isMinimized()
            || w->testAttribute(Qt::WA_DontShowOnScreen)
            || (w->windowType() != Qt::Window && w->windowType() != Qt::Dialog))
            continue;
        return true;
    }
    return false;
}

class WindowWatch : public QObject
{
public:
    explicit WindowWatch(QObject *parent)
        : QObject(parent)
    {
        m_clock.start();
        // THE APPLICATION going inactive, not one window: a family's own
        // windows and dialogs pass activation among themselves all the
        // time, and that is nobody's interference. Closing the last window
        // also makes the application inactive - so it counts only while a
        // test window is still on screen, which is when something else
        // took the foreground FROM it.
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive || !rootWindowOnScreen())
                return;
            // ANOTHER PROGRAM'S window, and nothing else. The application
            // also reads as inactive for a moment whenever activation
            // passes between two of its OWN windows - the first version of
            // this reported Lifecycle's own "Preset Images Resized" dialog
            // as the program that took the foreground. So: whose window is
            // in front? Ours, or none yet, is not a loss.
            const HWND front = GetForegroundWindow();
            DWORD owner = 0;
            if (front)
                GetWindowThreadProcessId(front, &owner);
            if (!front || owner == GetCurrentProcessId())
                return;
            wchar_t title[96] = {0};
            GetWindowTextW(front, title, 96);
            if (m_lost++ == 0) {
                m_firstLostMs = m_clock.elapsed();
                m_firstTaker = QString::fromWCharArray(title);
            }
            std::fflush(stdout);
            std::fprintf(stderr,
                         "TESTWINDOW: a test window LOST THE FOREGROUND here, "
                         "%.1f s into the run - another program was activated "
                         "(\"%ls\"), not by the test\n",
                         m_clock.elapsed() / 1000.0, title);
            std::fflush(stderr);
        });
    }
    ~WindowWatch() override
    {
        if (m_count > 0) {
            std::fprintf(stderr,
                         "TESTWINDOW: a test window was minimised %d time(s) "
                         "during this run, first %.1f s in. No test does that "
                         "- someone or something else did - and checks that "
                         "read the screen or the floating bars can fail for "
                         "that reason alone.\n",
                         m_count, m_firstMs / 1000.0);
        }
        if (m_lost > 0) {
            std::fprintf(stderr,
                         "TESTWINDOW: a test window lost the foreground %d "
                         "time(s) during this run, first %.1f s in, to \"%ls\". "
                         "No test does that - someone or something else "
                         "activated another program - and checks that need "
                         "the window to keep focus (a pending QuickShape is "
                         "baked when its canvas loses it) can fail for that "
                         "reason alone.\n",
                         m_lost, m_firstLostMs / 1000.0,
                         reinterpret_cast<const wchar_t *>(m_firstTaker.utf16()));
        }
        std::fflush(stderr);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::WindowStateChange || !object->isWidgetType())
            return false;
        auto *w = static_cast<QWidget *>(object);
        if (!w->isWindow() || w->parentWidget())
            return false; // roots only: tool windows follow their parent
        const auto *change = static_cast<QWindowStateChangeEvent *>(event);
        if ((w->windowState() & Qt::WindowMinimized)
            && !(change->oldState() & Qt::WindowMinimized)) {
            if (m_count++ == 0)
                m_firstMs = m_clock.elapsed();
            // Said HERE as well, so the log shows which checks it sits
            // between (stdout is flushed after every check).
            std::fflush(stdout);
            std::fprintf(stderr,
                         "TESTWINDOW: a test window was MINIMISED here, "
                         "%.1f s into the run - not by the test\n",
                         m_clock.elapsed() / 1000.0);
            std::fflush(stderr);
        }
        return false; // observe only
    }

private:
    QElapsedTimer m_clock;
    int m_count = 0; // minimised
    qint64 m_firstMs = 0;
    int m_lost = 0; // lost the foreground to another program
    qint64 m_firstLostMs = 0;
    QString m_firstTaker;
};

#endif // QT_WIDGETS_LIB

void applicationCreated()
{
    g_applicationExists.store(true); // startup is over; the watchdog stops
#ifdef QT_WIDGETS_LIB
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return; // no widgets in this run, so no windows to place
    // Whatever screen is chosen, and also when none is ("cursor").
    QCoreApplication::instance()->installEventFilter(
        new WindowWatch(QCoreApplication::instance()));
    const Choice choice = chooseScreen();
    if (!choice.screen) {
        std::fprintf(stderr, "TESTSCREEN: not pinned - windows follow the "
                             "cursor (%s)\n",
                     qPrintable(choice.why));
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "TESTSCREEN: test windows open on \"%s\" (%s)\n",
                 qPrintable(choice.screen->name()), qPrintable(choice.why));
    std::fflush(stderr);
    QCoreApplication::instance()->installEventFilter(
        new ScreenPin(choice.screen, QCoreApplication::instance()));
#endif
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(applicationCreated)
