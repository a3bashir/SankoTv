#include "DashboardPage.h"
#include "RecentProjectsView.h"
#include "RecentThumbnails.h"
#include "SankoTheme.h"
#include "brushlib/StudioControls.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <Qt>

namespace studio = brushlib::studio;

DashboardPage::DashboardPage(QWidget *parent)
    : QWidget(parent)
{
    // One cache for the life of the page: thumbnails decoded for one visit
    // are still there on the next.
    m_thumbnails = new RecentThumbnails(this);

    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(createHeaderBar());
    rootLayout->addWidget(createContentArea(), 1);
}

QWidget *DashboardPage::createHeaderBar()
{
    QWidget *header = new QWidget;
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(60);
    header->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QHBoxLayout *layout = new QHBoxLayout(header);
    layout->setContentsMargins(20, 0, 20, 0);
    layout->setSpacing(10);
    m_headerLayout = layout;

    // (The logo that stood at the left is gone by design. What may appear
    // there instead is the host's header accessory - setHeaderAccessory.)
    layout->addStretch(1);

    // --- Open Project button: the secondary action, left of the primary ---
    // Opens the file picker. The recents below cover the projects worked
    // on lately; this is the way to everything else.
    QPushButton *openProject = new QPushButton(QStringLiteral("Open Project"));
    openProject->setCursor(Qt::PointingHandCursor);
    openProject->setStyleSheet(
        QStringLiteral("QPushButton {"
                       "  background-color: %1;"
                       "  color: %2;"
                       "  border: 1px solid %3;"
                       "  border-radius: 6px;"
                       "  padding: 7px 18px;"
                       "  font-size: 14px;"
                       "  font-weight: 500;"
                       "}"
                       "QPushButton:hover { border-color: %4; color: %5; }")
            .arg(studio::kFieldBg.name(), studio::kFieldText.name(),
                 studio::kFieldBorder.name(), studio::kFieldBorderHover.name(),
                 QColor(Qt::white).name()));
    connect(openProject, &QPushButton::clicked, this,
            &DashboardPage::openProjectRequested);
    layout->addWidget(openProject);

    // --- New Project button (right) --------------------------------------
    QPushButton *newProject = new QPushButton(QStringLiteral("New Project"));
    newProject->setCursor(Qt::PointingHandCursor);
    newProject->setStyleSheet(SankoTheme::themed("QPushButton {"
        "  background-color: %ACCENT%;"
        "  color: #0a0a0a;"
        "  border: none;"
        "  border-radius: 6px;"
        "  padding: 8px 18px;"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background-color: %ACCENT_HOVER%; }"
        "QPushButton:pressed { background-color: %ACCENT_PRESSED%; }"));
    connect(newProject, &QPushButton::clicked, this, &DashboardPage::newProjectRequested);
    layout->addWidget(newProject);

    return header;
}

QWidget *DashboardPage::createContentArea()
{
    QWidget *content = new QWidget;
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(28);

    // --- Recent projects: one centred column, as wide as the three cards --
    // The title and the view share the column so the title's left edge is
    // the first card's left edge.
    QWidget *column = new QWidget;
    column->setFixedWidth(RecentProjectsView::kViewW);
    QVBoxLayout *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(14);

    m_recentsTitle = new QLabel(QStringLiteral("Recent Projects"));
    m_recentsTitle->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 600;")
            .arg(studio::kFieldText.name()));
    columnLayout->addWidget(m_recentsTitle);

    m_recents = new RecentProjectsView(m_thumbnails);
    connect(m_recents, &RecentProjectsView::openRequested, this,
            &DashboardPage::openRecentRequested);
    columnLayout->addWidget(m_recents);

    QHBoxLayout *centred = new QHBoxLayout;
    centred->addStretch(1);
    centred->addWidget(column);
    centred->addStretch(1);
    layout->addLayout(centred);

    // --- Empty state: no recents at all (first run) -----------------------
    // No card is drawn for a project that does not exist: a placeholder box
    // that looks like a project is exactly what this page used to show.
    m_emptyHint = new QLabel(QStringLiteral("Create a new project to get started"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px;")
            .arg(studio::kFieldLabel.name()));
    layout->addWidget(m_emptyHint);

    layout->addStretch(1);

    // The view says when it has re-read the list - including after it
    // removed a missing project itself - and the page follows.
    connect(m_recents, &RecentProjectsView::reloaded, this, [this, column] {
        const bool any = !m_recents->isEmpty();
        column->setVisible(any);
        m_emptyHint->setVisible(!any);
    });
    reloadRecents();

    return content;
}

void DashboardPage::reloadRecents()
{
    if (m_recents)
        m_recents->reload();
}

void DashboardPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reloadRecents();
}

void DashboardPage::setHeaderAccessory(QWidget *accessory)
{
    if (m_accessory.data() == accessory)
        return;
    // Only if it is still ours: the host may already have taken it back.
    if (m_accessory && m_headerLayout->indexOf(m_accessory.data()) >= 0)
        m_headerLayout->removeWidget(m_accessory.data());
    m_accessory = accessory;
    if (accessory) {
        m_headerLayout->insertWidget(0, accessory); // reparents it here
        accessory->show();
    }
}
