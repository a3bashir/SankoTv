#pragma once

#include "Brush.h"
#include "StrokeBuilder.h"

#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QVector>
#include <QWaitCondition>
#include <QWidget>

class QTimer;

namespace brushlib {

// The studio's live "Drawing Canvas" pad (Figma 274:112 / 274:121): the user
// draws here with the brush AS EDITED, before committing. This widget is NOT
// the document — strokes live only in this widget, render through the same
// SankoPaintHostAdapter/StrokeBuilder machinery as everything else, and
// never touch Layer::image, composite caches, the app QUndoStack, or the
// document dirty flag.
//
// Rendering model: a LOW-PRIORITY worker thread renders; the UI thread only
// captures points and blits results.
//   full  job: background + every committed stroke  -> becomes the BASELINE
//   live  job: baseline copy + the in-progress stroke -> display only
//   commit    : a live job flagged to become the new baseline on completion
// Jobs coalesce (a newer live/full replaces its pending kind; commits are
// never dropped) and stale results (brush revision / stroke count mismatch)
// are discarded and repaired with a fresh full render.
//
// Parameter edits re-render every stroke with the new brush. Size/hardness
// edits invalidate the engine's tip cache, so those are DEBOUNCED at 150ms
// trailing (other parameters 40ms): a continuous slider drag produces a
// bounded number of full renders, not one per pixel of slider movement.
//
// Smudge brushes over transparency are a no-op (the engine correctly
// reports an invalid commit), so in Smudge mode the background is the Phase
// 2 three-colour-band fixture — the stroke visibly DRAGS colour.
class ScratchCanvas : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kTipDebounceMs = 150; // size/hardness (tip cache)
    static constexpr int kEditDebounceMs = 40; // every other parameter

    explicit ScratchCanvas(QWidget *parent = nullptr);
    ~ScratchCanvas() override;

    // tipInvalidating: the edit changes the stamp shape (size/hardness) and
    // costs a tip-cache regeneration — debounced longer.
    void setBrush(const ::Brush &brush, bool tipInvalidating);
    void clearStrokes();

    int fullRendersPerformed() const { return m_fullRenders; } // seam (i)
    int strokeCount() const { return m_strokes.size(); }
    bool hasVisibleInk() const; // seam (j): any non-background pixel

signals:
    void renderCompleted(); // a worker result landed (seam settle hook)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void tabletEvent(QTabletEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    enum class JobKind { Full, Live, Commit };
    struct Job
    {
        JobKind kind = JobKind::Full;
        ::Brush brush;
        QImage base;    // Live/Commit: the baseline to stroke over
        QSize size;
        QVector<QVector<StrokePoint>> strokes; // Full: all committed strokes
        QVector<StrokePoint> live;
        int brushRevision = 0;
        int strokeCountAtEnqueue = 0;
        quint64 epoch = 0;
    };
    struct Result
    {
        JobKind kind;
        QImage image;
        int brushRevision;
        int strokeCountAtEnqueue;
        quint64 epoch;
    };

    void beginStroke(const QPointF &pos, qreal pressure, qreal tiltX,
                     qreal tiltY, qreal rotation);
    void moveStroke(const QPointF &pos, qreal pressure, qreal tiltX,
                    qreal tiltY, qreal rotation);
    void endStroke();
    void scheduleFullRender(int delayMs);
    void enqueueFullRender();
    void enqueue(Job job);
    void workerLoop();
    void handleResult(const Result &r);
    QImage backgroundImage(const QSize &size) const;
    static QImage backgroundOf(const Job &job); // worker-side (no members)
    static QImage renderStrokeOnto(const QImage &base, const ::Brush &brush,
                                   const QVector<StrokePoint> &points,
                                   quint64 seed);
    static quint64 strokeSeed(int index);

    ::Brush m_brush;
    int m_brushRevision = 0;
    QVector<QVector<StrokePoint>> m_strokes;
    QVector<StrokePoint> m_active;
    bool m_drawing = false;

    QImage m_baseline; // background + committed strokes, current brush
    QImage m_display;  // what paintEvent blits
    int m_baselineStrokes = -1; // stroke count the baseline represents

    QTimer *m_rebuildTimer = nullptr;
    int m_fullRenders = 0;

    QThread *m_thread = nullptr;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QQueue<Job> m_queue;
    quint64 m_epoch = 0;
    bool m_quit = false;
};

} // namespace brushlib
