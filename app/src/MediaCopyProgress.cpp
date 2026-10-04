#include "MediaCopyProgress.h"

#include <QProgressDialog>
#include <QWidget>

namespace {
ProjectMedia::Progress g_tapForTest;
const int kShowAfterMs = 500;
} // namespace

MediaCopyProgress::MediaCopyProgress(QWidget *parent, const QString &label)
    : m_parent(parent), m_label(label)
{
}

MediaCopyProgress::~MediaCopyProgress()
{
    delete m_dialog.data();
}

void MediaCopyProgress::setTapForTest(ProjectMedia::Progress tap)
{
    g_tapForTest = std::move(tap);
}

ProjectMedia::Progress MediaCopyProgress::callback()
{
    return [this](qint64 done, qint64 total) { return report(done, total); };
}

bool MediaCopyProgress::report(qint64 done, qint64 total)
{
    if (g_tapForTest && !g_tapForTest(done, total))
        return false;
    if (!m_clock.isValid())
        m_clock.start();
    if (!m_dialog) {
        if (m_clock.elapsed() < kShowAfterMs || done >= total)
            return true; // quick enough that a window would only flicker
        m_dialog = new QProgressDialog(m_label, QStringLiteral("Cancel"), 0,
                                       1000, m_parent);
        m_dialog->setWindowTitle(QStringLiteral("Copying Into Project"));
        m_dialog->setWindowModality(Qt::ApplicationModal);
        m_dialog->setMinimumDuration(0);
        m_dialog->setAutoClose(false);
        m_dialog->setAutoReset(false);
        m_dialog->show();
    }
    // A modal QProgressDialog runs the event loop inside setValue: that is
    // what paints it and what lets Cancel be pressed. Input to every other
    // window is blocked while it is up. (Asked BEFORE as well as after: a
    // cancelled QProgressDialog hides itself, and a further setValue would
    // show it again.)
    if (m_dialog->wasCanceled())
        return false;
    m_dialog->setValue(total > 0 ? int(done * 1000 / total) : 0);
    return !m_dialog->wasCanceled();
}
