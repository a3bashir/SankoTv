#include "QuickShape/quickshape_recognizer.h"

#include <QLineF>
#include <QRectF>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace quickshape {

bool isClosedShapeType(const QString &shapeName)
{
    return shapeName != "Line" && shapeName != "Arc" && shapeName != "Elliptical Arc"
        && shapeName != "Angled line" && shapeName != "Zigzag"
        && shapeName != "Polyline";
}

static QPainterPath pathFromPoints(const QVector<QPointF> &points, bool closed)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points.first());
    for (qsizetype i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);
    if (closed)
        path.closeSubpath();
    return path;
}

bool QuickShapeResult::isClosed() const
{
    return isClosedShapeType(name);
}

QPainterPath QuickShapeResult::painterPath() const
{
    return pathFromPoints(target, isClosed());
}

bool QuickShapeCommit::isClosed() const
{
    return isClosedShapeType(name);
}

QPainterPath QuickShapeCommit::painterPath() const
{
    return pathFromPoints(points, isClosed());
}

namespace {

constexpr int SampleCount = 64;
constexpr int MaximumHistorySteps = 30;
constexpr qreal Pi = 3.14159265358979323846;

bool isClosedShapeName(const QString &name)
{
    return name != "Line" && name != "Arc" && name != "Elliptical Arc"
        && name != "Angled line"
        && name != "Zigzag" && name != "Polyline";
}

qreal distance(const QPointF &a, const QPointF &b)
{
    return QLineF(a, b).length();
}

QRectF pointBounds(const QVector<QPointF> &points)
{
    if (points.isEmpty())
        return {};
    qreal left = points.first().x();
    qreal right = left;
    qreal top = points.first().y();
    qreal bottom = top;
    for (const QPointF &point : points) {
        left = qMin(left, point.x());
        right = qMax(right, point.x());
        top = qMin(top, point.y());
        bottom = qMax(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

QVector<QPointF> resample(const QVector<QPointF> &input, int count)
{
    QVector<QPointF> result;
    if (input.isEmpty())
        return result;
    if (input.size() == 1) {
        result.fill(input.first(), count);
        return result;
    }

    QVector<qreal> lengths(input.size(), 0.0);
    for (qsizetype i = 1; i < input.size(); ++i)
        lengths[i] = lengths[i - 1] + distance(input[i - 1], input[i]);
    const qreal total = lengths.last();
    if (total < 0.001) {
        result.fill(input.first(), count);
        return result;
    }

    int segment = 1;
    for (int i = 0; i < count; ++i) {
        const qreal wanted = total * i / qreal(count - 1);
        while (segment < lengths.size() - 1 && lengths[segment] < wanted)
            ++segment;
        const qreal span = lengths[segment] - lengths[segment - 1];
        const qreal t = span > 0.0 ? (wanted - lengths[segment - 1]) / span : 0.0;
        result.append(input[segment - 1] * (1.0 - t) + input[segment] * t);
    }
    return result;
}

QVector<qreal> resampleValues(const QVector<QPointF> &points, const QVector<qreal> &values,
                              int count)
{
    QVector<qreal> result;
    if (points.isEmpty() || values.size() != points.size())
        return result;
    if (points.size() == 1) {
        result.fill(values.first(), count);
        return result;
    }
    QVector<qreal> lengths(points.size(), 0.0);
    for (qsizetype i = 1; i < points.size(); ++i)
        lengths[i] = lengths[i - 1] + distance(points[i - 1], points[i]);
    const qreal total = lengths.last();
    int segment = 1;
    for (int i = 0; i < count; ++i) {
        const qreal wanted = total * i / qreal(count - 1);
        while (segment < lengths.size() - 1 && lengths[segment] < wanted)
            ++segment;
        const qreal span = lengths[segment] - lengths[segment - 1];
        const qreal t = span > 0.0 ? (wanted - lengths[segment - 1]) / span : 0.0;
        result.append(values[segment - 1] * (1.0 - t) + values[segment] * t);
    }
    return result;
}

QPointF rotatePoint(const QPointF &p, qreal radians)
{
    const qreal c = qCos(radians);
    const qreal s = qSin(radians);
    return {p.x() * c - p.y() * s, p.x() * s + p.y() * c};
}

QVector<QPointF> sampleClosedPolygon(const QVector<QPointF> &vertices, int count)
{
    QVector<QPointF> loop = vertices;
    loop.append(vertices.first());
    QVector<QPointF> openSamples = resample(loop, count + 1);
    openSamples.removeLast();
    return openSamples;
}

qreal pointToSegmentDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const qreal denominator = QPointF::dotProduct(ab, ab);
    if (denominator < 0.0001)
        return distance(point, a);
    const qreal t = qBound(0.0, QPointF::dotProduct(point - a, ab) / denominator, 1.0);
    return distance(point, a + ab * t);
}

qreal pointToPathDistance(const QPointF &point, const QVector<QPointF> &path, bool closed)
{
    qreal best = std::numeric_limits<qreal>::max();
    const int segmentCount = closed ? path.size() : path.size() - 1;
    for (int i = 0; i < segmentCount; ++i)
        best = qMin(best, pointToSegmentDistance(point, path[i], path[(i + 1) % path.size()]));
    return best;
}

qreal geometricFitError(const QVector<QPointF> &stroke, const QVector<QPointF> &model,
                        bool modelClosed)
{
    if (stroke.size() < 2 || model.size() < 2)
        return std::numeric_limits<qreal>::max();
    qreal strokeToModel = 0.0;
    for (const QPointF &point : stroke)
        strokeToModel += pointToPathDistance(point, model, modelClosed);
    strokeToModel /= stroke.size();

    // The reverse term makes sure the artist covered the whole proposed shape.
    qreal modelToStroke = 0.0;
    for (const QPointF &point : model)
        modelToStroke += pointToPathDistance(point, stroke, false);
    modelToStroke /= model.size();
    return strokeToModel * 0.68 + modelToStroke * 0.32;
}

QVector<QPointF> uniformEllipse(const QPointF &center, qreal radiusX, qreal radiusY,
                                qreal angle, int count)
{
    QVector<QPointF> dense;
    constexpr int DenseCount = 256;
    for (int i = 0; i < DenseCount; ++i) {
        const qreal a = 2.0 * Pi * i / DenseCount;
        dense.append(center + rotatePoint(QPointF(radiusX * qCos(a), radiusY * qSin(a)), angle));
    }
    return sampleClosedPolygon(dense, count);
}

QVector<QPointF> ellipticalArc(const QPointF &center, qreal radiusX, qreal radiusY,
                               qreal rotation, qreal startPhase, qreal sweep, int count)
{
    QVector<QPointF> points;
    points.reserve(count);
    for (int i = 0; i < count; ++i) {
        const qreal phase = startPhase + sweep * i / qreal(count - 1);
        points.append(center + rotatePoint(QPointF(radiusX * qCos(phase),
                                                    radiusY * qSin(phase)), rotation));
    }
    return points;
}

qreal majorOpenSweep(qreal signedSweep)
{
    qreal sweep = std::fmod(qAbs(signedSweep), 2.0 * Pi);
    if (sweep < Pi)
        sweep = 2.0 * Pi - sweep;
    return signedSweep < 0.0 ? -sweep : sweep;
}

struct CircleFit
{
    bool valid = false;
    QPointF center;
    qreal radius = 0.0;
    qreal sweep = 0.0;
};

CircleFit fitCircle(const QVector<QPointF> &points)
{
    CircleFit fit;
    if (points.size() < 5)
        return fit;
    QPointF mean;
    for (const QPointF &point : points)
        mean += point;
    mean /= points.size();
    qreal matrix[3][4] = {};
    for (const QPointF &point : points) {
        const qreal x = point.x() - mean.x();
        const qreal y = point.y() - mean.y();
        const qreal z = x * x + y * y;
        const qreal row[3] = {x, y, 1.0};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                matrix[r][c] += row[r] * row[c];
            matrix[r][3] += row[r] * z;
        }
    }
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row)
            if (qAbs(matrix[row][column]) > qAbs(matrix[pivot][column])) pivot = row;
        if (qAbs(matrix[pivot][column]) < 0.000001)
            return fit;
        for (int c = column; c < 4; ++c)
            std::swap(matrix[column][c], matrix[pivot][c]);
        const qreal divisor = matrix[column][column];
        for (int c = column; c < 4; ++c)
            matrix[column][c] /= divisor;
        for (int row = 0; row < 3; ++row) {
            if (row == column) continue;
            const qreal factor = matrix[row][column];
            for (int c = column; c < 4; ++c)
                matrix[row][c] -= factor * matrix[column][c];
        }
    }
    const qreal a = matrix[0][3], b = matrix[1][3], c = matrix[2][3];
    const qreal radiusSquared = c + (a * a + b * b) / 4.0;
    if (radiusSquared <= 1.0)
        return fit;
    fit.center = mean + QPointF(a / 2.0, b / 2.0);
    fit.radius = qSqrt(radiusSquared);
    qreal previous = qAtan2(points.first().y() - fit.center.y(),
                            points.first().x() - fit.center.x());
    for (qsizetype i = 1; i < points.size(); ++i) {
        const qreal current = qAtan2(points[i].y() - fit.center.y(),
                                    points[i].x() - fit.center.x());
        fit.sweep += qAtan2(qSin(current - previous), qCos(current - previous));
        previous = current;
    }
    fit.valid = true;
    return fit;
}

