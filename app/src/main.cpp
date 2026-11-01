#include "MainWindow.h"
#include "SankoScrollBarStyle.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("SankoTV"));
    QApplication::setOrganizationName(QStringLiteral("Sanko"));

    // App-wide style — the ONE place it lives (see the header): the
    // scrollbars (scoped to QScrollBar selectors only, so it reaches every
    // scrollbar - dialogs, combo popups, text edits included - and nothing
    // else) and the text-selection colour of every text field (2026-11-01).
    app.setStyleSheet(sankoAppStyle());

    MainWindow window;
    window.show();

    return app.exec();
}
