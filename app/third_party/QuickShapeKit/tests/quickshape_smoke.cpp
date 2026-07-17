#include "QuickShape/quickshape_recognizer.h"
#include "QuickShape/quickshape_session.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtMath>
#include <iostream>

using namespace quickshape;

static QVector<QPointF> sampledPath(const QVector<QPointF> &vertices, int perEdge = 28)
{
    QVector<QPointF> points;
    for (int edge = 1; edge < vertices.size(); ++edge) {
        for (int i = 0; i < perEdge; ++i) {
            const qreal t = i / qreal(perEdge);
            points.append(vertices[edge - 1] * (1.0 - t) + vertices[edge] * t);
        }
    }
    points.append(vertices.last());
    return points;
}

static QVector<QPointF> ellipseStroke()
{
    QVector<QPointF> points;
    for (int i = 0; i <= 100; ++i) {
        const qreal angle = 2.0 * 3.141592653589793 * i / 100.0;
        points.append({300.0 + 165.0 * qCos(angle), 230.0 + 65.0 * qSin(angle)});
    }
    return points;
}

static bool recognizedAs(const QVector<QPointF> &points, const QString &name)
{
    const QuickShapeResult result = QuickShapeRecognizer::recognize(points);
    const bool passed = result.accepted && result.name == name
        && result.rough.size() == result.target.size() && !result.painterPath().isEmpty();
    std::cout << (passed ? "PASS " : "FAIL ") << name.toStdString()
              << " => " << result.name.toStdString() << '\n';
    return passed;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    int failures = 0;

    failures += !recognizedAs(sampledPath({{70, 220}, {530, 220}}), "Line");
    failures += !recognizedAs(sampledPath({{80, 330}, {430, 330}, {505, 175}, {505, 70}}),
                              "Angled line");
    failures += !recognizedAs(ellipseStroke(), "Ellipse");
    failures += !recognizedAs(sampledPath({{90, 90}, {500, 90}, {500, 390},
                                            {90, 390}, {90, 90}}), "Rectangle");
    failures += !recognizedAs(sampledPath({{500, 245}, {465, 120}, {365, 70},
                                            {235, 70}, {120, 150}, {115, 300},
                                            {205, 405}, {375, 405}, {500, 300},
                                            {500, 245}}), "Polygon");

    QuickShapeSession session;
    bool active = false;
    bool committed = false;
    QuickShapeCommit commit;
    QObject::connect(&session, &QuickShapeSession::activeShapeChanged,
                     [&active](bool available) { active = available; });
    QObject::connect(&session, &QuickShapeSession::commitRequested,
                     [&committed, &commit](const QuickShapeCommit &value) {
                         committed = true;
                         commit = value;
                     });

    const QVector<QPointF> rectangle = sampledPath({{110, 100}, {500, 100},
                                                     {500, 380}, {110, 380}, {110, 100}});
    session.pointerPress(rectangle.first(), 0.4);
    for (qsizetype i = 1; i < rectangle.size(); ++i)
        session.pointerMove(rectangle[i], 0.4 + 0.5 * i / qreal(rectangle.size()));
    QEventLoop holdLoop;
    QTimer::singleShot(680, &holdLoop, &QEventLoop::quit);
    holdLoop.exec();
    session.pointerRelease(rectangle.last(), 0.9);
    session.requestCommit();

    const bool sessionPassed = committed && !active && commit.name == "Rectangle"
        && commit.points.size() == commit.pressures.size()
        && !commit.painterPath().isEmpty();
    std::cout << (sessionPassed ? "PASS " : "FAIL ")
              << "hold, morph, pressure mapping, and host commit signal\n";
    failures += !sessionPassed;
    return failures;
}