QVector<QPointF> circularArc(const CircleFit &fit, const QPointF &strokeStart, int count)
{
    QVector<QPointF> points;
    const qreal start = qAtan2(strokeStart.y() - fit.center.y(),
                              strokeStart.x() - fit.center.x());
    for (int i = 0; i < count; ++i) {
        const qreal angle = start + fit.sweep * i / qreal(count - 1);
        points.append(fit.center + QPointF(fit.radius * qCos(angle), fit.radius * qSin(angle)));
    }
    return points;
}

QVector<QPointF> regularPolygon(const QPointF &center, qreal radius, int sides,
                                qreal rotation, int count)
{
    QVector<QPointF> vertices;
    for (int side = 0; side < sides; ++side) {
        const qreal angle = rotation + 2.0 * Pi * side / sides;
        vertices.append(center + QPointF(radius * qCos(angle), radius * qSin(angle)));
    }
    return sampleClosedPolygon(vertices, count);
}

int strongCornerCount(const QVector<QPointF> &stroke, bool closed)
{
    if (stroke.size() < 16)
        return 0;
    struct Peak { qreal angle; int index; };
    QVector<Peak> peaks;
    constexpr int step = 3;
    const int n = stroke.size();
    const int begin = closed ? 0 : step;
    const int end = closed ? n : n - step;
    for (int i = begin; i < end; ++i) {
        const int before = closed ? (i - step + n) % n : i - step;
        const int after = closed ? (i + step) % n : i + step;
        const QPointF incoming = stroke[i] - stroke[before];
        const QPointF outgoing = stroke[after] - stroke[i];
        const qreal lengths = QLineF(QPointF(), incoming).length() * QLineF(QPointF(), outgoing).length();
        if (lengths < 0.001)
            continue;
        const qreal cosine = qBound(-1.0, QPointF::dotProduct(incoming, outgoing) / lengths, 1.0);
        const qreal turn = qAcos(cosine);
        // A lower threshold mistakes the smoothly curved ends of a flat ellipse
        // for several polygon corners. Genuine hand-drawn corners remain well
        // above this value after arc-length resampling.
        if (turn > qDegreesToRadians(38.0))
            peaks.append({turn, i});
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak &a, const Peak &b) {
        return a.angle > b.angle;
    });
    QVector<int> selected;
    for (const Peak &peak : peaks) {
        bool separated = true;
        for (int index : selected) {
            const int delta = qAbs(index - peak.index);
            const int separation = closed ? qMin(delta, n - delta) : delta;
            if (separation < 7) {
                separated = false;
                break;
            }
        }
        if (separated)
            selected.append(peak.index);
        if (selected.size() == 8)
            break;
    }
    return selected.size();
}

