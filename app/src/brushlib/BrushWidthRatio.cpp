#include "BrushWidthRatio.h"

#include "BrushPresetCodec.h"
#include "StrokeBuilder.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace brushlib {
namespace {

QMutex g_mutex;
QHash<QByteArray, WidthRatio> g_memory;
std::atomic<int> g_measureCount{0};
std::atomic<QThread *> g_lastMeasureThread{nullptr};

::Brush normalised(const ::Brush &brush)
{
    ::Brush b = brush;
    b.setSize(BrushWidthRatio::kReferenceSize);
    b.setColor(QColor(Qt::black));
    b.setEraseMode(false);
    return b;
}

// Does the width depend on the stroke's heading? A heading-driven angle
// turns the tip with the path, and a round procedural tip has no angle to
// speak of: one stroke measures them. Everything else gets eight headings
// and the BROAD one wins.
bool headingIndependent(const ::Brush &b)
{
    if (b.controlSource(::Brush::DynamicProperty::AngleJitter)
        == ::Brush::ControlSource::Direction)
        return true;
    return !b.hasCustomShape() && b.tipRoundness() >= 0.999;
}

// Width across the path at 10% of the stroke's own peak, one heading, at
// the given engine size: the COUNT of profile samples at or above the
// threshold - the approved definition, and exactly what the permanent
// test measures independently at the actual size. (A fractional-boundary
// variant was tried and rejected: it reads a fringe pixel as 0.3 where
// the eye, and the definition, count it whole - every label came out a
// pixel or two low.)
double widthAt(const ::Brush &brush, int size, double headingDegrees)
{
    const int span = int(std::ceil(size * 1.6)); // half-span sampled
    // 280 px of stroke, the central 120 sampled: the ends (where the mark
    // is still building) stay outside the sample at every spacing, and the
    // stroke is no longer than that needs - the cost is dabs x dab area,
    // and this runs on the UI thread when the worker has not got there
    // first.
    const int halfLength = 140;
    const int side = 2 * (halfLength + span + 16);
    const QPointF centre(side / 2.0, side / 2.0);
    const double theta = qDegreesToRadians(headingDegrees);
    const QPointF along(std::cos(theta), std::sin(theta));
    const QPointF across(-std::sin(theta), std::cos(theta));

    ::Brush b = brush;
    b.setSize(size);
    StrokeBuilder builder(QSize(side, side), b, true);
    for (int i = 0; i <= 56; ++i) {
        StrokePoint p;
        p.position = centre + along * (-double(halfLength) + i * 5.0);
        p.pressure = 1.0;
        p.timestamp = quint64(i * 5);
        builder.addRawPoint(p);
    }
    const QImage mask = builder.strokeMask();
    if (mask.isNull())
        return 0.0;

    std::vector<double> profile(2 * span + 1, 0.0);
    int samples = 0;
    for (int s = -60; s <= 60; s += 2) {
        ++samples;
        for (int t = -span; t <= span; ++t) {
            const QPointF p = centre + along * double(s) + across * double(t);
            const int x = qRound(p.x());
            const int y = qRound(p.y());
            if (x >= 0 && y >= 0 && x < mask.width() && y < mask.height())
                profile[t + span] += mask.constScanLine(y)[x];
        }
    }
    double peak = 0.0;
    for (double &v : profile) {
        v /= samples;
        peak = std::max(peak, v);
    }
    if (peak <= 0.0)
        return 0.0;
    const double threshold = 0.1 * peak;
    int first = -1;
    int last = -1;
    for (int i = 0; i <= 2 * span; ++i) {
        if (profile[i] >= threshold) {
            if (first < 0)
                first = i;
            last = i;
        }
    }
    return first < 0 ? 0.0 : double(last - first + 1);
}

// The broad-side width at one size: one heading when the width cannot
// depend on it, eight otherwise.
double broadWidthAt(const ::Brush &b, int size)
{
    if (headingIndependent(b))
        return widthAt(b, size, 0.0);
    double broad = 0.0;
    for (int k = 0; k < 8; ++k)
        broad = std::max(broad, widthAt(b, size, k * 22.5));
    return broad;
}

QString diskFile(const QString &diskDir, const QByteArray &key)
{
    return diskDir + QStringLiteral("/widths-r%1/").arg(
               BrushWidthRatio::kRevision)
        + QString::fromLatin1(key.toHex()) + QStringLiteral(".txt");
}

// The two widths become the affine model display = ratio x engine +
// offset. Only the two MEASUREMENTS are ever stored; this classification
// is recomputed from the brush every time, so a rule change cannot be
// outvoted by an old disk entry.
WidthRatio classify(const ::Brush &brush, double widthLow, double widthHigh)
{
    WidthRatio r;
    r.widthLow = widthLow;
    r.widthHigh = widthHigh;
    r.measured = widthHigh / double(BrushWidthRatio::kReferenceSize);
    const bool scatter = brush.scatterPerpendicular() > 0.0
        || brush.scatterAlong() > 0.0;
    r.excluded = scatter && r.measured > BrushWidthRatio::kScatterExclusion;
    if (r.excluded) {
        r.ratio = 1.0;
        r.offset = 0.0;
        return r;
    }
    const double dx = double(BrushWidthRatio::kReferenceSize
                             - BrushWidthRatio::kReferenceSizeLow);
    const double slope = (widthHigh - widthLow) / dx;
    const double offset = widthLow - slope * BrushWidthRatio::kReferenceSizeLow;
    // A sparse or breaking-up stamp can give a fit that says nonsense
    // (a slope near zero, or a fringe wider than a few pixels): fall back
    // to the plain proportion through the high reference.
    if (slope >= 0.02 && slope <= 8.0 && std::abs(offset) <= 6.0) {
        r.ratio = slope;
        r.offset = offset;
    } else {
        r.ratio = r.measured;
        r.offset = 0.0;
    }
    return r;
}

// A measurement is plausible between a hairline and a wide scatter
// envelope; anything else in a disk entry is garbage and is re-measured.
bool plausible(double width, int size)
{
    return std::isfinite(width) && width >= 0.02 * size && width <= 8.0 * size;
}

} // namespace

