#pragma once

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class RecentProjectsView;
class RecentThumbnails;

// The start window: what the app opens to, and what Close Project returns
// to. It shows the recent projects (three as cards, the rest as compact rows
// - see RecentProjectsView) and offers the two ways in that are not a
// recent: Open Project and New Project.
//
// The main window's menu bar is hidden while this page is up, so everything
// a first-time or returning artist needs is on the page itself.
class DashboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(QWidget *parent = nullptr);

    // Re-read the recents store. Runs on every show - the list changes
    // whenever a project is opened, saved or created, and this page is
    // never showing while that happens.
    void reloadRecents();
    RecentProjectsView *recentsView() const { return m_recents; }
    RecentThumbnails *thumbnails() const { return m_thumbnails; }
    QLabel *emptyHint() const { return m_emptyHint; }

    // A slot at the left of the header for something the host needs to keep
    // on screen while its menu bar is hidden (the developer recorder's
    // indicator, which normally sits in the menu bar's corner). The page
    // takes the widget into its layout; pass nullptr to give it back.
    void setHeaderAccessory(QWidget *accessory);
    QWidget *headerAccessory() const { return m_accessory.data(); }

signals:
    void newProjectRequested();
    void openProjectRequested();                   // the file picker
    void openRecentRequested(const QString &path); // a card or a row

protected:
    void showEvent(QShowEvent *event) override;

private:
    QWidget *createHeaderBar();
    QWidget *createContentArea();

    QHBoxLayout *m_headerLayout = nullptr;
    // QPointer: the accessory is the host's widget, and the host may move
    // it elsewhere or delete it without telling this page.
    QPointer<QWidget> m_accessory;
    RecentThumbnails *m_thumbnails = nullptr;
    RecentProjectsView *m_recents = nullptr;
    QLabel *m_recentsTitle = nullptr;
    QLabel *m_emptyHint = nullptr;
};