void rdpMark(const QVector<QPointF> &points, int first, int last, qreal tolerance,
             QVector<bool> &keep)
{
    if (last <= first + 1)
        return;
    qreal maximumDistance = 0.0;
    int farthest = -1;
    for (int i = first + 1; i < last; ++i) {
        const qreal deviation = pointToSegmentDistance(points[i], points[first], points[last]);
        if (deviation > maximumDistance) {
            maximumDistance = deviation;
            farthest = i;
        }
    }
    if (farthest >= 0 && maximumDistance > tolerance) {
        keep[farthest] = true;
        rdpMark(points, first, farthest, tolerance, keep);
        rdpMark(points, farthest, last, tolerance, keep);
    }
}

qreal turnAngle(const QPointF &a, const QPointF &corner, const QPointF &b)
{
    const QPointF incoming = corner - a;
    const QPointF outgoing = b - corner;
    const qreal product = distance(a, corner) * distance(corner, b);
    if (product < 0.001)
        return 0.0;
    return qAcos(qBound(-1.0, QPointF::dotProduct(incoming, outgoing) / product, 1.0));
}

qreal polygonRegularityError(const QVector<QPointF> &vertices)
{
    if (vertices.size() < 3)
        return std::numeric_limits<qreal>::max();
    qreal meanEdge = 0.0;
    for (int i = 0; i < vertices.size(); ++i)
        meanEdge += distance(vertices[i], vertices[(i + 1) % vertices.size()]);
    meanEdge /= vertices.size();
    if (meanEdge < 0.001)
        return std::numeric_limits<qreal>::max();

    qreal edgeVariance = 0.0;
    qreal turnError = 0.0;
    const qreal expectedTurn = 2.0 * Pi / vertices.size();
    for (int i = 0; i < vertices.size(); ++i) {
        const qreal edge = distance(vertices[i], vertices[(i + 1) % vertices.size()]);
        edgeVariance += (edge - meanEdge) * (edge - meanEdge);
        turnError += qAbs(turnAngle(vertices[(i - 1 + vertices.size()) % vertices.size()],
                                    vertices[i],
                                    vertices[(i + 1) % vertices.size()]) - expectedTurn);
    }
    const qreal edgeVariation = qSqrt(edgeVariance / vertices.size()) / meanEdge;
    return edgeVariation + (turnError / vertices.size()) / qMax(0.1, expectedTurn) * 0.55;
}

qreal parallelAngleDifference(qreal first, qreal second)
{
    qreal difference = qAbs(qAtan2(qSin(first - second), qCos(first - second)));
    if (difference > Pi / 2.0)
        difference = Pi - difference;
    return qAbs(difference);
}

QVector<QPointF> simplifyPolyline(const QVector<QPointF> &points, qreal diagonal)
{
    if (points.size() < 3)
        return points;
    QVector<bool> keep(points.size(), false);
    keep.first() = true;
    keep.last() = true;
    rdpMark(points, 0, points.size() - 1, diagonal * 0.028, keep);

    QVector<QPointF> vertices;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (keep[i] && (vertices.isEmpty() || distance(vertices.last(), points[i]) > diagonal * 0.025))
            vertices.append(points[i]);
    }
    if (vertices.size() < 2)
        return {points.first(), points.last()};

    // Remove tiny hooks, then merge almost-collinear adjacent sections.
    bool changed = true;
    while (changed && vertices.size() > 2) {
        changed = false;
        for (int i = 1; i < vertices.size() - 1; ++i) {
            const qreal before = distance(vertices[i - 1], vertices[i]);
            const qreal after = distance(vertices[i], vertices[i + 1]);
            const qreal angle = turnAngle(vertices[i - 1], vertices[i], vertices[i + 1]);
            if (qMin(before, after) < diagonal * 0.045 || angle < qDegreesToRadians(12.0)) {
                vertices.removeAt(i);
                changed = true;
                break;
            }
        }
    }
    return vertices;
}

QVector<QPointF> collapseRetracedPolyline(const QVector<QPointF> &vertices, qreal diagonal)
{
    if (vertices.size() < 3)
        return vertices;
    const qreal clusterRadius = diagonal * 0.075;
    QVector<QPointF> nodes;
    QVector<int> counts;
    QVector<int> sequence;
    for (const QPointF &vertex : vertices) {
        int bestNode = -1;
        qreal bestDistance = clusterRadius;
        for (int i = 0; i < nodes.size(); ++i) {
            const qreal d = distance(vertex, nodes[i]);
            if (d < bestDistance) {
                bestDistance = d;
                bestNode = i;
            }
        }
        if (bestNode < 0) {
            bestNode = nodes.size();
            nodes.append(vertex);
            counts.append(1);
        } else {
            nodes[bestNode] = (nodes[bestNode] * counts[bestNode] + vertex)
                / qreal(counts[bestNode] + 1);
            ++counts[bestNode];
        }
        if (sequence.isEmpty() || sequence.last() != bestNode)
            sequence.append(bestNode);
    }

    QVector<QPair<int, int>> edges;
    QVector<QVector<int>> neighbors(nodes.size());
    for (int i = 1; i < sequence.size(); ++i) {
        int a = sequence[i - 1];
        int b = sequence[i];
        if (a == b)
            continue;
        if (a > b)
            std::swap(a, b);
        bool exists = false;
        for (const auto &edge : edges) {
            if (edge.first == a && edge.second == b) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            edges.append({a, b});
            neighbors[a].append(b);
            neighbors[b].append(a);
        }
    }

    int start = -1;
    int endpoints = 0;
    bool simpleChain = nodes.size() >= 2;
    for (int i = 0; i < neighbors.size(); ++i) {
        if (neighbors[i].size() == 1) {
            ++endpoints;
            start = i;
        } else if (neighbors[i].size() != 2) {
            simpleChain = false;
        }
    }
    if (!simpleChain || endpoints != 2 || edges.size() != nodes.size() - 1)
        return vertices;

    QVector<QPointF> skeleton;
    int previous = -1;
    int current = start;
    while (current >= 0) {
        skeleton.append(nodes[current]);
        int next = -1;
        for (int neighbor : neighbors[current]) {
            if (neighbor != previous) {
                next = neighbor;
                break;
            }
        }
        previous = current;
        current = next;
    }
    return skeleton;
}