QByteArray BrushWidthRatio::key(const ::Brush &brush)
{
    return BrushPresetCodec::settingsHash(normalised(brush));
}

WidthRatio BrushWidthRatio::measure(const ::Brush &brush)
{
    ++g_measureCount;
    g_lastMeasureThread = QThread::currentThread();
    const ::Brush b = normalised(brush);
    double low = broadWidthAt(b, kReferenceSizeLow);
    double high = broadWidthAt(b, kReferenceSize);
    // An empty or degenerate stroke: say what we know (ratio 1).
    if (!plausible(high, kReferenceSize))
        high = kReferenceSize;
    if (!plausible(low, kReferenceSizeLow))
        low = high * kReferenceSizeLow / double(kReferenceSize);
    return classify(b, low, high);
}

WidthRatio BrushWidthRatio::ratioFor(const ::Brush &brush,
                                     const QString &diskDir)
{
    const QByteArray k = key(brush);
    {
        QMutexLocker lock(&g_mutex);
        const auto it = g_memory.constFind(k);
        if (it != g_memory.constEnd())
            return it.value();
    }
    WidthRatio result;
    bool have = false;
    if (!diskDir.isEmpty()) {
        QFile f(diskFile(diskDir, k));
        if (f.open(QIODevice::ReadOnly)) {
            // "<lowSize> <highSize> <widthLow> <widthHigh>": the entry
            // says which sizes it measured, so a file written under other
            // reference sizes can never be read as this model's numbers
            // (the revision directory guards the same thing; the day the
            // sizes moved without a bump, the library rows showed the old
            // model's labels until this check existed).
            const QStringList parts = QString::fromLatin1(f.readAll())
                                          .trimmed()
                                          .split(QLatin1Char(' '),
                                                 Qt::SkipEmptyParts);
            bool okLow = false;
            bool okHigh = false;
            const bool sizesMatch = parts.size() == 4
                && parts.at(0).toInt() == kReferenceSizeLow
                && parts.at(1).toInt() == kReferenceSize;
            const double low =
                sizesMatch ? parts.at(2).toDouble(&okLow) : 0.0;
            const double high =
                sizesMatch ? parts.at(3).toDouble(&okHigh) : 0.0;
            if (sizesMatch && okLow && okHigh
                && plausible(low, kReferenceSizeLow)
                && plausible(high, kReferenceSize)) {
                // Only the two MEASUREMENTS are stored; the classification
                // is recomputed from the brush, so a rule change can never
                // be outvoted by an old file.
                result = classify(normalised(brush), low, high);
                have = true;
            }
        }
    }
    if (!have) {
        result = measure(brush);
        if (!diskDir.isEmpty()) {
            const QString file = diskFile(diskDir, k);
            QDir().mkpath(QFileInfo(file).absolutePath());
            QFile f(file);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QByteArray::number(kReferenceSizeLow) + ' '
                        + QByteArray::number(kReferenceSize) + ' '
                        + QByteArray::number(result.widthLow, 'g', 17) + ' '
                        + QByteArray::number(result.widthHigh, 'g', 17));
        }
    }
    QMutexLocker lock(&g_mutex);
    g_memory.insert(k, result);
    return result;
}

