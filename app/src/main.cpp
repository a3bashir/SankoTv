#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("SankoTV"));
    QApplication::setOrganizationName(QStringLiteral("Sanko"));

    MainWindow window;
    window.show();

    return app.exec();
}