qreal maximumTurnAngle(const QVector<QPointF> &vertices)
{
    qreal maximum = 0.0;
    for (int i = 1; i < vertices.size() - 1; ++i)
        maximum = qMax(maximum, turnAngle(vertices[i - 1], vertices[i], vertices[i + 1]));
    return maximum;
}

struct TwoSegmentFit
{
    bool valid = false;
    QVector<QPointF> vertices;
    qreal error = std::numeric_limits<qreal>::max();
    qreal turn = 0.0;
};

TwoSegmentFit fitTwoSegmentCorner(const QVector<QPointF> &stroke, qreal diagonal)
{
    TwoSegmentFit best;
    if (stroke.size() < 24 || diagonal < 1.0)
        return best;

    // Keep the breakpoint away from the endpoints so a tiny hook cannot be
    // promoted into an intentional corner.
    const int margin = qMax(6, stroke.size() / 8);
    for (int split = margin; split < stroke.size() - margin; ++split) {
        const QPointF corner = stroke[split];
        const qreal firstLength = distance(stroke.first(), corner);
        const qreal secondLength = distance(corner, stroke.last());
        if (qMin(firstLength, secondLength) < diagonal * 0.18)
            continue;

        qreal error = 0.0;
        for (int i = 0; i <= split; ++i)
            error += pointToSegmentDistance(stroke[i], stroke.first(), corner);
        for (int i = split; i < stroke.size(); ++i)
            error += pointToSegmentDistance(stroke[i], corner, stroke.last());
        error /= stroke.size() * diagonal;

        if (error < best.error) {
            best.valid = true;
            best.vertices = {stroke.first(), corner, stroke.last()};
            best.error = error;
            best.turn = turnAngle(stroke.first(), corner, stroke.last());
        }
    }
    return best;
}

bool hasSmoothCurveFlow(const QVector<QPointF> &stroke)
{
    if (stroke.size() < 18)
        return false;
    constexpr int step = 3;
    qreal signedTurn = 0.0;
    qreal absoluteTurn = 0.0;
    qreal maximumTurn = 0.0;
    for (int i = step; i < stroke.size() - step; ++i) {
        const QPointF incoming = stroke[i] - stroke[i - step];
        const QPointF outgoing = stroke[i + step] - stroke[i];
        const qreal cross = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
        const qreal dot = QPointF::dotProduct(incoming, outgoing);
        const qreal turn = qAtan2(cross, dot);
        signedTurn += turn;
        absoluteTurn += qAbs(turn);
        maximumTurn = qMax(maximumTurn, qAbs(turn));
    }
    if (absoluteTurn < qDegreesToRadians(35.0))
        return false;
    const qreal directionalConsistency = qAbs(signedTurn) / absoluteTurn;
    const qreal concentration = maximumTurn / absoluteTurn;
    return directionalConsistency > 0.72 && concentration < 0.38
        && maximumTurn < qDegreesToRadians(58.0);
}

struct OpenEllipseEvidence
{
    bool valid = false;
    qreal sweepTurns = 0.0;
    qreal radialError = 1.0;
    qreal directionConsistency = 0.0;
    qreal ellipseResidual = 1.0;
    qreal boxyResidual = 1.0;
    qreal startPhase = 0.0;
    qreal signedSweep = 0.0;
};

OpenEllipseEvidence analyzeOpenEllipse(const QVector<QPointF> &local,
                                       const QRectF &bounds)
{
    OpenEllipseEvidence evidence;
    const qreal radiusX = bounds.width() / 2.0;
    const qreal radiusY = bounds.height() / 2.0;
    if (local.size() < 18 || radiusX < 8.0 || radiusY < 8.0)
        return evidence;

    qreal previousPhase = 0.0;
    qreal signedSweep = 0.0;
    qreal absoluteSweep = 0.0;
    qreal radialError = 0.0;
    qreal ellipseResidual = 0.0;
    qreal boxyResidual = 0.0;
    for (qsizetype i = 0; i < local.size(); ++i) {
        const qreal x = (local[i].x() - bounds.center().x()) / radiusX;
        const qreal y = (local[i].y() - bounds.center().y()) / radiusY;
        const qreal phase = qAtan2(y, x);
        if (i == 0)
            evidence.startPhase = phase;
        radialError += qAbs(qSqrt(x * x + y * y) - 1.0);
        ellipseResidual += qAbs(x * x + y * y - 1.0);
        boxyResidual += qAbs(qPow(qAbs(x), 4.0) + qPow(qAbs(y), 4.0) - 1.0);
        if (i > 0) {
            const qreal delta = qAtan2(qSin(phase - previousPhase),
                                       qCos(phase - previousPhase));
            signedSweep += delta;
            absoluteSweep += qAbs(delta);
        }
        previousPhase = phase;
    }

    evidence.sweepTurns = qAbs(signedSweep) / (2.0 * Pi);
    evidence.signedSweep = signedSweep;
    evidence.radialError = radialError / local.size();
    evidence.directionConsistency = absoluteSweep > 0.001
        ? qAbs(signedSweep) / absoluteSweep : 0.0;
    evidence.ellipseResidual = ellipseResidual / local.size();
    evidence.boxyResidual = boxyResidual / local.size();
    const bool ellipseLike = evidence.ellipseResidual + 0.02 < evidence.boxyResidual;

    // A gapped ellipse still advances almost monotonically around normalized
    // ellipse space. This remains true for very tall or flat ovals, where a
    // circular fit underestimates coverage and mistakes the tight ends for
    // polyline corners.
    evidence.valid = evidence.sweepTurns > 0.78
        && evidence.sweepTurns < 1.12
        && evidence.directionConsistency > 0.94
        && evidence.radialError < 0.135
        && ellipseLike;
    return evidence;
}

