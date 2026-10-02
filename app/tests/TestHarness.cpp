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
//      the launcher's      the console window if this process has a visible
//                          one; else the nearest parent process that owns a
//                          visible window, preferring the foreground window
//                          when it is that process's
//      the primary screen
//    and one line on stderr says which was chosen and why.
//
//    HOW IT MOVES A WINDOW WITHOUT CHANGING WHAT A TEST MEASURES: only a
//    ROOT window (no parent) that nobody positioned is touched, once,
//    before it becomes visible, and it keeps its offset within the screen -
//    so the only thing that changes is which monitor. A window already on
//    the target screen is not touched at all. Dialogs and tool windows
//    position themselves against their parent and follow it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

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
#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QVector>
#include <QWidget>
#include <QWindow>
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

struct TopLevelSearch
{
    DWORD pid = 0;
    HWND foreground = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK findLauncherWindow(HWND hwnd, LPARAM lp)
{
    auto *s = reinterpret_cast<TopLevelSearch *>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left < 200 || r.bottom - r.top < 120)
        return TRUE; // not a main window: a tray helper, a tooltip host
    if (hwnd == s->foreground) {
        s->found = hwnd; // the window the user is actually looking at
        return FALSE;
    }
    if (!s->found)
        s->found = hwnd;
    return TRUE;
}

// The window this test was launched from, and a word for the report line.
HWND launcherWindow(QString *why)
{
    // A classic console: the window itself. (Under Windows Terminal the
    // console window is a hidden stand-in, so this is skipped and the
    // parent-process walk below finds the terminal's own window.)
    if (HWND console = GetConsoleWindow()) {
        if (IsWindowVisible(console)) {
            *why = QStringLiteral("this process's console window");
            return console;
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return nullptr;
    HWND result = nullptr;
    DWORD pid = GetCurrentProcessId();
    for (int depth = 0; depth < 12 && pid && !result; ++depth) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        DWORD parent = 0;
        QString name;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == pid) {
                    parent = pe.th32ParentProcessID;
                    name = QString::fromWCharArray(pe.szExeFile);
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        if (name.isEmpty())
            break; // the chain is broken: a parent has exited
        if (depth > 0) { // never this process's own windows
            TopLevelSearch s;
            s.pid = pid;
            s.foreground = GetForegroundWindow();
            EnumWindows(findLauncherWindow, reinterpret_cast<LPARAM>(&s));
            if (s.found) {
                result = s.found;
                *why = QStringLiteral("the window of %1, which launched this "
                                      "test%2")
                           .arg(name, s.found == s.foreground
                                          ? QStringLiteral(" (its foreground "
                                                           "window)")
                                          : QString());
            }
        }
        pid = parent;
    }
    CloseHandle(snap);
    return result;
}

QScreen *screenOfNativeWindow(HWND hwnd)
{
    if (!hwnd)
        return nullptr;
    // Ask Windows which MONITOR the window is on and match Qt's screens by
    // their own monitor handles - no coordinates converted, so differing
    // scale factors cannot put the answer on the neighbour.
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!monitor)
        return nullptr;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *s : screens) {
        const auto *native =
            s->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (native && native->handle() == monitor)
            return s;
    }
    return nullptr;
}

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
    QString why;
    if (QScreen *s = screenOfNativeWindow(launcherWindow(&why))) {
        c.screen = s;
        c.why = note + why;
        return c;
    }
    c.screen = QGuiApplication::primaryScreen();
    c.why = note + QStringLiteral("no launcher window found, so the primary "
                                  "screen");
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

#endif // QT_WIDGETS_LIB

void applicationCreated()
{
    g_applicationExists.store(true); // startup is over; the watchdog stops
#ifdef QT_WIDGETS_LIB
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return; // no widgets in this run, so no windows to place
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
