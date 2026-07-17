#include <QuickShape/quickshape_recognizer.h>

int main()
{
    const QVector<QPointF> points = {{0, 0}, {30, 0}, {60, 0}, {90, 0}};
    return quickshape::QuickShapeRecognizer::recognize(points).accepted ? 0 : 1;
}