QVector<QPointF> simplifyClosedPolygon(const QVector<QPointF> &stroke, qreal diagonal)
{
    if (stroke.size() < 8)
        return {};
    int strongestCorner = 0;
    qreal strongestTurn = 0.0;
    constexpr int step = 3;
    const int n = stroke.size();
    for (int i = 0; i < n; ++i) {
        const qreal angle = turnAngle(stroke[(i - step + n) % n], stroke[i],
                                      stroke[(i + step) % n]);
        if (angle > strongestTurn) {
            strongestTurn = angle;
            strongestCorner = i;
        }
    }
    QVector<QPointF> ordered;
    ordered.reserve(n + 1);
    for (int offset = 0; offset < n; ++offset)
        ordered.append(stroke[(strongestCorner + offset) % n]);
    ordered.append(ordered.first());
    QVector<QPointF> vertices = simplifyPolyline(ordered, diagonal);
    if (vertices.size() >= 2 && distance(vertices.first(), vertices.last()) < diagonal * 0.08)
        vertices.removeLast();
    return vertices;
}

QVector<QPointF> alignClosed(const QVector<QPointF> &candidate, const QVector<QPointF> &rough,
                             qreal scale, qreal *bestError)
{
    QVector<QPointF> best;
    *bestError = std::numeric_limits<qreal>::max();
    if (candidate.isEmpty() || rough.isEmpty() || scale <= 0.0)
        return best;
    if (candidate.size() != rough.size())
        return candidate;
    const int n = candidate.size();
    for (int reverse = 0; reverse < 2; ++reverse) {
        for (int shift = 0; shift < n; ++shift) {
            qreal error = 0.0;
            for (int i = 0; i < n; ++i) {
                const int index = reverse ? (shift - i + n * 2) % n : (shift + i) % n;
                error += distance(rough[i], candidate[index]) / scale;
            }
            error /= n;
            if (error < *bestError) {
                *bestError = error;
                best.clear();
                for (int i = 0; i < n; ++i) {
                    const int index = reverse ? (shift - i + n * 2) % n : (shift + i) % n;
                    best.append(candidate[index]);
                }
            }
        }
    }
    return best;
}
} // namespace

