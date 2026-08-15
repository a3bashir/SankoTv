#include "MainWindow.h"
#include "SankoScrollBarStyle.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("SankoTV"));
    QApplication::setOrganizationName(QStringLiteral("Sanko"));

    // App-wide scrollbar style — the ONE place it lives (see the header).
    // Scoped to QScrollBar selectors only, so it reaches every scrollbar
    // (dialogs, combo popups, text edits included) and nothing else.
    app.setStyleSheet(sankoScrollBarStyle());

    MainWindow window;
    window.show();

    return app.exec();
}