bool BrushWidthRatio::cached(const ::Brush &brush, WidthRatio *out)
{
    const QByteArray k = key(brush);
    QMutexLocker lock(&g_mutex);
    const auto it = g_memory.constFind(k);
    if (it == g_memory.constEnd())
        return false;
    if (out)
        *out = it.value();
    return true;
}

int BrushWidthRatio::displaySizeWith(const WidthRatio &r, int engineSize)
{
    return qBound(1, qRound(engineSize * r.ratio + r.offset), 5000);
}

int BrushWidthRatio::engineSizeWith(const WidthRatio &r, int displaySize,
                                    int presetDefaultEngine)
{
    if (presetDefaultEngine > 0
        && displaySizeWith(r, presetDefaultEngine) == displaySize)
        return presetDefaultEngine;
    return qBound(1, qRound((displaySize - r.offset) / qMax(r.ratio, 0.02)),
                  5000);
}

int BrushWidthRatio::displaySize(const ::Brush &brush, int engineSize)
{
    return displaySizeWith(ratioFor(brush), engineSize);
}

int BrushWidthRatio::engineSize(const ::Brush &brush, int displaySize,
                                int presetDefaultEngine)
{
    return engineSizeWith(ratioFor(brush), displaySize, presetDefaultEngine);
}

void BrushWidthRatio::clearMemoryCacheForTest()
{
    QMutexLocker lock(&g_mutex);
    g_memory.clear();
}

int BrushWidthRatio::measureCountForTest()
{
    return g_measureCount.load();
}

QThread *BrushWidthRatio::lastMeasureThreadForTest()
{
    return g_lastMeasureThread.load();
}

// ---------------------------------------------------------------------------
// BrushWidthMeasurer: the studio's off-thread measurement.

BrushWidthMeasurer::BrushWidthMeasurer(QObject *parent)
    : QObject(parent)
{
    m_thread = QThread::create([this] { loop(); });
    m_thread->setObjectName(QStringLiteral("BrushWidthMeasurer"));
    m_thread->start(QThread::LowPriority);
}

BrushWidthMeasurer::~BrushWidthMeasurer()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_queue.clear();
        m_wake.wakeAll();
    }
    m_thread->wait(); // one measurement in flight at most
    delete m_thread;
}

void BrushWidthMeasurer::request(const ::Brush &brush, const QString &diskDir)
{
    const QByteArray key = BrushWidthRatio::key(brush);
    QMutexLocker lock(&m_mutex);
    if (m_quit)
        return;
    // The newest request is the one that matters: an older brush state
    // still queued is dropped (its result would be stale by the time it
    // arrived), but a measurement already in flight completes and reports.
    m_queue.clear();
    m_queue.enqueue({key, brush, diskDir});
    m_wake.wakeOne();
}

void BrushWidthMeasurer::loop()
{
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_queue.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_quit)
                return;
            job = m_queue.dequeue();
        }
        const WidthRatio r = BrushWidthRatio::ratioFor(job.brush, job.diskDir);
        {
            QMutexLocker lock(&m_mutex);
            if (m_quit)
                return;
        }
        emit ready(job.key, r);
    }
}

} // namespace brushlib