QuickShapeResult QuickShapeRecognizer::recognize(const QVector<QPointF> &points)
{
    QuickShapeResult out;
    if (points.size() < 4)
        return out;

    QVector<QPointF> rough = resample(points, SampleCount);
    const QRectF bounds = pointBounds(rough);
    const qreal diagonal = qSqrt(bounds.width() * bounds.width() + bounds.height() * bounds.height());
    if (diagonal < 24.0)
        return out;

    QPointF center;
    for (const QPointF &p : rough)
        center += p;
    center /= rough.size();

    qreal xx = 0.0, yy = 0.0, xy = 0.0;
    for (const QPointF &p : rough) {
        const QPointF d = p - center;
        xx += d.x() * d.x(); yy += d.y() * d.y(); xy += d.x() * d.y();
    }
    const qreal angle = 0.5 * qAtan2(2.0 * xy, xx - yy);

    QVector<QPointF> local;
    for (const QPointF &p : rough) {
        const QPointF q = rotatePoint(p - center, -angle);
        local.append(q);
    }
    const QRectF localBounds = pointBounds(local);
    const qreal closure = distance(rough.first(), rough.last()) / diagonal;
    const CircleFit circleFit = fitCircle(rough);
    const qreal turns = circleFit.valid ? qAbs(circleFit.sweep) / (2.0 * Pi) : 0.0;
    const bool nearCompleteLoop = turns > 0.58 && closure < 0.62;
    const bool smoothCurveFlow = hasSmoothCurveFlow(rough);
    const qreal ovalAspect = qMax(localBounds.width(), localBounds.height())
        / qMax(1.0, qMin(localBounds.width(), localBounds.height()));
    const OpenEllipseEvidence openEllipse = analyzeOpenEllipse(local, localBounds);
    const TwoSegmentFit twoSegmentFit = fitTwoSegmentCorner(rough, diagonal);
    const bool strongTwoSegmentCorner = twoSegmentFit.valid
        && twoSegmentFit.error < 0.020
        && twoSegmentFit.turn > qDegreesToRadians(28.0);
    QVector<QPointF> preliminaryAngularVertices = simplifyPolyline(rough, diagonal);
    preliminaryAngularVertices = collapseRetracedPolyline(preliminaryAngularVertices, diagonal);
    const qreal preliminaryAngularity = maximumTurnAngle(preliminaryAngularVertices);
    const int openCornerEvidence = strongCornerCount(rough, false);
    const qreal preliminaryAngularError = preliminaryAngularVertices.size() >= 3
        ? geometricFitError(rough, resample(preliminaryAngularVertices, SampleCount), false)
              / diagonal
        : std::numeric_limits<qreal>::max();
    const bool angularWithoutEllipseFit = !openEllipse.valid
        && openCornerEvidence >= 1;
    const bool angularDespiteEllipseFit = openEllipse.valid
        && openCornerEvidence >= 3
        && preliminaryAngularError < 0.012
        && openEllipse.radialError > 0.085;
    const bool strongOpenAngularEvidence = closure > 0.12
        && preliminaryAngularVertices.size() >= 3
        && preliminaryAngularVertices.size() <= 12
        && (angularWithoutEllipseFit || angularDespiteEllipseFit)
        && preliminaryAngularity > qDegreesToRadians(32.0)
        && preliminaryAngularError < 0.060;
    const bool openEllipseCandidate = closure > 0.12 && closure < 0.78
        && openEllipse.valid && !strongTwoSegmentCorner && !strongOpenAngularEvidence;
    const bool multiWindingEllipseCandidate = ovalAspect > 1.45
        && openEllipse.sweepTurns >= 1.12
        && openEllipse.sweepTurns < 2.25
        && openEllipse.directionConsistency > 0.94
        && openEllipse.radialError < 0.18
        && openEllipse.ellipseResidual + 0.02 < openEllipse.boxyResidual
        && !strongTwoSegmentCorner;

    // A flat ellipse may be traced more than once. Circular sweep fitting is
    // unreliable at extreme aspect ratios, so use phase in normalized ellipse
    // space and collapse every winding into one mathematical oval.
    if (multiWindingEllipseCandidate) {
        const QPointF ellipseCenter = center + rotatePoint(localBounds.center(), angle);
        const bool nearlyClosed = closure <= 0.22;
        QVector<QPointF> target;
        if (nearlyClosed) {
            target = uniformEllipse(ellipseCenter,
                                    localBounds.width() / 2.0,
                                    localBounds.height() / 2.0,
                                    angle, SampleCount);
            out.name = "Ellipse";
        } else {
            const qreal openSweep = majorOpenSweep(openEllipse.signedSweep);
            target = ellipticalArc(ellipseCenter,
                                   localBounds.width() / 2.0,
                                   localBounds.height() / 2.0,
                                   angle, openEllipse.startPhase,
                                   openSweep, SampleCount);
            out.name = "Elliptical Arc";
        }
        qreal alignmentError = 0.0;
        out.accepted = true;
        out.confidence = qBound(0.55, 0.90 - openEllipse.radialError, 0.93);
        out.rough = rough;
        out.target = nearlyClosed
            ? alignClosed(target, rough, diagonal, &alignmentError) : target;
        out.center = ellipseCenter;
        return out;
    }

    // Repeated round gestures are strong intentional evidence. Collapse all
    // windings directly to one clean circle/ellipse instead of penalizing the
    // final endpoint for stopping part-way through another revolution.
    if (circleFit.valid && turns > 1.30 && !strongTwoSegmentCorner) {
        const qreal aspect = qMax(localBounds.width(), localBounds.height())
            / qMax(1.0, qMin(localBounds.width(), localBounds.height()));
        QVector<QPointF> target;
        const bool nearlyClosed = closure <= 0.22;
        if (!nearlyClosed && aspect < 1.45) {
            CircleFit openFit = circleFit;
            openFit.sweep = majorOpenSweep(circleFit.sweep);
            out.name = "Arc";
            target = circularArc(openFit, rough.first(), SampleCount);
        } else if (!nearlyClosed) {
            out.name = "Elliptical Arc";
            target = ellipticalArc(center + rotatePoint(localBounds.center(), angle),
                                   localBounds.width() / 2.0,
                                   localBounds.height() / 2.0,
                                   angle, openEllipse.startPhase,
                                   majorOpenSweep(openEllipse.signedSweep), SampleCount);
        } else if (aspect < 1.32) {
            out.name = "Circle";
            target = uniformEllipse(circleFit.center, circleFit.radius, circleFit.radius,
                                    0.0, SampleCount);
        } else {
            out.name = "Ellipse";
            const QPointF ellipseCenter = center + rotatePoint(localBounds.center(), angle);
            target = uniformEllipse(ellipseCenter, localBounds.width() / 2.0,
                                    localBounds.height() / 2.0, angle, SampleCount);
        }
        qreal alignmentError = 0.0;
        out.accepted = true;
        out.confidence = 0.94;
        out.rough = rough;
        out.target = nearlyClosed
            ? alignClosed(target, rough, diagonal, &alignmentError) : target;
        out.center = circleFit.center;
        return out;
    }

    // Preserve a visible endpoint gap. Near-round strokes remain circular arcs;
    // stretched strokes use a true open elliptical arc. Only connected or
    // almost-connected endpoints become a closed Circle/Ellipse.
    if (openEllipseCandidate) {
        const QPointF ellipseCenter = center + rotatePoint(localBounds.center(), angle);
        QVector<QPointF> target;
        const bool nearlyClosed = closure <= 0.22;
        if (nearlyClosed) {
            target = uniformEllipse(ellipseCenter,
                                    localBounds.width() / 2.0,
                                    localBounds.height() / 2.0,
                                    angle, SampleCount);
            out.name = ovalAspect < 1.25 ? "Circle" : "Ellipse";
        } else if (ovalAspect < 1.45 && circleFit.valid) {
            target = circularArc(circleFit, rough.first(), SampleCount);
            out.name = "Arc";
        } else {
            target = ellipticalArc(ellipseCenter,
                                   localBounds.width() / 2.0,
                                   localBounds.height() / 2.0,
                                   angle, openEllipse.startPhase,
                                   openEllipse.signedSweep, SampleCount);
            out.name = "Elliptical Arc";
        }
        qreal alignmentError = 0.0;
        out.accepted = true;
        out.confidence = qBound(0.48,
            0.82 + (openEllipse.sweepTurns - 0.78) * 0.30
                - openEllipse.radialError * 1.20,
            0.94);
        out.rough = rough;
        out.target = nearlyClosed
            ? alignClosed(target, rough, diagonal, &alignmentError) : target;
        out.center = ellipseCenter;
        return out;
    }

    // Use the PCA axis for the open-line fit. Unlike endpoint fitting, this cleans
    // small hooks and overshoots at either end of a sketched line.
    const QPointF lineStart = center + rotatePoint(QPointF(localBounds.left(), 0.0), angle);
    const QPointF lineEnd = center + rotatePoint(QPointF(localBounds.right(), 0.0), angle);
    QVector<QPointF> lineTarget = resample({lineStart, lineEnd}, SampleCount);
    if (distance(rough.first(), lineEnd) < distance(rough.first(), lineStart))
        std::reverse(lineTarget.begin(), lineTarget.end());
    const qreal thinness = qMin(localBounds.width(), localBounds.height())
        / qMax(1.0, qMax(localBounds.width(), localBounds.height()));
    qreal bestError = geometricFitError(rough, lineTarget, false) / diagonal;
    // Retraced or scribbled straight strokes often finish near their starting
    // point. A narrow PCA strip is stronger evidence than endpoint closure.
    if (thinness >= 0.115)
        bestError += qMax(0.0, 0.30 - closure) * 0.85;
    if (smoothCurveFlow)
        bestError += 0.18;
    QString bestName = "Line";
    QVector<QPointF> bestTarget = lineTarget;

    QVector<QPointF> angularVertices = preliminaryAngularVertices;
    if (strongTwoSegmentCorner)
        angularVertices = twoSegmentFit.vertices;
    const qreal angularSpan = angularVertices.size() >= 2
        ? distance(angularVertices.first(), angularVertices.last()) / diagonal : 0.0;
    const qreal angularity = maximumTurnAngle(angularVertices);
    if (angularVertices.size() >= 3 && angularVertices.size() <= 12
        && angularSpan > 0.16 && angularity > qDegreesToRadians(28.0)
        && (!nearCompleteLoop || strongTwoSegmentCorner || strongOpenAngularEvidence)
        && (!smoothCurveFlow || strongTwoSegmentCorner || strongOpenAngularEvidence)) {
        const QVector<QPointF> angularTarget = resample(angularVertices, SampleCount);
        qreal angularError = geometricFitError(rough, angularTarget, false) / diagonal;
        angularError += (angularVertices.size() - 2) * 0.0035;
        // Strong corners receive a small evidence bonus. This prevents a clean V
        // from being rounded into a circular arc with a superficially similar fit.
        angularError -= qMin(0.025, (angularity - qDegreesToRadians(28.0)) * 0.018);
        if (angularError < bestError) {
            bestError = angularError;
            bestName = angularVertices.size() == 3 ? "Angled line" : "Zigzag";
            bestTarget = angularTarget;
        }
    }

    if (circleFit.valid && qAbs(circleFit.sweep) > 0.42
        && qAbs(circleFit.sweep) < 1.98 * Pi && closure > 0.12) {
        const QVector<QPointF> arcTarget = circularArc(circleFit, rough.first(), SampleCount);
        qreal arcError = geometricFitError(rough, arcTarget, false) / diagonal;
        if (strongTwoSegmentCorner)
            arcError += 0.18;
        if (strongOpenAngularEvidence)
            arcError += 0.22;
        if (angularity > qDegreesToRadians(32.0))
            arcError += qMin(0.14, (angularity - qDegreesToRadians(32.0)) * 0.11);
        const qreal radiusRatio = circleFit.radius / diagonal;
        if (radiusRatio > 8.0)
            arcError += (radiusRatio - 8.0) * 0.01;
        if (arcError < bestError) {
            bestError = arcError;
            bestName = "Arc";
            bestTarget = arcTarget;
        }
    }

    const int cornerCount = closure < 0.48
        ? strongCornerCount(rough, closure < 0.08) : 0;
    auto considerClosed = [&](const QString &name, const QVector<QPointF> &candidate,
                              qreal shapePenalty) {
        if (strongTwoSegmentCorner)
            return;
        qreal error = geometricFitError(rough, candidate, true) / diagonal;
        const qreal endpointPenalty = turns > 1.30 ? 0.0 : qMax(0.0, closure - 0.32) * 0.62;
        error += endpointPenalty + shapePenalty;
        if (smoothCurveFlow && closure >= 0.12 && turns < 1.30)
            error += 0.20;
        if (error < bestError) {
            qreal alignmentError = 0.0;
            bestTarget = alignClosed(candidate, rough, diagonal, &alignmentError);
            bestError = error;
            bestName = name;
        }
    };

    QVector<QPointF> structuralVertices;
    if (closure < 0.16 && cornerCount >= 2)
        structuralVertices = simplifyClosedPolygon(rough, diagonal);

    // A detected three-corner contour becomes a clean scalene triangle. This
    // preserves the artist's intended corner positions instead of forcing an
    // isosceles template around the bounding box.
    if (structuralVertices.size() == 3) {
        considerClosed("Triangle", sampleClosedPolygon(structuralVertices, SampleCount), -0.014);
    }

    // For four genuine near-right corners, align an exact rectangle to the
    // artist's strongest edge. Opposite sides are guaranteed parallel and all
    // corners are mathematically 90 degrees.
    if (structuralVertices.size() == 4) {
        qreal rightAngleError = 0.0;
        QVector<qreal> edgeAngles;
        for (int i = 0; i < 4; ++i) {
            rightAngleError += qAbs(turnAngle(structuralVertices[(i + 3) % 4],
                                               structuralVertices[i],
                                               structuralVertices[(i + 1) % 4]) - Pi / 2.0);
            const QPointF edge = structuralVertices[(i + 1) % 4] - structuralVertices[i];
            edgeAngles.append(qAtan2(edge.y(), edge.x()));
        }
        rightAngleError /= 4.0;
        const qreal parallelError = (parallelAngleDifference(edgeAngles[0], edgeAngles[2])
            + parallelAngleDifference(edgeAngles[1], edgeAngles[3])) / 2.0;
        if (rightAngleError < qDegreesToRadians(6.0)
            && parallelError < qDegreesToRadians(5.0)) {
            for (qreal edgeAngle : edgeAngles) {
                QVector<QPointF> oriented;
                for (const QPointF &point : rough)
                    oriented.append(rotatePoint(point - center, -edgeAngle));
                const QRectF box = pointBounds(oriented);
                const QPointF boxCenter = box.center();
                auto world = [&](const QPointF &localPoint) {
                    return center + rotatePoint(localPoint, edgeAngle);
                };
                const QVector<QPointF> rectangle = {
                    world(QPointF(box.left(), box.top())),
                    world(QPointF(box.right(), box.top())),
                    world(QPointF(box.right(), box.bottom())),
                    world(QPointF(box.left(), box.bottom()))
                };
                considerClosed("Rectangle", sampleClosedPolygon(rectangle, SampleCount), -0.016);
            }
        }
    }

    // Preserve intentional non-regular closed geometry as a cleaned mathematical
    // polygon instead of forcing every four-sided shape into a rectangle.
    if ((!smoothCurveFlow || cornerCount >= 3)
        && structuralVertices.size() >= 4 && structuralVertices.size() <= 10) {
            const QString polygonName = structuralVertices.size() == 4
                ? "Quadrilateral" : "Polygon";
            const qreal polygonPenalty = 0.001 + (structuralVertices.size() - 4) * 0.001;
            considerClosed(polygonName,
                           sampleClosedPolygon(structuralVertices, SampleCount),
                           polygonPenalty);
    }

    const qreal multiLoopPolygonPenalty = turns > 1.30 ? 0.14 : 0.0;
    if (circleFit.valid && (closure < 0.42 || turns > 0.72)) {
        const qreal aspect = qMax(localBounds.width(), localBounds.height())
            / qMax(1.0, qMin(localBounds.width(), localBounds.height()));
        const qreal cornerPenalty = turns <= 1.30 && cornerCount >= 4 ? 0.040 : 0.0;
        const qreal roundShapeBonus = nearCompleteLoop && aspect < 1.14 ? -0.026 : 0.0;
        const qreal circlePenalty = qMax(0.0, aspect - 1.16) * 0.055 + cornerPenalty
            + roundShapeBonus - (turns > 1.30 ? 0.018 : 0.0);
        considerClosed("Circle", uniformEllipse(circleFit.center, circleFit.radius,
                                                 circleFit.radius, 0.0, SampleCount),
                       circlePenalty);
    }

    qreal polygonRadius = 0.0;
    if (circleFit.valid) {
        for (const QPointF &point : rough)
            polygonRadius = qMax(polygonRadius, distance(point, circleFit.center));
    }
    const qreal structuralRegularity = structuralVertices.size() >= 3
        ? polygonRegularityError(structuralVertices)
        : std::numeric_limits<qreal>::max();

    // Search rotation instead of trusting one PCA angle. This is important for
    // triangles (whose covariance axis is often ambiguous) and hand-drawn shapes.
    for (int degrees = 0; degrees < 360; degrees += 5) {
        const qreal candidateAngle = qDegreesToRadians(qreal(degrees));
        QVector<QPointF> oriented;
        oriented.reserve(rough.size());
        for (const QPointF &point : rough)
            oriented.append(rotatePoint(point - center, -candidateAngle));
        const QRectF box = pointBounds(oriented);
        const QPointF boxCenter = box.center();
        const qreal halfW = qMax(box.width() / 2.0, 8.0);
        const qreal halfH = qMax(box.height() / 2.0, 8.0);
        auto world = [&](const QPointF &localPoint) {
            return center + rotatePoint(localPoint, candidateAngle);
        };

        if (degrees < 180) {
            // Ellipse samples are resampled by arc length, preventing wide ovals
            // from being unfairly compared against uniformly sampled rectangles.
            const QPointF ellipseCenter = world(boxCenter);
            const qreal ellipsePenalty = turns > 1.30
                ? -0.014
                : (nearCompleteLoop && cornerCount <= 2
                    ? -0.020
                    : (cornerCount >= 3 ? 0.012 * (cornerCount - 2) : 0.0));
            considerClosed("Ellipse", uniformEllipse(ellipseCenter, halfW, halfH,
                                                       candidateAngle, SampleCount),
                           ellipsePenalty);

            if (structuralVertices.size() < 5) {
                const QVector<QPointF> rectangleVertices = {
                    world(boxCenter + QPointF(-halfW, -halfH)),
                    world(boxCenter + QPointF( halfW, -halfH)),
                    world(boxCenter + QPointF( halfW,  halfH)),
                    world(boxCenter + QPointF(-halfW,  halfH))
                };
                const qreal rectanglePenalty = (cornerCount < 3
                    ? 0.040 * (3 - cornerCount) : 0.0) + multiLoopPolygonPenalty;
                considerClosed("Rectangle", sampleClosedPolygon(rectangleVertices, SampleCount),
                               rectanglePenalty);
            }
        }

        if (structuralVertices.size() < 4) {
            const QVector<QPointF> triangleVertices = {
                world(boxCenter + QPointF(0.0, -halfH)),
                world(boxCenter + QPointF(halfW, halfH)),
                world(boxCenter + QPointF(-halfW, halfH))
            };
            const qreal trianglePenalty = (cornerCount < 2 ? 0.038 * (2 - cornerCount) : 0.004)
                + multiLoopPolygonPenalty;
            considerClosed("Triangle", sampleClosedPolygon(triangleVertices, SampleCount),
                           trianglePenalty);
        }

        if (circleFit.valid && degrees < 180) {
            const bool hasStructuralSideCount = structuralVertices.size() >= 3;
            const bool allowPentagon = hasStructuralSideCount
                ? structuralVertices.size() == 5 && structuralRegularity < 0.60
                : qAbs(cornerCount - 5) <= 1;
            const bool allowHexagon = hasStructuralSideCount
                ? structuralVertices.size() == 6 && structuralRegularity < 0.60
                : qAbs(cornerCount - 6) <= 1;
            if (allowPentagon) {
                const qreal pentagonPenalty = hasStructuralSideCount
                    ? -0.012 + structuralRegularity * 0.040 + multiLoopPolygonPenalty
                    : -0.010 + qAbs(cornerCount - 5) * 0.006 + multiLoopPolygonPenalty;
                considerClosed("Pentagon", regularPolygon(circleFit.center, polygonRadius, 5,
                                                            candidateAngle, SampleCount),
                               pentagonPenalty);
            }
            if (allowHexagon) {
                const qreal hexagonPenalty = hasStructuralSideCount
                    ? -0.011 + structuralRegularity * 0.038 + multiLoopPolygonPenalty
                    : -0.009 + qAbs(cornerCount - 6) * 0.006 + multiLoopPolygonPenalty;
                considerClosed("Hexagon", regularPolygon(circleFit.center, polygonRadius, 6,
                                                           candidateAngle, SampleCount),
                               hexagonPenalty);
            }
        }
    }

    const qreal confidence = qBound(0.0, 1.0 - bestError / 0.22, 1.0);
    if (confidence < 0.34)
        return out;

    out.accepted = true;
    out.name = bestName;
    out.confidence = confidence;
    out.rough = rough;
    out.target = bestTarget;
    out.center = center;
    return out;
}


} // namespace quickshape
