#include "DashboardPage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <Qt>

namespace {

// Card geometry. Thumbnail keeps a 16:9 ratio.
constexpr int kCardWidth = 320;
constexpr int kThumbWidth = kCardWidth - 32;             // minus card padding
constexpr int kThumbHeight = kThumbWidth * 9 / 16;       // 16:9

} // namespace

DashboardPage::DashboardPage(QWidget *parent)
    : QWidget(parent)
{
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

    // --- Logo (left) ------------------------------------------------------
    QLabel *logo = new QLabel;
    logo->setFixedSize(220, 44);
    logo->setAttribute(Qt::WA_TranslucentBackground, true);
    logo->setStyleSheet(QStringLiteral("background: transparent;"));
    QPixmap pm(QStringLiteral(":/assets/logo.png"));
    if (!pm.isNull()) {
        logo->setPixmap(pm.scaled(220, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logo->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    } else {
        logo->setText(QStringLiteral("SANKO TV"));
        logo->setStyleSheet(QStringLiteral(
            "background: transparent; color: #ffffff; font-size: 20px; font-weight: 700;"));
    }
    layout->addWidget(logo);

    layout->addStretch(1);

    // --- New Project button (right) --------------------------------------
    QPushButton *newProject = new QPushButton(QStringLiteral("New Project"));
    newProject->setCursor(Qt::PointingHandCursor);
    newProject->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: #f5a623;"
        "  color: #0a0a0a;"
        "  border: none;"
        "  border-radius: 6px;"
        "  padding: 8px 18px;"
        "  font-size: 14px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background-color: #ffb733; }"
        "QPushButton:pressed { background-color: #e0991c; }"));
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

    // --- Cards row (centered) --------------------------------------------
    QHBoxLayout *cards = new QHBoxLayout;
    cards->setSpacing(24);
    cards->addStretch(1);
    cards->addWidget(createProjectCard(
        QStringLiteral("Untitled Project 01"), QStringLiteral("Modified 2 hours ago")));
    cards->addWidget(createProjectCard(
        QStringLiteral("Untitled Project 02"), QStringLiteral("Modified yesterday")));
    cards->addWidget(createProjectCard(
        QStringLiteral("Untitled Project 03"), QStringLiteral("Modified 3 days ago")));
    cards->addStretch(1);

    layout->addLayout(cards);

    // --- Empty state ------------------------------------------------------
    QLabel *empty = new QLabel(QStringLiteral("Create a new project to get started"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(QStringLiteral("color: #666666; font-size: 13px;"));
    layout->addWidget(empty);

    layout->addStretch(1);

    return content;
}

QWidget *DashboardPage::createProjectCard(const QString &name, const QString &modified)
{
    QFrame *card = new QFrame;
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedWidth(kCardWidth);
    card->setStyleSheet(QStringLiteral(
        "QFrame {"
        "  background-color: #161616;"
        "  border: 1px solid #2a2a2a;"
        "  border-radius: 10px;"
        "}"));

    QVBoxLayout *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // Thumbnail placeholder (16:9 grey rectangle).
    QLabel *thumb = new QLabel;
    thumb->setFixedSize(kThumbWidth, kThumbHeight);
    thumb->setStyleSheet(QStringLiteral(
        "background-color: #333333; border: none; border-radius: 6px;"));
    layout->addWidget(thumb);

    // Project name.
    QLabel *title = new QLabel(name);
    title->setStyleSheet(QStringLiteral(
        "color: #ffffff; font-size: 15px; font-weight: 600; border: none;"));
    layout->addWidget(title);

    // Last modified date.
    QLabel *date = new QLabel(modified);
    date->setStyleSheet(QStringLiteral("color: #666666; font-size: 12px; border: none;"));
    layout->addWidget(date);

    return card;
}
